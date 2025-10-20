#pragma once
#include <cstdint>
#include <vector>

// -----------------------------------------------------------------------------
// PetSpeaker
// One-bit speaker renderer. Call onCpuCycles(N) as CPU advances, and call
// setLevel(level) whenever the CB2 pin changes. Then call renderBlock(...) each
// audio tick to synthesize S16 samples.
// -----------------------------------------------------------------------------
class PetSpeaker {
public:
    explicit PetSpeaker(double cpuHz = 1'000'000.0)
        : m_cpuHz(cpuHz) {}

    void setCpuHz(double hz)         { m_cpuHz = (hz > 1.0 ? hz : 1.0); }
    void setAmplitude(int amp16)     { m_amp = (amp16 < 0 ? -amp16 : amp16); }

    // Advance PET time by N CPU cycles.
    inline void onCpuCycles(uint32_t cycles) { m_curCycle += cycles; }

    // Notify a level change at the current PET cycle.
    inline void setLevel(bool level) {
        if (level != m_curLevel) {
            m_curLevel = level;
            m_edges.emplace_back(m_curCycle, level);
        }
    }

    // Render 'frames' samples of S16 mono at 'outRateHz' into dst.
    void renderBlock(int16_t* dst, int frames, int outRateHz);

private:
    struct Edge { uint64_t cyc; bool lvl; };

    double      m_cpuHz      = 1'000'000.0;
    bool        m_curLevel   = false;
    uint64_t    m_curCycle   = 0;
    int         m_amp        = 12000;
    long double m_phaseCyc   = 0.0L;   // next sample start in PET cycles
    std::vector<Edge> m_edges;
};
