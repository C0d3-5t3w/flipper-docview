#include "bt_service.h"
#include "bt_hal_compat.h"
#include "../docview.h"
#include <furi_hal.h>
#include <stdint.h>
#include <string.h>
#include <furi.h> // Include furi for logging

#define TAG "BtService" // Define TAG for logging

#define FILE_CONTROL_START 0x01
#define FILE_CONTROL_END   0x02
#define FILE_CONTROL_ERROR 0xFF

#define MAX_BLE_PACKET_SIZE 20

static BtEventCallback status_callback = NULL;
static void* status_context = NULL;
static FuriMutex* bt_mutex = NULL;

bool bt_service_init(void) {
    if(bt_mutex) return true; // Already initialized

    bt_mutex = furi_mutex_alloc(FuriMutexTypeNormal);
    if(!bt_mutex) {
        FURI_LOG_E(TAG, "Failed to allocate mutex");
        return false;
    }

    bool success = false;
    if(furi_mutex_acquire(bt_mutex, FuriWaitForever) == FuriStatusOk) {
        // Check if BT stack is running and record exists
        if(furi_hal_bt_is_alive() && furi_record_exists(RECORD_BT)) {
            // Attempt to initialize BT HAL compatibility layer
            bt_init(); // This might turn on the BT chip if not already on
            if(bt_is_active()) {
                status_callback = NULL;
                status_context = NULL;
                success = true;
                FURI_LOG_I(TAG, "BT Service Initialized");
            } else {
                FURI_LOG_W(TAG, "BT HAL init failed or BT is not active");
            }
        } else {
            FURI_LOG_W(TAG, "BT stack not alive or BT record missing");
        }
        furi_mutex_release(bt_mutex);
    } else {
        FURI_LOG_E(TAG, "Failed to acquire mutex during init");
    }

    if(!success) {
        furi_mutex_free(bt_mutex);
        bt_mutex = NULL;
        FURI_LOG_E(TAG, "BT Service Initialization Failed");
    }

    return success;
}

void bt_service_deinit(void) {
    if(!bt_mutex) return; // Not initialized or already deinitialized

    // Stop any active file transfers first (acquire mutex inside)
    ble_file_service_deinit();

    // Clean up BT status callback
    if(furi_mutex_acquire(bt_mutex, FuriWaitForever) == FuriStatusOk) {
        if(status_callback) {
            // Optionally notify subscriber that BT is turning off
            // status_callback(BtStatusOff, status_context);
        }
        status_callback = NULL;
        status_context = NULL;
        furi_mutex_release(bt_mutex);
    }

    // Free mutex last
    furi_mutex_free(bt_mutex);
    bt_mutex = NULL;
    FURI_LOG_I(TAG, "BT Service Deinitialized");
}

void bt_service_subscribe_status(BtEventCallback callback, void* context) {
    if(!bt_mutex) {
        FURI_LOG_W(TAG, "Cannot subscribe, BT service not initialized");
        return;
    }
    if(furi_mutex_acquire(bt_mutex, FuriWaitForever) == FuriStatusOk) {
        status_callback = callback;
        status_context = context;

        if(callback) {
            // Immediately report current status
            if(bt_is_active()) {
                // Assuming Advertising is the default state when active and not connected
                // More sophisticated state tracking might be needed depending on requirements
                callback(BtStatusAdvertising, context);
            } else {
                callback(BtStatusOff, context);
            }
        }
        furi_mutex_release(bt_mutex);
    }
}

void bt_service_unsubscribe_status(void) {
    if(!bt_mutex) return; // Not initialized

    if(furi_mutex_acquire(bt_mutex, FuriWaitForever) == FuriStatusOk) {
        status_callback = NULL;
        status_context = NULL;
        furi_mutex_release(bt_mutex);
    }
}

bool ble_file_service_init(void) {
    if(!bt_mutex) {
        FURI_LOG_E(TAG, "File service init failed: BT service not initialized");
        return false;
    }
    bool success = false;

    if(furi_mutex_acquire(bt_mutex, FuriWaitForever) == FuriStatusOk) {
        if(bt_is_active()) {
            // Placeholder for actual file service profile setup if needed
            success = true;
            FURI_LOG_I(TAG, "BLE File Service Ready");
        } else {
            FURI_LOG_W(TAG, "File service init failed: BT not active");
        }
        furi_mutex_release(bt_mutex);
    }

    return success;
}

bool ble_file_service_send(uint8_t* data, size_t size) {
    if(!bt_mutex) return false;

    bool success = false;
    if(furi_mutex_acquire(bt_mutex, FuriWaitForever) == FuriStatusOk) {
        if(!bt_is_active()) {
            FURI_LOG_W(TAG, "Send failed: BT not active");
            furi_mutex_release(bt_mutex);
            return false;
        }

        size_t remaining = size;
        size_t offset = 0;
        success = true; // Assume success unless a chunk fails

        while(remaining > 0) {
            size_t chunk_size = remaining > MAX_BLE_PACKET_SIZE ? MAX_BLE_PACKET_SIZE : remaining;
            bool sent_success = false;
            // Retry sending a chunk a few times
            for(uint8_t attempts = 0; attempts < 3 && !sent_success; attempts++) {
                // Re-check bt_is_active() in case it disconnected during transfer
                if(!bt_is_active()) {
                    FURI_LOG_W(TAG, "Send failed: BT disconnected during transfer");
                    success = false;
                    break;
                }

                // Use a temporary buffer on stack or ensure bt_serial_tx handles const data
                uint8_t tx_buffer[MAX_BLE_PACKET_SIZE];
                memcpy(tx_buffer, data + offset, chunk_size);
                int32_t sent = bt_serial_tx(tx_buffer, (uint16_t)chunk_size);

                if(sent == (int32_t)chunk_size) {
                    sent_success = true;
                } else {
                    FURI_LOG_W(TAG, "Chunk send failed, attempt %d", attempts + 1);
                    furi_delay_ms(10); // Small delay before retry
                }
            }

            if(!sent_success) {
                FURI_LOG_E(TAG, "Failed to send chunk after retries");
                success = false;
                break; // Exit the while loop if a chunk fails definitively
            }

            offset += chunk_size;
            remaining -= chunk_size;
            // Add a small delay between packets to avoid overwhelming the receiver
            // This value might need tuning
            furi_delay_ms(5);
        }
        furi_mutex_release(bt_mutex);
    } else {
        FURI_LOG_E(TAG, "Send failed: Could not acquire mutex");
    }
    return success;
}

bool ble_file_service_start_transfer(const char* file_name, uint32_t file_size) {
    if(!bt_mutex) return false;

    bool success = false;
    if(furi_mutex_acquire(bt_mutex, FuriWaitForever) == FuriStatusOk) {
        if(!bt_is_active()) {
            FURI_LOG_W(TAG, "Start transfer failed: BT not active");
            furi_mutex_release(bt_mutex);
            return false;
        }

        uint8_t start_packet[MAX_BLE_PACKET_SIZE]; // Use max size for safety
        memset(start_packet, 0, sizeof(start_packet)); // Clear the buffer

        start_packet[0] = FILE_CONTROL_START;
        // Pack file size (Big Endian)
        start_packet[1] = (file_size >> 24) & 0xFF;
        start_packet[2] = (file_size >> 16) & 0xFF;
        start_packet[3] = (file_size >> 8) & 0xFF;
        start_packet[4] = file_size & 0xFF;

        // Pack file name, ensuring null termination within buffer limits if possible
        size_t name_len = strlen(file_name);
        size_t max_name_len = sizeof(start_packet) - 5 -
                              1; // Reserve space for control byte, size, and null terminator
        if(name_len > max_name_len) {
            name_len = max_name_len;
            FURI_LOG_W(TAG, "Filename truncated for BLE transfer");
        }
        memcpy(&start_packet[5], file_name, name_len);
        start_packet[5 + name_len] = '\0'; // Null-terminate the name

        size_t packet_len = 5 + name_len + 1; // Include null terminator

        int32_t sent = bt_serial_tx(start_packet, (uint16_t)packet_len);
        success = (sent == (int32_t)packet_len);

        if(!success) {
            FURI_LOG_E(TAG, "Failed to send start transfer packet");
        }

        furi_mutex_release(bt_mutex);
    } else {
        FURI_LOG_E(TAG, "Start transfer failed: Could not acquire mutex");
    }
    return success;
}

bool ble_file_service_end_transfer(void) {
    if(!bt_mutex) return false;

    bool success = false;
    if(furi_mutex_acquire(bt_mutex, FuriWaitForever) == FuriStatusOk) {
        if(!bt_is_active()) {
            // Don't log error here, might be normal if disconnected
            furi_mutex_release(bt_mutex);
            return false;
        }
        uint8_t end_packet[1] = {FILE_CONTROL_END};
        int32_t sent = bt_serial_tx(end_packet, 1);
        success = (sent == 1);
        if(!success) {
            FURI_LOG_E(TAG, "Failed to send end transfer packet");
        }
        furi_mutex_release(bt_mutex);
    } else {
        FURI_LOG_E(TAG, "End transfer failed: Could not acquire mutex");
    }
    return success;
}

void ble_file_service_deinit(void) {
    if(!bt_mutex) return; // Not initialized

    // Send error packet only if BT is currently active
    if(furi_mutex_acquire(bt_mutex, FuriWaitForever) == FuriStatusOk) {
        if(bt_is_active()) {
            uint8_t error_packet[1] = {FILE_CONTROL_ERROR};
            // Best effort send, ignore result
            bt_serial_tx(error_packet, 1);
            FURI_LOG_I(TAG, "Sent error packet during deinit");
        }
        // Placeholder for actual file service profile teardown if needed
        furi_mutex_release(bt_mutex);
    }
    // No need to log error if mutex acquisition fails here
}
