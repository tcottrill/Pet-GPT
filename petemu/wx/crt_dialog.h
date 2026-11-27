#pragma once
#include <wx/dialog.h>
#include <array>
class wxSlider;
class wxSpinCtrlDouble;
class wxCheckBox;
class wxRadioBox;

class CrtDialog final : public wxDialog {
public:
    explicit CrtDialog(wxWindow* parent);
    void Sync();
private:
    std::array<wxSlider*, 15> sliders_{};
    std::array<wxSpinCtrlDouble*, 15> values_{};
    wxCheckBox* enabled_{};
    wxRadioBox* tint_{};
};
