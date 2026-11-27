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
    std::array<wxSlider*, 11> sliders_{};
    std::array<wxSpinCtrlDouble*, 11> values_{};
    wxCheckBox* enabled_{};
    wxRadioBox* tint_{};
};
