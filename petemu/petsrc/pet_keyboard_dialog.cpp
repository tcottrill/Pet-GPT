#include "pet_keyboard_dialog.h"
#include "pet_kbd_input.h"
#include "iniFile.h"

#include "../wx/keyboard_input.h"
#include <wx/wx.h>
#include "rawinput.h"
#include <wx/gbsizer.h>
#include <wx/scrolwin.h>
#include <wx/display.h>
#include <vector>
namespace {
constexpr int keyBase=5000;
wxString reserved_label(int vk) {
 switch(vk) {
 case VK_ESCAPE:return "Exit";case VK_F6:return "Screenshot";case VK_PAUSE:return "Pause";case VK_F8:return "Log CRT";case VK_F9:return "CRT knob";
 case VK_F10:return "CRT";case VK_F11:return "Fullscreen";case VK_F12:return "Gfx mode";
 case VK_PRIOR:return "CRT +";case VK_NEXT:return "CRT -";
 case VK_LSHIFT:case VK_RSHIFT:case VK_LMENU:case VK_RMENU:return "Modifier";
 case VK_LCONTROL:case VK_RCONTROL:return "Shortcuts";default:return "System";
 }
}
class Editor : public wxDialog {
public:
 PetKeyBindings draft=pet_keyboard_bindings();
 int selected='A'; bool capturing=false, shift=false;
 std::vector<int> actions;
 struct Cap {int vk; wxString pc; wxButton* button;};
 std::vector<Cap> caps;
 wxChoice* choice; wxStaticText* status; wxButton *clear,*restore,*applyButton;
 Editor(wxWindow* parent):wxDialog(parent,wxID_ANY,"PET keyboard mapping",wxDefaultPosition,wxDefaultSize,wxDEFAULT_DIALOG_STYLE|wxRESIZE_BORDER) {
  auto* outer=new wxBoxSizer(wxVERTICAL);
  outer->Add(new wxStaticText(this,wxID_ANY,get_pet_business_kbd()?"CBM 8032 business keyboard":get_pet_graphics_mode()?"PET keyboard - graphics typing":"PET keyboard - business typing"),0,wxALL,8);
  auto* scroll=new wxScrolledWindow(this); scroll->SetScrollRate(10,10);
  auto* grid=new wxGridBagSizer(2,2);
  auto add=[&](int vk,const wchar_t* text,float x,float y,float w=1,float h=1) {
   auto* button=new wxButton(scroll,keyBase+vk,wxString(text),wxDefaultPosition,FromDIP(wxSize(int(w*52),int(h*48))));
   const int row=y==48?0:y==86?1:y==112?2:y==138?3:y==164?4:5;
   grid->Add(button,wxGBPosition(row,int(x*4)),wxGBSpan(int(h),int(w*4)),wxEXPAND);
   caps.push_back({vk,wxString(text),button});
   button->Bind(wxEVT_BUTTON,[this,vk](wxCommandEvent&){selected=vk;capturing=pet_keyboard_editable(vk);refresh();});
  };
    add(VK_ESCAPE,L"Esc",0,48);
    for (int i=0;i<12;++i) {
        auto name=L"F"+std::to_wstring(i+1);
        add(VK_F1+i,name.c_str(),2.f+i+(i/4)*.5f,48);
    }
    add(VK_SNAPSHOT,L"PrtSc",15.5f,48); add(VK_SCROLL,L"Scroll",16.5f,48); add(VK_PAUSE,L"Pause",17.5f,48);
    add(VK_OEM_3,L"` ~",0,86);
    for (int i=0;i<10;++i) { wchar_t s[]={wchar_t('0'+(i+1)%10),0}; add('0'+(i+1)%10,s,1.f+i,86); }
    add(VK_OEM_MINUS,L"- _",11,86); add(VK_OEM_PLUS,L"= +",12,86); add(VK_BACK,L"Backspace",13,86,2);
    add(VK_TAB,L"Tab",0,112,1.5f);
    const wchar_t* q=L"QWERTYUIOP";
    for (int i=0;i<10;++i) { wchar_t s[]={q[i],0}; add(q[i],s,1.5f+i,112); }
    add(VK_OEM_4,L"[ {",11.5f,112); add(VK_OEM_6,L"] }",12.5f,112); add(VK_OEM_5,L"\\ |",13.5f,112,1.5f);
    add(VK_CAPITAL,L"Caps Lock",0,138,1.75f);
    const wchar_t* a=L"ASDFGHJKL";
    for (int i=0;i<9;++i) { wchar_t s[]={a[i],0}; add(a[i],s,1.75f+i,138); }
    add(VK_OEM_1,L"; :",10.75f,138); add(VK_OEM_7,L"' \"",11.75f,138); add(VK_RETURN,L"Enter",12.75f,138,2.25f);
    add(VK_LSHIFT,L"Shift",0,164,2.25f);
    const wchar_t* z=L"ZXCVBNM";
    for (int i=0;i<7;++i) { wchar_t s[]={z[i],0}; add(z[i],s,2.25f+i,164); }
    add(VK_OEM_COMMA,L", <",9.25f,164); add(VK_OEM_PERIOD,L". >",10.25f,164); add(VK_OEM_2,L"/ ?",11.25f,164);
    add(VK_RSHIFT,L"Shift",12.25f,164,2.75f);
    add(VK_LCONTROL,L"Ctrl",0,190,1.25f); add(VK_LWIN,L"Win",1.25f,190,1.25f); add(VK_LMENU,L"Alt",2.5f,190,1.25f);
    add(VK_SPACE,L"Space",3.75f,190,6.25f); add(VK_RMENU,L"Alt",10,190,1.25f); add(VK_RWIN,L"Win",11.25f,190,1.25f);
    add(VK_APPS,L"Menu",12.5f,190,1.25f); add(VK_RCONTROL,L"Ctrl",13.75f,190,1.25f);
    add(VK_INSERT,L"Ins",15.5f,86); add(VK_HOME,L"Home",16.5f,86); add(VK_PRIOR,L"PgUp",17.5f,86);
    add(VK_DELETE,L"Del",15.5f,112); add(VK_END,L"End",16.5f,112); add(VK_NEXT,L"PgDn",17.5f,112);
    add(VK_UP,L"Up",16.5f,164); add(VK_LEFT,L"Left",15.5f,190); add(VK_DOWN,L"Down",16.5f,190); add(VK_RIGHT,L"Right",17.5f,190);
    add(VK_NUMLOCK,L"Num",19,86); add(VK_DIVIDE,L"/",20,86); add(VK_MULTIPLY,L"*",21,86); add(VK_SUBTRACT,L"-",22,86);
    for (int row=0;row<3;++row) for (int col=0;col<3;++col) {
        int n=7-row*3+col; wchar_t s[]={wchar_t('0'+n),0}; add(VK_NUMPAD0+n,s,19.f+col,112.f+row*26);
    }
    add(VK_ADD,L"+",22,112,1,2); add(VK_SEPARATOR,L"Enter",22,164,1,2);
    add(VK_NUMPAD0,L"0",19,190,2); add(VK_DECIMAL,L".",21,190);

  scroll->SetSizer(grid); outer->Add(scroll,1,wxEXPAND|wxLEFT|wxRIGHT,8);
  auto* preview=new wxCheckBox(this,wxID_ANY,"Preview Shift"); outer->Add(preview,0,wxALL,8);
  preview->Bind(wxEVT_CHECKBOX,[this](wxCommandEvent& e){shift=e.IsChecked();refresh();});
  status=new wxStaticText(this,wxID_ANY,""); outer->Add(status,0,wxEXPAND|wxALL,8);
  auto* edit=new wxBoxSizer(wxHORIZONTAL); choice=new wxChoice(this,wxID_ANY);
  for(int a=PET_KEY_DEFAULT;a<=PET_KEY_SHIFT;++a) if(pet_keyboard_valid_action(a)) {actions.push_back(a);choice->Append(wxString(pet_keyboard_action_name(a)));}
  edit->Add(choice,1,wxRIGHT,8);
  clear=new wxButton(this,wxID_ANY,"Clear key");restore=new wxButton(this,wxID_ANY,"Restore key");auto* defaults=new wxButton(this,wxID_ANY,"Restore all defaults");
  edit->Add(clear,0,wxRIGHT,8);edit->Add(restore,0,wxRIGHT,8);edit->Add(defaults);
  outer->Add(edit,0,wxEXPAND|wxALL,8);
  choice->Bind(wxEVT_CHOICE,[this](wxCommandEvent& e){draft[selected]=actions[e.GetSelection()];capturing=false;refresh();});
  clear->Bind(wxEVT_BUTTON,[this](wxCommandEvent&){draft[selected]=PET_KEY_NONE;capturing=false;refresh();});
  restore->Bind(wxEVT_BUTTON,[this](wxCommandEvent&){draft[selected]=PET_KEY_DEFAULT;capturing=false;refresh();});
  defaults->Bind(wxEVT_BUTTON,[this](wxCommandEvent&){draft.fill(PET_KEY_DEFAULT);capturing=false;refresh();});
  auto* buttons=new wxStdDialogButtonSizer;
  auto* ok=new wxButton(this,wxID_OK);auto* cancel=new wxButton(this,wxID_CANCEL);applyButton=new wxButton(this,wxID_APPLY);
  buttons->AddButton(ok);buttons->AddButton(cancel);buttons->AddButton(applyButton);buttons->Realize();outer->Add(buttons,0,wxALIGN_RIGHT|wxALL,8);
  applyButton->Bind(wxEVT_BUTTON,[this](wxCommandEvent&){apply();});
  ok->Bind(wxEVT_BUTTON,[this](wxCommandEvent&){apply();EndModal(wxID_OK);});
  Bind(wxEVT_CHAR_HOOK,[this](wxKeyEvent& e){
   if(!capturing){e.Skip();return;}
   if(e.IsAutoRepeat())return;
   const int vk=pet_key_from_wx(e.GetKeyCode());
   if(vk==VK_ESCAPE){capturing=false;refresh();return;}
   if(!pet_keyboard_editable(vk)||e.ControlDown()||e.AltDown()||e.MetaDown()){status->SetLabel("Reserved key or shortcut. Press another key; Escape cancels.");return;}
   PetKeyBindings defaults;defaults.fill(PET_KEY_DEFAULT);
   auto label=pet_keyboard_label(vk,false,&defaults);
   if(label.size()==1 && label[0]>='a' && label[0]<='z')label[0]-=32;
   for(int a=32;a<=PET_KEY_SHIFT;++a) if(pet_keyboard_valid_action(a)) {
    auto name=pet_keyboard_action_name(a);if(a==PET_KEY_TAB)name=L"TAB";if(a==PET_KEY_ESCAPE)name=L"ESC";
    if(name==label){draft[selected]=a;capturing=false;refresh();return;}
   }
   status->SetLabel("That key has no default PET function. Press another key; Escape cancels.");
  });
  SetSizer(outer);
  int display=wxDisplay::GetFromWindow(parent?parent:this);if(display==wxNOT_FOUND)display=0;
  wxRect area=wxDisplay(static_cast<unsigned>(display)).GetClientArea();
  SetSize(wxSize(std::min(FromDIP(1250),area.width-32),std::min(FromDIP(610),area.height-32)));
  SetMinSize(wxSize(std::min(FromDIP(700),area.width-32),std::min(FromDIP(430),area.height-32)));
  CentreOnParent();refresh();
 }
 void refresh() {
  for(auto& c:caps){
   auto label=pet_keyboard_editable(c.vk)?wxString(pet_keyboard_label(c.vk,shift,&draft)):reserved_label(c.vk);
   c.button->SetLabel(c.pc+": "+label+(draft[c.vk]!=PET_KEY_DEFAULT?" *":""));
   c.button->SetBackgroundColour(c.vk==selected ? wxColour(219,235,254) : draft[c.vk]!=PET_KEY_DEFAULT ? wxColour(225,244,231) : wxSystemSettings::GetColour(wxSYS_COLOUR_BTNFACE));
   c.button->SetToolTip(c.pc+": "+label);
  }
  status->SetLabel(capturing?"Press a key whose PET function should be assigned. Escape cancels capture.":"Click a PC key, then capture a PET function or choose an assignment. Apply or OK saves changes.");
  bool editable=pet_keyboard_editable(selected);choice->Enable(editable);clear->Enable(editable);restore->Enable(editable);
  for(size_t i=0;i<actions.size();++i)if(actions[i]==draft[selected])choice->SetSelection(int(i));
  applyButton->Enable(draft!=pet_keyboard_bindings());
 }
 void apply(){for(int vk=0;vk<256;++vk)if(pet_keyboard_editable(vk))pet_keyboard_set_binding(vk,draft[vk]);set_config_string("input","keyboard_bindings",pet_keyboard_serialize().c_str());capturing=false;refresh();}
};
}
void pet_keyboard_load_settings(){auto* text=get_config_string("input","keyboard_bindings","default");pet_keyboard_deserialize(text);delete[] text;}
void pet_keyboard_show_dialog(wxWindow* owner){RawInput_ReleaseAll();Editor dialog(owner);dialog.ShowModal();RawInput_ReleaseAll();}


