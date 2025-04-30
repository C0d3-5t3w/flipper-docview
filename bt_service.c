#include "bt_service.h"
#include "bt_hal_compat.h"
#include <notification/notification_messages.h>
#include <stdint.h>
#include <string.h>

#define FILE_CONTROL_START 0x01
#define FILE_CONTROL_END   0x02
#define FILE_CONTROL_ERROR 0xFF

#define MAX_BLE_PACKET_SIZE 20

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
        size_t chunk_size = (remaining > MAX_BLE_PACKET_SIZE) ? MAX_BLE_PACKET_SIZE : remaining;

        bool success = false;
        for(uint8_t attempts = 0; attempts < 3 && !success; attempts++) {
            if(furi_hal_bt_is_active()) {
                uint8_t* tx_buffer = furi_hal_bt_get_tx_buffer();
                memcpy(tx_buffer, data + offset, chunk_size);
                furi_hal_bt_serial_tx(tx_buffer, chunk_size);
                success = true;
            } else {
                furi_delay_ms(10);
            }
        }

        if(!success) {
            return false;
        }

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
        uint8_t* tx_buffer = furi_hal_bt_get_tx_buffer();
        memcpy(tx_buffer, start_packet, name_len + 5);
        furi_hal_bt_serial_tx(tx_buffer, name_len + 5);
        return true;
    }
    return false;
}

bool ble_file_service_end_transfer(void) {
    uint8_t end_packet[1] = {FILE_CONTROL_END};
    if(furi_hal_bt_is_active()) {
        uint8_t* tx_buffer = furi_hal_bt_get_tx_buffer();
        memcpy(tx_buffer, end_packet, 1);
        furi_hal_bt_serial_tx(tx_buffer, 1);
        return true;
    }
    return false;
}

void ble_file_service_deinit(void) {
}
