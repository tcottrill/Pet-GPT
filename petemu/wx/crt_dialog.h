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
    std::array<wxSlider*, 7> sliders_{};
    std::array<wxSpinCtrlDouble*, 7> values_{};
    wxCheckBox* enabled_{};
    wxRadioBox* tint_{};
};
