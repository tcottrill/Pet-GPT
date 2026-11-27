// -----------------------------------------------------------------------------
// pet_roms.h
// ROM-set loaders for the PET emulator. Each function reads the named ROM
// image files from `dir`, validates their sizes, and installs them into the
// given PetMachine (CPU ROM overlay + character generator).
// -----------------------------------------------------------------------------
#pragma once

#include <string>

class PetMachine;

// Original 1977 PET 2001, BASIC 1, seven 2 KB ROMs and original character ROM.
bool load_pet1_romset(PetMachine& pet, const std::string& dir);

// BASIC 2, zimmers.net-native file names; editorN selects the N (4KB) vs B
// (2KB) editor ROM.
bool load_pet2_romset(PetMachine& pet, const std::string& dir, bool editorN);

// BASIC 4, 40-column, "N" editor. crtc selects the 12-inch CRTC editor (60 Hz,
// or the 50 Hz one when hz50; hz50 is ignored without crtc).
bool load_pet4_romset(PetMachine& pet, const std::string& dir, bool crtc = false, bool hz50 = false);

// BASIC 4, 8032 (80-column business editor, 60 Hz or the 50 Hz one when hz50).
bool load_pet8032_romset(PetMachine& pet, const std::string& dir, bool hz50 = false);

// CBM 8296 (model 96): 8032-class editor with BASIC 4. hz50 selects the European
// 8296-D (4 KB editor + 4 KB character ROM with a CRTC-selected upper bank).
bool load_pet8296_romset(PetMachine& pet, const std::string& dir, bool hz50 = false);

// True when the optional 50 Hz editor ROM of model 12 or 8 is in `dir` (2048
// bytes), or both 8296-D files of model 96 (4096 bytes each). Always false for
// the other models. pet_romset_present ignores it.
bool pet_editor50_present(int which, const std::string& dir);

// True when every file the loader for model `which` (1, 2, 4, 12, 8, 96) needs is
// in `dir` (trailing separator) with an acceptable size. Touches no machine.
bool pet_romset_present(int which, const std::string& dir);
