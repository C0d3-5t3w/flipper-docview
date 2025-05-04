#include <gui/icon.h>
#include <gui/icon_i.h>
#include "docview_icons.h"

// Reference system icons - these are already defined in the firmware
// We just need to provide the declarations in our code

// Define a basic document icon
const uint8_t I_doc_data[] = {0xFF, 0x81, 0x81, 0x81, 0x81, 0x81, 0x81, 0x81, 0x81, 0xFF};

const uint8_t* const I_doc_frames[] = {I_doc_data};

const Icon I_doc =
    {.width = 10, .height = 10, .frame_count = 1, .frame_rate = 0, .frames = I_doc_frames};

// For system icons like Ok and Error, we'll create placeholders
// that match the system icons' layout but with our own data

const uint8_t I_Ok_custom_data[] = {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x08, 0x00, 0x14, 0x00,
                                    0x22, 0x00, 0x41, 0x00, 0x80, 0x00, 0x00, 0x00, 0x00, 0x00};

const uint8_t* const I_Ok_custom_frames[] = {I_Ok_custom_data};

const Icon I_Ok =
    {.width = 10, .height = 10, .frame_count = 1, .frame_rate = 0, .frames = I_Ok_custom_frames};

const uint8_t I_Error_custom_data[] = {0x00, 0x00, 0x41, 0x00, 0x22, 0x00, 0x14, 0x00, 0x08, 0x00,
                                       0x14, 0x00, 0x22, 0x00, 0x41, 0x00, 0x00, 0x00, 0x00, 0x00};

const uint8_t* const I_Error_custom_frames[] = {I_Error_custom_data};

const Icon I_Error = {
    .width = 10,
    .height = 10,
    .frame_count = 1,
    .frame_rate = 0,
    .frames = I_Error_custom_frames};
