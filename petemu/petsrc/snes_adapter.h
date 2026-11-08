// -----------------------------------------------------------------------------
// snes_adapter.h
// Emulates a SNES controller (as seen through the Commodore user-port adapter)
// on the PET's VIA Port A. The game drives LATCH/CLOCK as outputs and reads DATA
// as an input; this class reproduces the controller's shift register so any game
// written for the real adapter (pet-invaders, PETSCII Robots, ...) reads it.
//
// PET wiring (VIA Port A @ $E841), from the PETSCII Robots read routine:
//   LATCH = PA5 ($20)   CLOCK = PA3 ($08)   DATA = PA6 ($40)
// Protocol: pulse LATCH high->low (parallel load), then for each button:
//   read DATA, pulse CLOCK high->low (shift to next bit).
// Button (bit) order on the wire:
//   B, Y, Select, Start, Up, Down, Left, Right, A, X, L, R   (12 bits)
//
// DATA polarity: a real SNES controller is active-low (pressed = DATA low). Some
// CBM adapters invert it (pressed = DATA high, as PETSCII Robots reads). The
// `invert` flag selects which; default = active-low (real controller), which is
// what pet-invaders expects. Either way, the IDLE level (nothing pressed) reads
// as "released", so an unread/at-rest port never looks like a stuck button.
// -----------------------------------------------------------------------------
#pragma once

#include <cstdint>

class SnesAdapter
{
public:
    // Button bit positions (wire/read order).
    enum Button {
        BTN_B = 0, BTN_Y, BTN_SELECT, BTN_START,
        BTN_UP, BTN_DOWN, BTN_LEFT, BTN_RIGHT,
        BTN_A, BTN_X, BTN_L, BTN_R,
        BTN_COUNT
    };

    // VIA Port A bit masks.
    static constexpr uint8_t LATCH = 0x20; // PA5
    static constexpr uint8_t CLOCK = 0x08; // PA3
    static constexpr uint8_t DATA  = 0x40; // PA6

    void reset()
    {
        live_ = 0; latched_ = 0; index_ = 0;
        last_latch_ = false; last_clock_ = false;
        present();
    }

    // Configuration.
    void setEnabled(bool e) { enabled_ = e; if (!enabled_) { live_ = 0; reset(); } }
    bool enabled() const { return enabled_; }
    void setInvert(bool inv) { invert_ = inv; present(); }

    // Live button state: bit i (Button index) set => that button is pressed.
    void setButtons(uint16_t pressedMask)
    {
        live_ = enabled_ ? pressedMask : 0;
    }

    // Called whenever the game writes VIA Port A. `paOut` is the level the game
    // is driving (the VIA output register). Detects LATCH/CLOCK edges and updates
    // the bit currently presented on DATA.
    void onPortAWrite(uint8_t paOut)
    {
        const bool latch = (paOut & LATCH) != 0;
        const bool clock = (paOut & CLOCK) != 0;

        if (latch && !last_latch_) {
            // Latch rising: parallel-load the live buttons, present bit 0.
            latched_ = live_;
            index_ = 0;
            present();
        }
        else if (clock && !last_clock_ && !latch) {
            // Clock rising while latch low: shift to the next bit.
            ++index_;
            present();
        }

        last_latch_ = latch;
        last_clock_ = clock;
    }

    // Current DATA (PA6) level the adapter presents: true = high.
    bool dataBit() const { return data_high_; }

private:
    void present()
    {
        const bool pressed =
            (index_ >= 0 && index_ < BTN_COUNT) ? (((latched_ >> index_) & 1u) != 0) : false;
        // invert=false (real controller): pressed -> DATA low.
        // invert=true  (inverting adapter): pressed -> DATA high.
        data_high_ = invert_ ? pressed : !pressed;
    }

    bool     enabled_   = true;
    bool     invert_    = false;   // false = active-low (real SNES), the default
    uint16_t live_      = 0;
    uint16_t latched_   = 0;
    int      index_     = 0;
    bool     last_latch_ = false;
    bool     last_clock_ = false;
    bool     data_high_  = true;   // idle = released = high (active-low default)
};
