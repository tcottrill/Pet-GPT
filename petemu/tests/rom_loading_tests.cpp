#include "pet_machine.h"
#include "pet_roms.h"
#include <filesystem>
#include <fstream>
#include <cstdio>
namespace Log {
bool open(const std::string&) { return true; } void close() {}
void write(Level,const char*,const char*,int,const char*,...) {}
void setLevel(Level) {} void setConsoleOutputEnabled(bool) {}
}
int main() {
    namespace fs=std::filesystem;
    const fs::path dir=fs::temp_directory_path()/fs::u8path("pet-rom-test-\xc3\xa9");
    fs::create_directories(dir);
    auto put=[&](const char* name,size_t size) {
        std::ofstream out(dir/name,std::ios::binary);
        std::string bytes(size,'\x42'); out.write(bytes.data(),bytes.size());
    };
    put("basic-2-c000.901465-01.bin",4096); put("basic-2-d000.901465-02.bin",4096);
    put("edit-2-n.901447-24.bin",2048); put("kernal-2.901465-03.bin",4096);
    const std::string root=dir.u8string()+"/";
    PetMachine machine;
    const uint8_t sentinel=0x99; machine.loadRom(&sentinel,1,0xc000);
    int failures=0;
    auto check=[&](bool ok,const char* name) { if(!ok) { ++failures; printf("FAIL: %s\n",name); } };
    check(!load_pet2_romset(machine,root,true),"missing character ROM rejected");
    check(machine.bus().readByte(0xc000)==sentinel,"failed ROM switch preserves current BASIC");
    put("characters-2.901447-10.bin",2048);
    check(load_pet2_romset(machine,root,true),"UTF-8 ROM directory loads");
    check(machine.bus().readByte(0xc000)==0x42,"valid ROM installed");
    put("edit-2-n.901447-24.bin",15);
    check(!load_pet2_romset(machine,root,true),"invalid editor rejected");
    check(machine.bus().readByte(0xe000)==0x42,"invalid editor preserves previous ROM");
    // 50 Hz editor ROMs: fabricated files only, each editor filled with its own byte so the loaded one is identifiable.
    auto putFill=[&](const char* name,size_t size,char fill) {
        std::ofstream out(dir/name,std::ios::binary);
        std::string bytes(size,fill); out.write(bytes.data(),bytes.size());
    };
    check(!pet_editor50_present(12,root)&&!pet_editor50_present(8,root),"50 Hz editor absent when the files are missing");
    putFill("edit-4-40-n-50Hz.901498-01.bin",2047,'\x50'); putFill("edit-4-80-b-50Hz.901474-04-0283.bin",4096,'\x50');
    check(!pet_editor50_present(12,root)&&!pet_editor50_present(8,root),"50 Hz editor of the wrong size rejected");
    putFill("edit-4-40-n-50Hz.901498-01.bin",2048,'\x50'); putFill("edit-4-80-b-50Hz.901474-04-0283.bin",2048,'\x50');
    check(pet_editor50_present(12,root)&&pet_editor50_present(8,root),"2048-byte 50 Hz editors found for models 12 and 8");
    check(!pet_editor50_present(1,root)&&!pet_editor50_present(2,root)&&!pet_editor50_present(4,root),"no 50 Hz editor for models without a CRTC");
    check(!pet_editor50_present(12,(dir/"nowhere").u8string()+"/"),"50 Hz editor absent in a missing directory");
    putFill("basic-4-b000.901465-23.bin",4096,'\x42'); putFill("basic-4-b000.901465-19.bin",4096,'\x42');
    putFill("basic-4-c000.901465-20.bin",4096,'\x42'); putFill("basic-4-d000.901465-21.bin",4096,'\x42');
    putFill("kernal-4.901465-22.bin",4096,'\x42');
    putFill("edit-4-40-n-60Hz.901499-01.bin",2048,'\x60'); putFill("edit-4-80-b-60Hz.901474-03.bin",2048,'\x60');
    check(pet_romset_present(12,root)&&pet_romset_present(8,root),"a CRTC set is present on its 60 Hz editor alone");
    check(load_pet4_romset(machine,root,true)&&machine.bus().readByte(0xe000)==0x60,"model 12 defaults to the 60 Hz editor");
    check(load_pet4_romset(machine,root,true,true)&&machine.bus().readByte(0xe000)==0x50,"model 12 hz50 loads the 50 Hz editor");
    check(load_pet8032_romset(machine,root)&&machine.bus().readByte(0xe000)==0x60,"8032 defaults to the 60 Hz editor");
    check(load_pet8032_romset(machine,root,true)&&machine.bus().readByte(0xe000)==0x50,"8032 hz50 loads the 50 Hz editor");
    fs::remove(dir/"edit-4-40-n-50Hz.901498-01.bin");
    check(!load_pet4_romset(machine,root,true,true)&&pet_romset_present(12,root),"missing 50 Hz editor fails the hz50 load but not the set");
    // CBM 8296 (model 96): required 60 Hz set, optional 8296-D editor + 4 KB character ROM.
    check(pet_romset_present(96,root),"8296 set present on the 60 Hz files (BASIC B000 -23 revision)");
    fs::remove(dir/"basic-4-b000.901465-23.bin");
    check(!pet_romset_present(96,root)&&pet_romset_present(8,root),"8296 needs the -23 B000 ROM; the 8032 uses -19");
    putFill("basic-4-b000.901465-23.bin",4096,'\x42');
    check(!pet_editor50_present(96,root),"8296-D files absent");
    putFill("edit-50hz-324243-04.bin",4096,'\xd4');
    check(!pet_editor50_present(96,root),"8296-D needs the character ROM too");
    putFill("characters-324242-01.bin",2048,'\x00');
    check(!pet_editor50_present(96,root),"8296-D character ROM of the wrong size rejected");
    { std::ofstream out(dir/"characters-324242-01.bin",std::ios::binary);   // lower 2 KB dark, upper 2 KB solid
      std::string bytes(2048,'\x00'); bytes.append(2048,'\xff'); out.write(bytes.data(),bytes.size()); }
    check(pet_editor50_present(96,root)&&pet_romset_present(96,root),"8296-D optional files found, the set still present");
    putFill("edit-50hz-324243-04.bin",2048,'\xd4');
    check(!pet_editor50_present(96,root),"8296-D editor must be 4096 bytes");
    putFill("edit-50hz-324243-04.bin",4096,'\xd4');
    check(load_pet8296_romset(machine,root)&&machine.bus().readByte(0xe000)==0x60&&machine.bus().readByte(0xb000)==0x42,"8296 defaults to the 60 Hz editor");
    check(load_pet8296_romset(machine,root,true)&&machine.bus().readByte(0xe000)==0xd4&&machine.bus().readByte(0xe900)==0xd4,"8296-D loads the 4 KB editor at $E000");
    check(load_pet8296_romset(machine,root)&&machine.bus().readByte(0xe900)!=0xd4,"60 Hz load clears the rest of a previous 4 KB editor");
    fs::remove(dir/"characters-324242-01.bin");
    check(!load_pet8296_romset(machine,root,true)&&machine.bus().readByte(0xe000)==0x60,"missing 8296-D file fails the load and keeps the current ROMs");
    check(pet_romset_present(96,root),"a missing 8296-D file does not affect the 60 Hz set");
    // Character bank across loads: only the 8296-D character ROM installs a second pair.
    { std::ofstream out(dir/"characters-324242-01.bin",std::ios::binary);
      std::string bytes(2048,'\x00'); bytes.append(2048,'\xff'); out.write(bytes.data(),bytes.size()); }
    auto& bus=machine.bus(); auto& video=machine.video();
    bus.setScreenWindow(true); video.setColumns(80); machine.io().configureCrtc(true,2);
    auto crtcReg=[&](int reg,int value) { bus.writeByte(0xe880,(uint8_t)reg); bus.writeByte(0xe881,(uint8_t)value); };
    crtcReg(6,25); crtcReg(9,7); crtcReg(1,40); crtcReg(13,0);
    auto litAfter=[&](int r12) { crtcReg(12,r12); bus.writeByte(0x8000,1); return video.framebuffer()[0]!=0xff000000u; };
    check(load_pet8296_romset(machine,root,true),"8296-D reloads");
    check(!litAfter(0x00)&&litAfter(0x20),"8296-D: R12 bit 5 selects the upper half of the character ROM");
    check(load_pet8296_romset(machine,root)&&!litAfter(0x20),"a 60 Hz load leaves no second character bank");
    check(load_pet8296_romset(machine,root,true)&&litAfter(0x20)&&load_pet8032_romset(machine,root)&&!litAfter(0x20),"loading an 8032 set after the 8296-D drops the second bank");
    fs::remove_all(dir);
    printf("ROM loading: %d failures\n",failures); return failures?1:0;
}
