// -----------------------------------------------------------------------------
// via6522.h
// Portable MOS 6522 Versatile Interface Adapter (VIA) emulation.
// Modern C++17, self-contained, no external dependencies.
//
// This class models the full internal behavior of a real 6522 VIA:
//
// - Port A/B input/output pins
// - DDR registers for both ports
// - ORA/ORB output latches
// - CA1 / CA2 and CB1 / CB2 control/handshake/pulse logic
// - Full IFR/IER interrupt system (bits 0..6 + IFR7 global)
// - Timer 1 (T1): 16-bit counter + latch, one-shot or free-run
// - Timer 2 (T2): 16-bit counter + latch, internal clock or pulse counter
// - Shift register (SR): 8-bit shift in/out, 8 different clocking modes
// - PB7 output driven by T1 when enabled
// - Accurate edge detection (CA1/CB1), CA2/CB2 pulse and handshake modes
// -----------------------------------------------------------------------------

#ifndef VIA6522_H
#define VIA6522_H

#include <cstdint>


// -----------------------------------------------------------------------------
// CB2Edge
// Represents one CB2 transition for audio reconstruction.
// cycle      - CPU cycle at which the edge occurred.
// level      - New logic level after transition (0 or 1).
// -----------------------------------------------------------------------------
struct CB2Edge
{
    uint32_t cycle;   // absolute CPU cycle
    bool     level;   // new CB2 level

    CB2Edge() : cycle(0), level(false) {}
    CB2Edge(uint32_t c, bool l) : cycle(c), level(l) {}

    // Used by pet_cb2_audio.cpp
    inline uint32_t tick() const { return cycle; }
};

class VIA6522
{
public:
    VIA6522();
    void reset();

    // -------------------------------------------------------------------------
    // Port A/B raw input pins
    // Caller drives these with external device levels each cycle.
    // -------------------------------------------------------------------------
    void setPortAInput(uint8_t value);
    void setPortBInput(uint8_t value);

    // -------------------------------------------------------------------------
    // Port A/B output latch inspection (for wiring helpers)
    // -------------------------------------------------------------------------
    uint8_t getPortAOutput() const;
    uint8_t getPortBOutput() const;

    // -------------------------------------------------------------------------
    // CA1/CB1 input pins
    // -------------------------------------------------------------------------
    void setCA1(bool level);
    void setCB1(bool level);

    // -------------------------------------------------------------------------
    // CA2/CB2 input pins (for input modes)
    // -------------------------------------------------------------------------
    void setCA2(bool level);
    void setCB2(bool level);

    // CA2/CB2 outputs after mode/pulse logic
    bool getCA2Output() const;
    bool getCB2Output() const;

    // -------------------------------------------------------------------------
    // CB2 audio edge log access
    // These do not change VIA state and are safe for the host to call
    // once per video/audio frame.
    // -------------------------------------------------------------------------
    void cb2_reset_edge_log();
    uint32_t cb2_get_edge_count() const;
    const CB2Edge* cb2_get_edges() const;
    uint32_t cb2_get_tick_counter() const;

    // Lightweight debug/inspection helpers (no side effects)
    uint8_t getIFR() const { return ifr; }
    uint8_t getIER() const { return ier; }
    uint8_t getACR() const { return acr; }
    uint8_t getPCR() const { return pcr; }
    uint8_t getORA() const { return ora; }
    uint8_t getORB() const { return orb; }

    // -------------------------------------------------------------------------
    // Register Access
    //   readReg(offset)      // offset = 0..15
    //   writeReg(offset,val)
    // -------------------------------------------------------------------------
    uint8_t readReg(uint8_t offset);
    void    writeReg(uint8_t offset, uint8_t data);

    // -------------------------------------------------------------------------
    // Tick (call once per phi2 rising edge)
    // -------------------------------------------------------------------------
    void tick();

    // IRQ output: true if IFR7 is set
    bool getIRQ() const;

    // -------------------------------------------------------------------------
    // Timer 2 external pulse (PB6 negative edge)
    // Call this when PB6 sees a negative edge in external count mode.
    // -------------------------------------------------------------------------
    void externalT2Pulse();

    void runShiftRegister_T2();

    // -------------------------------------------------------------------------
   // irqLine
   // Returns true if the VIA would assert its IRQ line based on IFR/IER.
   // This matches the 6522 behavior: bit 7 of IFR is the OR of IFRx & IERx.
   // -------------------------------------------------------------------------
    bool irqLine() const
    {
        // Only bits 0-6 are maskable; bit 7 is summary.
        return (ifr & ier & 0x7F) != 0;
    }

    bool GetCB2LevelAtFrameStart() const
    {
        return cb2_level_frame_start;
    }

private:
    // -------------------------------------------------------------------------
    // Internal helpers
    // -------------------------------------------------------------------------
    uint8_t readPortA();
    uint8_t readPortB();

    void handleCA1Edge();
    void handleCB1Edge();

    // CA2 pulse / handshake helper (similar to updateCB2State)
    void updateCA2State(uint8_t mode);

    void runTimer1();
    void runTimer2();
    void runShiftRegister();

    void handleCA2Logic();
    void handleCB2Logic();
    void handlePB7Output();

    void updateIFR();

    void updatePBOutput();
    void handleCA2WriteSideEffects();
    void handleCB2WriteSideEffects();
    void updateCB2State(uint8_t mode);
    inline void driveCB2(bool level);
   
private:
    // Registers
    uint8_t ora;   // Output register A
    uint8_t orb;   // Output register B
    uint8_t ddra;  // Data direction A
    uint8_t ddrb;  // Data direction B
    uint8_t acr;   // Auxiliary control
    uint8_t pcr;   // Peripheral control
    uint8_t ifr;   // Interrupt flag
    uint8_t ier;   // Interrupt enable
    uint8_t sr;    // Shift register

    // Timers
    uint16_t t1_counter;
    uint16_t t1_latch;
    bool     t1_free_running;
    bool     t1_enabled;
    bool     t1_pb7_toggle;
    bool     t1_pb7_output;

    bool     t1_reload_pending = false;  // one-cycle 0xFFFF pass after T1 underflow (N+2 timing)
    bool     t2_reload_pending = false;  // one-cycle 0xFFFF pass after T2 underflow (N+2 timing)
    bool     t2_oneshot_fired  = false;  // plain-timer T2 already set IFR5 once

    uint16_t t2_counter;
    uint16_t t2_latch;
    bool     t2_enabled;
    bool     t2_counts_external; // ACR bit 5

    // Shift register state
    uint8_t  sr_latch;           // latched SR value for T2-driven reloads
    uint8_t  sr_bits_remaining;  // PHI2-driven modes bit counter
    uint8_t  sr_bits_left;       // T2-driven modes bit counter
    uint8_t  sr_mode;            // ACR bits 2..4
    bool     sr_clock_external;
    bool     sr_shift_out;
    bool     sr_t2_phase;          // Internal phase for T2-driven SR clock (used to divide by 2)
    bool     sr_reload_pending;    // New SR byte pending for controlled T2 modes
    bool cb2_level_frame_start;
    bool last_cb2_out;   // For detecting CB2 transitions

    // Port inputs
    uint8_t portA_in;
    uint8_t portB_in;

    // CA1/CB1 state
    bool ca1_in, old_ca1;
    bool cb1_in, old_cb1;

    // CA2/CB2
    bool    ca2_in;
    bool    cb2_in;
    bool    ca2_out;
    bool    cb2_out;
    bool    old_ca2;
    bool    old_cb2;
    uint8_t ca2_pulse_count;
    uint8_t cb2_pulse_count;

   
    // Add these to VIA6522 class members:
    bool cb2_out_state = false;     // current driven output level
    bool cb2_is_output = false;    // CB2 in output mode?
    bool cb2_shift_override = false; // SR overrides CB2?

    // -------------------------------------------------------------------------
    // CB2 audio edge logging
    // -------------------------------------------------------------------------
    static const uint32_t CB2_EDGE_MAX = 8192;

    CB2Edge   cb2_edges[CB2_EDGE_MAX];
    uint32_t  cb2_edge_count = 0;
    uint32_t  cb2_tick_counter = 0;
    bool      cb2_last_logged = false;

    void logCB2Edge();
    void logCB2Transition();
   
};

#endif // VIA6522_H
