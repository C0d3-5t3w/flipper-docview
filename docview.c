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
#include "bt_hal_compat.h"

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

static uint32_t Docview_navigation_exit_callback(void* _context) {
    UNUSED(_context);
    return VIEW_NONE;
}

static uint32_t Docview_navigation_submenu_callback(void* _context) {
    UNUSED(_context);
    return DocviewViewSubmenu;
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

void docview_ble_transfer_stop(DocviewApp* app) {
    furi_assert(app);

    // Signal thread to stop
    if(app->ble_state.thread) {
        furi_thread_flags_set(furi_thread_get_id(app->ble_state.thread), BLE_THREAD_FLAG_STOP);
        furi_thread_join(app->ble_state.thread);
        furi_thread_free(app->ble_state.thread);
        app->ble_state.thread = NULL;
    }

    if(app->ble_state.timeout_timer) {
        furi_timer_stop(app->ble_state.timeout_timer);
        furi_timer_free(app->ble_state.timeout_timer);
        app->ble_state.timeout_timer = NULL;
    }

    if(app->ble_state.mutex) {
        furi_mutex_free(app->ble_state.mutex);
        app->ble_state.mutex = NULL;
    }

    app->ble_state.transfer_active = false;
    ble_file_service_deinit();
}

void docview_ble_transfer_start(DocviewApp* app) {
    furi_assert(app);

    // Initialize mutex first
    app->ble_state.mutex = furi_mutex_alloc(FuriMutexTypeNormal);
    if(!app->ble_state.mutex) {
        app->ble_state.status = BleTransferStatusFailed;
        return;
    }

    // Initialize BLE state
    app->ble_state.status = BleTransferStatusIdle;
    app->ble_state.chunks_sent = 0;
    app->ble_state.bytes_sent = 0;
    app->ble_state.transfer_active = true;

    // Get file info
    Storage* storage = furi_record_open(RECORD_STORAGE);
    FileInfo file_info;

    if(storage_common_stat(storage, furi_string_get_cstr(app->ble_state.file_path), &file_info) ==
       FSE_OK) {
        app->ble_state.file_size = file_info.size;
        app->ble_state.total_chunks = (file_info.size + BLE_CHUNK_SIZE - 1) / BLE_CHUNK_SIZE;
    } else {
        app->ble_state.status = BleTransferStatusFailed;
        furi_record_close(RECORD_STORAGE);
        return;
    }

    furi_record_close(RECORD_STORAGE);

    // Initialize BLE service
    if(!ble_file_service_init()) {
        app->ble_state.status = BleTransferStatusFailed;
        docview_ble_transfer_update_status(app);
        return;
    }

    app->ble_state.status = BleTransferStatusAdvertising;
    docview_ble_transfer_update_status(app);

    // Start timeout timer
    app->ble_state.timeout_timer =
        furi_timer_alloc(docview_ble_timeout_callback, FuriTimerTypeOnce, app);
    if(!app->ble_state.timeout_timer) {
        docview_ble_transfer_stop(app);
        return;
    }
    furi_timer_start(app->ble_state.timeout_timer, BLE_TRANSFER_TIMEOUT);

    // Start transfer thread
    app->ble_state.thread =
        furi_thread_alloc_ex("BleTransfer", 2048, docview_ble_transfer_process_callback, app);
    if(!app->ble_state.thread) {
        docview_ble_transfer_stop(app);
        return;
    }
    furi_thread_start(app->ble_state.thread);
}

void docview_ble_transfer_update_status(DocviewApp* app) {
    const char* status_text;
    FuriString* temp_str = furi_string_alloc();

    switch(app->ble_state.status) {
    case BleTransferStatusAdvertising:
        status_text = "Waiting for connection\nFile: %s\nSize: %lu bytes";
        furi_string_printf(
            temp_str, status_text, app->ble_state.file_name, app->ble_state.file_size);
        popup_set_header(app->popup_ble, "BLE File Transfer", 64, 11, AlignCenter, AlignCenter);
        popup_set_text(
            app->popup_ble, furi_string_get_cstr(temp_str), 64, 32, AlignCenter, AlignCenter);
        break;

    case BleTransferStatusConnected:
        status_text = "Connected\nPreparing to send %s";
        furi_string_printf(temp_str, status_text, app->ble_state.file_name);
        popup_set_header(app->popup_ble, "BLE File Transfer", 64, 11, AlignCenter, AlignCenter);
        popup_set_text(
            app->popup_ble, furi_string_get_cstr(temp_str), 64, 32, AlignCenter, AlignCenter);
        break;

    case BleTransferStatusTransferring: {
        int progress = (app->ble_state.chunks_sent * 100) / app->ble_state.total_chunks;
        status_text = "Sending: %s\n%d%% (%lu/%lu bytes)";
        furi_string_printf(
            temp_str,
            status_text,
            app->ble_state.file_name,
            progress,
            app->ble_state.bytes_sent,
            app->ble_state.file_size);
        popup_set_header(app->popup_ble, "BLE File Transfer", 64, 11, AlignCenter, AlignCenter);
        popup_set_text(
            app->popup_ble, furi_string_get_cstr(temp_str), 64, 32, AlignCenter, AlignCenter);
        break;
    }

    case BleTransferStatusComplete:
        status_text = "Transfer complete\nFile: %s\nSize: %lu bytes";
        furi_string_printf(
            temp_str, status_text, app->ble_state.file_name, app->ble_state.file_size);
        popup_set_header(app->popup_ble, "BLE File Transfer", 64, 11, AlignCenter, AlignCenter);
        popup_set_text(
            app->popup_ble, furi_string_get_cstr(temp_str), 64, 32, AlignCenter, AlignCenter);
        break;

    case BleTransferStatusFailed:
        status_text = "Transfer failed\nFile: %s";
        furi_string_printf(temp_str, status_text, app->ble_state.file_name);
        popup_set_header(app->popup_ble, "BLE File Transfer", 64, 11, AlignCenter, AlignCenter);
        popup_set_text(
            app->popup_ble, furi_string_get_cstr(temp_str), 64, 32, AlignCenter, AlignCenter);
        break;

    default:
        break;
    }

    furi_string_free(temp_str);

    view_dispatcher_switch_to_view(app->view_dispatcher, DocviewViewBleTransfer);
}

void docview_ble_timeout_callback(void* context) {
    DocviewApp* app = context;

    app->ble_state.status = BleTransferStatusFailed;
    docview_ble_transfer_update_status(app);

    docview_ble_transfer_stop(app);
}

void docview_ble_status_changed_callback(BtStatus status, void* context) {
    DocviewApp* app = context;

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
}

int32_t docview_ble_transfer_process_callback(void* context) {
    DocviewApp* app = context;
    bool success = false;

    Storage* storage = furi_record_open(RECORD_STORAGE);
    File* file = storage_file_alloc(storage);

    if(storage_file_open(
           file, furi_string_get_cstr(app->ble_state.file_path), FSAM_READ, FSOM_OPEN_EXISTING)) {
        if(ble_file_service_start_transfer(app->ble_state.file_name, app->ble_state.file_size)) {
            app->ble_state.status = BleTransferStatusTransferring;
            docview_ble_transfer_update_status(app);

            uint8_t* chunk_buffer = malloc(BLE_CHUNK_SIZE);

            while(app->ble_state.chunks_sent < app->ble_state.total_chunks) {
                size_t bytes_read = storage_file_read(file, chunk_buffer, BLE_CHUNK_SIZE);
                if(bytes_read == 0) break;

                if(ble_file_service_send(chunk_buffer, bytes_read)) {
                    app->ble_state.bytes_sent += bytes_read;
                    app->ble_state.chunks_sent++;

                    if(app->ble_state.chunks_sent % 5 == 0) {
                        docview_ble_transfer_update_status(app);
                    }
                } else {
                    break;
                }

                furi_delay_ms(10);
            }

            free(chunk_buffer);

            ble_file_service_end_transfer();

            if(app->ble_state.chunks_sent == app->ble_state.total_chunks) {
                success = true;
            }
        }
    }

    storage_file_close(file);
    storage_file_free(file);
    furi_record_close(RECORD_STORAGE);

    if(success) {
        app->ble_state.status = BleTransferStatusComplete;
    } else {
        app->ble_state.status = BleTransferStatusFailed;
    }
    docview_ble_transfer_update_status(app);

    docview_ble_transfer_stop(app);

    return 0;
}

static bool docview_ble_transfer_input_callback(InputEvent* event, void* context) {
    DocviewApp* app = (DocviewApp*)context;

    if(event->type == InputTypeShort && event->key == InputKeyBack) {
        docview_ble_transfer_stop(app);
        view_dispatcher_switch_to_view(app->view_dispatcher, DocviewViewReader);
        return true;
    }

    return false;
}

static bool file_select_callback(
    FuriString* path,
    void* context,
    uint8_t** icon_ptr,
    FuriString* filename_str) {
    UNUSED(filename_str);
    UNUSED(icon_ptr);
    DocviewApp* app = context;

    if(furi_string_empty(path)) {
        view_dispatcher_switch_to_view(app->view_dispatcher, DocviewViewSubmenu);
        return false;
    }

    with_view_model(
        app->view_reader,
        DocviewReaderModel * model,
        {
            strlcpy(
                model->document_path, furi_string_get_cstr(path), sizeof(model->document_path));
            model->is_document_loaded = false;
            model->scroll_position = 0;
            model->h_scroll_offset = 0;
        },
        true);

    view_dispatcher_switch_to_view(app->view_dispatcher, DocviewViewReader);
    return true;
}

static bool airdrop_file_select_callback(
    FuriString* path,
    void* context,
    uint8_t** icon_ptr,
    FuriString* filename_str) {
    UNUSED(filename_str);
    UNUSED(icon_ptr);
    DocviewApp* app = context;

    if(furi_string_empty(path)) {
        view_dispatcher_switch_to_view(app->view_dispatcher, DocviewViewSubmenu);
        return false;
    }

    if(app->ble_state.file_path) {
        furi_string_free(app->ble_state.file_path);
    }
    app->ble_state.file_path = furi_string_alloc_set(path);

    FuriString* filename = furi_string_alloc();
    path_extract_filename(path, filename, true);
    strlcpy(
        app->ble_state.file_name,
        furi_string_get_cstr(filename),
        sizeof(app->ble_state.file_name));
    furi_string_free(filename);

    view_dispatcher_switch_to_view(app->view_dispatcher, DocviewViewBleTransfer);
    docview_ble_transfer_start(app);
    return true;
}

static void file_browser_callback(void* context) {
    DocviewApp* app = context;
    view_dispatcher_switch_to_view(app->view_dispatcher, DocviewViewSubmenu);
}

void Docview_submenu_callback(void* context, uint32_t index) {
    DocviewApp* app = context;

    if(index == DocviewSubmenuIndexOpenFile) {
        file_browser_stop(app->file_browser);
        file_browser_set_callback(app->file_browser, file_browser_callback, app);
        file_browser_set_item_callback(app->file_browser, file_select_callback, app);

        FuriString* path = furi_string_alloc_set_str(DOCUMENTS_FOLDER_PATH);
        file_browser_start(app->file_browser, path);
        furi_string_free(path);

        view_dispatcher_switch_to_view(app->view_dispatcher, DocviewViewFileBrowser);
    } else if(index == DocviewSubmenuIndexBleAirdrop) {
        file_browser_stop(app->file_browser);
        file_browser_set_callback(app->file_browser, file_browser_callback, app);
        file_browser_set_item_callback(app->file_browser, airdrop_file_select_callback, app);

        FuriString* path = furi_string_alloc_set_str(DOCUMENTS_FOLDER_PATH);
        file_browser_start(app->file_browser, path);
        furi_string_free(path);

        view_dispatcher_switch_to_view(app->view_dispatcher, DocviewViewFileBrowser);
    } else if(index == DocviewSubmenuIndexSettings) {
        view_dispatcher_switch_to_view(app->view_dispatcher, DocviewViewConfigure);
    } else if(index == DocviewSubmenuIndexAbout) {
        view_dispatcher_switch_to_view(app->view_dispatcher, DocviewViewAbout);
    }
}

static DocviewApp* Docview_app_alloc() {
    DocviewApp* app = (DocviewApp*)malloc(sizeof(DocviewApp));
    Gui* gui = furi_record_open(RECORD_GUI);

    app->view_dispatcher = view_dispatcher_alloc();
    view_dispatcher_attach_to_gui(app->view_dispatcher, gui, ViewDispatcherTypeFullscreen);
    view_dispatcher_set_event_callback_context(app->view_dispatcher, app);

    Storage* storage = furi_record_open(RECORD_STORAGE);
    if(!storage_dir_exists(storage, DOCUMENTS_FOLDER_PATH)) {
        storage_simply_mkdir(storage, DOCUMENTS_FOLDER_PATH);
    }
    furi_record_close(RECORD_STORAGE);

    app->submenu = submenu_alloc();
    submenu_add_item(
        app->submenu, "Open File", DocviewSubmenuIndexOpenFile, Docview_submenu_callback, app);
    submenu_add_item(
        app->submenu, "BLE Airdrop", DocviewSubmenuIndexBleAirdrop, Docview_submenu_callback, app);
    submenu_add_item(
        app->submenu, "Settings", DocviewSubmenuIndexSettings, Docview_submenu_callback, app);
    submenu_add_item(
        app->submenu, "About", DocviewSubmenuIndexAbout, Docview_submenu_callback, app);

    view_set_previous_callback(submenu_get_view(app->submenu), Docview_navigation_exit_callback);
    view_dispatcher_add_view(
        app->view_dispatcher, DocviewViewSubmenu, submenu_get_view(app->submenu));
    view_dispatcher_switch_to_view(app->view_dispatcher, DocviewViewSubmenu);

    app->text_input = text_input_alloc();
    view_dispatcher_add_view(
        app->view_dispatcher, DocviewViewTextInput, text_input_get_view(app->text_input));

    app->temp_buffer_size = 32;
    app->temp_buffer = (char*)malloc(app->temp_buffer_size);

    app->variable_item_list_config = variable_item_list_alloc();
    variable_item_list_reset(app->variable_item_list_config);

    VariableItem* item = variable_item_list_add(
        app->variable_item_list_config,
        font_size_config_label,
        COUNT_OF(font_sizes),
        Docview_font_size_change,
        app);
    uint8_t font_size_index = 1;
    variable_item_set_current_value_index(item, font_size_index);
    variable_item_set_current_value_text(item, font_size_names[font_size_index]);

    item = variable_item_list_add(
        app->variable_item_list_config,
        auto_scroll_config_label,
        2,
        Docview_auto_scroll_change,
        app);
    uint8_t auto_scroll_index = 0;
    variable_item_set_current_value_index(item, auto_scroll_index);
    variable_item_set_current_value_text(item, auto_scroll_names[auto_scroll_index]);

    view_set_previous_callback(
        variable_item_list_get_view(app->variable_item_list_config),
        Docview_navigation_submenu_callback);
    view_dispatcher_add_view(
        app->view_dispatcher,
        DocviewViewConfigure,
        variable_item_list_get_view(app->variable_item_list_config));

    app->view_reader = view_alloc();
    view_set_draw_callback(app->view_reader, Docview_view_reader_draw_callback);
    view_set_input_callback(app->view_reader, Docview_view_reader_input_callback);
    view_set_previous_callback(app->view_reader, Docview_navigation_submenu_callback);
    view_set_enter_callback(app->view_reader, Docview_view_reader_enter_callback);
    view_set_exit_callback(app->view_reader, Docview_view_reader_exit_callback);
    view_set_context(app->view_reader, app);

    view_allocate_model(app->view_reader, ViewModelTypeLockFree, sizeof(DocviewReaderModel));
    DocviewReaderModel* model = view_get_model(app->view_reader);
    model->font_size = font_sizes[font_size_index];
    model->scroll_position = 0;
    model->h_scroll_offset = 0;
    model->auto_scroll = (auto_scroll_index == 1);
    model->is_document_loaded = false;
    model->total_lines = 0;
    model->is_binary = false;
    model->long_line_detected = false;
    model->document_path[0] = '\0';

    view_dispatcher_add_view(app->view_dispatcher, DocviewViewReader, app->view_reader);

    app->ble_state.status = BleTransferStatusIdle;
    app->ble_state.file_path = NULL;
    app->ble_state.timeout_timer = NULL;
    app->ble_state.thread = NULL;

    app->popup_ble = popup_alloc();
    view_set_context(popup_get_view(app->popup_ble), app);
    view_set_input_callback(popup_get_view(app->popup_ble), docview_ble_transfer_input_callback);
    view_dispatcher_add_view(
        app->view_dispatcher, DocviewViewBleTransfer, popup_get_view(app->popup_ble));

    app->notifications = furi_record_open(RECORD_NOTIFICATION);
    app->dialogs = furi_record_open(RECORD_DIALOGS);

    app->bt = NULL;
    bt_service_subscribe_status(app->bt, docview_ble_status_changed_callback, app);

    FuriString* result_path = furi_string_alloc();
    app->file_browser = file_browser_alloc(result_path);
    furi_string_free(result_path);

    view_dispatcher_add_view(
        app->view_dispatcher, DocviewViewFileBrowser, file_browser_get_view(app->file_browser));

    return app;
}

static void Docview_app_free(DocviewApp* app) {
    furi_assert(app);

    // Cleanup BLE resources first
    bt_service_unsubscribe_status(app->bt);
    docview_ble_transfer_stop(app);

    if(app->ble_state.file_path) {
        furi_string_free(app->ble_state.file_path);
    }

    view_dispatcher_remove_view(app->view_dispatcher, DocviewViewBleTransfer);
    popup_free(app->popup_ble);

#ifdef BACKLIGHT_ON
    notification_message(app->notifications, &sequence_display_backlight_enforce_auto);
#endif
    furi_record_close(RECORD_NOTIFICATION);
    furi_record_close(RECORD_DIALOGS);

    view_dispatcher_remove_view(app->view_dispatcher, DocviewViewTextInput);
    text_input_free(app->text_input);

    free(app->temp_buffer);

    view_dispatcher_remove_view(app->view_dispatcher, DocviewViewReader);
    view_free(app->view_reader);

    view_dispatcher_remove_view(app->view_dispatcher, DocviewViewConfigure);
    variable_item_list_free(app->variable_item_list_config);

    view_dispatcher_remove_view(app->view_dispatcher, DocviewViewSubmenu);
    submenu_free(app->submenu);

    view_dispatcher_free(app->view_dispatcher);
    furi_record_close(RECORD_GUI);

    view_dispatcher_remove_view(app->view_dispatcher, DocviewViewFileBrowser);
    file_browser_free(app->file_browser);

    free(app);
}

int32_t main_Docview_app(void* _p) {
    UNUSED(_p);
    bt_init();

    DocviewApp* app = Docview_app_alloc();
    view_dispatcher_run(app->view_dispatcher);
    Docview_app_free(app);
    return 0;
}
