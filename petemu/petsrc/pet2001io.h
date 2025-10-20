#pragma once
#include <cstdint>
#include <functional>
#include <array>
#include <string>
#include <vector>
#include "pet2001ieee.h"
#include "pet2001video.h"


/// Faithful C++ port of pet2001io.js I/O core. It models:
/// - PIA1 (keyboard rows + screen blank on CA2)
/// - PIA2 (IEEE data & handshake bits)
/// - VIA (IEEE handshake, timers, CA/CB interrupts, charset toggle via PCR)
/// - IRQ combine logic identical to JS
class Pet2001IO {
public:
    // Low-8-bit register addresses, matching the JS
    enum : uint8_t {
        PIA1_PA = 0x10,
        PIA1_CRA = 0x11,
        PIA1_PB = 0x12,
        PIA1_CRB = 0x13,

        PIA2_PA = 0x20,
        PIA2_CRA = 0x21,
        PIA2_PB = 0x22,
        PIA2_CRB = 0x23,

        VIA_DRB = 0x40,
        VIA_DRA = 0x41,
        VIA_DDRB = 0x42,
        VIA_DDRA = 0x43,
        VIA_T1CL = 0x44,
        VIA_T1CH = 0x45,
        VIA_T1LL = 0x46,
        VIA_T1LH = 0x47,
        VIA_T2CL = 0x48,
        VIA_T2CH = 0x49,
        VIA_SR = 0x4A,
        VIA_ACR = 0x4B,
        VIA_PCR = 0x4C,
        VIA_IFR = 0x4D,
        VIA_IER = 0x4E,
        VIA_ANH = 0x4F
    };

    Pet2001IO(PetIEEE& ieeeBus,
        Pet2001Video& video,
        std::function<void(bool)> setIrqLine);

    void reset();

    void setIeeeHostRoot(const std::string& dir);
    bool setIeeeD64Image(const std::string& path);

    // Attach keyboard rows (10 bytes, each bit active-low like real PET matrix)
    void setKeyrows(const uint8_t rows[10]);

    // CPU bus access (map these where you place IO in your address space; pass offset&0xFF)
    uint8_t read(uint16_t io_lo8);
    void    write(uint16_t io_lo8, uint8_t data);

    // Called every CPU cycle (or at least ~1 MHz/60 frames like your main loop).
    void cycle();

    // PET core toggles SYNC ~60Hz; you may call this from your video timing or CPU PC-sync
    void sync(bool level);

    // Convenience forwarder
    void ieeeLoadData(uint16_t addr, const std::vector<uint8_t>& bytes);
	
    // Configuration of sync generator (call before reset())
    void setCpuClockHz(double hz);
    void setRefreshHz(double hz);
    void setSyncDutyLow(double frac);


private:
    void updateIrq();

    PetIEEE& m_ieee;
    Pet2001Video& m_video;                // *** CHANGED type ***
	
    // 
    void recomputeSyncDda();
    
    std::function<void(bool)> m_setIrq;   // level-sensitive IRQ sink

    // Keyboard rows (10), active-low semantics
    std::array<uint8_t, 10> keyrow{};

    // --- PIA1 state (keyboard + screen blank on CA2)
    uint8_t pia1_pa_in = 0xF0;
    uint8_t pia1_pa_out = 0x00;
    uint8_t pia1_ddra = 0x00;
    uint8_t pia1_cra = 0x00;

    uint8_t pia1_pb_in = 0xFF;
    uint8_t pia1_pb_out = 0x00;
    uint8_t pia1_ddrb = 0x00;
    uint8_t pia1_crb = 0x00;

    uint8_t pia1_ca2 = 0x00; // screen blank gate
    uint8_t pia1_cb1 = 0x00; // SYNC sample into CB1

    // --- PIA2 state (IEEE data & handshake)
    uint8_t pia2_pa_in = 0x00;
    uint8_t pia2_pa_out = 0x00;
    uint8_t pia2_ddra = 0x00;
    uint8_t pia2_cra = 0x00;

    uint8_t pia2_pb_in = 0x00;
    uint8_t pia2_pb_out = 0x00;
    uint8_t pia2_ddrb = 0x00;
    uint8_t pia2_crb = 0x00;

    // --- VIA state
    uint8_t via_drb_in = 0x00;
    uint8_t via_drb_out = 0x00;
    uint8_t via_dra_in = 0x00;
    uint8_t via_dra_out = 0x00;
    uint8_t via_ddrb = 0x00;
    uint8_t via_ddra = 0x00;

    uint8_t via_t1cl = 0xFF;
    uint8_t via_t1ch = 0xFF;
    uint8_t via_t1_1shot = 0x00;
    uint8_t via_t1ll = 0xFF;
    uint8_t via_t1lh = 0xFF;

    uint8_t via_t2cl = 0xFF;
    uint8_t via_t2ch = 0xFF;
    uint8_t via_t2_1shot = 0x00;
    uint8_t via_t2ll = 0x00; // (not explicit in JS; used as latch for T2CL writes)

    uint8_t via_sr = 0x00;
    uint8_t via_acr = 0x00;
    uint8_t via_pcr = 0x00;
    uint8_t via_ifr = 0x00;
    uint8_t via_ier = 0x80;

    // crude 60Hz SYNC generator like the JS (counts CPU cycles)
    int     video_cycle = 0;

	// Sync level tracking for PIA1 CB1
    double  cpu_hz_ = 1000000.0;   // default 1.000 MHz
    double  refresh_hz_ = 60.0;        // default 60 Hz
    double  duty_low_ = 5000.0 / 16667.0; // ~0.30, matches old behavior

    // Fixed-point phase state (Q0.32 style)
    uint32_t phase_ = 0;           // [0, 2^32-1] wraps each period
    uint32_t inc_ = 0;           // per-CPU-cycle phase increment
    uint32_t duty_thr_ = 0;           // threshold inside the period for low interval

    // Optional: current level to avoid redundant sync() calls
   // bool     sync_level_ = true;
  

};

