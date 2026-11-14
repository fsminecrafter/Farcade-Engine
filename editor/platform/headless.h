// Controls of the headless (Linux) platform, for tests and screenshots.
#pragma once

#include "platform/platform.h"

namespace fe::platform {

void headless_push(const Event& e);
// Fixes time_seconds() to `t` (negative: the real clock again).
void headless_set_time(double t);
bool headless_save_png(const char* path);
// The last open_with() / reveal_in_file_manager() request ("program file" / path).
const char* headless_last_open();
const char* headless_last_reveal();

}  // namespace fe::platform
