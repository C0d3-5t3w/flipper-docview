#pragma once
#include <furi.h>
#include <storage/storage.h>

// Initialize serial BLE profile
bool fbs_init(void);
// Send entire file pointed by 'path' over BLE serial
bool fbs_send_file(const char* path);
// Deinitialize profile
void fbs_deinit(void);
