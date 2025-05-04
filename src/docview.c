#include <furi.h>
#include <furi_hal.h>
#include <furi_hal_resources.h>
#include <gui/gui.h>
#include <gui/view.h>
#include <gui/view_dispatcher.h>
#include <gui/modules/submenu.h>
#include <gui/modules/text_input.h>
#include <gui/modules/widget.h>
#include <gui/modules/variable_item_list.h>
#include <gui/modules/popup.h>
#include <gui/modules/file_browser.h>
#include <notification.h>
#include <notification_messages.h>
#include <storage/storage.h>
#include <dialogs/dialogs.h>
#include <toolbox/path.h>
#include <string.h>

#include "docview.h"
#include "files/file_browser.h"
#include "icons/docview_icons.h"
#include "ble/fbs.h"

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

static const uint8_t font_sizes[] = {2, 3};

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
    furi_assert(app);

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
            bool document_loaded = false;
            with_view_model(
                app->view_reader,
                DocviewReaderModel * model,
                { document_loaded = model->is_document_loaded; },
                false);

            if(document_loaded) {
                with_view_model(
                    app->view_reader,
                    DocviewReaderModel * model,
                    {
                        if(app->ble_state.file_path) {
                            furi_string_free(app->ble_state.file_path);
                        }
                        app->ble_state.file_path = furi_string_alloc();
                        furi_string_set_str(app->ble_state.file_path, model->document_path);

                        FuriString* full_path_str = furi_string_alloc_set(model->document_path);
                        FuriString* filename_str = furi_string_alloc();
                        path_extract_filename(full_path_str, filename_str, false);

                        strlcpy(
                            app->ble_state.file_name,
                            furi_string_get_cstr(filename_str),
                            sizeof(app->ble_state.file_name));

                        furi_string_free(full_path_str);
                        furi_string_free(filename_str);
                    },
                    false);

                notification_message(app->notifications, &sequence_ok);
            } else {
                notification_message(app->notifications, &sequence_error);
            }
            return true;
        }
    }

    return false;
}

void docview_file_browser_void_callback(void* context) {
    DocviewApp* app = context;
    if(!app || !app->file_browser) return;

    FuriString* path = furi_string_alloc();

    docview_get_file_path(app->file_browser, path);

    if(!furi_string_empty(path)) {
        docview_file_browser_callback(furi_string_get_cstr(path), app);
    }

    furi_string_free(path);
}

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
            if(!app->ble_state.file_path) {
                app->ble_state.file_path = furi_string_alloc_set(DOCUMENTS_FOLDER_PATH);
            } else {
                if(furi_string_empty(app->ble_state.file_path)) {
                    furi_string_set(app->ble_state.file_path, DOCUMENTS_FOLDER_PATH);
                }
            }

            app->file_browser = file_browser_alloc(app->ble_state.file_path);
        }

        file_browser_configure(
            app->file_browser, DOCUMENT_EXT_FILTER, NULL, true, true, &I_doc, false);

        file_browser_set_callback(app->file_browser, docview_file_browser_void_callback, app);

        file_browser_start(app->file_browser, app->ble_state.file_path);
        view_dispatcher_switch_to_view(app->view_dispatcher, DocviewViewFileBrowser);
        break;

    case DocviewSubmenuIndexBleAirdrop: {
        DocviewReaderModel* model;
        with_view_model(app->view_reader, DocviewReaderModel * m, { model = m; }, false);
        if(!model->is_document_loaded) {
            notification_message(app->notifications, &sequence_error);
        } else if(!fbs_init() || !fbs_send_file(model->document_path)) {
            notification_message(app->notifications, &sequence_error);
        } else {
            notification_message(app->notifications, &sequence_ok);
        }
        break;
    }

    case DocviewSubmenuIndexSettings:
        FURI_LOG_I(TAG, "Settings selected (Not Implemented)");
        break;

    case DocviewSubmenuIndexAbout:
        FURI_LOG_I(TAG, "About selected (Not Implemented)");
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

bool docview_init_views(DocviewApp* app) {
    furi_assert(app);
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

    if(!app->file_browser) {
        if(!app->ble_state.file_path) {
            app->ble_state.file_path = furi_string_alloc_set(DOCUMENTS_FOLDER_PATH);
        } else if(furi_string_empty(app->ble_state.file_path)) {
            furi_string_set(app->ble_state.file_path, DOCUMENTS_FOLDER_PATH);
        }
        app->file_browser = file_browser_alloc(app->ble_state.file_path);
        view_dispatcher_add_view(
            app->view_dispatcher,
            DocviewViewFileBrowser,
            file_browser_get_view(app->file_browser));
    }

    view_dispatcher_set_event_callback_context(app->view_dispatcher, app);

    view_dispatcher_set_navigation_event_callback(
        app->view_dispatcher, docview_navigation_submenu_callback);

    return true;
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
    furi_assert(app->mutex);

    if(app->file_browser) {
        view_dispatcher_remove_view(app->view_dispatcher, DocviewViewFileBrowser);
    }
    view_dispatcher_remove_view(app->view_dispatcher, DocviewViewReader);
    view_dispatcher_remove_view(app->view_dispatcher, DocviewViewSubmenu);

    view_free(app->view_reader);
    submenu_free(app->submenu);

    view_dispatcher_free(app->view_dispatcher);
    furi_record_close(RECORD_GUI);

    if(app->ble_state.file_path) {
        furi_string_free(app->ble_state.file_path);
    }
    if(app->timer) {
        furi_timer_stop(app->timer);
        furi_timer_free(app->timer);
    }
    if(app->notifications) {
        furi_record_close(RECORD_NOTIFICATION);
    }
    if(app->dialogs) {
        furi_record_close(RECORD_DIALOGS);
    }

    if(app->file_browser) {
        file_browser_free(app->file_browser);
        app->file_browser = NULL;
    }

    if(app->mutex) {
        furi_mutex_release(app->mutex);
        furi_mutex_free(app->mutex);
    }

    free(app);
}

int32_t main_Docview_app(void* p) {
    UNUSED(p);
    int32_t ret = 0;
    DocviewApp* app = NULL;

    app = Docview_app_alloc();
    if(!app) {
        FURI_LOG_E(TAG, "Failed to allocate application structure");
        return 254;
    }

    app->ble_state.status = BleTransferStatusIdle;
    app->ble_state.transfer_active = false;
    app->ble_state.file_path = furi_string_alloc();
    if(!app->ble_state.file_path) {
        FURI_LOG_E(TAG, "Failed to allocate file path string");
        Docview_app_free(app);
        return 253;
    }

    app->notifications = furi_record_open(RECORD_NOTIFICATION);
    app->dialogs = furi_record_open(RECORD_DIALOGS);

    if(!docview_init_views(app)) {
        FURI_LOG_E(TAG, "Failed to initialize GUI views");
        if(app->notifications) furi_record_close(RECORD_NOTIFICATION);
        if(app->dialogs) furi_record_close(RECORD_DIALOGS);
        Docview_app_free(app);
        return 252;
    }

    view_dispatcher_attach_to_gui(app->view_dispatcher, app->gui, ViewDispatcherTypeFullscreen);

    view_dispatcher_switch_to_view(app->view_dispatcher, DocviewViewSubmenu);

    FURI_LOG_I(TAG, "Starting event loop");
    view_dispatcher_run(app->view_dispatcher);
    FURI_LOG_I(TAG, "Event loop finished");

    Docview_app_free(app);

    return ret;
}
