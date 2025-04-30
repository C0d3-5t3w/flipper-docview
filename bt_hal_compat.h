#pragma once

#include <furi_hal.h>
#include <furi.h>


static inline uint16_t furi_hal_bt_get_max_packet_size(void) {
    
    return 20;
}


static inline uint8_t* furi_hal_bt_get_tx_buffer(void) {
    
    static uint8_t tx_buffer[128];
    return tx_buffer;
}


typedef void (*BtSerialCallback)(uint8_t* data, size_t size, void* ctx);


static inline uint16_t furi_hal_bt_serial_tx(uint8_t* data, size_t size) {
    UNUSED(data);
    UNUSED(size);
    
    
    return size; 
}


static inline void furi_hal_bt_serial_set_event_callback(
    BtSerialCallback callback, 
    void* context) {
    UNUSED(callback);
    UNUSED(context);
    
}
