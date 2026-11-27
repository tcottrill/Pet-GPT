// Per-machine phosphor persistence: built-in defaults and pet.ini key names.
// Header-only and free of GL so it can be unit-tested.
#pragma once
#include <string>

struct PhosphorDefaults { float persistence, decayMs, tail, tailMs; };

// Starting points, not measurements, except the CBM 8296 row, which was
// fitted to a video of the real machine. Unknown ids use the 8032 row.
inline PhosphorDefaults phosphor_defaults(int modelId)
{
    switch (modelId) {
    case 1:  return { 0.10f, 30.0f,   0.0f, 4000.0f };   // PET 2001, 9-inch white
    case 2:                                              // 2001N
    case 4:  return { 0.12f, 60.0f,   0.0f, 4000.0f };   // 4000, 9-inch
    case 96: return { 1.00f, 1300.0f, 0.5f, 3000.0f };   // CBM 8296 (fitted)
    case 12:                                             // 4000, 12-inch
    case 8:                                              // 8032
    default: return { 0.15f, 100.0f,  0.0f, 4000.0f };
    }
}

// pet.ini [video] key for one persistence knob on one machine, e.g. "mono_persist_ms_m96".
inline std::string phosphor_key(const char* knobName, int modelId)
{
    return std::string(knobName) + "_m" + std::to_string(modelId);
}
