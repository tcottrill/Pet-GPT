#include "pet_speaker.h"
#include <algorithm>

void PetSpeaker::renderBlock(int16_t* dst, int frames, int outRateHz)
{
    if (!dst || frames <= 0 || outRateHz <= 0) return;

    const long double cycPerSample =
        static_cast<long double>(m_cpuHz) / static_cast<long double>(outRateHz);

    // Edges are appended in time order, but sort defensively.
    std::sort(m_edges.begin(), m_edges.end(),
              [](const Edge& a, const Edge& b){ return a.cyc < b.cyc; });

    // Find level at start of block
    size_t ei = 0;
    bool level = m_curLevel;
    while (ei < m_edges.size() &&
           static_cast<long double>(m_edges[ei].cyc) <= m_phaseCyc) {
        level = m_edges[ei].lvl;
        ++ei;
    }

    for (int i = 0; i < frames; ++i) {
        const long double sampleStart = m_phaseCyc + cycPerSample * static_cast<long double>(i);
        const long double sampleEnd   = sampleStart + cycPerSample;

        // Apply any edges in this sample (end-of-sample value wins).
        while (ei < m_edges.size() &&
               static_cast<long double>(m_edges[ei].cyc) < sampleEnd) {
            level = m_edges[ei].lvl;
            ++ei;
        }

        dst[i] = level ? static_cast<int16_t>(+m_amp) : static_cast<int16_t>(-m_amp);
    }

    m_phaseCyc += cycPerSample * static_cast<long double>(frames);

    // Drop consumed edges (everything strictly before next sample start)
    const long double keepAfter = m_phaseCyc;
    auto it = std::lower_bound(
        m_edges.begin(), m_edges.end(), keepAfter,
        [](const Edge& e, const long double val){ return static_cast<long double>(e.cyc) < val; });
    if (it != m_edges.begin()) m_edges.erase(m_edges.begin(), it);
}
