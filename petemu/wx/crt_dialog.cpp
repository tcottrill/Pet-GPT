#include "crt_dialog.h"
#include "emulator.h"
#include "iniFile.h"
#include <wx/wx.h>
#include <wx/spinctrl.h>
#include <cmath>

CrtDialog::CrtDialog(wxWindow* parent)
    : wxDialog(parent, wxID_ANY, "CRT Monitor Settings", wxDefaultPosition,
               wxDefaultSize, wxDEFAULT_DIALOG_STYLE | wxRESIZE_BORDER) {
    auto* outer = new wxBoxSizer(wxVERTICAL);
    enabled_ = new wxCheckBox(this, wxID_ANY, "Enable shader");
    outer->Add(enabled_, 0, wxALL, FromDIP(10));
    const wxString colors[] = {"Green phosphor", "Black && white"};
    tint_ = new wxRadioBox(this, wxID_ANY, "Monitor", wxDefaultPosition, wxDefaultSize,
                         2, colors, 2, wxRA_SPECIFY_COLS);
    outer->Add(tint_, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, FromDIP(10));
    auto* grid = new wxFlexGridSizer(3, FromDIP(8), FromDIP(8));
    grid->AddGrowableCol(1);
    const char* names[] = {"Horizontal blur", "Vertical blur", "Halation", "Halation radius",
                           "Scanlines", "Contrast", "Brightness"};
    for (int i = 0; i < 7; ++i) {
        float lo, hi, step;
        pet_shader_range(i, &lo, &hi, &step);
        grid->Add(new wxStaticText(this, wxID_ANY, names[i]), 0, wxALIGN_CENTER_VERTICAL);
        sliders_[i] = new wxSlider(this, wxID_ANY, 0, 0, 1000, wxDefaultPosition, FromDIP(wxSize(210, -1)));
        values_[i] = new wxSpinCtrlDouble(this, wxID_ANY, "", wxDefaultPosition,
            FromDIP(wxSize(100, -1)), wxSP_ARROW_KEYS, lo, hi, pet_shader_get(i), step);
        values_[i]->SetDigits(2);
        grid->Add(sliders_[i], 1, wxEXPAND);
        grid->Add(values_[i], 0, wxEXPAND);
        sliders_[i]->Bind(wxEVT_SLIDER, [this, i, lo, hi](wxCommandEvent&) {
            pet_shader_set(i, lo + (hi - lo) * sliders_[i]->GetValue() / 1000.0f);
            values_[i]->SetValue(pet_shader_get(i));
        });
        values_[i]->Bind(wxEVT_SPINCTRLDOUBLE, [this, i](wxSpinDoubleEvent&) {
            pet_shader_set(i, static_cast<float>(values_[i]->GetValue())); Sync();
        });
        values_[i]->Bind(wxEVT_KILL_FOCUS, [this, i](wxFocusEvent& event) {
            pet_shader_set(i, static_cast<float>(values_[i]->GetValue())); Sync(); event.Skip();
        });
    }
    outer->Add(grid, 1, wxEXPAND | wxALL, FromDIP(10));
    auto* buttons = new wxBoxSizer(wxHORIZONTAL);
    auto* defaults = new wxButton(this, wxID_ANY, "Restore Defaults");
    buttons->Add(defaults); buttons->AddStretchSpacer();
    auto* close = new wxButton(this, wxID_CLOSE, "Close"); buttons->Add(close);
    outer->Add(buttons, 0, wxEXPAND | wxALL, FromDIP(10));
    SetSizerAndFit(outer); SetMinSize(GetSize());
    enabled_->Bind(wxEVT_CHECKBOX, [this](wxCommandEvent&) {
        pet_set_crt(enabled_->GetValue()); set_config_bool("video", "crt", enabled_->GetValue());
    });
    tint_->Bind(wxEVT_RADIOBOX, [this](wxCommandEvent&) {
        pet_set_monitor(tint_->GetSelection() == 0);
        set_config_bool("video", "crt_tint", tint_->GetSelection() == 0);
    });
    defaults->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { pet_shader_defaults(); Sync(); });
    close->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { Hide(); });
    Bind(wxEVT_CLOSE_WINDOW, [this](wxCloseEvent&) { Hide(); });
    Sync();
}

void CrtDialog::Sync() {
    enabled_->SetValue(pet_get_crt() != 0); tint_->SetSelection(pet_get_monitor() ? 0 : 1);
    for (int i = 0; i < 7; ++i) {
        float lo, hi, step; pet_shader_range(i, &lo, &hi, &step);
        const float value = pet_shader_get(i);
        sliders_[i]->SetValue(hi > lo ? static_cast<int>(std::lround((value - lo) * 1000 / (hi - lo))) : 0);
        values_[i]->SetValue(value);
    }
}
