#pragma once

#include <furi.h>
#include <gui/modules/file_browser.h>

bool docview_is_file_selected(FileBrowser* browser);

void docview_get_file_path(FileBrowser* browser, FuriString* result);

bool docview_file_browser_init(FileBrowser* browser);
