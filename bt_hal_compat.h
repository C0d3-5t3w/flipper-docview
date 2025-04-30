#pragma once

#include <stdint.h>
#include <stddef.h>
#include <furi_hal.h>


#ifdef FURI_HAL_BT_INCLUDE_TX
#include <furi_hal_bt.h>
#define BT_ENABLED 1
#else
#define BT_ENABLED 0
#endif


static inline bool bt_is_active(void) {
#if BT_ENABLED
    return furi_hal_bt_is_active();
#else
    return false;
#endif
}


static inline int32_t bt_serial_tx(const uint8_t* data, uint16_t size) {
#if BT_ENABLED
    return furi_hal_bt_serial_tx(data, size);
#else
    UNUSED(data);
    UNUSED(size);
    return 0;
#endif
}


static inline void bt_init(void) {
#if BT_ENABLED
    furi_hal_bt_init();
#endif
}


static inline uint16_t bt_get_max_packet_size(void) {
    return 20;
}

static inline uint8_t* bt_get_tx_buffer(void) {
    static uint8_t tx_buffer[128];
    return tx_buffer;
}

typedef void (*BtSerialCallback)(uint8_t* data, size_t size, void* ctx);
