#pragma once

#include <furi.h>
#include <gui/modules/file_browser.h>
#include <storage/storage.h>

/**
 * @brief Helper to check if a file is selected in the browser
 * 
 * @param browser FileBrowser instance
 * @return bool true if file is selected
 */
bool docview_is_file_selected(FileBrowser* browser);

/**
 * @brief Get the path from the file browser
 * 
 * @param browser FileBrowser instance
 * @param result FuriString to store the path
 */
void docview_get_file_path(FileBrowser* browser, FuriString* result);

/**
 * @brief Initialize file browser for document selection
 * 
 * @param browser File browser instance to initialize
 * @return true if successful
 */
bool docview_file_browser_init(FileBrowser* browser);

// SDK function prototypes that we use
#ifndef FILE_BROWSER_API_DEFINED
#define FILE_BROWSER_API_DEFINED
void file_browser_get_current_path(FileBrowser* browser, FuriString* result);
#endif
