#include "jarvis_crypto.h"
#include <string.h>
#include "aes/esp_aes_gcm.h"
#include "mbedtls/private/cipher.h" // MBEDTLS_CIPHER_ID_AES
#include "mbedtls/private/gcm.h"    // MBEDTLS_GCM_DECRYPT

// Generate your own key: head -c 32 /dev/urandom | xxd -i
// Must match jarvis_crypto.c on the Flipper side
const uint8_t JARVIS_KEY[JARVIS_KEY_LEN] = {
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
};

static void build_iv(uint32_t counter, uint8_t iv[12]) {
    memset(iv, 0, 12);
    iv[0] = counter & 0xFF;
    iv[1] = (counter >> 8) & 0xFF;
    iv[2] = (counter >> 16) & 0xFF;
    iv[3] = (counter >> 24) & 0xFF;
}

bool jarvis_open(const JarvisWire *in,
                 uint8_t *out_device_id, uint8_t *out_command_id) {
    if (in->magic != JARVIS_MAGIC) return false;

    uint8_t iv[12];
    build_iv(in->counter, iv);

    esp_gcm_context gcm;
    esp_aes_gcm_init(&gcm);
    if (esp_aes_gcm_setkey(&gcm, MBEDTLS_CIPHER_ID_AES, JARVIS_KEY, 256) != 0) {
        esp_aes_gcm_free(&gcm);
        return false;
    }

    uint8_t pt[2];
    // tag_len=8 verifies the first 8 bytes of the standard 16-byte GCM tag
    int rc = esp_aes_gcm_auth_decrypt(&gcm, 2,
                                      iv, 12,
                                      NULL, 0,            // no AAD
                                      in->mac, JARVIS_MAC_LEN,
                                      in->ciphertext, pt);
    esp_aes_gcm_free(&gcm);

    if (rc != 0) return false;
    *out_device_id  = pt[0];
    *out_command_id = pt[1];
    return true;
}
