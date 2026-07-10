#ifndef PET_KBD_INPUT_H
#define PET_KBD_INPUT_H

// -----------------------------------------------------------------------------
// PET 2001 / 2001N Keyboard Matrix Input (Windows, RawInput-friendly)
// Public header for the keyboard-input module.
//
// This module builds a PET-compatible 8x10 active-low keyboard matrix from the
// host keyboard state and (optionally) pushes it to your emulator I/O.
//
// Behavior implemented in the companion .cpp:
//   - RUN/STOP  = CapsLock (VK_CAPITAL)
//   - BREAK     = RUN/STOP + Shift (CapsLock + Left/Right Shift)
//   - Cursor UP = VK_UP (synthesized: presses PET Cursor-Down + Shift)
//   - Cursor DN = VK_DOWN (direct mapping)
//   - CLR/HOME  = VK_HOME (Shift+Home also asserts Shift in the matrix)
//   - OEM punctuation fallbacks for: ' " = + , < . > - _ / ?
//   - L/R modifiers mirrored into aggregate VKs for ToUnicodeEx
//   - F12 toggles "graphics mode":
//         * ON  (default): Shift+letter also asserts PET Shift so ROM yields
//                            PETSCII graphics for the letter.
//         * OFF (business): letters do not assert PET Shift; punctuation still can.
//
// Integration assumptions (same as the .cpp):
//   - You track a global `key[256]` (GDI/Raw Input style) where nonzero => key is down.
//   - You have a global `g_pet` object exposing bus().io().setKeyrows(const uint8_t[10]).
//     If you prefer to avoid globals, use build_pet_rows_from_keys() + push_rows_to_emulator().
//
// Threading: call from your emulation thread or synchronize externally.
// -----------------------------------------------------------------------------

#include <cstdint>

class PetMachine;

/// Number of rows in the PET key matrix
static constexpr int PET_KBD_ROWS = 10;

/// Number of columns in the PET key matrix
static constexpr int PET_KBD_COLS = 8;

/// Size (in bytes) of the PET key matrix buffer (one byte per row)
static constexpr int PET_KBD_ROWS_BYTES = PET_KBD_ROWS;

// -----------------------------------------------------------------------------
// Graphics-mode control (same mode that F12 toggles in the .cpp).
// -----------------------------------------------------------------------------

/// Enable/disable **graphics mode**.
/// When enabled (true):
///   - If host Shift is held while typing a **letter**, the module also asserts
///     PET Shift in the matrix so the ROM emits the PETSCII graphic for that letter.
/// When disabled (false; "business" style):
///   - Letters do **not** assert PET Shift (punctuation and special combos still can).
void set_pet_graphics_mode(bool enable) noexcept;

/// Returns current graphics mode (true = graphics mode ON).
bool get_pet_graphics_mode() noexcept;

/// Toggles graphics mode ON/OFF (same as pressing F12 in the global-path builder).
void toggle_pet_graphics_mode() noexcept;

/// Business (8032) keyboard matrix: on = VICE-buuk business positions,
/// off = the graphics/normal matrix. Set by the machine-model switch.
void set_pet_business_kbd(bool on) noexcept;
bool get_pet_business_kbd() noexcept;

// -----------------------------------------------------------------------------
// Core builders
// -----------------------------------------------------------------------------

/// Builds a PET keyboard matrix from a supplied 256-entry VK state array.
/// Does **not** push to the emulator I/O.
/// @param out_rows  Buffer of 10 bytes (one per row). On return:
///                  - Active-low matrix: bit clear => key pressed.
///                  - Idle bits remain set (0xFF).
/// @param key_state A 256-entry VK array where non-zero => VK pressed.
///                  Typically your Raw Input "is down" table.
/// Notes:
///   - Host RUN/STOP is CapsLock; BREAK is CapsLock+Shift.
///   - Cursor Up is synthesized as Cursor (Down) + Shift.
///   - CLR/HOME + Shift asserts Shift in the matrix.
///   - OEM punctuation fallbacks and ToUnicodeEx path are applied internally.
///   - Letters normalized to uppercase for PET lookup.
///   - If graphics mode is ON, Shift+letter also asserts PET Shift.
//void build_pet_rows_from_keys(std::uint8_t out[PET_KBD_ROWS_BYTES], const unsigned char key_state[256]) noexcept;
// -----------------------------------------------------------------------------
// Convenience wrappers (global integration)
// -----------------------------------------------------------------------------

/// Builds the PET matrix from the **global** `key[256]`, **pushes it** to
/// `g_pet->bus().io().setKeyrows(...)`, and also handles the runtime F12 toggle.
/// This mirrors the behavior of the legacy single entry point.
/// @param out_rows Buffer of 10 bytes (one per row) which will be filled and then pushed.
void build_pet_rows_from_vk(std::uint8_t out[PET_KBD_ROWS_BYTES]);


/// One-call convenience: build from the global key[256] and push to the emulator
/// (allocates a small local buffer internally; identical results to
///  build_pet_rows_from_vk() but you don't need to provide a buffer).
//void update_and_push_from_globals() noexcept;

void update_keyboard(PetMachine* pet);

#endif // PET_KBD_INPUT_H
