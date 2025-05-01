#include <furi.h>
#include <furi_hal.h>
#include <furi_hal_bt.h>
#include <gui/gui.h>
#include <gui/view.h>
#include <gui/view_dispatcher.h>
#include <gui/modules/submenu.h>
#include <gui/modules/text_input.h>
#include <gui/modules/widget.h>
#include <gui/modules/variable_item_list.h>
#include <gui/modules/popup.h>
#include <notification/notification.h>
#include <notification/notification_messages.h>
#include <storage/storage.h>
#include <dialogs/dialogs.h>
#include <toolbox/path.h>
#include <string.h>

#include "docview_app.h"
#include "bt_service.h"
#include "docview_icons.h"

#define TAG "Docview"

#define BACKLIGHT_ON 1

#define TEXT_BUFFER_SIZE 4096
#define LINES_ON_SCREEN  6
#define MAX_LINE_LENGTH  128

#define DOCUMENT_EXT_FILTER   "*"
#define DOCUMENTS_FOLDER_PATH EXT_PATH("documents")
#define BINARY_CHECK_BYTES    512

#define BLE_CHUNK_SIZE       512
#define BLE_TRANSFER_TIMEOUT 30000

static bool docview_navigation_exit_callback(void* context) {
    UNUSED(context);
    return true;
}

static bool docview_navigation_submenu_callback(void* context) {
    UNUSED(context);
    view_dispatcher_switch_to_view(((DocviewApp*)context)->view_dispatcher, DocviewViewSubmenu);
    return true;
}

static void docview_navigation_submenu_void_callback(void* context) {
    docview_navigation_submenu_callback(context);
}

static bool is_binary_content(const char* buffer, size_t size) {
    if(size < 8) return false;

    int binary_count = 0;
    int check_bytes = size < BINARY_CHECK_BYTES ? size : BINARY_CHECK_BYTES;

    for(int i = 0; i < check_bytes; i++) {
        char c = buffer[i];

        if(c == '\r' || c == '\n' || c == '\t' || c == ' ') {
            continue;
        }

        if(c < 32 || c > 126) {
            binary_count++;
        }
    }

    return (binary_count > (check_bytes / 10));
}

static void clean_binary_content(char* buffer, size_t size) {
    for(size_t i = 0; i < size; i++) {
        if((buffer[i] < 32 || buffer[i] > 126) && buffer[i] != '\r' && buffer[i] != '\n' &&
           buffer[i] != '\t') {
            buffer[i] = '.';
        }
    }
}

static bool Docview_load_document(DocviewReaderModel* model) {
    Storage* storage = furi_record_open(RECORD_STORAGE);
    File* file = storage_file_alloc(storage);
    bool success = false;

    if(storage_file_open(file, model->document_path, FSAM_READ, FSOM_OPEN_EXISTING)) {
        uint16_t bytes_read = storage_file_read(file, model->text_buffer, TEXT_BUFFER_SIZE - 1);

        if(bytes_read > 0) {
            model->text_buffer[bytes_read] = '\0';

            model->is_binary = is_binary_content(model->text_buffer, bytes_read);

            if(model->is_binary) {
                clean_binary_content(model->text_buffer, bytes_read);
            }

            model->total_lines = 0;
            char* line_start = model->text_buffer;
            model->lines[model->total_lines++] = line_start;

            for(char* c = model->text_buffer; *c != '\0'; c++) {
                if(*c == '\n') {
                    *c = '\0';
                    if(*(c + 1) != '\0') {
                        model->lines[model->total_lines++] = c + 1;
                        if(model->total_lines >= TEXT_BUFFER_SIZE / 20) {
                            break;
                        }
                    }
                }
            }

            model->is_document_loaded = true;
            success = true;
        }
    }

    storage_file_close(file);
    storage_file_free(file);
    furi_record_close(RECORD_STORAGE);
    return success;
}

static const char* font_size_config_label = "Font Size";
static char* font_size_names[] = {"Tiny", "Large"};
static const uint8_t font_sizes[] = {2, 3};

static void Docview_font_size_change(VariableItem* item) {
    DocviewApp* app = variable_item_get_context(item);
    uint8_t index = variable_item_get_current_value_index(item);
    variable_item_set_current_value_text(item, font_size_names[index]);

    with_view_model(
        app->view_reader,
        DocviewReaderModel * model,
        { model->font_size = font_sizes[index]; },
        true);
}

static const char* auto_scroll_config_label = "Auto-scroll";
static char* auto_scroll_names[] = {"Off", "On"};

static void Docview_auto_scroll_change(VariableItem* item) {
    DocviewApp* app = variable_item_get_context(item);
    uint8_t index = variable_item_get_current_value_index(item);
    variable_item_set_current_value_text(item, auto_scroll_names[index]);

    with_view_model(
        app->view_reader, DocviewReaderModel * model, { model->auto_scroll = (index == 1); }, true);
}

static void Docview_view_reader_draw_callback(Canvas* canvas, void* model) {
    DocviewReaderModel* my_model = (DocviewReaderModel*)model;

    if(!my_model->is_document_loaded) {
        canvas_set_font(canvas, FontPrimary);
        canvas_draw_str_aligned(canvas, 64, 32, AlignCenter, AlignCenter, "Loading document...");
        return;
    }

    uint8_t font_height;
    canvas_set_color(canvas, ColorBlack);

    switch(my_model->font_size) {
    case 2:
        canvas_set_font(canvas, FontSecondary);
        font_height = 8;
        break;
    case 3:
    default:
        canvas_set_font(canvas, FontPrimary);
        font_height = 12;
        break;
    }

    uint8_t lines_to_show = (64 - 10) / font_height;

    canvas_set_font(canvas, FontSecondary);

    const char* filename = strrchr(my_model->document_path, '/');
    if(filename) {
        filename++;
    } else {
        filename = my_model->document_path;
    }

    canvas_draw_str_aligned(canvas, 0, 0, AlignLeft, AlignTop, filename);

    char page_info[32];
    snprintf(
        page_info,
        sizeof(page_info),
        "%d/%d %s",
        my_model->scroll_position / lines_to_show + 1,
        (my_model->total_lines + lines_to_show - 1) / lines_to_show,
        my_model->is_binary ? "[BIN]" : "");

    canvas_draw_str_aligned(canvas, 128, 0, AlignRight, AlignTop, page_info);

    canvas_draw_line(canvas, 0, 9, 128, 9);

    switch(my_model->font_size) {
    case 2:
        canvas_set_font(canvas, FontSecondary);
        break;
    case 3:
    default:
        canvas_set_font(canvas, FontPrimary);
        break;
    }

    int16_t y_pos = 10;

    my_model->long_line_detected = false;

    for(int i = 0; i < lines_to_show && (i + my_model->scroll_position) < my_model->total_lines;
        i++) {
        char* line = my_model->lines[i + my_model->scroll_position];

        int line_width = canvas_string_width(canvas, line);
        if(line_width > 128) {
            my_model->long_line_detected = true;

            char visible_line[MAX_LINE_LENGTH + 1];
            size_t line_len = strlen(line);

            size_t start_pos = 0;
            if(my_model->h_scroll_offset < line_len) {
                start_pos = my_model->h_scroll_offset;
            } else {
                if(my_model->auto_scroll) {
                    my_model->h_scroll_offset = 0;
                } else {
                    my_model->h_scroll_offset = line_len > 0 ? line_len - 1 : 0;
                }
                start_pos = my_model->h_scroll_offset;
            }

            strncpy(visible_line, line + start_pos, MAX_LINE_LENGTH);
            visible_line[MAX_LINE_LENGTH] = '\0';

            canvas_draw_str(canvas, 0, y_pos + font_height, visible_line);
        } else {
            canvas_draw_str(canvas, 0, y_pos + font_height, line);
        }

        y_pos += font_height;
    }

    if(my_model->total_lines == 0) {
        canvas_draw_str_aligned(canvas, 64, 32, AlignCenter, AlignCenter, "Empty document");
    }

    canvas_set_font(canvas, FontSecondary);
    if(my_model->auto_scroll) {
        canvas_draw_str_aligned(canvas, 64, 64, AlignCenter, AlignBottom, "AUTO ⏬");
    } else {
        canvas_draw_str_aligned(canvas, 64, 64, AlignCenter, AlignBottom, "⬆️⬇️");
    }
}

static void Docview_view_reader_timer_callback(void* context) {
    DocviewApp* app = (DocviewApp*)context;

    with_view_model(
        app->view_reader,
        DocviewReaderModel * model,
        {
            if(model->auto_scroll && model->is_document_loaded) {
                if(model->long_line_detected) {
                    if(model->scroll_position < model->total_lines) {
                        char* current_line = model->lines[model->scroll_position];
                        size_t line_len = strlen(current_line);

                        model->h_scroll_offset += 2;

                        if(model->h_scroll_offset > line_len) {
                            model->h_scroll_offset = 0;
                            if(model->scroll_position < model->total_lines - 1) {
                                model->scroll_position++;
                            }
                        }
                    }
                } else {
                    model->h_scroll_offset = 0;
                    if(model->scroll_position < model->total_lines - 1) {
                        model->scroll_position++;
                    }
                }
            }
        },
        true);
}

static void Docview_view_reader_enter_callback(void* context) {
    DocviewApp* app = (DocviewApp*)context;

    with_view_model(
        app->view_reader,
        DocviewReaderModel * model,
        {
            if(!model->is_document_loaded) {
                Docview_load_document(model);
            }
        },
        true);

    uint32_t period = furi_ms_to_ticks(1000);
    furi_assert(app->timer == NULL);
    app->timer =
        furi_timer_alloc(Docview_view_reader_timer_callback, FuriTimerTypePeriodic, context);
    furi_timer_start(app->timer, period);
}

static void Docview_view_reader_exit_callback(void* context) {
    DocviewApp* app = (DocviewApp*)context;

    furi_timer_stop(app->timer);
    furi_timer_free(app->timer);
    app->timer = NULL;
}

static bool Docview_view_reader_input_callback(InputEvent* event, void* context) {
    DocviewApp* app = (DocviewApp*)context;

    if(event->type == InputTypeShort || event->type == InputTypeRepeat) {
        if(event->key == InputKeyUp) {
            with_view_model(
                app->view_reader,
                DocviewReaderModel * model,
                {
                    if(model->scroll_position > 0) {
                        model->h_scroll_offset = 0;
                        model->scroll_position--;
                    }
                },
                true);
            return true;
        } else if(event->key == InputKeyDown) {
            with_view_model(
                app->view_reader,
                DocviewReaderModel * model,
                {
                    if(model->scroll_position < model->total_lines - 1) {
                        model->h_scroll_offset = 0;
                        model->scroll_position++;
                    }
                },
                true);
            return true;
        } else if(event->key == InputKeyLeft) {
            with_view_model(
                app->view_reader,
                DocviewReaderModel * model,
                {
                    if(model->long_line_detected && !model->auto_scroll) {
                        if(model->h_scroll_offset > 0) {
                            model->h_scroll_offset -= 5;
                            if(model->h_scroll_offset >
                               strlen(model->lines[model->scroll_position])) {
                                model->h_scroll_offset = 0;
                            }
                        } else {
                            uint8_t lines_to_show = model->font_size == 2 ? 8 : 5;
                            if(model->scroll_position >= lines_to_show) {
                                model->scroll_position -= lines_to_show;
                            } else {
                                model->scroll_position = 0;
                            }
                        }
                    } else {
                        uint8_t lines_to_show = model->font_size == 2 ? 8 : 5;
                        if(model->scroll_position >= lines_to_show) {
                            model->scroll_position -= lines_to_show;
                        } else {
                            model->scroll_position = 0;
                        }
                        model->h_scroll_offset = 0;
                    }
                },
                true);
            return true;
        } else if(event->key == InputKeyRight) {
            with_view_model(
                app->view_reader,
                DocviewReaderModel * model,
                {
                    if(model->long_line_detected && !model->auto_scroll) {
                        char* current_line = model->lines[model->scroll_position];
                        size_t line_len = strlen(current_line);
                        size_t visible_len = MAX_LINE_LENGTH;

                        if(model->h_scroll_offset + visible_len < line_len) {
                            model->h_scroll_offset += 5;
                        } else {
                            uint8_t lines_to_show = model->font_size == 2 ? 8 : 5;
                            if(model->scroll_position < model->total_lines - 1) {
                                model->h_scroll_offset = 0;
                                model->scroll_position += lines_to_show;
                                if(model->scroll_position >= model->total_lines) {
                                    model->scroll_position = model->total_lines - 1;
                                }
                            }
                        }
                    } else {
                        uint8_t lines_to_show = model->font_size == 2 ? 8 : 5;
                        model->h_scroll_offset = 0;
                        model->scroll_position += lines_to_show;
                        if(model->scroll_position >= model->total_lines) {
                            model->scroll_position = model->total_lines - 1;
                        }
                    }
                },
                true);
            return true;
        } else if(event->key == InputKeyOk) {
            with_view_model(
                app->view_reader,
                DocviewReaderModel * model,
                {
                    model->auto_scroll = !model->auto_scroll;
                    model->h_scroll_offset = 0;
                },
                true);
            return true;
        }
    } else if(event->type == InputTypeLong) {
        if(event->key == InputKeyOk) {
            with_view_model(
                app->view_reader,
                DocviewReaderModel * model,
                {
                    if(model->is_document_loaded) {
                        if(app->ble_state.file_path) {
                            furi_string_free(app->ble_state.file_path);
                        }
                        app->ble_state.file_path = furi_string_alloc();
                        furi_string_set_str(app->ble_state.file_path, model->document_path);

                        const char* filename = strrchr(model->document_path, '/');
                        if(filename) {
                            filename++;
                        } else {
                            filename = model->document_path;
                        }
                        strlcpy(
                            app->ble_state.file_name, filename, sizeof(app->ble_state.file_name));
                    }
                },
                false);

            view_dispatcher_switch_to_view(app->view_dispatcher, DocviewViewBleTransfer);

            docview_ble_transfer_start(app);
            return true;
        }
    }

    return false;
}

int32_t docview_ble_transfer_process_callback(void* context) {
    DocviewApp* app = context;
    furi_assert(app);

    Storage* storage = furi_record_open(RECORD_STORAGE);
    File* file = storage_file_alloc(storage);
    uint8_t* buffer = malloc(BLE_CHUNK_SIZE);
    bool success = false;

    do {
        if(!storage_file_open(
               file,
               furi_string_get_cstr(app->ble_state.file_path),
               FSAM_READ,
               FSOM_OPEN_EXISTING)) {
            break;
        }

        if(!ble_file_service_start_transfer(app->ble_state.file_name, app->ble_state.file_size)) {
            break;
        }

        app->ble_state.status = BleTransferStatusTransferring;
        docview_ble_transfer_update_status(app);

        while(app->ble_state.bytes_sent < app->ble_state.file_size) {
            // Check stop flag
            uint32_t flags = furi_thread_flags_get();
            if(flags & BLE_THREAD_FLAG_STOP) break;

            size_t bytes_read = storage_file_read(file, buffer, BLE_CHUNK_SIZE);
            if(bytes_read == 0) break;

            if(!ble_file_service_send(buffer, bytes_read)) {
                break;
            }

            app->ble_state.bytes_sent += bytes_read;
            app->ble_state.chunks_sent++;
            docview_ble_transfer_update_status(app);
        }

        if(app->ble_state.bytes_sent == app->ble_state.file_size) {
            success = ble_file_service_end_transfer();
        }

    } while(0);

    // Cleanup
    storage_file_close(file);
    storage_file_free(file);
    furi_record_close(RECORD_STORAGE);
    free(buffer);

    // Update final status
    furi_mutex_acquire(app->ble_state.mutex, FuriWaitForever);
    app->ble_state.status = success ? BleTransferStatusComplete : BleTransferStatusFailed;
    docview_ble_transfer_update_status(app);
    furi_mutex_release(app->ble_state.mutex);

    return 0;
}

void docview_ble_transfer_update_status(DocviewApp* app) {
    furi_assert(app);
    furi_assert(app->popup_ble);

    if(!furi_mutex_acquire(app->ble_state.mutex, 100)) {
        return;
    }

    // Update popup based on transfer status
    popup_reset(app->popup_ble);

    switch(app->ble_state.status) {
    case BleTransferStatusIdle:
        popup_set_header(app->popup_ble, "BLE File Transfer", 64, 2, AlignCenter, AlignTop);
        popup_set_text(app->popup_ble, "Ready", 64, 32, AlignCenter, AlignTop);
        popup_set_icon(app->popup_ble, 0, 12, &I_BleWaitConnecting_15x15);
        break;

    case BleTransferStatusAdvertising:
        popup_set_header(app->popup_ble, "BLE File Transfer", 64, 2, AlignCenter, AlignTop);
        popup_set_text(app->popup_ble, "Waiting for connection...", 64, 32, AlignCenter, AlignTop);
        popup_set_icon(app->popup_ble, 0, 12, &I_BleWaitConnecting_15x15);
        break;

    case BleTransferStatusConnected:
        popup_set_header(app->popup_ble, "BLE File Transfer", 64, 2, AlignCenter, AlignTop);
        popup_set_text(
            app->popup_ble, "Connected\nPreparing transfer...", 64, 32, AlignCenter, AlignTop);
        popup_set_icon(app->popup_ble, 0, 12, &I_BleConnected_15x15);
        break;

    case BleTransferStatusTransferring: {
        char progress_str[32];
        float progress = 0.0f;
        if(app->ble_state.file_size > 0) {
            progress = (float)app->ble_state.bytes_sent / (float)app->ble_state.file_size * 100.0f;
        }
        snprintf(
            progress_str,
            sizeof(progress_str),
            "%.1f%% (%lu/%lu KB)",
            (double)progress,
            (unsigned long)app->ble_state.bytes_sent / 1024,
            (unsigned long)app->ble_state.file_size / 1024);

        popup_set_header(app->popup_ble, "BLE File Transfer", 64, 2, AlignCenter, AlignTop);
        popup_set_text(app->popup_ble, progress_str, 64, 32, AlignCenter, AlignTop);
        popup_set_icon(app->popup_ble, 0, 12, &I_Loading_10x10);

        // Show file name
        char file_info[64];
        snprintf(file_info, sizeof(file_info), "File: %s", app->ble_state.file_name);
        popup_set_text(app->popup_ble, file_info, 64, 42, AlignCenter, AlignTop);
        break;
    }

    case BleTransferStatusComplete:
        popup_set_header(app->popup_ble, "Transfer Complete", 64, 2, AlignCenter, AlignTop);
        popup_set_text(app->popup_ble, "File sent successfully", 64, 32, AlignCenter, AlignTop);
        popup_set_icon(app->popup_ble, 0, 12, &I_Ok_15x15);
        popup_set_callback(app->popup_ble, docview_navigation_submenu_void_callback);
        popup_set_context(app->popup_ble, app);
        popup_set_timeout(app->popup_ble, 3000);
        break;

    case BleTransferStatusFailed:
        popup_set_header(app->popup_ble, "Transfer Failed", 64, 2, AlignCenter, AlignTop);
        popup_set_text(app->popup_ble, "Error sending file", 64, 32, AlignCenter, AlignTop);
        popup_set_icon(app->popup_ble, 0, 12, &I_Error_15x15);
        popup_set_callback(app->popup_ble, docview_navigation_submenu_void_callback);
        popup_set_context(app->popup_ble, app);
        popup_set_timeout(app->popup_ble, 3000);
        break;
    }

    furi_mutex_release(app->ble_state.mutex);
    view_dispatcher_send_custom_event(app->view_dispatcher, DocviewEventIdRedrawScreen);
}

void docview_ble_timeout_callback(void* context) {
    DocviewApp* app = context;
    furi_assert(app);

    furi_mutex_acquire(app->ble_state.mutex, FuriWaitForever);
    if(app->ble_state.status != BleTransferStatusComplete) {
        app->ble_state.status = BleTransferStatusFailed;
        docview_ble_transfer_update_status(app);
    }
    furi_mutex_release(app->ble_state.mutex);

    docview_ble_transfer_stop(app);
}

void docview_ble_transfer_start(DocviewApp* app) {
    furi_assert(app);

    if(app->ble_state.transfer_active) {
        return;
    }

    // Initialize mutex if not already done
    if(!app->ble_state.mutex) {
        app->ble_state.mutex = furi_mutex_alloc(FuriMutexTypeNormal);
    }

    furi_mutex_acquire(app->ble_state.mutex, FuriWaitForever);

    // Initialize BLE state
    app->ble_state.status = BleTransferStatusAdvertising;
    app->ble_state.bytes_sent = 0;
    app->ble_state.chunks_sent = 0;

    // Get file size
    Storage* storage = furi_record_open(RECORD_STORAGE);
    FileInfo file_info;
    if(storage_common_stat(storage, furi_string_get_cstr(app->ble_state.file_path), &file_info) ==
       FSE_OK) {
        app->ble_state.file_size = file_info.size;
    } else {
        app->ble_state.file_size = 0;
    }
    furi_record_close(RECORD_STORAGE);

    // Initialize file service
    if(!ble_file_service_init()) {
        app->ble_state.status = BleTransferStatusFailed;
        docview_ble_transfer_update_status(app);
        furi_mutex_release(app->ble_state.mutex);
        return;
    }

    app->ble_state.transfer_active = true;

    // Set up timeout
    app->ble_state.timeout_timer =
        furi_timer_alloc(docview_ble_timeout_callback, FuriTimerTypeOnce, app);
    if(app->ble_state.timeout_timer) {
        furi_timer_start(app->ble_state.timeout_timer, BLE_TRANSFER_TIMEOUT);
    }

    // Update UI
    docview_ble_transfer_update_status(app);
    furi_mutex_release(app->ble_state.mutex);

    // Create and start transfer thread
    if(!app->ble_state.thread) {
        app->ble_state.thread = furi_thread_alloc();
        furi_thread_set_name(app->ble_state.thread, "DocviewBLETransfer");
        furi_thread_set_stack_size(app->ble_state.thread, 2048);
        furi_thread_set_callback(app->ble_state.thread, docview_ble_transfer_process_callback);
        furi_thread_set_context(app->ble_state.thread, app);
        furi_thread_start(app->ble_state.thread);
    }
}

void docview_ble_transfer_stop(DocviewApp* app) {
    furi_assert(app);

    if(!app->ble_state.transfer_active) {
        return;
    }

    // Stop timeout timer
    if(app->ble_state.timeout_timer) {
        furi_timer_stop(app->ble_state.timeout_timer);
        furi_timer_free(app->ble_state.timeout_timer);
        app->ble_state.timeout_timer = NULL;
    }

    // Signal thread to stop and wait for it
    if(app->ble_state.thread) {
        furi_thread_flags_set(furi_thread_get_id(app->ble_state.thread), BLE_THREAD_FLAG_STOP);
        furi_thread_join(app->ble_state.thread);
        furi_thread_free(app->ble_state.thread);
        app->ble_state.thread = NULL;
    }

    // Clean up BLE services
    ble_file_service_deinit();

    furi_mutex_acquire(app->ble_state.mutex, FuriWaitForever);
    app->ble_state.transfer_active = false;
    furi_mutex_release(app->ble_state.mutex);
}

// We need proper callback function for file browser that matches FileBrowserCallback signature
// and adapter function to work with our existing code
static void docview_file_browser_void_callback(void* context) {
    // This function will be called when file selection is completed
    // The selected path is already stored in the file_browser's result_path
    DocviewApp* app = context;
    if(!app || !app->file_browser) return;

    // Get the selected path from file browser
    FuriString* path = furi_string_alloc();
    file_browser_get_result_path(app->file_browser, path);

    if(!furi_string_empty(path)) {
        // Call our existing function with the path
        docview_file_browser_callback(furi_string_get_cstr(path), app);
    }

    furi_string_free(path);
}

// Original callback remains for other usages
bool docview_file_browser_callback(const char* path, void* context) {
    DocviewApp* app = context;
    if(!app || !path) return false;

    with_view_model(
        app->view_reader,
        DocviewReaderModel * model,
        {
            strlcpy(model->document_path, path, sizeof(model->document_path));
            model->is_document_loaded = false;
            model->scroll_position = 0;
            model->h_scroll_offset = 0;
        },
        true);

    view_dispatcher_switch_to_view(app->view_dispatcher, DocviewViewReader);
    return true;
}

static void docview_submenu_callback(void* context, uint32_t index) {
    DocviewApp* app = context;
    furi_assert(app);

    switch(index) {
    case DocviewSubmenuIndexOpenFile:
        if(!app->file_browser) {
            FuriString* path = furi_string_alloc_set(DOCUMENTS_FOLDER_PATH);
            app->file_browser = file_browser_alloc(path);
            furi_string_free(path);

            file_browser_configure(
                app->file_browser, DOCUMENT_EXT_FILTER, NULL, true, false, NULL, NULL);

            // Use the void callback that matches FileBrowserCallback signature
            file_browser_set_callback(app->file_browser, docview_file_browser_void_callback, app);
        }

        // Prepare a path for the result
        FuriString* result_path = furi_string_alloc();
        file_browser_start(app->file_browser, result_path);
        furi_string_free(result_path);
        break;

    case DocviewSubmenuIndexBleAirdrop:
        if(app->ble_state.file_path && !furi_string_empty(app->ble_state.file_path)) {
            view_dispatcher_switch_to_view(app->view_dispatcher, DocviewViewBleTransfer);
            docview_ble_transfer_start(app);
        }
        break;

    case DocviewSubmenuIndexSettings:
        break;

    case DocviewSubmenuIndexAbout:
        break;
    }
}

static View* docview_reader_view_alloc(DocviewApp* app) {
    View* view = view_alloc();
    view_allocate_model(view, ViewModelTypeLocking, sizeof(DocviewReaderModel));

    with_view_model(
        view,
        DocviewReaderModel * model,
        {
            model->font_size = font_sizes[0];
            model->scroll_position = 0;
            model->h_scroll_offset = 0;
            model->auto_scroll = false;
            model->is_document_loaded = false;
        },
        true);

    view_set_context(view, app);
    view_set_draw_callback(view, Docview_view_reader_draw_callback);
    view_set_input_callback(view, Docview_view_reader_input_callback);
    view_set_enter_callback(view, Docview_view_reader_enter_callback);
    view_set_exit_callback(view, Docview_view_reader_exit_callback);

    return view;
}

static bool docview_init_views(DocviewApp* app) {
    app->submenu = submenu_alloc();

    submenu_add_item(
        app->submenu, "Open Document", DocviewSubmenuIndexOpenFile, docview_submenu_callback, app);
    submenu_add_item(
        app->submenu, "BLE Airdrop", DocviewSubmenuIndexBleAirdrop, docview_submenu_callback, app);
    submenu_add_item(
        app->submenu, "Settings", DocviewSubmenuIndexSettings, docview_submenu_callback, app);
    submenu_add_item(
        app->submenu, "About", DocviewSubmenuIndexAbout, docview_submenu_callback, app);

    view_dispatcher_add_view(
        app->view_dispatcher, DocviewViewSubmenu, submenu_get_view(app->submenu));

    app->view_reader = docview_reader_view_alloc(app);
    view_dispatcher_add_view(app->view_dispatcher, DocviewViewReader, app->view_reader);

    app->popup_ble = popup_alloc();
    view_dispatcher_add_view(
        app->view_dispatcher, DocviewViewBleTransfer, popup_get_view(app->popup_ble));

    view_dispatcher_enable_queue(app->view_dispatcher);
    view_dispatcher_set_event_callback_context(app->view_dispatcher, app);

    view_dispatcher_set_navigation_event_callback(
        app->view_dispatcher, docview_navigation_submenu_callback);

    return true;
}

void docview_ble_status_changed_callback(BtStatus status, void* context) {
    DocviewApp* app = context;
    furi_assert(app);

    furi_mutex_acquire(app->ble_state.mutex, FuriWaitForever);

    if(status == BtStatusConnected) {
        app->ble_state.status = BleTransferStatusConnected;
        docview_ble_transfer_update_status(app);
    } else if(status != BtStatusOff && status != BtStatusAdvertising) {
        app->ble_state.status = BleTransferStatusFailed;
        docview_ble_transfer_update_status(app);
    }

    if(status != BtStatusConnected && app->ble_state.status == BleTransferStatusConnected) {
        docview_ble_transfer_stop(app);
    }

    furi_mutex_release(app->ble_state.mutex);
}

DocviewApp* Docview_app_alloc(void) {
    DocviewApp* app = malloc(sizeof(DocviewApp));
    if(!app) return NULL;

    memset(app, 0, sizeof(DocviewApp));

    app->mutex = furi_mutex_alloc(FuriMutexTypeNormal);
    if(!app->mutex) {
        free(app);
        return NULL;
    }

    app->gui = furi_record_open(RECORD_GUI);
    if(!app->gui) {
        furi_mutex_free(app->mutex);
        free(app);
        return NULL;
    }

    app->view_dispatcher = view_dispatcher_alloc();
    if(!app->view_dispatcher) {
        furi_record_close(RECORD_GUI);
        furi_mutex_free(app->mutex);
        free(app);
        return NULL;
    }

    return app;
}

void Docview_app_free(DocviewApp* app) {
    furi_assert(app);

    if(app->ble_state.transfer_active) {
        docview_ble_transfer_stop(app);
    }

    bt_service_unsubscribe_status();
    bt_service_deinit();

    if(app->ble_state.mutex) {
        furi_mutex_free(app->ble_state.mutex);
    }
    if(app->ble_state.file_path) {
        furi_string_free(app->ble_state.file_path);
    }

    if(app->file_browser) {
        file_browser_stop(app->file_browser);
        file_browser_free(app->file_browser);
    }

    view_dispatcher_remove_view(app->view_dispatcher, DocviewViewBleTransfer);
    popup_free(app->popup_ble);

    view_dispatcher_remove_view(app->view_dispatcher, DocviewViewReader);
    view_free(app->view_reader);

    view_dispatcher_remove_view(app->view_dispatcher, DocviewViewSubmenu);
    submenu_free(app->submenu);

    view_dispatcher_free(app->view_dispatcher);
    furi_record_close(RECORD_GUI);

    furi_mutex_free(app->mutex);
    free(app);
}

int32_t main_Docview_app(void* p) {
    UNUSED(p);
    DocviewApp* app = NULL;
    int32_t ret = 255;

    if(!furi_hal_bt_is_alive()) {
        FURI_LOG_E(TAG, "Bluetooth system not alive");
        return ret;
    }

    if(!bt_service_init()) {
        FURI_LOG_E(TAG, "Failed to initialize Bluetooth");
        return ret;
    }

    FuriMutex* ble_mutex = furi_mutex_alloc(FuriMutexTypeNormal);
    if(!ble_mutex) {
        FURI_LOG_E(TAG, "Failed to allocate BLE mutex");
        bt_service_deinit();
        return ret;
    }

    do {
        app = Docview_app_alloc();
        if(!app) {
            FURI_LOG_E(TAG, "Failed to allocate application");
            break;
        }

        app->ble_state.mutex = ble_mutex;

        if(!docview_init_views(app)) {
            FURI_LOG_E(TAG, "Failed to initialize views");
            break;
        }

        app->ble_state.status = BleTransferStatusIdle;
        app->ble_state.transfer_active = false;
        app->ble_state.thread = NULL;
        app->ble_state.timeout_timer = NULL;
        app->ble_state.file_path = furi_string_alloc();
        if(!app->ble_state.file_path) {
            FURI_LOG_E(TAG, "Failed to allocate file path string");
            break;
        }

        bt_service_subscribe_status(docview_ble_status_changed_callback, app);

        if(!view_dispatcher_attach_to_gui(
               app->view_dispatcher, app->gui, ViewDispatcherTypeFullscreen)) {
            FURI_LOG_E(TAG, "Failed to attach view dispatcher");
            break;
        }

        view_dispatcher_switch_to_view(app->view_dispatcher, DocviewViewSubmenu);

        view_dispatcher_run(app->view_dispatcher);
        ret = 0;

    } while(0);

    // Safely cleanup resources
    if(app) {
        Docview_app_free(app);
    } else {
        // Only free these resources if app allocation failed but we allocated them
        furi_mutex_free(ble_mutex);
        bt_service_deinit();
    }

    return ret;
}
