#include <furi.h>
#include <gui/modules/file_browser.h>

// Helper function to check if we have a valid file selection
bool file_browser_has_selection(FileBrowser* browser) {
    furi_assert(browser);
    // In a real implementation, we'd verify if a file is actually selected
    // This is a simplified version
    return (browser != NULL);
}

// Helper function to get file path from browser
void docview_get_file_path(FileBrowser* browser, FuriString* path) {
    furi_assert(browser);
    furi_assert(path);

    if(browser) {
        // Call the official Flipper OS API
        file_browser_get_path(browser, path);
    } else {
        furi_string_reset(path);
    }
}
