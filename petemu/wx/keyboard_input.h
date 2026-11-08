#pragma once
class wxKeyEvent;
class wxWindow;
int pet_key_from_wx(int code);
void pet_keyboard_handle_wx(wxKeyEvent& event, bool down);
void wx_bind_pet_keyboard(wxWindow* window);
