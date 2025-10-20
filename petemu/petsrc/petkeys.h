#ifndef PETKEYS_H
#define PETKEYS_H

#include <array>
#include <cstdint>
#include <functional>

class PetKeys {
public:
    PetKeys();

    // Reset all keys (idle = 1 / not pressed)
    void clear();

    // ASCII-first API: feed printable/control chars directly (0..127)
    void pressAscii(uint8_t ascii);
    void releaseAscii(uint8_t ascii);

    // Low-level PET-matrix API (row: 0..9, col: 0..15), active-low
    // 'shift' indicates holding the PET's matrix "SHIFT" latch concurrently.
    void pressKey(int col, int row, bool shift);
    void releaseKey(int col, int row, bool shift);

    // Build from your RawInput state in one shot (key[256] + modifiers).
    // Calls the sink automatically at the end.
    void refreshFromRawInput();

    // Pack the internal 20-byte (even/odd) store to 10 rows (8 columns),
    // active-low, exactly as the JS does: out[r] = even[r] & odd[r].
    void snapshot(uint8_t out[10]) const;

    // Optional: push immediately (re-pack + send to sink) on demand.
    void pushNow() const;

    // Wire a sink that receives the 10 packed rows whenever the matrix changes.
    // Provide a callable that does: ioUnit.setKeyrows(rows10);
    void setUpdateSink(std::function<void(const uint8_t[10])> sink);

    // Inspect raw 20-byte even/odd storage if needed
    const std::array<uint8_t, 20>& raw20() const { return keyrows20; }

    // ASCII->PET lookup tables (ported from your JS)
    static const int ascii_to_pet_row[128];
    static const int ascii_to_pet_col[128];

private:
    // Internals: two bytes per row
    //   keyrows20[row*2 + 0] => even columns  (0,2,4,6,8,10,12,14)
    //   keyrows20[row*2 + 1] => odd  columns  (1,3,5,7,9,11,13,15)
    // bit index = (col >> 1)  : (0|1)->bit0, (2|3)->bit1, ... (14|15)->bit7
    std::array<uint8_t, 20> keyrows20;

    std::function<void(const uint8_t[10])> m_sink{};
    void notifySink() const;

    // Helpers for inlining bit-twiddling without a separate set_bit():
    static inline int byteIndexFromRowCol(int row, int col) {
        return row * 2 + ((col & 1) ? 1 : 0);
    }
    static inline int bitFromCol(int col) {
        return (col >> 1) & 7;
    }
    static inline void pressMatrix(std::array<uint8_t, 20>& rows, int row, int col) {
        const int b = byteIndexFromRowCol(row, col);
        const int bit = bitFromCol(col);
        rows[b] = static_cast<uint8_t>(rows[b] & ~(1u << bit)); // active LOW
    }
    static inline void releaseMatrix(std::array<uint8_t, 20>& rows, int row, int col) {
        const int b = byteIndexFromRowCol(row, col);
        const int bit = bitFromCol(col);
        rows[b] = static_cast<uint8_t>(rows[b] | (1u << bit)); // idle HIGH
    }

    // Special “wide/tall” key helper-bits (exactly like your JS)
    void applyWideTallOnPress(int col, int row);
    void applyWideTallOnRelease(int col, int row);
};

#endif // PETKEYS_H
