#pragma once
#include <stdint.h>
#include <stdbool.h>

bool subghz_tx_send(uint8_t device_id, uint8_t command_id, uint32_t counter);
