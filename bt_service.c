#include "bt_service.h"
#include "bt_hal_compat.h"
#include "docview_app.h"
#include <furi_hal.h>
#include <stdint.h>
#include <string.h>

#define FILE_CONTROL_START 0x01
#define FILE_CONTROL_END   0x02
#define FILE_CONTROL_ERROR 0xFF

#define MAX_BLE_PACKET_SIZE 20

static BtEventCallback status_callback = NULL;
static void* status_context = NULL;
static FuriMutex* bt_mutex = NULL;

bool bt_service_init(void) {
    if(bt_mutex) return false; // Already initialized

    bt_mutex = furi_mutex_alloc(FuriMutexTypeNormal);
    if(!bt_mutex) return false;

    bool success = false;
    if(furi_mutex_acquire(bt_mutex, FuriWaitForever) == FuriStatusOk) {
        bt_init();
        if(bt_is_active()) {
            status_callback = NULL;
            status_context = NULL;
            success = true;
        }
        furi_mutex_release(bt_mutex);
    }

    if(!success) {
        furi_mutex_free(bt_mutex);
        bt_mutex = NULL;
    }

    return success;
}

void bt_service_deinit(void) {
    furi_assert(bt_mutex);

    if(furi_mutex_acquire(bt_mutex, FuriWaitForever) == FuriStatusOk) {
        // Stop any active transfers first
        ble_file_service_deinit();

        if(status_callback) {
            status_callback(BtStatusOff, status_context);
            status_callback = NULL;
            status_context = NULL;
        }

        furi_mutex_release(bt_mutex);
    }

    furi_mutex_free(bt_mutex);
    bt_mutex = NULL;
}

void bt_service_subscribe_status(void* bt, BtEventCallback callback, void* context) {
    UNUSED(bt);
    if(furi_mutex_acquire(bt_mutex, FuriWaitForever) == FuriStatusOk) {
        status_callback = callback;
        status_context = context;

        if(callback) {
            if(bt_is_active()) {
                callback(BtStatusAdvertising, context);
            } else {
                callback(BtStatusOff, context);
            }
        }
        furi_mutex_release(bt_mutex);
    }
}

void bt_service_unsubscribe_status(void* bt) {
    UNUSED(bt);
    if(furi_mutex_acquire(bt_mutex, FuriWaitForever) == FuriStatusOk) {
        status_callback = NULL;
        status_context = NULL;
        furi_mutex_release(bt_mutex);
    }
}

bool ble_file_service_init(void) {
    furi_assert(bt_mutex);
    bool success = false;

    if(furi_mutex_acquire(bt_mutex, FuriWaitForever) == FuriStatusOk) {
        if(bt_is_active()) {
            success = true;
        }
        furi_mutex_release(bt_mutex);
    }

    return success;
}

bool ble_file_service_send(uint8_t* data, size_t size) {
    bool success = false;
    if(furi_mutex_acquire(bt_mutex, FuriWaitForever) == FuriStatusOk) {
        size_t remaining = size;
        size_t offset = 0;
        while(remaining > 0) {
            size_t chunk_size = remaining > MAX_BLE_PACKET_SIZE ? MAX_BLE_PACKET_SIZE : remaining;
            bool sent_success = false;
            for(uint8_t attempts = 0; attempts < 3 && !sent_success; attempts++) {
                if(bt_is_active()) {
                    uint8_t tx_buffer[MAX_BLE_PACKET_SIZE];
                    memcpy(tx_buffer, data + offset, chunk_size);
                    int32_t sent = bt_serial_tx(tx_buffer, (uint16_t)chunk_size);
                    if(sent == (int32_t)chunk_size) {
                        sent_success = true;
                    }
                } else {
                    furi_delay_ms(10);
                }
            }
            if(!sent_success) {
                success = false;
                break;
            }
            offset += chunk_size;
            remaining -= chunk_size;
            furi_delay_ms(5);
        }
        success = true;
        furi_mutex_release(bt_mutex);
    }
    return success;
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
    if(bt_is_active()) {
        int32_t sent = bt_serial_tx(start_packet, (uint16_t)(name_len + 5));
        return sent == (int32_t)(name_len + 5);
    }
    return false;
}

bool ble_file_service_end_transfer(void) {
    uint8_t end_packet[1] = {FILE_CONTROL_END};
    if(bt_is_active()) {
        int32_t sent = bt_serial_tx(end_packet, 1);
        return sent == 1;
    }
    return false;
}

void ble_file_service_deinit(void) {
    furi_assert(bt_mutex);

    if(bt_is_active()) {
        if(furi_mutex_acquire(bt_mutex, FuriWaitForever) == FuriStatusOk) {
            uint8_t error_packet[1] = {FILE_CONTROL_ERROR};
            bt_serial_tx(error_packet, 1);
            furi_mutex_release(bt_mutex);
        }
    }
}
