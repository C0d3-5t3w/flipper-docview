#pragma once

#include <furi.h>
#include <furi_hal.h>
#include <furi_hal_bt.h>
#include "docview_app.h" 


typedef void (*BtEventCallback)(BtStatus event, void* context);


bool bt_service_init(void);
void bt_service_deinit(void);
void bt_service_subscribe_status(BtEventCallback callback, void* context);
void bt_service_unsubscribe_status(void); // Remove void* bt parameter


bool ble_file_service_init(void);
bool ble_file_service_send(uint8_t* data, size_t size);
bool ble_file_service_start_transfer(const char* file_name, uint32_t file_size);
bool ble_file_service_end_transfer(void);
void ble_file_service_deinit(void);
