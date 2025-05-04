#include "fbs.h"
#include <ble_profile_serial.h> // correct SDK header under lib/ble_profile
#include <storage/storage.h>
#include <stdio.h>

static BleProfileSerial* svc = NULL;
static bool connected = false;

static void on_connect(void* ctx) {
    (void)ctx;
    connected = true;
}
static void on_disconnect(void* ctx) {
    (void)ctx;
    connected = false;
}

bool fbs_init(void) {
    svc = ble_profile_serial_init();
    if(!svc) return false;
    ble_profile_serial_set_connection_callbacks(svc, on_connect, on_disconnect, NULL);
    return true;
}

bool fbs_send_file(const char* path) {
    if(!svc || !connected) return false;
    Storage* storage = furi_record_open(RECORD_STORAGE);
    File* f = storage_file_alloc(storage);
    if(!storage_file_open(f, path, FSAM_READ, FSOM_OPEN_EXISTING)) {
        storage_file_free(f);
        furi_record_close(RECORD_STORAGE);
        return false;
    }
    uint8_t buf[256];
    size_t rd;
    while((rd = storage_file_read(f, buf, sizeof(buf))) > 0) {
        if(!ble_profile_serial_tx(svc, buf, rd)) break;
    }
    storage_file_close(f);
    storage_file_free(f);
    furi_record_close(RECORD_STORAGE);
    return true;
}

void fbs_deinit(void) {
    if(svc) {
        ble_profile_serial_deinit(svc);
        svc = NULL;
    }
}
