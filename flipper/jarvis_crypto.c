#include "jarvis_crypto.h"
#include <string.h>
#include <furi_hal.h>
#include <furi_hal_crypto.h>

// Generate your own key: head -c 32 /dev/urandom | xxd -i
// Must match jarvis_crypto.c on the ESP32 side
const uint8_t JARVIS_KEY[JARVIS_KEY_LEN] = {
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
};

// IV = counter (4 LE) || zeros (8) = 12 bytes
static void build_iv(uint32_t counter, uint8_t iv[12]) {
    memset(iv, 0, 12);
    iv[0] = counter & 0xFF;
    iv[1] = (counter >> 8) & 0xFF;
    iv[2] = (counter >> 16) & 0xFF;
    iv[3] = (counter >> 24) & 0xFF;
}

bool jarvis_seal(uint32_t counter, uint8_t device_id, uint8_t command_id,
                 JarvisWire *out) {
    uint8_t iv[12];
    build_iv(counter, iv);

    uint8_t pt[2] = {device_id, command_id};
    uint8_t full_tag[16];

    FuriHalCryptoGCMState st = furi_hal_crypto_gcm_encrypt_and_tag(
        JARVIS_KEY, iv,
        NULL, 0,           // no AAD
        pt, out->ciphertext, 2,
        full_tag);

    if(st != FuriHalCryptoGCMStateOk) return false;

    out->magic   = JARVIS_MAGIC;
    out->counter = counter;
    memcpy(out->mac, full_tag, JARVIS_MAC_LEN);
    return true;
}

bool jarvis_open(const JarvisWire *in,
                 uint8_t *out_device_id, uint8_t *out_command_id) {
    // Not used on Flipper TX side, but keep stub for shared header
    (void)in; (void)out_device_id; (void)out_command_id;
    return false;
}
