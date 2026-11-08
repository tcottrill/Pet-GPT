#pragma once
class wxKeyEvent;
class wxWindow;
int pet_key_from_wx(int code);
void pet_keyboard_handle_wx(wxKeyEvent& event, bool down);
// wxEVT_CHAR after a skipped AltGr key-down: types the composed character.
void pet_keyboard_handle_wx_char(wxKeyEvent& event);
// Focus loss or suspension: release every PET key, including AltGr ones.
void pet_keyboard_wx_release_all();
void wx_bind_pet_keyboard(wxWindow* window);
