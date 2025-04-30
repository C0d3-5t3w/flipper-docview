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

// Add our custom headers
#include "docview_app.h"
#include "bt_service.h"

#define TAG "Docview"

// Change this to BACKLIGHT_AUTO if you don't want the backlight to be continuously on.
#define BACKLIGHT_ON 1

// Document viewer specific definitions
#define TEXT_BUFFER_SIZE      4096
#define LINES_ON_SCREEN       6
#define MAX_LINE_LENGTH       128
// Supporting multiple file types - wildcard for all files
#define DOCUMENT_EXT_FILTER   "*"
#define DOCUMENTS_FOLDER_PATH EXT_PATH("documents")
#define BINARY_CHECK_BYTES    512 // Number of bytes to check for binary content

// BLE file transfer definitions
#define BLE_CHUNK_SIZE       512
#define BLE_TRANSFER_TIMEOUT 30000 // 30 seconds timeout for transfer

/**
 * @brief      Callback for exiting the application.
 * @details    This function is called when user press back button.  We return VIEW_NONE to
 *            indicate that we want to exit the application.
 * @param      _context  The context - unused
 * @return     next view id
*/
static uint32_t Docview_navigation_exit_callback(void* _context) {
    UNUSED(_context);
    return VIEW_NONE;
}

/**
 * @brief      Callback for returning to submenu.
 * @details    This function is called when user press back button.  We return VIEW_NONE to
 *            indicate that we want to navigate to the submenu.
 * @param      _context  The context - unused
 * @return     next view id
*/
static uint32_t Docview_navigation_submenu_callback(void* _context) {
    UNUSED(_context);
    return DocviewViewSubmenu;
}

/**
 * @brief Check if file appears to be binary
 * @param buffer Buffer containing file content
 * @param size Size of the buffer
 * @return true if file appears to be binary, false otherwise
 */
static bool is_binary_content(const char* buffer, size_t size) {
    // Skip checking if file is too small
    if(size < 8) return false;

    // Check first few bytes for non-printable characters (except whitespace)
    int binary_count = 0;
    int check_bytes = size < BINARY_CHECK_BYTES ? size : BINARY_CHECK_BYTES;

    for(int i = 0; i < check_bytes; i++) {
        char c = buffer[i];
        // Skip carriage return, newline, tab and space
        if(c == '\r' || c == '\n' || c == '\t' || c == ' ') {
            continue;
        }
        // If not printable ASCII, count as binary
        if(c < 32 || c > 126) {
            binary_count++;
        }
    }

    // If more than 10% of checked content is non-printable, consider it binary
    return (binary_count > (check_bytes / 10));
}

/**
 * @brief Clean binary data for text display
 * @param buffer Buffer containing file content
 * @param size Size of the buffer
 */
static void clean_binary_content(char* buffer, size_t size) {
    for(size_t i = 0; i < size; i++) {
        // Replace non-printable characters with a placeholder
        if((buffer[i] < 32 || buffer[i] > 126) && buffer[i] != '\r' && buffer[i] != '\n' &&
           buffer[i] != '\t') {
            buffer[i] = '.';
        }
    }
}

/**
 * Process document text and split it into lines for rendering
 */
static bool Docview_load_document(DocviewReaderModel* model) {
    Storage* storage = furi_record_open(RECORD_STORAGE);
    File* file = storage_file_alloc(storage);
    bool success = false;

    if(storage_file_open(file, model->document_path, FSAM_READ, FSOM_OPEN_EXISTING)) {
        // Read file content
        uint16_t bytes_read = storage_file_read(file, model->text_buffer, TEXT_BUFFER_SIZE - 1);

        if(bytes_read > 0) {
            model->text_buffer[bytes_read] = '\0'; // Ensure null-termination

            // Check if this appears to be a binary file
            model->is_binary = is_binary_content(model->text_buffer, bytes_read);

            // If binary, clean the content for display
            if(model->is_binary) {
                clean_binary_content(model->text_buffer, bytes_read);
            }

            // Split text into lines
            model->total_lines = 0;
            char* line_start = model->text_buffer;
            model->lines[model->total_lines++] = line_start;

            for(char* c = model->text_buffer; *c != '\0'; c++) {
                if(*c == '\n') {
                    *c = '\0'; // Terminate the current line
                    if(*(c + 1) != '\0') {
                        model->lines[model->total_lines++] = c + 1; // Start new line
                        if(model->total_lines >= TEXT_BUFFER_SIZE / 20) {
                            break; // Too many lines, stop
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

// Font size options
static const char* font_size_config_label = "Font Size";
static char* font_size_names[] = {"Tiny", "Large"};
static const uint8_t font_sizes[] = {
    2,
    3 // Using indices for Small/Medium from original code
};

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

// Auto-scroll option
static const char* auto_scroll_config_label = "Auto-scroll";
static char* auto_scroll_names[] = {"Off", "On"};

static void Docview_auto_scroll_change(VariableItem* item) {
    DocviewApp* app = variable_item_get_context(item);
    uint8_t index = variable_item_get_current_value_index(item);
    variable_item_set_current_value_text(item, auto_scroll_names[index]);

    with_view_model(
        app->view_reader, DocviewReaderModel * model, { model->auto_scroll = (index == 1); }, true);
}

/**
 * @brief      Callback for drawing the document reader screen.
 * @details    This function is called when the screen needs to be redrawn
 * @param      canvas  The canvas to draw on.
 * @param      model   The model - DocviewReaderModel object.
*/
static void Docview_view_reader_draw_callback(Canvas* canvas, void* model) {
    DocviewReaderModel* my_model = (DocviewReaderModel*)model;

    if(!my_model->is_document_loaded) {
        canvas_set_font(canvas, FontPrimary);
        canvas_draw_str_aligned(canvas, 64, 32, AlignCenter, AlignCenter, "Loading document...");
        return;
    }

    uint8_t font_height;
    canvas_set_color(canvas, ColorBlack);

    // Select font and set appropriate line height based on font size
    switch(my_model->font_size) {
    case 2: // Tiny (formerly "Small")
        canvas_set_font(canvas, FontSecondary);
        font_height = 8;
        break;
    case 3: // Large (formerly "Medium")
    default:
        canvas_set_font(canvas, FontPrimary);
        font_height = 12;
        break;
    }

    // Calculate how many lines can fit on screen
    uint8_t lines_to_show = (64 - 10) / font_height; // 10px for header

    // Draw header with file info
    canvas_set_font(canvas, FontSecondary);

    // Get just the filename from the path
    const char* filename = strrchr(my_model->document_path, '/');
    if(filename) {
        filename++; // Skip the '/'
    } else {
        filename = my_model->document_path;
    }

    // Draw filename in the top status bar
    canvas_draw_str_aligned(canvas, 0, 0, AlignLeft, AlignTop, filename);

    // Draw page info
    char page_info[32];
    snprintf(
        page_info,
        sizeof(page_info),
        "%d/%d %s",
        my_model->scroll_position / lines_to_show + 1,
        (my_model->total_lines + lines_to_show - 1) / lines_to_show,
        my_model->is_binary ? "[BIN]" : ""); // Show [BIN] indicator for binary files

    canvas_draw_str_aligned(canvas, 128, 0, AlignRight, AlignTop, page_info);

    // Draw horizontal line under header
    canvas_draw_line(canvas, 0, 9, 128, 9);

    // Set font for content display based on selected font size
    switch(my_model->font_size) {
    case 2: // Tiny
        canvas_set_font(canvas, FontSecondary);
        break;
    case 3: // Large
    default:
        canvas_set_font(canvas, FontPrimary);
        break;
    }

    // Start drawing from y=10 (below the header)
    int16_t y_pos = 10;

    // Flag to track if we have a long line that needs horizontal scrolling
    my_model->long_line_detected = false;

    // Draw document content
    for(int i = 0; i < lines_to_show && (i + my_model->scroll_position) < my_model->total_lines;
        i++) {
        char* line = my_model->lines[i + my_model->scroll_position];

        // Check if current line is too long to fit on screen
        int line_width = canvas_string_width(canvas, line);
        if(line_width > 128) {
            // If this line is longer than screen width, apply horizontal scrolling
            // Now we'll apply horizontal scrolling to all lines, not just the first one
            my_model->long_line_detected = true;

            // Create a temporary buffer for the scrolled line view
            char visible_line[MAX_LINE_LENGTH + 1];
            size_t line_len = strlen(line);

            // Determine where to start copying based on horizontal offset
            // For manual scroll, we'll use the h_scroll_offset directly
            size_t start_pos = 0;
            if(my_model->h_scroll_offset < line_len) {
                start_pos = my_model->h_scroll_offset;
            } else {
                // Reset horizontal offset if we're at the end of the line
                if(my_model->auto_scroll) {
                    my_model->h_scroll_offset = 0;
                } else {
                    // For manual mode, keep at the end
                    my_model->h_scroll_offset = line_len > 0 ? line_len - 1 : 0;
                }
                start_pos = my_model->h_scroll_offset;
            }

            // Copy visible portion of the line
            strncpy(visible_line, line + start_pos, MAX_LINE_LENGTH);
            visible_line[MAX_LINE_LENGTH] = '\0';

            // Draw the visible portion
            canvas_draw_str(canvas, 0, y_pos + font_height, visible_line);
        } else {
            // Line fits on screen, draw normally
            canvas_draw_str(canvas, 0, y_pos + font_height, line);
        }

        y_pos += font_height;
    }

    // If there's no content, show a message
    if(my_model->total_lines == 0) {
        canvas_draw_str_aligned(canvas, 64, 32, AlignCenter, AlignCenter, "Empty document");
    }

    // Show auto-scroll indicator if enabled
    canvas_set_font(canvas, FontSecondary);
    if(my_model->auto_scroll) {
        canvas_draw_str_aligned(canvas, 64, 64, AlignCenter, AlignBottom, "AUTO ⏬");
    } else {
        canvas_draw_str_aligned(canvas, 64, 64, AlignCenter, AlignBottom, "⬆️⬇️");
    }
}

/**
 * @brief      Callback for timer elapsed.
 * @details    Used for auto-scroll feature for both vertical and horizontal scrolling
 * @param      context  The context - DocviewApp object.
*/
static void Docview_view_reader_timer_callback(void* context) {
    DocviewApp* app = (DocviewApp*)context;

    with_view_model(
        app->view_reader,
        DocviewReaderModel * model,
        {
            if(model->auto_scroll && model->is_document_loaded) {
                // Handle horizontal scrolling for long lines first
                if(model->long_line_detected) {
                    // Get the current line
                    if(model->scroll_position < model->total_lines) {
                        char* current_line = model->lines[model->scroll_position];
                        size_t line_len = strlen(current_line);

                        // Advance horizontal scroll position
                        model->h_scroll_offset += 2; // Scroll by 2 characters at a time

                        // If we've scrolled to the end of the line
                        if(model->h_scroll_offset > line_len) {
                            // Reset horizontal scroll and move to next line
                            model->h_scroll_offset = 0;
                            if(model->scroll_position < model->total_lines - 1) {
                                model->scroll_position++;
                            }
                        }
                    }
                } else {
                    // No long line, just do vertical scrolling
                    model->h_scroll_offset = 0; // Reset horizontal offset
                    if(model->scroll_position < model->total_lines - 1) {
                        model->scroll_position++;
                    }
                }
            }
        },
        true);
}

/**
 * @brief      Callback when the user enters the reader screen.
 * @details    Loads the document and starts timer for auto-scroll if enabled
 * @param      context  The context - DocviewApp object.
*/
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

    uint32_t period = furi_ms_to_ticks(1000); // Auto-scroll every second
    furi_assert(app->timer == NULL);
    app->timer =
        furi_timer_alloc(Docview_view_reader_timer_callback, FuriTimerTypePeriodic, context);
    furi_timer_start(app->timer, period);
}

/**
 * @brief      Callback when the user exits the reader screen.
 * @details    Stops the auto-scroll timer
 * @param      context  The context - DocviewApp object.
*/
static void Docview_view_reader_exit_callback(void* context) {
    DocviewApp* app = (DocviewApp*)context;

    furi_timer_stop(app->timer);
    furi_timer_free(app->timer);
    app->timer = NULL;
}

/**
 * @brief      Callback for reader screen input with BLE option.
 * @details    Handles UP/DOWN for manual scrolling and adds BLE transfer option
 * @param      event    The event - InputEvent object.
 * @param      context  The context - DocviewApp object.
 * @return     true if the event was handled, false otherwise.
*/
static bool Docview_view_reader_input_callback(InputEvent* event, void* context) {
    DocviewApp* app = (DocviewApp*)context;

    if(event->type == InputTypeShort || event->type == InputTypeRepeat) {
        if(event->key == InputKeyUp) {
            with_view_model(
                app->view_reader,
                DocviewReaderModel * model,
                {
                    // Only reset horizontal scroll when moving between lines
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
                    // Only reset horizontal scroll when moving between lines
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
                        // When manual scrolling is active and we have a long line,
                        // scroll horizontally first
                        if(model->h_scroll_offset > 0) {
                            model->h_scroll_offset -= 5; // Scroll left by 5 chars at a time
                            if(model->h_scroll_offset >
                               strlen(model->lines[model->scroll_position])) {
                                model->h_scroll_offset = 0;
                            }
                        } else {
                            // If already at left edge, page up
                            uint8_t lines_to_show = model->font_size == 2 ? 8 : 5;
                            if(model->scroll_position >= lines_to_show) {
                                model->scroll_position -= lines_to_show;
                            } else {
                                model->scroll_position = 0;
                            }
                        }
                    } else {
                        // Standard page up behavior
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
                        // When manual scrolling is active and we have a long line,
                        // scroll horizontally first
                        char* current_line = model->lines[model->scroll_position];
                        size_t line_len = strlen(current_line);
                        size_t visible_len = MAX_LINE_LENGTH;

                        // If we haven't reached the end of the line
                        if(model->h_scroll_offset + visible_len < line_len) {
                            model->h_scroll_offset += 5; // Scroll right by 5 chars at a time
                        } else {
                            // If at the end of the line, page down
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
                        // Standard page down behavior
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
            // Toggle auto-scroll
            with_view_model(
                app->view_reader,
                DocviewReaderModel * model,
                {
                    model->auto_scroll = !model->auto_scroll;
                    model->h_scroll_offset = 0; // Reset horizontal position when toggling
                },
                true);
            return true;
        }
    } else if(event->type == InputTypeLong) {
        if(event->key == InputKeyOk) {
            // Long press OK to bring up BLE transfer option
            with_view_model(
                app->view_reader,
                DocviewReaderModel * model,
                {
                    // Only allow BLE transfer if a document is loaded
                    if(model->is_document_loaded) {
                        // Copy the current document path to BLE state
                        if(app->ble_state.file_path) {
                            furi_string_free(app->ble_state.file_path);
                        }
                        app->ble_state.file_path = furi_string_alloc();
                        furi_string_set_str(app->ble_state.file_path, model->document_path);

                        // Extract filename for display
                        const char* filename = strrchr(model->document_path, '/');
                        if(filename) {
                            filename++; // Skip the '/'
                        } else {
                            filename = model->document_path;
                        }
                        strlcpy(
                            app->ble_state.file_name, filename, sizeof(app->ble_state.file_name));
                    }
                },
                false); // Don't force redraw because we're switching views

            // Switch to BLE transfer view
            view_dispatcher_switch_to_view(app->view_dispatcher, DocviewViewBleTransfer);

            // Start the BLE transfer process
            docview_ble_transfer_start(app);
            return true;
        }
    }

    return false;
}

/**
 * @brief Start the BLE file transfer process
 * @param app DocviewApp context
 */
void docview_ble_transfer_start(DocviewApp* app) {
    // Reset transfer state
    app->ble_state.status = BleTransferStatusAdvertising;
    app->ble_state.chunks_sent = 0;
    app->ble_state.bytes_sent = 0;

    // Get file size
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

    // Initialize BLE file service
    if(!ble_file_service_init()) {
        app->ble_state.status = BleTransferStatusFailed;
        docview_ble_transfer_update_status(app);
        return;
    }

    // Update UI with initial status
    docview_ble_transfer_update_status(app);

    // Set up timeout timer
    app->ble_state.timeout_timer =
        furi_timer_alloc(docview_ble_timeout_callback, FuriTimerTypeOnce, app);
    furi_timer_start(app->ble_state.timeout_timer, BLE_TRANSFER_TIMEOUT);

    // Start the file transfer process directly by creating a thread
    FuriThread* thread =
        furi_thread_alloc_ex("BleTransfer", 1024, docview_ble_transfer_process_callback, app);
    furi_thread_start(thread);
}

/**
 * @brief Stop the BLE file transfer process
 * @param app DocviewApp context
 */
void docview_ble_transfer_stop(DocviewApp* app) {
    // Stop BLE service
    ble_file_service_deinit();

    // Clean up timeout timer
    if(app->ble_state.timeout_timer) {
        furi_timer_stop(app->ble_state.timeout_timer);
        furi_timer_free(app->ble_state.timeout_timer);
        app->ble_state.timeout_timer = NULL;
    }
}

/**
 * @brief Update the BLE transfer status UI
 * @param app DocviewApp context
 */
void docview_ble_transfer_update_status(DocviewApp* app) {
    const char* status_text;
    FuriString* temp_str = furi_string_alloc();

    // Update popup based on transfer status
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
        // Calculate progress percentage
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

    // Force redraw
    view_dispatcher_switch_to_view(app->view_dispatcher, DocviewViewBleTransfer);
}

/**
 * @brief Callback for BLE transfer timeout
 * @param context DocviewApp context
 */
void docview_ble_timeout_callback(void* context) {
    DocviewApp* app = context;

    // Handle timeout by setting failed status
    app->ble_state.status = BleTransferStatusFailed;
    docview_ble_transfer_update_status(app);

    // Clean up
    docview_ble_transfer_stop(app);
}

/**
 * @brief Callback for BLE status changes
 * @param status Current BT state
 * @param context DocviewApp context
 */
void docview_ble_status_changed_callback(BtStatus status, void* context) {
    DocviewApp* app = context;

    // Handle different BT status updates
    if(status == BtStatusConnected) {
        app->ble_state.status = BleTransferStatusConnected;
        docview_ble_transfer_update_status(app);
    } else if(status != BtStatusOff && status != BtStatusAdvertising) {
        app->ble_state.status = BleTransferStatusFailed;
        docview_ble_transfer_update_status(app);
    }

    // Clean up if we're disconnected
    if(status != BtStatusConnected && app->ble_state.status == BleTransferStatusConnected) {
        // If we were connected but now we're not, handle disconnection
        docview_ble_transfer_stop(app);
    }
}

/**
 * @brief Process the actual file transfer
 * @param context DocviewApp context
 * @return 0 if successful
 */
int32_t docview_ble_transfer_process_callback(void* context) {
    DocviewApp* app = context;
    bool success = false;

    // Open file for transfer
    Storage* storage = furi_record_open(RECORD_STORAGE);
    File* file = storage_file_alloc(storage);

    if(storage_file_open(
           file, furi_string_get_cstr(app->ble_state.file_path), FSAM_READ, FSOM_OPEN_EXISTING)) {
        // Start the file transfer session
        if(ble_file_service_start_transfer(app->ble_state.file_name, app->ble_state.file_size)) {
            // Update status to transferring
            app->ble_state.status = BleTransferStatusTransferring;
            docview_ble_transfer_update_status(app);

            // Allocate chunk buffer
            uint8_t* chunk_buffer = malloc(BLE_CHUNK_SIZE);

            // Send the file data in chunks
            while(app->ble_state.chunks_sent < app->ble_state.total_chunks) {
                // Read a chunk from file
                size_t bytes_read = storage_file_read(file, chunk_buffer, BLE_CHUNK_SIZE);
                if(bytes_read == 0) break;

                // Send the chunk via BLE
                if(ble_file_service_send(chunk_buffer, bytes_read)) {
                    // Update progress
                    app->ble_state.bytes_sent += bytes_read;
                    app->ble_state.chunks_sent++;

                    // Update UI every few chunks
                    if(app->ble_state.chunks_sent % 5 == 0) {
                        docview_ble_transfer_update_status(app);
                    }
                } else {
                    // Failed to send chunk
                    break;
                }

                // Short delay to allow receiver to process data
                furi_delay_ms(10);
            }

            free(chunk_buffer);

            // End the transfer session
            ble_file_service_end_transfer();

            // Check if all chunks were sent
            if(app->ble_state.chunks_sent == app->ble_state.total_chunks) {
                success = true;
            }
        }
    }

    storage_file_close(file);
    storage_file_free(file);
    furi_record_close(RECORD_STORAGE);

    // Update final status
    if(success) {
        app->ble_state.status = BleTransferStatusComplete;
    } else {
        app->ble_state.status = BleTransferStatusFailed;
    }
    docview_ble_transfer_update_status(app);

    // Clean up after transfer
    docview_ble_transfer_stop(app);

    return 0;
}

/**
 * @brief Input callback for the BLE transfer popup
 * @param event Input event
 * @param context DocviewApp context
 * @return true if event handled
 */
static bool docview_ble_transfer_input_callback(InputEvent* event, void* context) {
    DocviewApp* app = context;

    if(event->type == InputTypeShort && event->key == InputKeyBack) {
        // Cancel transfer and return to reader view
        docview_ble_transfer_stop(app);
        view_dispatcher_switch_to_view(app->view_dispatcher, DocviewViewReader);
        return true;
    }

    return false;
}

/* Docview submenu callback - implemented below but defined in docview_app.h */
void Docview_submenu_callback(void* context, uint32_t index) {
    DocviewApp* app = context;

    if(index == DocviewSubmenuIndexOpenFile) {
        // File browser logic would go here
    } else if(index == DocviewSubmenuIndexSettings) {
        view_dispatcher_switch_to_view(app->view_dispatcher, DocviewViewConfigure);
    } else if(index == DocviewSubmenuIndexAbout) {
        view_dispatcher_switch_to_view(app->view_dispatcher, DocviewViewAbout);
    }
}

/**
 * @brief      Allocate the Docview application.
 * @details    This function allocates the Docview application resources.
 * @return     DocviewApp object.
 */
static DocviewApp* Docview_app_alloc() {
    DocviewApp* app = (DocviewApp*)malloc(sizeof(DocviewApp));
    Gui* gui = furi_record_open(RECORD_GUI);

    app->view_dispatcher = view_dispatcher_alloc();
    view_dispatcher_attach_to_gui(app->view_dispatcher, gui, ViewDispatcherTypeFullscreen);
    view_dispatcher_set_event_callback_context(app->view_dispatcher, app);

    // Create the documents directory if it doesn't exist
    Storage* storage = furi_record_open(RECORD_STORAGE);
    if(!storage_dir_exists(storage, DOCUMENTS_FOLDER_PATH)) {
        storage_simply_mkdir(storage, DOCUMENTS_FOLDER_PATH);
    }
    furi_record_close(RECORD_STORAGE);

    app->submenu = submenu_alloc();
    submenu_add_item(
        app->submenu, "Open File", DocviewSubmenuIndexOpenFile, Docview_submenu_callback, app);
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

    // Settings menu
    app->variable_item_list_config = variable_item_list_alloc();
    variable_item_list_reset(app->variable_item_list_config);

    // Font size setting
    VariableItem* item = variable_item_list_add(
        app->variable_item_list_config,
        font_size_config_label,
        COUNT_OF(font_sizes),
        Docview_font_size_change,
        app);
    uint8_t font_size_index = 1; // Default to Large (index 1)
    variable_item_set_current_value_index(item, font_size_index);
    variable_item_set_current_value_text(item, font_size_names[font_size_index]);

    // Auto-scroll setting
    item = variable_item_list_add(
        app->variable_item_list_config,
        auto_scroll_config_label,
        2,
        Docview_auto_scroll_change,
        app);
    uint8_t auto_scroll_index = 0; // Default to off
    variable_item_set_current_value_index(item, auto_scroll_index);
    variable_item_set_current_value_text(item, auto_scroll_names[auto_scroll_index]);

    view_set_previous_callback(
        variable_item_list_get_view(app->variable_item_list_config),
        Docview_navigation_submenu_callback);
    view_dispatcher_add_view(
        app->view_dispatcher,
        DocviewViewConfigure,
        variable_item_list_get_view(app->variable_item_list_config));

    // Document reader view
    app->view_reader = view_alloc();
    view_set_draw_callback(app->view_reader, Docview_view_reader_draw_callback);
    view_set_input_callback(app->view_reader, Docview_view_reader_input_callback);
    view_set_previous_callback(app->view_reader, Docview_navigation_submenu_callback);
    view_set_enter_callback(app->view_reader, Docview_view_reader_enter_callback);
    view_set_exit_callback(app->view_reader, Docview_view_reader_exit_callback);
    view_set_context(app->view_reader, app);

    view_allocate_model(app->view_reader, ViewModelTypeLockFree, sizeof(DocviewReaderModel));
    DocviewReaderModel* model = view_get_model(app->view_reader);
    model->font_size = font_sizes[font_size_index]; // Default to Large
    model->scroll_position = 0;
    model->h_scroll_offset = 0; // Initialize horizontal scroll offset
    model->auto_scroll = (auto_scroll_index == 1);
    model->is_document_loaded = false;
    model->total_lines = 0;
    model->is_binary = false; // Initialize binary flag
    model->long_line_detected = false; // Initialize long line flag
    model->document_path[0] = '\0';

    view_dispatcher_add_view(app->view_dispatcher, DocviewViewReader, app->view_reader);

    // Initialize BLE transfer state
    app->ble_state.status = BleTransferStatusIdle;
    app->ble_state.file_path = NULL;
    app->ble_state.timeout_timer = NULL;

    // Create BLE transfer popup
    app->popup_ble = popup_alloc();
    view_set_context(popup_get_view(app->popup_ble), app);
    view_set_input_callback(popup_get_view(app->popup_ble), docview_ble_transfer_input_callback);
    view_dispatcher_add_view(
        app->view_dispatcher, DocviewViewBleTransfer, popup_get_view(app->popup_ble));

    app->notifications = furi_record_open(RECORD_NOTIFICATION);
    app->dialogs = furi_record_open(RECORD_DIALOGS);

    // Open BT service record
    app->bt =
        NULL; // Just use NULL since we're handling everything in our own bt_service implementation
    bt_service_subscribe_status(app->bt, docview_ble_status_changed_callback, app);

    return app;
}

static void Docview_app_free(DocviewApp* app) {
    // Unsubscribe and close BT service
    bt_service_unsubscribe_status(app->bt);
    // No need to close RECORD_BT since we're not using it

    // Clean up BLE transfer if active
    docview_ble_transfer_stop(app);

    // Free BLE state resources
    if(app->ble_state.file_path) {
        furi_string_free(app->ble_state.file_path);
    }

    // Remove BLE transfer view
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

    // About view would be removed here if implemented

    view_dispatcher_remove_view(app->view_dispatcher, DocviewViewReader);
    view_free(app->view_reader);

    view_dispatcher_remove_view(app->view_dispatcher, DocviewViewConfigure);
    variable_item_list_free(app->variable_item_list_config);

    view_dispatcher_remove_view(app->view_dispatcher, DocviewViewSubmenu);
    submenu_free(app->submenu);

    view_dispatcher_free(app->view_dispatcher);
    furi_record_close(RECORD_GUI);

    free(app);
}

/**
 * @brief      Main function for Docview application.
 * @details    This function is the entry point for the Docview application.
 * @param      _p  Input parameter - unused
 * @return     0 - Success
 */
int32_t main_Docview_app(void* _p) {
    UNUSED(_p);
    furi_hal_bt_init(); // Initialize the hardware layer (keep this)

    DocviewApp* app = Docview_app_alloc();
    view_dispatcher_run(app->view_dispatcher);
    Docview_app_free(app);
    return 0;
}
