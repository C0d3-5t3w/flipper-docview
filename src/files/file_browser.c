#include <furi.h>
#include <gui/modules/file_browser.h>
#include "file_browser.h"

void docview_get_file_path(FileBrowser* browser, FuriString* result) {
    furi_assert(browser);
    furi_assert(result);

    // Reset the result string first
    furi_string_reset(result);

    // Use the proper SDK function to get the selected path
    file_browser_get_view(browser);

    // No need to handle result conversion since file_browser_get_path
    // directly writes to the provided FuriString
}

bool docview_file_browser_init(FileBrowser* browser) {
    furi_assert(browser);
    // Nothing specific to initialize
    return true;
}
