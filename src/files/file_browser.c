#include "file_browser.h"
#include <furi.h>
#include <gui/modules/file_browser.h>
#include <storage/storage.h>

/**
 * @brief Get the path from FileBrowser and store it in a FuriString
 * 
 * This wrapper uses the SDK's file_browser API and ensures compatibility
 *
 * @param browser Pointer to FileBrowser instance
 * @param result FuriString to store path
 */
void docview_get_file_path(FileBrowser* browser, FuriString* result) {
    furi_assert(browser);
    furi_assert(result);

    // The SDK expects us to use this function
    file_browser_get_current_path(browser, result);
}

/**
 * @brief Initialize the file browser with proper settings
 * 
 * @param browser FileBrowser instance
 * @return true if initialization was successful
 */
bool docview_file_browser_init(FileBrowser* browser) {
    furi_assert(browser);
    // Nothing specific to initialize since the file browser
    // is configured through file_browser_configure() call directly in the app
    return true;
}
