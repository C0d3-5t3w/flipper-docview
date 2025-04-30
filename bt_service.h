#pragma once

#include <furi.h>
#include <furi_hal.h>
#include <furi_hal_bt.h>
#include <bt/bt_service/bt.h>


bool ble_file_service_init(void);


bool ble_file_service_send(uint8_t* data, size_t size);


bool ble_file_service_start_transfer(const char* file_name, uint32_t file_size);


bool ble_file_service_end_transfer(void);


void ble_file_service_deinit(void);
