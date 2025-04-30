#pragma once

#include <stdint.h>
#include <stddef.h>
#include <furi_hal_bt.h>

// Provide real BT serial TX
extern int32_t furi_hal_bt_serial_tx(const uint8_t* data, uint16_t size);

static inline uint16_t furi_hal_bt_get_max_packet_size(void) {
    return 20;
}

static inline uint8_t* furi_hal_bt_get_tx_buffer(void) {
    static uint8_t tx_buffer[128];
    return tx_buffer;
}

typedef void (*BtSerialCallback)(uint8_t* data, size_t size, void* ctx);
