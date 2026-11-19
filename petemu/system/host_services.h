#pragma once
#include <string>
#include <vector>
// UI services supplied by the wx frontend, keeping windows out of the machine.
void host_message(const char* title, const char* message, bool error);
void host_status(const char* text);   // non-modal note (window title)
bool host_rom_setup();                // no ROM set loads: guide the user; true = try again, false = give up
int host_select_program(const std::vector<std::string>& labels);
bool host_is_focused();
