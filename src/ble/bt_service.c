#include "bt_service.h"
#include <furi.h> // Include furi first
#include <furi_hal.h> // Then include general HAL
#include <furi_hal_bt.h> // Then include specific BT HAL
#include <furi_hal_resources.h> // Include resources definitions
#include <stdint.h>
#include <string.h>
#include "bt_hal_compat.h" // Include our compatibility layer
#include "furi_hal_bt_custom.h" // Include our custom BT declarations

#define TAG "BtService" // Define TAG for logging

#define FILE_CONTROL_START 0x01
#define FILE_CONTROL_END   0x02
#define FILE_CONTROL_ERROR 0xFF

static BtEventCallback status_callback = NULL;
static void* status_context = NULL;
static FuriMutex* bt_mutex = NULL;
static BtStatus current_bt_status = BtStatusOff; // Track internal status
static FuriHalBtStatusCallback hal_callback_handle = NULL;

// Internal HAL status callback
static void bt_hal_status_callback(FuriHalBtStatus status, void* context) {
    UNUSED(context);
    BtStatus new_status = BtStatusOff; // Default

    // Map HAL status to our internal enum
    switch(status) {
    case FuriHalBtStatusIdle:
        // Consider Idle as Off unless explicitly advertising/connected
        new_status = BtStatusOff;
        break;
    case FuriHalBtStatusAdvertising:
        new_status = BtStatusAdvertising;
        break;
    case FuriHalBtStatusConnected:
        new_status = BtStatusConnected;
        break;
    default:
        new_status = BtStatusOff;
        break;
    }

    // Update status and notify subscriber if changed
    if(furi_mutex_acquire(bt_mutex, FuriWaitForever) == FuriStatusOk) {
        if(current_bt_status != new_status) {
            FURI_LOG_I(TAG, "BT HAL Status Changed: %d -> %d", current_bt_status, new_status);
            current_bt_status = new_status;
            if(status_callback) {
                // Run user callback outside mutex if possible, or ensure it's quick
                status_callback(current_bt_status, status_context);
            }
        }
        furi_mutex_release(bt_mutex);
    }
}

bool bt_service_init(void) {
    if(bt_mutex) return true; // Already initialized

    bt_mutex = furi_mutex_alloc(FuriMutexTypeNormal);
    if(!bt_mutex) {
        FURI_LOG_E(TAG, "Failed to allocate mutex");
        return false;
    }

    bool success = false;
    if(furi_mutex_acquire(bt_mutex, FuriWaitForever) == FuriStatusOk) {
        // Check if BT stack is running and the "bt" record exists
        if(furi_hal_bt_is_alive() && furi_record_exists(RECORD_BT)) { // Use RECORD_BT
            if(furi_hal_bt_is_active()) {
                status_callback = NULL;
                status_context = NULL;
                current_bt_status = BtStatusOff; // Initial state

                // Register HAL status callback
                hal_callback_handle =
                    furi_hal_bt_set_status_changed_callback(bt_hal_status_callback, NULL);

                // Update initial status based on HAL
                bt_hal_status_callback(furi_hal_bt_get_status(), NULL);

                success = true;
                FURI_LOG_I(TAG, "BT Service Initialized");
            } else {
                FURI_LOG_W(TAG, "BT is not active");
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

    // Unregister HAL callback
    if(hal_callback_handle) {
        furi_hal_bt_set_status_changed_callback(NULL, NULL);
        hal_callback_handle = NULL;
    }

    // Clean up BT status callback
    if(furi_mutex_acquire(bt_mutex, FuriWaitForever) == FuriStatusOk) {
        status_callback = NULL;
        status_context = NULL;
        current_bt_status = BtStatusOff;
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
            callback(current_bt_status, context);
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
        // Check status inside the loop as well
        if(current_bt_status != BtStatusConnected) {
            FURI_LOG_W(TAG, "Send failed: BT not connected");
            furi_mutex_release(bt_mutex);
            return false;
        }

        // Get max packet size dynamically
        uint16_t max_ble_packet_size = bt_get_max_packet_size();
        if(max_ble_packet_size == 0) {
            // Handle error case where packet size is zero (e.g., BT not ready)
            FURI_LOG_E(TAG, "Send failed: Invalid max packet size (0)");
            furi_mutex_release(bt_mutex);
            return false;
        }

        size_t remaining = size;
        size_t offset = 0;
        success = true; // Assume success unless a chunk fails

        while(remaining > 0) {
            // Re-check status inside the loop
            if(current_bt_status != BtStatusConnected) {
                FURI_LOG_W(TAG, "Send failed: BT disconnected during transfer");
                success = false;
                break;
            }

            size_t chunk_size = remaining > max_ble_packet_size ? max_ble_packet_size : remaining;
            bool sent_success = false;
            // Retry sending a chunk a few times
            for(uint8_t attempts = 0; attempts < 3 && !sent_success; attempts++) {
                // Re-check status before each attempt
                if(current_bt_status != BtStatusConnected) {
                    FURI_LOG_W(TAG, "Send attempt failed: BT disconnected");
                    success = false;
                    break; // Break retry loop
                }

                // Use a temporary buffer on stack or ensure bt_serial_tx handles const data
                // bt_serial_tx takes const uint8_t*, so direct pointer is fine
                int32_t sent = bt_serial_tx(data + offset, (uint16_t)chunk_size);

                if(sent == (int32_t)chunk_size) {
                    sent_success = true;
                } else {
                    FURI_LOG_W(TAG, "Chunk send failed (ret %ld), attempt %d", sent, attempts + 1);
                    furi_delay_ms(20); // Increase delay slightly before retry
                }
            }

            if(!success) break; // Break while loop if BT disconnected during retries

            if(!sent_success) {
                FURI_LOG_E(TAG, "Failed to send chunk after retries");
                success = false;
                break; // Exit the while loop if a chunk fails definitively
            }

            offset += chunk_size;
            remaining -= chunk_size;
            // Add a small delay between packets to avoid overwhelming the receiver
            // This value might need tuning
            furi_delay_ms(10); // Keep a small delay
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
        // Check if connected before starting
        if(current_bt_status != BtStatusConnected) {
            FURI_LOG_W(TAG, "Start transfer failed: BT not connected");
            furi_mutex_release(bt_mutex);
            return false;
        }

        // Get max packet size for buffer allocation, ensure it's large enough for header
        uint16_t max_packet_size = bt_get_max_packet_size();
        if(max_packet_size < 64) { // Need space for control, size, and some name
            FURI_LOG_E(
                TAG, "Start transfer failed: Max packet size too small (%d)", max_packet_size);
            furi_mutex_release(bt_mutex);
            return false;
        }

        uint8_t start_packet[max_packet_size]; // Use dynamic size
        memset(start_packet, 0, sizeof(start_packet)); // Clear the buffer

        start_packet[0] = FILE_CONTROL_START;
        // Pack file size (Big Endian)
        start_packet[1] = (file_size >> 24) & 0xFF;
        start_packet[2] = (file_size >> 16) & 0xFF;
        start_packet[3] = (file_size >> 8) & 0xFF;
        start_packet[4] = file_size & 0xFF;

        // Pack file name, ensuring null termination within buffer limits if possible
        size_t name_len = strlen(file_name);
        // Calculate max name length based on actual max_packet_size
        size_t max_name_len = max_packet_size - 5 - 1;
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
            FURI_LOG_E(TAG, "Failed to send start transfer packet (ret %ld)", sent);
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
        // Check if connected before sending end packet
        if(current_bt_status != BtStatusConnected) {
            // Don't log error here, might be normal if disconnected just before this call
            furi_mutex_release(bt_mutex);
            return false;
        }
        uint8_t end_packet[1] = {FILE_CONTROL_END};
        int32_t sent = bt_serial_tx(end_packet, 1);
        success = (sent == 1);
        if(!success) {
            FURI_LOG_E(TAG, "Failed to send end transfer packet (ret %ld)", sent);
        }
        furi_mutex_release(bt_mutex);
    } else {
        FURI_LOG_E(TAG, "End transfer failed: Could not acquire mutex");
    }
    return success;
}

void ble_file_service_deinit(void) {
    if(!bt_mutex) return; // Not initialized

    // Send error packet only if BT is currently connected
    if(furi_mutex_acquire(bt_mutex, FuriWaitForever) == FuriStatusOk) {
        if(current_bt_status == BtStatusConnected) {
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
