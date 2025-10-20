#include "petkeys.h"
#include <cstring>   // memset
#include <algorithm>

#include "rawinput.h"    // extern unsigned char key[256]; GetModifierFlags(), RI_MOD_*
// May also define KEY_ENTER/KEY_BACKSPACE/KEY_TAB/KEY_ESC/KEY_SPACE

// ----------------------------- construction / clear --------------------------

PetKeys::PetKeys() {
    clear();
}

void PetKeys::clear() {
    keyrows20.fill(0xFF); // idle HIGH (unpressed), active-low matrix
    notifySink();         // push idle state if a sink is wired
}

// ----------------------------- sink plumbing ---------------------------------

void PetKeys::setUpdateSink(std::function<void(const uint8_t[10])> sink) {
    m_sink = std::move(sink);
}

void PetKeys::notifySink() const {
    if (!m_sink) return;
    uint8_t out[10];
    snapshot(out);  // pack 20 -> 10, active-low
    m_sink(out);
}

void PetKeys::pushNow() const {
    notifySink();
}

// -------------------------- special wide/tall helpers ------------------------
//
// These mirror what your JS does when particular keys are pressed.
// They directly toggle helper bits (active-low) in other matrix cells.
// (We keep them exactly as before to match your working JS behavior.)
//
void PetKeys::applyWideTallOnPress(int col, int row) {
    // Space: row 4, col 5  -> set row 8, col 6's bit (in JS terms). We keep your prior mapping:
    if (row == 4 && col == 5) {
        // row 8, bit 3 (you used this combo previously)
        // We’ll encode it via the matrix convention (row 8, col = 6 => (6>>1)=3 in even byte):
        pressMatrix(keyrows20, /*row=*/8, /*col=*/6);
    }
    // Return: row 4, col 6 -> row 9, col 7 helper (prior mapping)
    else if (row == 4 && col == 6) {
        // row 9, col 7 => odd byte, bit (7>>1)=3
        pressMatrix(keyrows20, /*row=*/9, /*col=*/7);
    }
    // CLR/HOME: row 2, col 10 -> row 6, col 10 helper (prior mapping)
    else if (row == 2 && col == 10) {
        pressMatrix(keyrows20, /*row=*/6, /*col=*/10);
    }
    // RUN/STOP: row 3, col 10 -> row 4, col 10 helper (prior mapping)
    else if (row == 3 && col == 10) {
        pressMatrix(keyrows20, /*row=*/4, /*col=*/10);
    }
}

void PetKeys::applyWideTallOnRelease(int col, int row) {
    if (row == 4 && col == 5) {
        releaseMatrix(keyrows20, /*row=*/8, /*col=*/6);
    }
    else if (row == 4 && col == 6) {
        releaseMatrix(keyrows20, /*row=*/9, /*col=*/7);
    }
    else if (row == 2 && col == 10) {
        releaseMatrix(keyrows20, /*row=*/6, /*col=*/10);
    }
    else if (row == 3 && col == 10) {
        releaseMatrix(keyrows20, /*row=*/4, /*col=*/10);
    }
}

// ------------------------------ low-level API --------------------------------

void PetKeys::pressKey(int col, int row, bool shift) {
    if (row < 0 || row >= 10 || col < 0 || col >= 16) return;

    // main key
    pressMatrix(keyrows20, row, col);

    // Shift latch (exactly like your prior code): row 8, col 0
    if (shift) {
        pressMatrix(keyrows20, /*row=*/8, /*col=*/0);
    }

    // Extra helper bits (space/return/CLR/RUN)
    applyWideTallOnPress(col, row);

    notifySink();
}

void PetKeys::releaseKey(int col, int row, bool shift) {
    if (row < 0 || row >= 10 || col < 0 || col >= 16) return;

    releaseMatrix(keyrows20, row, col);

    if (shift) {
        releaseMatrix(keyrows20, /*row=*/8, /*col=*/0);
    }

    applyWideTallOnRelease(col, row);

    notifySink();
}

// ------------------------------ ASCII-first API ------------------------------

void PetKeys::pressAscii(uint8_t ascii) {
    if (ascii < 128) {
        const int row = ascii_to_pet_row[ascii];
        const int col = ascii_to_pet_col[ascii];
        if (row >= 0 && col >= 0) pressKey(col, row, /*shift*/false);
    }
}

void PetKeys::releaseAscii(uint8_t ascii) {
    if (ascii < 128) {
        const int row = ascii_to_pet_row[ascii];
        const int col = ascii_to_pet_col[ascii];
        if (row >= 0 && col >= 0) releaseKey(col, row, /*shift*/false);
    }
}

// ------------------------- build from RawInput state -------------------------
//
// You said rawinput::key[256] “are always updated and correct ASCII”.
// We trust that: any non-zero key[a] => ASCII 'a' currently down.
// We also assert the PET SHIFT matrix bit from modifiers (not via ASCII).
//
void PetKeys::refreshFromRawInput() {
    // Reset to idle HIGH
    keyrows20.fill(0xFF);

    // Shift as a matrix key (not inferred from ASCII)
    const int mods = GetModifierFlags();
    const bool sh = (mods & RI_MOD_SHIFT) != 0;
    if (sh) {
        pressMatrix(keyrows20, /*row=*/8, /*col=*/0);
    }

    extern unsigned char key[256];
    for (int a = 0; a < 256; ++a) {
        if (!key[a]) continue;
        const uint8_t ascii = static_cast<uint8_t>(a);
        if (ascii < 128) {
            const int row = ascii_to_pet_row[ascii];
            const int col = ascii_to_pet_col[ascii];
            if (row >= 0 && col >= 0) {
                pressMatrix(keyrows20, row, col);
                // special helpers if those ASCII codes correspond to those matrix cells
                applyWideTallOnPress(col, row);
            }
        }
    }

    // Some layouts may not place these into the ASCII range of key[], cover them:
#ifdef KEY_ENTER
    if (key[KEY_ENTER]) {
        // ASCII Return = 0x0D
        const uint8_t ascii = 0x0D;
        const int row = ascii_to_pet_row[ascii];
        const int col = ascii_to_pet_col[ascii];
        if (row >= 0 && col >= 0) applyWideTallOnPress(col, row), pressMatrix(keyrows20, row, col);
    }
#endif
#ifdef KEY_BACKSPACE
    if (key[KEY_BACKSPACE]) {
        const uint8_t ascii = 0x08;
        const int row = ascii_to_pet_row[ascii];
        const int col = ascii_to_pet_col[ascii];
        if (row >= 0 && col >= 0) pressMatrix(keyrows20, row, col);
    }
#endif
#ifdef KEY_TAB
    if (key[KEY_TAB]) {
        const uint8_t ascii = 0x09;
        const int row = ascii_to_pet_row[ascii];
        const int col = ascii_to_pet_col[ascii];
        if (row >= 0 && col >= 0) pressMatrix(keyrows20, row, col);
    }
#endif
#ifdef KEY_ESC
    if (key[KEY_ESC]) {
        const uint8_t ascii = 0x1B;
        const int row = ascii_to_pet_row[ascii];
        const int col = ascii_to_pet_col[ascii];
        if (row >= 0 && col >= 0) pressMatrix(keyrows20, row, col);
    }
#endif
#ifdef KEY_SPACE
    if (key[KEY_SPACE]) {
        const uint8_t ascii = ' ';
        const int row = ascii_to_pet_row[ascii];
        const int col = ascii_to_pet_col[ascii];
        if (row >= 0 && col >= 0) applyWideTallOnPress(col, row), pressMatrix(keyrows20, row, col);
    }
#endif

    notifySink();
}

// ------------------------------- snapshot(10) --------------------------------
//
// Pack even/odd halves into 8 columns (active-low):
// out[row] = keyrows20[row*2 + 0] & keyrows20[row*2 + 1]
//
void PetKeys::snapshot(uint8_t out[10]) const {
    for (int r = 0; r < 10; ++r) {
        out[r] = static_cast<uint8_t>(keyrows20[r * 2] & keyrows20[r * 2 + 1]);
    }
}

// -------------------------- ASCII->PET lookup tables -------------------------
//
// These match the JS behavior you were using. Keep them verbatim.

const int PetKeys::ascii_to_pet_row[128] = {
    -1,-1,-1,-1,-1,-1,-1,-1,0,-1,-1,-1,-1,3,-1,-1,
    -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
    4,0,0,0,0,0,0,0,0,0,2,3,3,4,4,1,
    4,3,3,3,2,2,2,1,1,1,2,3,4,4,4,3,
    4,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
    -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,4,0,4,-1,-1,
    -1,2,3,3,2,1,2,2,2,1,2,2,2,3,3,1,
    1,1,1,2,1,1,3,1,3,1,3,-1,-1,-1,-1,-1,
};

const int PetKeys::ascii_to_pet_col[128] = {
    -1,-1,-1,-1,-1,-1,-1,-1,15,-1,-1,-1,-1,10,-1,-1,
    -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
    6,0,1,2,3,4,6,5,8,9,15,15,7,14,13,15,
    12,12,13,14,12,13,14,12,13,14,9,8,7,15,8,9,
    2,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
    -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,3,7,4,-1,-1,
    -1,0,4,2,2,2,3,4,5,7,6,7,8,6,5,8,
    9,0,3,1,4,6,3,1,1,5,0,-1,-1,-1,-1,-1,
};
