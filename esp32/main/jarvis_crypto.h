#pragma once
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#define JARVIS_KEY_LEN 32
#define JARVIS_CT_LEN  2
#define JARVIS_MAC_LEN 8
#define JARVIS_MAGIC   0xAA

// Wire packet: 15 bytes total
// [magic:1][counter:4 LE][ciphertext:2][mac:8]
typedef struct __attribute__((packed)) {
    uint8_t  magic;
    uint32_t counter;
    uint8_t  ciphertext[JARVIS_CT_LEN];
    uint8_t  mac[JARVIS_MAC_LEN];
} JarvisWire;

_Static_assert(sizeof(JarvisWire) == 15, "JarvisWire must be 15 bytes");

extern const uint8_t JARVIS_KEY[JARVIS_KEY_LEN];

bool jarvis_open(const JarvisWire *in,
                 uint8_t *out_device_id, uint8_t *out_command_id);
