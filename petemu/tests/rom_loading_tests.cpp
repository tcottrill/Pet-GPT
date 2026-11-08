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
    fs::remove_all(dir);
    printf("ROM loading: %d failures\n",failures); return failures?1:0;
}
