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

    if(size <= furi_hal_bt_get_max_packet_size()) {
        memcpy(furi_hal_bt_get_tx_buffer(), data, size);
        return true;
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
