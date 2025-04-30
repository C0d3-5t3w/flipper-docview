#include "ble_transport.h"
#include "bt_hal_compat.h"
#include <string.h>

static void (*user_rx_callback)(uint8_t*, size_t, void*) = NULL;
static void* user_context = NULL;

bool ble_transport_init(void) {
    return furi_hal_bt_is_active();
}

bool ble_transport_tx(const uint8_t* data, size_t size) {
    if(!furi_hal_bt_is_active()) {
        return false;
    }
    uint16_t max_packet_size = furi_hal_bt_get_max_packet_size();
    if(size <= max_packet_size) {
        uint8_t* tx_buffer = furi_hal_bt_get_tx_buffer();
        memcpy(tx_buffer, data, size);
        int32_t sent = furi_hal_bt_serial_tx(tx_buffer, (uint16_t)size);
        return sent == (int32_t)size;
    }
    return false;
}

void ble_transport_set_rx_callback(void (*callback)(uint8_t*, size_t, void*), void* context) {
    user_rx_callback = callback;
    user_context = context;
}

void ble_transport_deinit(void) {
    user_rx_callback = NULL;
    user_context = NULL;
}
