#pragma once

#include <furi.h>
#include <furi_hal_bt.h>


bool ble_transport_init(void);


bool ble_transport_tx(const uint8_t* data, size_t size);


void ble_transport_set_rx_callback(void (*callback)(uint8_t*, size_t, void*), void* context);


void ble_transport_deinit(void);
