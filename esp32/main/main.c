#include <stdio.h>
#include <inttypes.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include <cc1101.h>
#include "driver/gpio.h"
#include "jarvis_crypto.h"

static const char *TAG = "MAIN";

// Anti-replay: reject packets with counter <= last_counter
static uint32_t last_counter = 0;

#if CONFIG_RECEIVER
void rx_task(void *pvParameter) {
    ESP_LOGI(pcTaskGetName(NULL), "Start");

    int tick = 0;
    int prev_rssi = -100;

    while (1) {
        uint8_t rssi_raw = readStatusReg(0x34);
        int rssi_dbm = (rssi_raw >= 128) ? (rssi_raw - 256) / 2 - 74
                                         : (int)rssi_raw / 2 - 74;
        uint8_t marc = readStatusReg(CC1101_MARCSTATE) & 0x1F;

        // RSSI-only diagnostic: flag when signal appears
        if (rssi_dbm > -60 && prev_rssi <= -60) {
            int8_t freqest = (int8_t)readStatusReg(CC1101_FREQEST);
            int freq_off_khz = (int)freqest * 26000 / 16384;
            printf("SIGNAL %ddBm FREQEST=%d (%dkHz off) marc=0x%02X\n",
                   rssi_dbm, freqest, freq_off_khz, marc);
        }
        prev_rssi = rssi_dbm;

        // Packet reception (ISR-driven)
        if (packet_available()) {
            CCPACKET pkt;
            if (receiveData(&pkt) > 0) {
                int prssi = (pkt.rssi >= 128) ? (pkt.rssi - 256) / 2 - 74
                                              : pkt.rssi / 2 - 74;
                if (pkt.crc_ok && pkt.length == sizeof(JarvisWire)) {
                    JarvisWire wire;
                    memcpy(&wire, pkt.data, sizeof(JarvisWire));
                    uint8_t device_id, command_id;
                    if (jarvis_open(&wire, &device_id, &command_id)) {
                        if (wire.counter > last_counter) {
                            last_counter = wire.counter;
                            // Machine-readable line for home server
                            printf("JARVIS device=%u cmd=%u counter=%lu rssi=%d\n",
                                   device_id, command_id,
                                   (unsigned long)wire.counter, prssi);
                        } else {
                            printf("# replay-rejected counter=%lu (last=%lu)\n",
                                   (unsigned long)wire.counter,
                                   (unsigned long)last_counter);
                        }
                    } else {
                        printf("# auth-failed len=%d rssi=%d\n", pkt.length, prssi);
                    }
                } else {
                    // Unexpected length or CRC fail — likely noise
                    printf("# noise len=%d crc=%d rssi=%d\n",
                           pkt.length, pkt.crc_ok, prssi);
                }
            }
        }

        // Alive heartbeat + periodic VCO recalibration
        // RXOFF_MODE=stay_in_RX skips FS_AUTOCAL (IDLE→RX), so we force it here
        if (++tick >= 5000) {
            tick = 0;
            printf("alive rssi=%ddBm marc=0x%02X\n", rssi_dbm, marc);
            setIdleState();
            cmdStrobe(CC1101_SRX);
        }
        // Recovery: return to RX if CC1101 left it for any reason
        if (marc != 0x0D) {
            setIdleState();
            flushRxFifo();
            cmdStrobe(CC1101_SRX);
        }

        vTaskDelay(1);
    }
    vTaskDelete(NULL);
}
#endif

void app_main() {
    uint8_t freq;
#if CONFIG_CC1101_FREQ_433
    freq = CFREQ_433;
    ESP_LOGW(TAG, "Frequency: 433MHz");
#elif CONFIG_CC1101_FREQ_868
    freq = CFREQ_868;
#elif CONFIG_CC1101_FREQ_915
    freq = CFREQ_915;
#elif CONFIG_CC1101_FREQ_315
    freq = CFREQ_315;
#endif

    uint8_t mode;
#if CONFIG_CC1101_SPEED_9600
    mode = CSPEED_9600;
    ESP_LOGW(TAG, "Speed: 9600bps");
#elif CONFIG_CC1101_SPEED_38400
    mode = CSPEED_38400;
#elif CONFIG_CC1101_SPEED_4800
    mode = CSPEED_4800;
#elif CONFIG_CC1101_SPEED_19200
    mode = CSPEED_19200;
#endif

    esp_err_t ret = init(freq, mode);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "CC1101 not installed");
        while (1) { vTaskDelay(1); }
    }

    // Packet mode, GFSK 9.99kbps @ 433.92MHz, matching Flipper JARVIS_PRESET.
    // FOCCFG=0x17 gives AFC ±BW/2 tracking (50kHz) — Flipper measured 23kHz off.
    writeReg(CC1101_IOCFG0,   0x06); // GDO0: high=sync rx'd, low=end-of-packet
    writeReg(CC1101_FIFOTHR,  0x47);
    writeReg(CC1101_SYNC1,    0x46);
    writeReg(CC1101_SYNC0,    0x4C);
    writeReg(CC1101_PKTLEN,   0x00); // no max-length limit in variable-length mode
    writeReg(CC1101_PKTCTRL1, 0x04); // APPEND_STATUS=1, no address check
    writeReg(CC1101_PKTCTRL0, 0x05); // variable length, CRC enabled
    writeReg(CC1101_FSCTRL1,  0x06);
    writeReg(CC1101_FREQ2,    0x10);
    writeReg(CC1101_FREQ1,    0xB0);
    writeReg(CC1101_FREQ0,    0x71);
    writeReg(CC1101_MDMCFG4,  0xC8);
    writeReg(CC1101_MDMCFG3,  0x93);
    writeReg(CC1101_MDMCFG2,  0x12); // GFSK, 16/16 sync word detection
    writeReg(CC1101_MDMCFG1,  0x22);
    writeReg(CC1101_MDMCFG0,  0xF8);
    writeReg(CC1101_DEVIATN,   0x34);
    writeReg(CC1101_MCSM1,    0x3C); // RXOFF_MODE=stay in RX, CCA=default
    writeReg(CC1101_MCSM0,    0x18);
    writeReg(CC1101_FOCCFG,   0x17); // ← FOC_LIMIT = ±BW/2 (50kHz) instead of ±BW/4 (25kHz)
    writeReg(CC1101_AGCCTRL2, 0x43);
    writeReg(CC1101_AGCCTRL1, 0x40);
    writeReg(CC1101_AGCCTRL0, 0x91);
    writeReg(CC1101_FREND1,   0x56);
    writeReg(CC1101_FREND0,   0x10);
    writeReg(CC1101_FSCAL3,   0xE9);
    writeReg(CC1101_FSCAL2,   0x2A);
    writeReg(CC1101_FSCAL1,   0x00);
    writeReg(CC1101_FSCAL0,   0x1F);
    writeReg(CC1101_CHANNR,   0x00);

    ESP_LOGW(TAG, "PACKET mode — PKTCTRL0=%02X MDMCFG2=%02X FOCCFG=%02X",
             readConfigReg(CC1101_PKTCTRL0), readConfigReg(CC1101_MDMCFG2),
             readConfigReg(CC1101_FOCCFG));

    cmdStrobe(CC1101_SIDLE);
    flushRxFifo();
    cmdStrobe(CC1101_SRX);

#if CONFIG_RECEIVER
    xTaskCreate(&rx_task, "RX", 1024 * 4, NULL, 5, NULL);
#endif
}
