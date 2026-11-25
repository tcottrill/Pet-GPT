// The About box (version, theme choice, Ko-fi button) and the theme setting, as in
// FreeSnip (C:\Source2026\clip, src/wx/about.cpp).
#pragma once
#include <wx/app.h>

class wxWindow;

namespace petgpt {
// The saved theme ("theme" in [ui] of pet.ini: system, light or dark), for
// wxApp::SetAppearance before the first window opens.
wxApp::Appearance savedAppearance();
// Opens ko-fi.com/timcottrill in the browser (Help > Support me on Ko-fi).
void openKofiPage();
void showAbout(wxWindow* parent);
}
