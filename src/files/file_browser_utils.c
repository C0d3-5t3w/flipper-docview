#include <furi.h>
#include <gui/modules/file_browser.h>
#include "file_browser.h"

/**
 * Helper to check if a file is selected in the browser
 */
bool docview_is_file_selected(FileBrowser* browser) {
    furi_assert(browser);

    // Check if browser exists
    if(!browser) return false;

    // Check if a path is selected by getting the path and checking if it's empty
    FuriString* path = furi_string_alloc();
    docview_get_file_path(browser, path);

    bool has_selection = !furi_string_empty(path);
    furi_string_free(path);

    return has_selection;
}
