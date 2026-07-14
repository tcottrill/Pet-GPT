// -----------------------------------------------------------------------------
// pet_roms.h
// ROM-set loaders for the PET emulator. Each function reads the named ROM
// image files from `dir`, validates their sizes, and installs them into the
// given PetMachine (CPU ROM overlay + character generator).
// -----------------------------------------------------------------------------
#pragma once

#include <string>

class PetMachine;

// BASIC 2, zimmers.net-native file names; editorN selects the N (4KB) vs B
// (2KB) editor ROM.
bool load_pet2_romset(PetMachine& pet, const std::string& dir, bool editorN);

// BASIC 4, 40-column, "N" editor.
bool load_pet4_romset(PetMachine& pet, const std::string& dir);

// BASIC 4, 8032 (80-column, 60Hz business editor).
bool load_pet8032_romset(PetMachine& pet, const std::string& dir);
