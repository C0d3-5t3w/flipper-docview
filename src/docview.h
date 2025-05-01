#pragma once

#include <furi.h>
#include <furi_hal.h>
#include <gui/gui.h>
#include <gui/view.h>
#include <gui/view_dispatcher.h>
#include <gui/modules/submenu.h>
#include <gui/modules/text_input.h>
#include <gui/modules/widget.h>
#include <gui/modules/variable_item_list.h>
#include <gui/modules/popup.h>
#include <gui/modules/file_browser.h>
#include <notification/notification.h>
#include <notification/notification_messages.h>
#include <storage/storage.h>
#include <dialogs/dialogs.h>

// Define our own BT types to avoid dependency on the header
typedef enum {
    BtStatusAdvertising,
    BtStatusConnected,
    BtStatusDisconnected,
    BtStatusOff,
} BtStatus;

typedef void* Bt;

typedef enum {
    DocviewSubmenuIndexOpenFile,
    DocviewSubmenuIndexBleAirdrop,
    DocviewSubmenuIndexSettings,
    DocviewSubmenuIndexAbout,
} DocviewSubmenuIndex;

typedef enum {
    DocviewViewSubmenu,    
    DocviewViewTextInput,  
    DocviewViewFileBrowser, 
    DocviewViewConfigure,   
    DocviewViewReader,      
    DocviewViewAbout,       
    DocviewViewBleTransfer, 
} DocviewView;

typedef enum {
    DocviewEventIdRedrawScreen = 0,
    DocviewEventIdScroll = 1,
    DocviewEventIdBleStart = 2,
    DocviewEventIdBleComplete = 3,
    DocviewEventIdBleFailed = 4,
} DocviewEventId;

typedef enum {
    BleTransferStatusIdle,
    BleTransferStatusAdvertising,
    BleTransferStatusConnected,
    BleTransferStatusTransferring,
    BleTransferStatusComplete,
    BleTransferStatusFailed,
} BleTransferStatus;

// Thread flags for signaling between threads
#define BLE_THREAD_FLAG_STOP (1 << 0)
#define BLE_THREAD_FLAG_ALL (BLE_THREAD_FLAG_STOP)

typedef struct {
    BleTransferStatus status;
    uint16_t total_chunks;
    uint16_t chunks_sent;
    uint32_t bytes_sent;
    uint32_t file_size;
    char file_name[64];
    FuriString* file_path;
    FuriTimer* timeout_timer;
    FuriThread* thread;
    FuriMutex* mutex;  // Add mutex for BLE state protection 
    bool transfer_active;
} BleTransferState;

typedef struct DocviewApp {
    // Core resources (initialized first, freed last)
    FuriMutex* mutex;  // Main app mutex
    Gui* gui;          // GUI must be initialized before views
    ViewDispatcher* view_dispatcher;

    // BLE state
    BleTransferState ble_state;
    
    // Rest of the struct members
    NotificationApp* notifications;  
    Submenu* submenu;                
    TextInput* text_input;           
    VariableItemList* variable_item_list_config; 
    View* view_reader;               
    Widget* widget_about;            
    DialogsApp* dialogs;             
    Popup* popup_ble;                
    VariableItem* font_size_item;    
    char* temp_buffer;               
    uint32_t temp_buffer_size;       
    FuriTimer* timer;
    FileBrowser* file_browser;
} DocviewApp;

typedef struct {
    uint8_t font_size;             
    int16_t scroll_position;       
    size_t h_scroll_offset;        
    uint16_t total_lines;          
    bool auto_scroll;              
    bool is_binary;                
    char document_path[256];       
    char text_buffer[4096];        
    char* lines[4096 / 20];        
    bool is_document_loaded;       
    bool long_line_detected;       
} DocviewReaderModel;

// Application functions
DocviewApp* Docview_app_alloc(void); 
void Docview_app_free(DocviewApp* app);

// BLE callback functions
void docview_ble_status_changed_callback(BtStatus status, void* context);
void docview_ble_transfer_start(DocviewApp* app);
void docview_ble_transfer_stop(DocviewApp* app);
void docview_ble_transfer_update_status(DocviewApp* app);
void docview_ble_timeout_callback(void* context);

// File browsing helper declarations - only declare our custom functions
bool docview_file_browser_callback(const char* path, void* context);
void docview_file_browser_void_callback(void* context);

// Process callback - non-static declaration
int32_t docview_ble_transfer_process_callback(void* context); 

// Entry point
int32_t main_Docview_app(void* p);
