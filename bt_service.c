#include "bt_service.h"
#include "bt_hal_compat.h"
#include "docview_app.h"
#include <furi_hal.h>
#include <furi_hal_bt.h>
#include <stdint.h>
#include <string.h>

#define FILE_CONTROL_START 0x01
#define FILE_CONTROL_END   0x02
#define FILE_CONTROL_ERROR 0xFF

#define MAX_BLE_PACKET_SIZE 20

// Globals for BT service callbacks
static BtEventCallback status_callback = NULL;
static void* status_context = NULL;

// Initialize the BT service
bool bt_service_init(void) {
    return furi_hal_bt_is_active();
}

// Deinitialize the BT service
void bt_service_deinit(void) {
    // Clean up callback
    status_callback = NULL;
    status_context = NULL;
}

// Subscribe to BT status changes
void bt_service_subscribe_status(void* bt, BtEventCallback callback, void* context) {
    UNUSED(bt);
    status_callback = callback;
    status_context = context;
    // Initial connection status notification
    if(callback) {
        if(furi_hal_bt_is_active()) {
            callback(BtStatusAdvertising, context);
        } else {
            callback(BtStatusOff, context);
        }
    }
}

// Unsubscribe from BT status changes
void bt_service_unsubscribe_status(void* bt) {
    UNUSED(bt);
    status_callback = NULL;
    status_context = NULL;
}

// File service implementation (unchanged)
bool ble_file_service_init(void) {
    if(!furi_hal_bt_is_active()) {
        return false;
    }
    return true;
}

bool ble_file_service_send(uint8_t* data, size_t size) {
    size_t remaining = size;
    size_t offset = 0;
    while(remaining > 0) {
        size_t chunk_size = remaining > MAX_BLE_PACKET_SIZE ? MAX_BLE_PACKET_SIZE : remaining;
        bool success = false;
        for(uint8_t attempts = 0; attempts < 3 && !success; attempts++) {
            if(furi_hal_bt_is_active()) {
                uint8_t tx_buffer[MAX_BLE_PACKET_SIZE];
                memcpy(tx_buffer, data + offset, chunk_size);
                int32_t sent = furi_hal_bt_serial_tx(tx_buffer, (uint16_t)chunk_size);
                if(sent == (int32_t)chunk_size) {
                    success = true;
                }
            } else {
                furi_delay_ms(10);
            }
        }
        if(!success) return false;
        offset += chunk_size;
        remaining -= chunk_size;
        furi_delay_ms(5);
    }
    return true;
}

bool ble_file_service_start_transfer(const char* file_name, uint32_t file_size) {
    uint8_t start_packet[MAX_BLE_PACKET_SIZE];
    start_packet[0] = FILE_CONTROL_START;
    start_packet[1] = (file_size >> 24) & 0xFF;
    start_packet[2] = (file_size >> 16) & 0xFF;
    start_packet[3] = (file_size >> 8) & 0xFF;
    start_packet[4] = file_size & 0xFF;
    size_t name_len = strlen(file_name);
    if(name_len > MAX_BLE_PACKET_SIZE - 5) name_len = MAX_BLE_PACKET_SIZE - 5;
    memcpy(&start_packet[5], file_name, name_len);
    if(furi_hal_bt_is_active()) {
        int32_t sent = furi_hal_bt_serial_tx(start_packet, (uint16_t)(name_len + 5));
        return sent == (int32_t)(name_len + 5);
    }
    return false;
}

bool ble_file_service_end_transfer(void) {
    uint8_t end_packet[1] = {FILE_CONTROL_END};
    if(furi_hal_bt_is_active()) {
        int32_t sent = furi_hal_bt_serial_tx(end_packet, 1);
        return sent == 1;
    }
    return false;
}

void ble_file_service_deinit(void) {
    // Nothing to do here, keeping function for API consistency
}
