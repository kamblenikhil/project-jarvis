#include "subghz_tx.h"
#include "jarvis_crypto.h"
#include <furi.h>
#include <furi_hal.h>
#include <furi_hal_subghz.h>

#define TAG       "JarvisTX"
#define FREQUENCY 433920000

// exact registers from Momentum firmware cc1101_configs.c
// PA table bytes must immediately follow the 0x00 0x00 terminator
static const uint8_t JARVIS_PRESET[] = {
    0x02, 0x0D, // IOCFG0
    0x03, 0x47, // FIFOTHR
    0x08, 0x05, // PKTCTRL0
    0x0B, 0x06, // FSCTRL1
    0x04, 0x46, // SYNC1
    0x05, 0x4C, // SYNC0
    0x09, 0x00, // ADDR
    0x06, 0x00, // PKTLEN
    0x10, 0xC8, // MDMCFG4
    0x11, 0x93, // MDMCFG3
    0x12, 0x12, // MDMCFG2
    0x13, 0x22, // MDMCFG1
    0x14, 0xF8, // MDMCFG0
    0x15, 0x34, // DEVIATN
    0x18, 0x18, // MCSM0
    0x19, 0x16, // FOCCFG
    0x1B, 0x43, // AGCCTRL2
    0x1C, 0x40, // AGCCTRL1
    0x1D, 0x91, // AGCCTRL0
    0x1E, 0x56, // FREND1
    0x1F, 0x10, // FREND0
    0x20, 0xE9, // FSCAL3
    0x21, 0x2A, // FSCAL2
    0x22, 0x00, // FSCAL1
    0x23, 0x1F, // FSCAL0
    0x00, 0x00, // end marker — PA table must follow immediately
    0xC0, 0x00, 0x00, 0x00, // PA table — 10dBm
    0x00, 0x00, 0x00, 0x00
};

bool subghz_tx_send(uint8_t device_id, uint8_t command_id, uint32_t counter) {
    FURI_LOG_I(TAG, "Sending: device=%d command=%d counter=%lu",
               device_id, command_id, (unsigned long)counter);

    JarvisWire packet;
    if(!jarvis_seal(counter, device_id, command_id, &packet)) {
        FURI_LOG_E(TAG, "jarvis_seal failed");
        return false;
    }

    FURI_LOG_I(TAG, "Wire: %02X cnt=%08lX ct=%02X%02X mac=%02X%02X%02X%02X%02X%02X%02X%02X",
               packet.magic, (unsigned long)packet.counter,
               packet.ciphertext[0], packet.ciphertext[1],
               packet.mac[0], packet.mac[1], packet.mac[2], packet.mac[3],
               packet.mac[4], packet.mac[5], packet.mac[6], packet.mac[7]);

    furi_hal_subghz_reset();
    furi_hal_subghz_idle();
    furi_hal_subghz_load_custom_preset(JARVIS_PRESET);

    uint32_t actual = furi_hal_subghz_set_frequency_and_path(FREQUENCY);
    FURI_LOG_I(TAG, "Frequency: %lu Hz", (unsigned long)actual);

    furi_hal_subghz_flush_tx();

    for(int repeat = 0; repeat < 3; repeat++) {
        // CC1101 packet-mode TX sequence: write data to TXFIFO while IDLE,
        // then assert STX. Calling write_packet after tx() issues SFTX
        // during active TX which is undefined per the CC1101 datasheet.
        furi_hal_subghz_idle();
        furi_delay_ms(5);

        // Load TXFIFO first (write_packet flushes then writes length + data)
        furi_hal_subghz_write_packet(
            (uint8_t*)&packet,
            sizeof(JarvisWire));

        // Now enter TX — CC1101 sends preamble → sync → TXFIFO contents → CRC
        furi_hal_subghz_tx();

        // At 9.99kbps: 4 preamble + 2 sync + 1 len + 8 data + 2 CRC = 17 bytes ≈ 14ms
        furi_delay_ms(50);
        FURI_LOG_I(TAG, "Repeat %d sent", repeat + 1);
    }

    furi_hal_subghz_sleep();
    FURI_LOG_I(TAG, "Sent OK");
    return true;
}
