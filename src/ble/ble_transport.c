#include "ble_transport.h"
#include "bt_hal_compat.h"
#include <string.h>
#include <furi.h> // Include for logging

#define TAG "BleTransport" // Define TAG for logging

static void (*user_rx_callback)(uint8_t*, size_t, void*) = NULL;
static void* user_context = NULL;

bool ble_transport_init(void) {
    // Check if the underlying BT HAL compatibility layer reports active
    bool active = bt_is_active();
    if(!active) {
        FURI_LOG_W(TAG, "Init: BT is not active");
    }
    return active;
}

bool ble_transport_tx(const uint8_t* data, size_t size) {
    if(!bt_is_active()) {
        FURI_LOG_W(TAG, "TX failed: BT is not active");
        return false;
    }

    uint16_t max_packet_size = bt_get_max_packet_size();
    if(size > max_packet_size) {
        FURI_LOG_E(
            TAG, "TX failed: Data size (%d) exceeds max packet size (%d)", size, max_packet_size);
        return false; // Prevent sending oversized packets
    }

    // The bt_hal_compat layer handles the buffer, just call tx
    int32_t sent = bt_serial_tx(data, (uint16_t)size);
    if(sent != (int32_t)size) {
        FURI_LOG_W(TAG, "TX failed: bt_serial_tx returned %ld, expected %d", sent, size);
        return false;
    }

    return true;
}

void ble_transport_set_rx_callback(void (*callback)(uint8_t*, size_t, void*), void* context) {
    user_rx_callback = callback;
    user_context = context;
}

void ble_transport_deinit(void) {
    user_rx_callback = NULL;
    user_context = NULL;
}
