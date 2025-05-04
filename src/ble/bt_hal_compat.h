#pragma once

#include <stdint.h>
#include <stddef.h>
#include <furi_hal.h>
#include <furi_hal_bt.h>
#include "furi_hal_bt_custom.h"

// We'll use our custom implementation instead of conditionally compiling
#define BT_ENABLED 1

// These functions are directly from SDK
static inline bool bt_is_active(void) {
    return furi_hal_bt_is_active();
}

static inline bool bt_is_alive(void) {
    return furi_hal_bt_is_alive();
}

// These are our custom implementations accessed through defines
static inline int32_t bt_serial_tx(const uint8_t* data, uint16_t size) {
    return furi_hal_bt_serial_tx(data, size);
}

static inline void bt_init(void) {
    // Initialize BT subsystem if needed
    // This is a placeholder since our custom implementation doesn't need it
}

static inline uint16_t bt_get_max_packet_size(void) {
    return furi_hal_bt_get_max_packet_size();
}

typedef void (*BtSerialCallback)(uint8_t* data, size_t size, void* ctx);
