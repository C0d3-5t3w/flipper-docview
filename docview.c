#include <furi.h>
#include <furi_hal.h>
#include <gui/gui.h>
#include <gui/view.h>
#include <gui/view_dispatcher.h>
#include <gui/modules/submenu.h>
#include <gui/modules/text_input.h>
#include <gui/modules/widget.h>
#include <gui/modules/variable_item_list.h>
#include <notification/notification.h>
#include <notification/notification_messages.h>
#include <storage/storage.h>
#include <dialogs/dialogs.h>
#include <toolbox/path.h>
#include <docview_icons.h>

#define TAG "docview"

// Change this to BACKLIGHT_AUTO if you don't want the backlight to be continuously on.
#define BACKLIGHT_ON 1

// Document viewer specific definitions
#define TEXT_BUFFER_SIZE      4096
#define LINES_ON_SCREEN       6
#define MAX_LINE_LENGTH       128
#define DOCUMENT_EXTENSION    ".txt"
#define DOCUMENTS_FOLDER_PATH EXT_PATH("documents")

// Our application menu has 3 items
typedef enum {
    DocviewSubmenuIndexOpenFile,
    DocviewSubmenuIndexSettings,
    DocviewSubmenuIndexAbout,
} DocviewSubmenuIndex;

// Each view is a screen we show the user.
typedef enum {
    DocviewViewSubmenu, // The menu when the app starts
    DocviewViewTextInput, // Input for configuring text settings
    DocviewViewFileBrowser, // File browser screen
    DocviewViewConfigure, // The configuration screen
    DocviewViewReader, // The document reader screen
    DocviewViewAbout, // The about screen with directions, link to social channel, etc.
} DocviewView;

typedef enum {
    DocviewEventIdRedrawScreen = 0,
    DocviewEventIdScroll = 1,
} DocviewEventId;

typedef struct {
    ViewDispatcher* view_dispatcher; // Switches between our views
    NotificationApp* notifications; // Used for controlling the backlight
    Submenu* submenu; // The application menu
    TextInput* text_input; // The text input screen
    VariableItemList* variable_item_list_config; // The configuration screen
    View* view_reader; // The document reader screen
    Widget* widget_about; // The about screen
    DialogsApp* dialogs; // File browser

    VariableItem* font_size_item; // Font size setting item
    char* temp_buffer; // Temporary buffer for text input
    uint32_t temp_buffer_size; // Size of temporary buffer

    FuriTimer* timer; // Timer for redrawing the screen
} DocviewApp;

typedef struct {
    uint8_t font_size; // Font size (1-3)
    int16_t scroll_position; // Current scroll position
    uint16_t total_lines; // Total number of lines in document
    bool auto_scroll; // Auto-scroll enabled

    char document_path[256]; // Current document path
    char text_buffer[TEXT_BUFFER_SIZE]; // Buffer for document content
    char* lines[TEXT_BUFFER_SIZE / 20]; // Pointers to start of each line

    bool is_document_loaded; // Flag to indicate if document is loaded
} DocviewReaderModel;

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
 * @brief      Handle submenu item selection.
 * @details    This function is called when user selects an item from the submenu.
 * @param      context  The context - DocviewApp object.
 * @param      index     The DocviewSubmenuIndex item that was clicked.
*/
static void Docview_submenu_callback(void* context, uint32_t index) {
    DocviewApp* app = (DocviewApp*)context;
    switch(index) {
    case DocviewSubmenuIndexOpenFile:
        // Open the file browser to select a document
        {
            FuriString* path = furi_string_alloc();
            furi_string_set(path, DOCUMENTS_FOLDER_PATH);

            DialogsFileBrowserOptions browser_options;
            dialog_file_browser_set_basic_options(&browser_options, DOCUMENT_EXTENSION, &I_doc);

            bool result = dialog_file_browser_show(app->dialogs, path, path, &browser_options);

            if(result) {
                // User selected a file
                with_view_model(
                    app->view_reader,
                    DocviewReaderModel * model,
                    {
                        strncpy(
                            model->document_path,
                            furi_string_get_cstr(path),
                            sizeof(model->document_path) - 1);
                        model->document_path[sizeof(model->document_path) - 1] =
                            '\0'; // Ensure null termination
                        model->scroll_position = 0;
                        model->is_document_loaded = false; // Will be loaded when entering view
                    },
                    true);

                view_dispatcher_switch_to_view(app->view_dispatcher, DocviewViewReader);
            }

            furi_string_free(path);
        }
        break;
    case DocviewSubmenuIndexSettings:
        view_dispatcher_switch_to_view(app->view_dispatcher, DocviewViewConfigure);
        break;
    case DocviewSubmenuIndexAbout:
        view_dispatcher_switch_to_view(app->view_dispatcher, DocviewViewAbout);
        break;
    default:
        break;
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
        storage_file_close(file);
    }

    storage_file_free(file);
    furi_record_close(RECORD_STORAGE);
    return success;
}

// Font size options
static const char* font_size_config_label = "Font Size";
static char* font_size_names[] = {"Very Very Small", "Very Small", "Small", "Medium", "Large"};
static const float font_sizes[] =
    {0.1, 0.5, 1.5, 2.0, 2.5}; // Corresponds to font height multipliers

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
    switch(my_model->font_size) {
    case 0: // Very Very Small
        canvas_set_font(canvas, FontSecondary);
        font_height = 5; // Extremely compact line height
        break;
    case 1: // Very Small
        canvas_set_font(canvas, FontSecondary);
        font_height = 6; // Very compact line height
        break;
    case 2: // Small
        canvas_set_font(canvas, FontSecondary);
        font_height = 8;
        break;
    case 4: // Large
        canvas_set_font(canvas, FontBigNumbers);
        font_height = 16;
        break;
    case 3: // Medium
    default:
        canvas_set_font(canvas, FontPrimary);
        font_height = 12;
        break;
    }

    // Display the document content
    int16_t y_pos = 0;
    uint8_t lines_to_show = 64 / font_height;

    // Draw header with file info
    canvas_set_font(canvas, FontSecondary);
    canvas_set_color(canvas, ColorBlack);

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
        "%d/%d",
        my_model->scroll_position / lines_to_show + 1,
        (my_model->total_lines + lines_to_show - 1) / lines_to_show);
    canvas_draw_str_aligned(canvas, 128, 0, AlignRight, AlignTop, page_info);

    // Draw horizontal line under header
    canvas_draw_line(canvas, 0, 9, 128, 9);

    // Set font for content display
    switch(my_model->font_size) {
    case 0:
        canvas_set_font(canvas, FontSecondary);
        break;
    case 4:
        canvas_set_font(canvas, FontBigNumbers);
        break;
    case 3:
    default:
        canvas_set_font(canvas, FontPrimary);
        break;
    }

    // Start drawing from y=10 (below the header)
    y_pos = 10;

    // Draw document content
    for(int i = 0; i < lines_to_show && (i + my_model->scroll_position) < my_model->total_lines;
        i++) {
        canvas_draw_str(
            canvas, 0, y_pos + font_height, my_model->lines[i + my_model->scroll_position]);
        y_pos += font_height;
    }

    // If there's no content, show a message
    if(my_model->total_lines == 0) {
        canvas_draw_str_aligned(canvas, 64, 32, AlignCenter, AlignCenter, "Empty document");
    }

    // Show navigation hints at the bottom
    canvas_set_font(canvas, FontSecondary);
    canvas_draw_str_aligned(canvas, 64, 64, AlignCenter, AlignBottom, "<3");
}

/**
 * @brief      Callback for timer elapsed.
 * @details    Used for auto-scroll feature
 * @param      context  The context - DocviewApp object.
*/
static void Docview_view_reader_timer_callback(void* context) {
    DocviewApp* app = (DocviewApp*)context;

    with_view_model(
        app->view_reader,
        DocviewReaderModel * model,
        {
            if(model->auto_scroll && model->is_document_loaded) {
                if(model->scroll_position < model->total_lines - 1) {
                    model->scroll_position++;
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
 * @brief      Callback for reader screen input.
 * @details    Handles UP/DOWN for manual scrolling
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
                    if(model->scroll_position > 0) {
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
                        model->scroll_position++;
                    }
                },
                true);
            return true;
        } else if(event->key == InputKeyLeft) {
            // Page up - scroll multiple lines
            with_view_model(
                app->view_reader,
                DocviewReaderModel * model,
                {
                    uint8_t lines_to_show;
                    if(model->font_size == 0)
                        lines_to_show = 12; // Very Very Small
                    else if(model->font_size == 1)
                        lines_to_show = 10; // Very Small
                    else if(model->font_size == 2)
                        lines_to_show = 8; // Small
                    else if(model->font_size == 4)
                        lines_to_show = 3; // Large
                    else
                        lines_to_show = 5; // Medium

                    if(model->scroll_position >= lines_to_show) {
                        model->scroll_position -= lines_to_show;
                    } else {
                        model->scroll_position = 0;
                    }
                },
                true);
            return true;
        } else if(event->key == InputKeyRight) {
            // Page down - scroll multiple lines
            with_view_model(
                app->view_reader,
                DocviewReaderModel * model,
                {
                    uint8_t lines_to_show;
                    if(model->font_size == 0)
                        lines_to_show = 12; // Very Very Small
                    else if(model->font_size == 1)
                        lines_to_show = 10; // Very Small
                    else if(model->font_size == 2)
                        lines_to_show = 8; // Small
                    else if(model->font_size == 4)
                        lines_to_show = 3; // Large
                    else
                        lines_to_show = 5; // Medium

                    model->scroll_position += lines_to_show;
                    if(model->scroll_position >= model->total_lines) {
                        model->scroll_position = model->total_lines - 1;
                    }
                },
                true);
            return true;
        } else if(event->key == InputKeyOk) {
            // Toggle auto-scroll
            with_view_model(
                app->view_reader,
                DocviewReaderModel * model,
                { model->auto_scroll = !model->auto_scroll; },
                true);
            return true;
        }
    }

    return false;
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
    uint8_t font_size_index = 3; // Default to medium (index 3 now)
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
    model->font_size = font_sizes[font_size_index];
    model->scroll_position = 0;
    model->auto_scroll = (auto_scroll_index == 1);
    model->is_document_loaded = false;
    model->total_lines = 0;
    model->document_path[0] = '\0';
    view_dispatcher_add_view(app->view_dispatcher, DocviewViewReader, app->view_reader);

    app->widget_about = widget_alloc();
    widget_add_text_scroll_element(
        app->widget_about,
        0,
        0,
        128,
        64,
        "Document Viewer for Flipper Zero\n-Made by: C0d3-5t3w-\nUse this app to read text files.\n\n"
        "Place TXT files in the\n'documents' folder on your SD card.\n\n"
        "Controls:\n"
        "UP/DOWN: Scroll by line\n"
        "LEFT/RIGHT: Page up/down\n"
        "OK: Toggle auto-scroll\n\n");
    view_set_previous_callback(
        widget_get_view(app->widget_about), Docview_navigation_submenu_callback);
    view_dispatcher_add_view(
        app->view_dispatcher, DocviewViewAbout, widget_get_view(app->widget_about));

    app->notifications = furi_record_open(RECORD_NOTIFICATION);
    app->dialogs = furi_record_open(RECORD_DIALOGS);

#ifdef BACKLIGHT_ON
    notification_message(app->notifications, &sequence_display_backlight_enforce_on);
#endif

    return app;
}

/**
 * @brief      Free the Docview application.
 * @details    This function frees the Docview application resources.
 * @param      app  The Docview application object.
*/
static void Docview_app_free(DocviewApp* app) {
#ifdef BACKLIGHT_ON
    notification_message(app->notifications, &sequence_display_backlight_enforce_auto);
#endif
    furi_record_close(RECORD_NOTIFICATION);
    furi_record_close(RECORD_DIALOGS);

    view_dispatcher_remove_view(app->view_dispatcher, DocviewViewTextInput);
    text_input_free(app->text_input);
    free(app->temp_buffer);
    view_dispatcher_remove_view(app->view_dispatcher, DocviewViewAbout);
    widget_free(app->widget_about);
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

    DocviewApp* app = Docview_app_alloc();
    view_dispatcher_run(app->view_dispatcher);

    Docview_app_free(app);
    return 0;
}
