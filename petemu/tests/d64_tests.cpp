// Standalone D64 backend tests. Compiles the PetIEEE disk sources alone.
// Covers the two silent-correctness fixes:
//   #1  d64_alloc_block reaches every sector on 18-sector tracks (25..30).
//   #2  last-sector byte[1] is the CBM "offset of last used byte" (count+1),
//       so save->load round-trips and a full final sector reads 0xFF.
#include <cstdio>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
#include <set>
#include <fstream>
#include <iterator>
#include <filesystem>
#include <chrono>

#include "pet2001ieee.h"
#include "ieee_helpers.h"
#include "sys_log.h"

// --- no-op Log stubs so the IEEE sources link without the real logger ---
namespace Log {
    bool open(const std::string&) { return true; }
    void close() {}
    void write(Level, const char*, const char*, int, const char*, ...) {}
    void setLevel(Level) {}
    void setConsoleOutputEnabled(bool) {}
}

// --- tiny assert framework ---
static int g_fail = 0, g_checks = 0;
#define CHECK(cond) do { ++g_checks; if(!(cond)){ ++g_fail; \
    std::printf("FAIL %s:%d  CHECK(%s)\n", __FILE__, __LINE__, #cond);} } while(0)
#define CHECK_EQ(a,b) do { ++g_checks; long _va=(long)(a), _vb=(long)(b); \
    if(_va!=_vb){ ++g_fail; std::printf("FAIL %s:%d  CHECK_EQ(%s,%s)  got %ld != %ld\n", \
    __FILE__, __LINE__, #a, #b, _va, _vb);} } while(0)

// --- test seam into PetIEEE privates (declared as friend in the header) ---
struct PetIEEE_TestAccess {
    static const std::vector<uint8_t>& image(PetIEEE& e) { return e.d64; }
    static void image_path(PetIEEE& e, const std::string& p) { e.d64Path = p; }
    static bool host_load(PetIEEE& e, const std::string& n, std::vector<uint8_t>& b, uint16_t& a) { return e.loadHostPRG_Folder(n,b,a); }
    static void host_save(PetIEEE& e, const std::string& n) { e.saveFile(n, std::string("\1\4X",3)); }
    static bool save_prg(PetIEEE& e, const std::string& n, const std::vector<uint8_t>& b) { return e.d64_save_prg(n, b); }
    static bool load_prg(PetIEEE& e, const std::string& n, std::vector<uint8_t>& pay, uint16_t& addr) { return e.loadHostPRG_D64(n, pay, addr); }
    static bool parse_bam(PetIEEE& e, D64Catalog& c) { return e.d64_parse_bam(c); }
    static bool parse_dir(PetIEEE& e, D64Catalog& c) { return e.d64_parse_directory(c); }
    static bool alloc_block(PetIEEE& e, D64Catalog& c, int pref, int il, D64Block& out) { return e.d64_alloc_block(c, pref, il, out); }
    static bool read_sector(PetIEEE& e, int t, int s, uint8_t* dst) { return e.d64_read_sector(t, s, dst); }
    static std::string norm_prg(const std::string& s) { return PetIEEE::normalize_name_for_prg(s); }
    static bool build_dir(PetIEEE& e, std::vector<uint8_t>& out, const std::string& match) { return e.buildDirectoryPRG_D64(out, 0x0401, match); }
    static bool build_dir_folder(PetIEEE& e, std::vector<uint8_t>& out, const std::string& match) { return e.buildDirectoryPRG_Folder(out, 0x0401, match); }
    static bool cmd(PetIEEE& e, const std::string& s) { return e.process_command_channel_string(s); }
    static std::string status(PetIEEE& e) { return e.status15_; }
    static bool write_sector(PetIEEE& e, int t, int s, const uint8_t* src) { return e.d64_write_sector(t, s, src); }
    // Direct-access ('#') channel internals for the block-command tests
    static void open_da(PetIEEE& e, uint8_t sa) { e.open_direct_access_channel(sa); }
    static bool da_active(PetIEEE& e, int sa) { return e.da_active_[sa]; }
    static uint8_t* da_buf(PetIEEE& e, int sa) { return e.da_buf_[sa].data(); }
    static uint8_t da_ptr(PetIEEE& e, int sa) { return e.da_ptr_[sa]; }
    static std::vector<uint8_t>& stream_data(PetIEEE& e, int sa) { return e.streams[sa].data; }
    static size_t stream_index(PetIEEE& e, int sa) { return e.streams[sa].index; }
    static bool open_read(PetIEEE& e, const std::string& n, uint8_t sa, uint8_t type) {
        return e.openD64SEQ_for_read(n, sa, type);
    }
    static bool save_typed(PetIEEE& e, const std::string& n, const std::vector<uint8_t>& b, uint8_t t) {
        return e.d64_save_file(n, b.data(), b.size(), t);
    }
    // Drive real IEEE byte sequences through dataIn() so the OPEN-parameter
    // parsing (the bug site) is exercised end to end. Command bytes go out
    // under ATN low; payload bytes with ATN high, exactly like the bus glue.
    static void bus_cmd(PetIEEE& e, uint8_t b) { e.atn = false; e.dataIn(b); }
    static void bus_data(PetIEEE& e, uint8_t b) { e.atn = true; e.dataIn(b); }
    // OPEN lfn,8,sa,"name" : LISTEN 8, OPEN-secondary, name text, UNLISTEN.
    static void open_ch(PetIEEE& e, uint8_t sa, const std::string& name) {
        bus_cmd(e, 0x28);              // LISTEN device 8
        bus_cmd(e, (uint8_t)(0xF0 | sa)); // OPEN secondary
        for (char c : name) bus_data(e, (uint8_t)c);
        bus_cmd(e, 0x3F);              // UNLISTEN -> finalize OPEN
    }
    // PRINT#sa, data : LISTEN 8, data-secondary, bytes, UNLISTEN.
    static void put_data(PetIEEE& e, uint8_t sa, const std::vector<uint8_t>& b) {
        bus_cmd(e, 0x28);
        bus_cmd(e, (uint8_t)(0x60 | sa));
        for (uint8_t by : b) bus_data(e, by);
        bus_cmd(e, 0x3F);
    }
    // CLOSE sa : LISTEN 8, CLOSE-secondary (commits a write), UNLISTEN.
    static void close_ch(PetIEEE& e, uint8_t sa) {
        bus_cmd(e, 0x28);
        bus_cmd(e, (uint8_t)(0xE0 | sa));
        bus_cmd(e, 0x3F);
    }
    // Simulate the host fully reading the error channel (15) then UNTALK, which
    // is where DOS clears the status back to "00, OK".
    static void ch15_full_read_untalk(PetIEEE& e) {
        e.current_talk_sa = 15;
        e.state = PetIEEE::STATE_LOAD;
        e.data_index = 0;
        e.streams[15].index = e.streams[15].data.size();   // whole line consumed
        e.atn = false;
        e.dataIn(0x5F);                                     // UNTALK
    }
    // SAVE"name",8 : LISTEN, OPEN on SA 1 with the name, UNLISTEN, then the
    // KERNAL's LISTEN/SA $61/bytes/UNLISTEN; the commit happens on UNLISTEN.
    static void bus_save(PetIEEE& e, const std::string& name, const std::vector<uint8_t>& b) {
        open_ch(e, 1, name);
        bus_cmd(e, 0x28); bus_cmd(e, 0x61);
        for (uint8_t by : b) bus_data(e, by);
        bus_cmd(e, 0x3F);
    }
    static const std::string& cmd15_pending(PetIEEE& e) { return e.cmd15_buf_; }
    static bool write_active(PetIEEE& e, int sa) { return e.seq_write_active_[sa]; }
    static bool bam_free(PetIEEE& e, int t, int s) {
        D64Catalog c; if (!e.d64_parse_bam(c)) return false; return e.d64_bam_is_free(c, t, s);
    }
};

static int status_code(PetIEEE& e) {
    const std::string s = PetIEEE_TestAccess::status(e);
    return (s.size() >= 2) ? std::atoi(s.substr(0, 2).c_str()) : -1;
}

// --- D64/D71 geometry (local copy; mirrors PetIEEE::d64_sectors_per_track) ---
static int spt(int t) { if (t < 1) return 0; int z = (t >= 36 && t <= 70) ? t - 35 : t; if (z <= 17) return 21; if (z <= 24) return 19; if (z <= 30) return 18; if (z <= 35) return 17; return 0; }
static size_t soff(int t, int s) { int tot = 0; for (int i = 1; i < t; ++i) tot += spt(i); return (size_t)((tot + s) * 256); }

// Build a minimally-valid blank 35-track image (BAM + empty directory).
static std::vector<uint8_t> make_blank_d64() {
    std::vector<uint8_t> img(174848, 0);
    uint8_t* bam = img.data() + soff(18, 0);
    bam[0] = 18; bam[1] = 1; bam[2] = 0x41; bam[3] = 0x00;   // dir link 18/1, DOS 'A'
    for (int t = 1; t <= 35; ++t) {
        const int n = spt(t);
        uint8_t bits[3] = { 0,0,0 };
        for (int s = 0; s < n; ++s) bits[s >> 3] |= (uint8_t)(1u << (s & 7));
        int freec = n;
        if (t == 18) { bits[0] &= (uint8_t)~0x03; freec = n - 2; } // sectors 0,1 used
        const int o = 4 + (t - 1) * 4;
        bam[o + 0] = (uint8_t)freec; bam[o + 1] = bits[0]; bam[o + 2] = bits[1]; bam[o + 3] = bits[2];
    }
    for (int i = 0; i < 16; ++i) bam[0x90 + i] = 0xA0;          // disk name (A0 padded)
    const char* nm = "TEST DISK"; for (int i = 0; nm[i]; ++i) bam[0x90 + i] = (uint8_t)nm[i];
    bam[0xA0] = 0xA0; bam[0xA1] = 0xA0;
    bam[0xA2] = '0'; bam[0xA3] = '0';                          // disk ID
    bam[0xA4] = 0xA0;
    bam[0xA5] = '2'; bam[0xA6] = 'A';                          // DOS type
    bam[0xA7] = 0xA0; bam[0xA8] = 0xA0; bam[0xA9] = 0xA0; bam[0xAA] = 0xA0;
    uint8_t* dir = img.data() + soff(18, 1);
    dir[0] = 0x00; dir[1] = 0xFF;                              // empty dir sector
    return img;
}

// Build a minimally-valid blank 70-track 1571 image (.d71): side-0 BAM in
// 18/0 with the double-sided flag, side-1 bitmaps in 53/0, side-1 free counts
// at $DD in 18/0, empty directory at 18/1.
static std::vector<uint8_t> make_blank_d71() {
    std::vector<uint8_t> img(349696, 0);
    uint8_t* bam = img.data() + soff(18, 0);
    bam[0] = 18; bam[1] = 1; bam[2] = 0x41; bam[3] = 0x80;   // dir link, DOS 'A', DOUBLE-SIDED
    for (int t = 1; t <= 35; ++t) {
        const int n = spt(t);
        uint8_t bits[3] = { 0,0,0 };
        for (int s = 0; s < n; ++s) bits[s >> 3] |= (uint8_t)(1u << (s & 7));
        int freec = n;
        if (t == 18) { bits[0] &= (uint8_t)~0x03; freec = n - 2; }
        const int o = 4 + (t - 1) * 4;
        bam[o + 0] = (uint8_t)freec; bam[o + 1] = bits[0]; bam[o + 2] = bits[1]; bam[o + 3] = bits[2];
    }
    // Side 1: bitmaps in 53/0, counts at $DD in 18/0. Only 53/0 is reserved.
    uint8_t* bam2 = img.data() + soff(53, 0);
    for (int t = 36; t <= 70; ++t) {
        const int n = spt(t);
        uint8_t bits[3] = { 0,0,0 };
        for (int s = 0; s < n; ++s) bits[s >> 3] |= (uint8_t)(1u << (s & 7));
        int freec = n;
        if (t == 53) { bits[0] &= (uint8_t)~0x01; freec = n - 1; }
        bam[0xDD + (t - 36)] = (uint8_t)freec;
        const int b = 3 * (t - 36);
        bam2[b + 0] = bits[0]; bam2[b + 1] = bits[1]; bam2[b + 2] = bits[2];
    }
    for (int i = 0; i < 16; ++i) bam[0x90 + i] = 0xA0;
    const char* nm = "D71 DISK"; for (int i = 0; nm[i]; ++i) bam[0x90 + i] = (uint8_t)nm[i];
    bam[0xA0] = 0xA0; bam[0xA1] = 0xA0;
    bam[0xA2] = '7'; bam[0xA3] = '1';
    bam[0xA4] = 0xA0;
    bam[0xA5] = '2'; bam[0xA6] = 'A';
    bam[0xA7] = 0xA0; bam[0xA8] = 0xA0; bam[0xA9] = 0xA0; bam[0xAA] = 0xA0;
    uint8_t* dir = img.data() + soff(18, 1);
    dir[0] = 0x00; dir[1] = 0xFF;
    return img;
}

static bool write_file(const std::string& p, const std::vector<uint8_t>& b) {
    std::ofstream f(p, std::ios::binary);
    if (!f) return false;
    f.write((const char*)b.data(), (std::streamsize)b.size());
    return (bool)f;
}
static std::vector<uint8_t> read_file(const std::string& p) {
    std::ifstream f(p, std::ios::binary);
    return std::vector<uint8_t>((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
}

// Walk a file's T/S chain to the final sector and return its byte[1].
static int last_sector_byte1(PetIEEE& e, int st, int ss) {
    uint8_t sec[256];
    int t = st, s = ss;
    for (int g = 0; g < 10000 && t != 0; ++g) {
        if (!PetIEEE_TestAccess::read_sector(e, t, s, sec)) return -1;
        const int nt = sec[0], ns = sec[1];
        if (nt == 0) return ns;
        t = nt; s = ns;
    }
    return -1;
}

// ---------------------------------------------------------------------------
// #2: write the CBM "offset" convention and round-trip exactly.
// ---------------------------------------------------------------------------
static void test_lastsector_convention_and_roundtrip() {
    struct Case { const char* name; size_t len; int expectByte1; };
    const Case cases[] = {
        { "FULL",  254, 0xFF },   // exactly one full sector  -> offset 255
        { "PART",  100, 101  },   // partial single sector    -> offset 101
        { "MULTI", 300, 47   },   // 254 + 46; final data 46  -> offset 47
    };

    for (const auto& c : cases) {
        const std::string path = std::string("d64_test_") + c.name + ".d64";
        CHECK(write_file(path, make_blank_d64()));

        PetIEEE e;
        CHECK(e.setD64Image(path));

        std::vector<uint8_t> orig(c.len);
        for (size_t i = 0; i < c.len; ++i) orig[i] = (uint8_t)((i * 7 + 3) & 0xFF);

        CHECK(PetIEEE_TestAccess::save_prg(e, c.name, orig));

        // Flush correctness: on-disk image equals the in-memory image.
        // (Find the file's start T/S via the parsed directory.)
        D64Catalog cat{};
        CHECK(PetIEEE_TestAccess::parse_bam(e, cat));
        CHECK(PetIEEE_TestAccess::parse_dir(e, cat));
        CHECK(cat.entries.size() == 1);
        if (cat.entries.empty()) continue;

        const int b1 = last_sector_byte1(e, cat.entries[0].startT, cat.entries[0].startS);
        CHECK_EQ(b1, c.expectByte1);

        // Round-trip: load_prg returns addr (first 2 bytes) + payload (rest).
        std::vector<uint8_t> payload; uint16_t addr = 0;
        CHECK(PetIEEE_TestAccess::load_prg(e, c.name, payload, addr));
        std::vector<uint8_t> got;
        got.push_back((uint8_t)(addr & 0xFF));
        got.push_back((uint8_t)(addr >> 8));
        got.insert(got.end(), payload.begin(), payload.end());
        CHECK_EQ(got.size(), c.len);
        CHECK(got == orig);
    }
}

// ---------------------------------------------------------------------------
// #1: allocator covers every sector of an 18-sector track (25..30).
//     The old (offset*interleave)%18 walk reached only the 9 even sectors.
// ---------------------------------------------------------------------------
static void test_alloc_covers_18sector_track() {
    const std::string path = "d64_test_alloc.d64";
    CHECK(write_file(path, make_blank_d64()));

    PetIEEE e;
    CHECK(e.setD64Image(path));

    D64Catalog cat{};
    CHECK(PetIEEE_TestAccess::parse_bam(e, cat));
    CHECK(PetIEEE_TestAccess::parse_dir(e, cat));

    std::set<int> sectors;
    int onTrack25 = 0;
    bool anyOdd = false;
    for (int i = 0; i < 18; ++i) {
        D64Block b{};
        const bool ok = PetIEEE_TestAccess::alloc_block(e, cat, /*pref*/25, /*interleave*/10, b);
        CHECK(ok);
        if (ok && b.track == 25) { ++onTrack25; sectors.insert(b.sector); if (b.sector & 1) anyOdd = true; }
    }
    CHECK_EQ(onTrack25, 18);             // all 18 sectors allocatable on track 25
    CHECK_EQ((int)sectors.size(), 18);   // and they are 18 distinct sectors (0..17)
    CHECK(anyOdd);                        // odd sectors were unreachable before the fix
}

// ---------------------------------------------------------------------------
// Tier 1: SAVE filename normalization (strip @, @0:, 0:, quotes, mode tokens).
// ---------------------------------------------------------------------------
static void test_save_name_normalization() {
    CHECK(PetIEEE_TestAccess::norm_prg("GAME") == "GAME");
    CHECK(PetIEEE_TestAccess::norm_prg("0:GAME") == "GAME");
    CHECK(PetIEEE_TestAccess::norm_prg("1:GAME") == "GAME");
    CHECK(PetIEEE_TestAccess::norm_prg("@:GAME") == "GAME");       // save-with-replace, default drive
    CHECK(PetIEEE_TestAccess::norm_prg("@0:GAME") == "GAME");      // save-with-replace, drive 0
    CHECK(PetIEEE_TestAccess::norm_prg("@GAME") == "GAME");        // bare @
    CHECK(PetIEEE_TestAccess::norm_prg("\"GAME\"") == "GAME");     // quotes
    CHECK(PetIEEE_TestAccess::norm_prg("GAME,P,W") == "GAME");     // mode tokens
    CHECK(PetIEEE_TestAccess::norm_prg("game") == "GAME");         // upper-cased
    CHECK(PetIEEE_TestAccess::norm_prg("$") == "$");               // directory sentinel preserved
}

static bool bytes_contain(const std::vector<uint8_t>& v, const char* s) {
    const std::string hay((const char*)v.data(), v.size());
    return hay.find(s) != std::string::npos;
}
static int parse_blocks_free(const std::vector<uint8_t>& v) {
    const std::string hay((const char*)v.data(), v.size());
    const size_t p = hay.find("BLOCKS FREE");
    if (p == std::string::npos) return -1;
    size_t i = p;
    while (i > 0 && hay[i - 1] == ' ') --i;           // skip the space before "BLOCKS"
    size_t end = i;
    while (i > 0 && hay[i - 1] >= '0' && hay[i - 1] <= '9') --i;
    if (i == end) return -1;
    return std::atoi(hay.substr(i, end - i).c_str());
}

// ---------------------------------------------------------------------------
// Tier 1: LOAD wildcards return the first matching file.
// ---------------------------------------------------------------------------
static void test_load_wildcard() {
    const std::string path = "d64_test_wild.d64";
    CHECK(write_file(path, make_blank_d64()));
    PetIEEE e;
    CHECK(e.setD64Image(path));

    std::vector<uint8_t> alpha(50, 0xAA), beta(60, 0xBB);
    CHECK(PetIEEE_TestAccess::save_prg(e, "ALPHA", alpha));
    CHECK(PetIEEE_TestAccess::save_prg(e, "BETA", beta));

    std::vector<uint8_t> pay; uint16_t addr = 0;
    CHECK(PetIEEE_TestAccess::load_prg(e, "AL*", pay, addr));   // -> ALPHA
    std::vector<uint8_t> got; got.push_back(addr & 0xFF); got.push_back(addr >> 8);
    got.insert(got.end(), pay.begin(), pay.end());
    CHECK(got == alpha);

    pay.clear();
    CHECK(PetIEEE_TestAccess::load_prg(e, "B*", pay, addr));    // -> BETA
    got.clear(); got.push_back(addr & 0xFF); got.push_back(addr >> 8);
    got.insert(got.end(), pay.begin(), pay.end());
    CHECK(got == beta);

    pay.clear();
    CHECK(!PetIEEE_TestAccess::load_prg(e, "Z*", pay, addr));   // no match
}

// ---------------------------------------------------------------------------
// Tier 1: directory "$:pattern" filter + BLOCKS FREE excludes track 18.
// ---------------------------------------------------------------------------
static void test_dir_filter_and_blocks_free() {
    // Blank disk: a real 1541 reports 664 blocks free (683 total - 19 on track 18).
    const std::string blank = "d64_test_dir.d64";
    CHECK(write_file(blank, make_blank_d64()));
    PetIEEE e;
    CHECK(e.setD64Image(blank));

    std::vector<uint8_t> out;
    CHECK(PetIEEE_TestAccess::build_dir(e, out, ""));
    CHECK_EQ(parse_blocks_free(out), 664);

    // Add two files; "$:A*" lists only ALPHA.
    std::vector<uint8_t> a(20, 0x11), b(20, 0x22);
    CHECK(PetIEEE_TestAccess::save_prg(e, "ALPHA", a));
    CHECK(PetIEEE_TestAccess::save_prg(e, "BETA", b));

    out.clear();
    CHECK(PetIEEE_TestAccess::build_dir(e, out, ""));
    CHECK(bytes_contain(out, "ALPHA"));
    CHECK(bytes_contain(out, "BETA"));

    out.clear();
    CHECK(PetIEEE_TestAccess::build_dir(e, out, "A*"));
    CHECK(bytes_contain(out, "ALPHA"));
    CHECK(!bytes_contain(out, "BETA"));
}

// helper: load a PRG and rebuild the full byte stream (addr + payload).
static bool load_full(PetIEEE& e, const std::string& name, std::vector<uint8_t>& full) {
    std::vector<uint8_t> pay; uint16_t addr = 0;
    if (!PetIEEE_TestAccess::load_prg(e, name, pay, addr)) return false;
    full.clear(); full.push_back(addr & 0xFF); full.push_back(addr >> 8);
    full.insert(full.end(), pay.begin(), pay.end());
    return true;
}

// ---------------------------------------------------------------------------
// CMD15 RENAME (R:new=old)
// ---------------------------------------------------------------------------
static void test_cmd_rename() {
    const std::string path = "d64_test_ren.d64";
    CHECK(write_file(path, make_blank_d64()));
    PetIEEE e; CHECK(e.setD64Image(path));

    std::vector<uint8_t> data(120); for (size_t i = 0; i < data.size(); ++i) data[i] = (uint8_t)(i ^ 0x5A);
    CHECK(PetIEEE_TestAccess::save_prg(e, "OLD", data));

    CHECK(PetIEEE_TestAccess::cmd(e, "R:NEW=OLD"));
    CHECK_EQ(status_code(e), 0);
    std::vector<uint8_t> full;
    CHECK(load_full(e, "NEW", full));  CHECK(full == data);   // NEW has the content
    std::vector<uint8_t> pay; uint16_t a;
    CHECK(!PetIEEE_TestAccess::load_prg(e, "OLD", pay, a));   // OLD is gone

    CHECK(!PetIEEE_TestAccess::cmd(e, "R:NEW=GHOST"));        // source missing
    CHECK_EQ(status_code(e), 62);

    CHECK(PetIEEE_TestAccess::save_prg(e, "OTHER", data));
    CHECK(!PetIEEE_TestAccess::cmd(e, "R:NEW=OTHER"));        // target exists
    CHECK_EQ(status_code(e), 63);
}

// ---------------------------------------------------------------------------
// CMD15 COPY (C:new=old)
// ---------------------------------------------------------------------------
static void test_cmd_copy() {
    const std::string path = "d64_test_cpy.d64";
    CHECK(write_file(path, make_blank_d64()));
    PetIEEE e; CHECK(e.setD64Image(path));

    std::vector<uint8_t> src(300); for (size_t i = 0; i < src.size(); ++i) src[i] = (uint8_t)(i * 3 + 1);
    CHECK(PetIEEE_TestAccess::save_prg(e, "SRC", src));

    CHECK(PetIEEE_TestAccess::cmd(e, "C:DST=SRC"));
    CHECK_EQ(status_code(e), 0);
    std::vector<uint8_t> a, b;
    CHECK(load_full(e, "DST", a)); CHECK(a == src);   // copy matches
    CHECK(load_full(e, "SRC", b)); CHECK(b == src);   // original intact

    CHECK(!PetIEEE_TestAccess::cmd(e, "C:DST2=GHOST")); // source missing
    CHECK_EQ(status_code(e), 62);
    CHECK(!PetIEEE_TestAccess::cmd(e, "C:DST=SRC"));    // target exists
    CHECK_EQ(status_code(e), 63);
}

// ---------------------------------------------------------------------------
// CMD15 NEW (N:name,id) - wipes the disk, sets name/id, 664 blocks free.
// ---------------------------------------------------------------------------
static void test_cmd_new() {
    const std::string path = "d64_test_new.d64";
    CHECK(write_file(path, make_blank_d64()));
    PetIEEE e; CHECK(e.setD64Image(path));

    std::vector<uint8_t> d(50, 0x33);
    CHECK(PetIEEE_TestAccess::save_prg(e, "FOO", d));

    CHECK(PetIEEE_TestAccess::cmd(e, "N:MYDISK,AA"));
    CHECK_EQ(status_code(e), 0);

    std::vector<uint8_t> out;
    CHECK(PetIEEE_TestAccess::build_dir(e, out, ""));
    CHECK(bytes_contain(out, "MYDISK"));    // new disk name
    CHECK(!bytes_contain(out, "FOO"));      // file wiped
    CHECK_EQ(parse_blocks_free(out), 664);  // fresh disk
}

// ---------------------------------------------------------------------------
// CMD15 VALIDATE (V) - reclaim an orphaned (used-but-unreferenced) block.
// ---------------------------------------------------------------------------
static void test_cmd_validate() {
    const std::string path = "d64_test_val.d64";
    CHECK(write_file(path, make_blank_d64()));
    PetIEEE e; CHECK(e.setD64Image(path));

    std::vector<uint8_t> d(100, 0x77);
    CHECK(PetIEEE_TestAccess::save_prg(e, "FOO", d));

    // BAM bit helpers (1=free).
    auto bam_free = [](const uint8_t* bam, int track, int sector) -> bool {
        const int off = 4 + (track - 1) * 4;
        return (bam[off + 1 + (sector >> 3)] >> (sector & 7)) & 1;
    };

    // Corrupt: mark track 1 / sector 0 USED even though no file references it.
    uint8_t bam[256];
    CHECK(PetIEEE_TestAccess::read_sector(e, 18, 0, bam));
    CHECK(bam_free(bam, 1, 0));                 // free before
    bam[4 + 0 * 4 + 1] &= (uint8_t)~0x01;       // clear track1/sector0 bit -> used
    bam[4 + 0 * 4 + 0]--;                       // and its free count
    CHECK(PetIEEE_TestAccess::write_sector(e, 18, 0, bam));

    CHECK(PetIEEE_TestAccess::cmd(e, "V"));
    CHECK_EQ(status_code(e), 0);

    // After validate: orphan reclaimed; FOO's first block still used.
    CHECK(PetIEEE_TestAccess::read_sector(e, 18, 0, bam));
    CHECK(bam_free(bam, 1, 0));                 // reclaimed

    D64Catalog cat{};
    CHECK(PetIEEE_TestAccess::parse_bam(e, cat));
    CHECK(PetIEEE_TestAccess::parse_dir(e, cat));
    CHECK(cat.entries.size() == 1);
    if (!cat.entries.empty())
        CHECK(!bam_free(bam, cat.entries[0].startT, cat.entries[0].startS)); // FOO block used
}

// ---------------------------------------------------------------------------
// Regression: a machine reset must NOT eject the virtual drive. On real
// hardware, resetting the PET does not disconnect device 8 or eject the disk.
// reset() used to clear hostRoot/d64Path/d64, so the IEEE virtual drive went
// dead after the first reset (LOAD"$",8 -> FILE NOT FOUND with hostRoot="").
// ---------------------------------------------------------------------------
static void test_reset_keeps_mounts() {
    // (1) Host-folder backend must survive a machine reset.
    PetIEEE e1;
    e1.setHostRoot("./files");
    e1.reset();
    CHECK(e1.getHostRoot() == "./files");           // was "" before the fix

    // (2) A mounted D64 image must survive a reset (the disk stays in the
    //     drive): save a PRG, reset, then load it back from the still-mounted
    //     image. The load only succeeds if the in-memory image persists.
    const std::string path = "d64_test_reset.d64";
    CHECK(write_file(path, make_blank_d64()));
    PetIEEE e2;
    CHECK(e2.setD64Image(path));

    std::vector<uint8_t> blob(120);
    for (size_t i = 0; i < blob.size(); ++i) blob[i] = (uint8_t)((i * 5 + 1) & 0xFF);
    CHECK(PetIEEE_TestAccess::save_prg(e2, "AFTERRST", blob));

    e2.reset();                                      // before the fix: ejects the image

    CHECK(!e2.getD64Image().empty());                // path retained
    std::vector<uint8_t> payload; uint16_t addr = 0;
    CHECK(PetIEEE_TestAccess::load_prg(e2, "AFTERRST", payload, addr));  // bytes retained
    // load_prg splits the PRG into addr (first 2 bytes) + payload (rest).
    std::vector<uint8_t> got;
    got.push_back((uint8_t)(addr & 0xFF));
    got.push_back((uint8_t)(addr >> 8));
    got.insert(got.end(), payload.begin(), payload.end());
    CHECK(got == blob);
}

// ---------------------------------------------------------------------------
// Eject: clearing the mounted D64 (setD64Image("")) unmounts it and leaves the
// host-folder vdrive in place, so device 8 falls back to ./files.
// ---------------------------------------------------------------------------
static void test_eject_returns_to_vdrive() {
    const std::string path = "d64_test_eject.d64";
    CHECK(write_file(path, make_blank_d64()));

    PetIEEE e;
    e.setHostRoot("./files");
    CHECK(!e.isD64Mounted());                 // nothing mounted yet

    CHECK(e.setD64Image(path));
    CHECK(e.isD64Mounted());                  // disk in

    CHECK(e.setD64Image(""));                  // eject
    CHECK(!e.isD64Mounted());                 // disk out
    CHECK(e.getD64Image().empty());
    CHECK(e.getHostRoot() == "./files");       // vdrive still mounted
}

// Locks the exact D64 directory bytes so the shared-formatter refactor cannot
// change the (correct) D64 listing. Deterministic disk: blank + ALPHA + BETA.
static void test_dir_d64_golden() {
    static const uint8_t expected[] = {
        0x01,0x04,0x24,0x04,0x00,0x00,0x12,0x22,0x54,0x45,0x53,0x54,
        0x20,0x44,0x49,0x53,
        0x4b,0x20,0x20,0x20,0x20,0x20,0x20,0x20,0x22,0x20,0x30,0x30,
        0x20,0x32,0x41,0x92,
        0x14,0x14,0x14,0x14,0x00,0x4d,0x04,0x0a,0x00,0x14,0x14,0x14,
        0x14,0x14,0x14,0x20,
        0x20,0x20,0x20,0x31,0x20,0x22,0x41,0x4c,0x50,0x48,0x41,0x22,
        0x20,0x20,0x20,0x20,
        0x20,0x20,0x20,0x20,0x20,0x20,0x20,0x20,0x20,0x20,0x50,0x52,
        0x47,0x00,0x76,0x04,
        0x14,0x00,0x14,0x14,0x14,0x14,0x14,0x14,0x20,0x20,0x20,0x20,
        0x31,0x20,0x22,0x42,
        0x45,0x54,0x41,0x22,0x20,0x20,0x20,0x20,0x20,0x20,0x20,0x20,
        0x20,0x20,0x20,0x20,
        0x20,0x20,0x20,0x50,0x52,0x47,0x00,0x8e,0x04,0x1e,0x00,0x14,
        0x14,0x14,0x36,0x36,
        0x32,0x20,0x42,0x4c,0x4f,0x43,0x4b,0x53,0x20,0x46,0x52,0x45,
        0x45,0x2e,0x00,0x00,
        0x00,
    };
    const std::string p = "d64_test_golden.d64";
    CHECK(write_file(p, make_blank_d64()));
    PetIEEE e; CHECK(e.setD64Image(p));
    std::vector<uint8_t> a(20, 0x11), b(20, 0x22);
    CHECK(PetIEEE_TestAccess::save_prg(e, "ALPHA", a));
    CHECK(PetIEEE_TestAccess::save_prg(e, "BETA", b));
    std::vector<uint8_t> out;
    CHECK(PetIEEE_TestAccess::build_dir(e, out, ""));
    CHECK_EQ(out.size(), sizeof(expected));
    bool same = (out.size() == sizeof(expected));
    for (size_t i = 0; same && i < out.size(); ++i) if (out[i] != expected[i]) same = false;
    CHECK(same);
}

// The folder vdrive listing now uses the same formatter as the D64: reverse-video
// header, block counts, tight-quoted names without the .PRG suffix, BLOCKS FREE.
static void test_dir_vdrive_format() {
    const std::string dir = "vdrive_test_dir";
    std::error_code ec; std::filesystem::create_directories(dir, ec);
    CHECK(write_file(dir + "/GAMMA.prg", std::vector<uint8_t>(500, 0x33)));   // ~2 blocks
    CHECK(write_file(dir + "/DELTA.prg", std::vector<uint8_t>(20, 0x44)));    // 1 block

    PetIEEE e;
    e.setHostRoot(dir);
    std::vector<uint8_t> out;
    CHECK(PetIEEE_TestAccess::build_dir_folder(e, out, ""));

    // Has the reverse-video header marker ($12), the VDRIVE label, tight-quoted
    // names with the .PRG dropped, and the BLOCKS FREE. footer.
    bool hasRvs = false; for (uint8_t b : out) if (b == 0x12) { hasRvs = true; break; }
    CHECK(hasRvs);
    CHECK(bytes_contain(out, "VDRIVE"));
    CHECK(bytes_contain(out, "\"GAMMA\""));
    CHECK(bytes_contain(out, "\"DELTA\""));
    CHECK(!bytes_contain(out, ".PRG"));
    CHECK(bytes_contain(out, "BLOCKS FREE."));

    // Wildcard filter applies to the displayed (stripped) name.
    out.clear();
    CHECK(PetIEEE_TestAccess::build_dir_folder(e, out, "G*"));
    CHECK(bytes_contain(out, "GAMMA"));
    CHECK(!bytes_contain(out, "DELTA"));

    std::filesystem::remove_all(dir, ec);
}

// ---------------------------------------------------------------------------
// DOS block commands (U1/U2/B-P) on a '#' direct-access channel - the API the
// Zork interpreters use instead of named files.
// ---------------------------------------------------------------------------
static void test_block_commands() {
    const std::string path = "d64_test_BLOCK.d64";
    CHECK(write_file(path, make_blank_d64()));
    PetIEEE e; CHECK(e.setD64Image(path));

    // U1: read the BAM sector (T18/S0) into channel 8's buffer.
    PetIEEE_TestAccess::open_da(e, 8);
    CHECK(PetIEEE_TestAccess::cmd(e, "U1:8,0,18,00"));
    CHECK(status_code(e) == 0);
    auto& rd = PetIEEE_TestAccess::stream_data(e, 8);
    CHECK(rd.size() == 256);
    CHECK(rd[0] == 18 && rd[1] == 1 && rd[2] == 0x41);       // dir link + 'A'
    CHECK(std::memcmp(PetIEEE_TestAccess::da_buf(e, 8), rd.data(), 256) == 0);

    // U1 without an explicit OPEN "#": channel arms itself (permissive).
    CHECK(PetIEEE_TestAccess::cmd(e, "U1:5,0,1,0"));
    CHECK(status_code(e) == 0);
    CHECK(PetIEEE_TestAccess::da_active(e, 5));
    CHECK(PetIEEE_TestAccess::stream_data(e, 5).size() == 256);

    // Free-form separators (space-separated variant).
    CHECK(PetIEEE_TestAccess::cmd(e, "U1: 8 0 18 0"));
    CHECK(status_code(e) == 0);

    // B-P: repositions the read cursor within the armed sector.
    CHECK(PetIEEE_TestAccess::cmd(e, "B-P:8,4"));
    CHECK(status_code(e) == 0);
    CHECK(PetIEEE_TestAccess::stream_index(e, 8) == 4);
    CHECK(PetIEEE_TestAccess::da_ptr(e, 8) == 4);

    // U2: modify the buffer and write it to a scratch sector, then verify
    // the image (and the flushed file) contain the change.
    uint8_t* buf = PetIEEE_TestAccess::da_buf(e, 8);
    buf[0] = 0x00; buf[1] = 0xFF; buf[5] = 0xA5;
    CHECK(PetIEEE_TestAccess::cmd(e, "U2:8,0,1,3"));
    CHECK(status_code(e) == 0);
    uint8_t sec[256];
    CHECK(PetIEEE_TestAccess::read_sector(e, 1, 3, sec));
    CHECK(sec[1] == 0xFF && sec[5] == 0xA5);

    // Bad geometry -> 66, ILLEGAL TRACK AND SECTOR.
    CHECK(!PetIEEE_TestAccess::cmd(e, "U1:8,0,36,0"));
    CHECK(status_code(e) == 66);
    CHECK(!PetIEEE_TestAccess::cmd(e, "U1:8,0,18,21"));      // T18 has 19 sectors
    CHECK(status_code(e) == 66);

    // UI: DOS soft reset -> 73 power-on message.
    CHECK(PetIEEE_TestAccess::cmd(e, "UI"));
    CHECK(status_code(e) == 73);

    // Unimplemented B- subcommand stays a syntax error.
    CHECK(!PetIEEE_TestAccess::cmd(e, "B-R:8,0,1,0"));

    std::error_code ec; std::filesystem::remove(path, ec);
}

// ---------------------------------------------------------------------------
// Tier-1 DOS additions: typed OPEN-for-read (PRG/USR as data, wildcards) and
// B-A/B-F block allocate/free.
// ---------------------------------------------------------------------------
static void test_typed_open_and_block_alloc() {
    const std::string path = "d64_test_TIER1.d64";
    CHECK(write_file(path, make_blank_d64()));
    PetIEEE e; CHECK(e.setD64Image(path));

    // One file of each data type.
    std::vector<uint8_t> prg = { 0x01, 0x04, 0xAA, 0xBB, 0xCC };   // load addr + body
    std::vector<uint8_t> seq = { 'H', 'I', 0x0D };
    std::vector<uint8_t> usr = { 0x55, 0x66 };
    CHECK(PetIEEE_TestAccess::save_typed(e, "OVERLAY", prg, 0x82)); // PRG
    CHECK(PetIEEE_TestAccess::save_typed(e, "NOTES",   seq, 0x81)); // SEQ
    CHECK(PetIEEE_TestAccess::save_typed(e, "BLOB",    usr, 0x83)); // USR

    // PRG readable as a data file; stream includes the 2-byte load address.
    CHECK(PetIEEE_TestAccess::open_read(e, "OVERLAY", 2, 2));
    CHECK(PetIEEE_TestAccess::stream_data(e, 2) == prg);
    // ...and with no type filter (OPEN without suffix).
    CHECK(PetIEEE_TestAccess::open_read(e, "OVERLAY", 2, 0));
    // ...but NOT when the caller demanded SEQ.
    CHECK(!PetIEEE_TestAccess::open_read(e, "OVERLAY", 2, 1));

    // USR readable (explicit and untyped).
    CHECK(PetIEEE_TestAccess::open_read(e, "BLOB", 3, 3));
    CHECK(PetIEEE_TestAccess::stream_data(e, 3) == usr);
    CHECK(PetIEEE_TestAccess::open_read(e, "BLOB", 3, 0));

    // SEQ still works, and wildcards match like the LOAD path.
    CHECK(PetIEEE_TestAccess::open_read(e, "NOTES", 4, 1));
    CHECK(PetIEEE_TestAccess::stream_data(e, 4) == seq);
    CHECK(PetIEEE_TestAccess::open_read(e, "OVER*", 5, 0));
    CHECK(PetIEEE_TestAccess::stream_data(e, 5) == prg);
    CHECK(PetIEEE_TestAccess::open_read(e, "N?TES", 6, 0));
    CHECK(!PetIEEE_TestAccess::open_read(e, "ZORK*", 7, 0));

    // B-A: allocate a free block, then re-allocating answers 65,NO BLOCK
    // with the next free block as the hint. B-F releases it again.
    CHECK(PetIEEE_TestAccess::bam_free(e, 30, 5));
    CHECK(PetIEEE_TestAccess::cmd(e, "B-A:0,30,5"));
    CHECK(status_code(e) == 0);
    CHECK(!PetIEEE_TestAccess::bam_free(e, 30, 5));
    CHECK(!PetIEEE_TestAccess::cmd(e, "B-A:0,30,5"));
    CHECK(status_code(e) == 65);
    CHECK(PetIEEE_TestAccess::status(e).find("65,NO BLOCK,30,06") != std::string::npos);
    CHECK(PetIEEE_TestAccess::cmd(e, "B-F:0,30,5"));
    CHECK(status_code(e) == 0);
    CHECK(PetIEEE_TestAccess::bam_free(e, 30, 5));

    // Geometry validation matches U1/U2.
    CHECK(!PetIEEE_TestAccess::cmd(e, "B-A:0,36,0"));
    CHECK(status_code(e) == 66);

    std::error_code ec; std::filesystem::remove(path, ec);
}

// ---------------------------------------------------------------------------
// 1571 / .d71 double-sided read-write: geometry, split BAM, block commands and
// allocation across side 1 (tracks 36-70), and a SEQ save/reload round trip.
// ---------------------------------------------------------------------------
static void test_d71_readwrite() {
    const std::string path = "d64_test_D71.d71";
    CHECK(write_file(path, make_blank_d71()));
    PetIEEE e; CHECK(e.setD64Image(path));

    // Blank .d71 reports 1328 blocks free (both sides, tracks 18 and 53 excluded).
    std::vector<uint8_t> dir;
    CHECK(PetIEEE_TestAccess::build_dir(e, dir, ""));
    CHECK(bytes_contain(dir, "1328 BLOCKS FREE"));

    // Block read/write on a side-1 data track (T40) round-trips through flush.
    PetIEEE_TestAccess::open_da(e, 8);
    CHECK(PetIEEE_TestAccess::cmd(e, "U1:8,0,40,5"));
    CHECK(status_code(e) == 0);
    uint8_t* buf = PetIEEE_TestAccess::da_buf(e, 8);
    buf[0] = 0xDE; buf[10] = 0xAD; buf[255] = 0xEF;
    CHECK(PetIEEE_TestAccess::cmd(e, "U2:8,0,40,5"));
    CHECK(status_code(e) == 0);
    uint8_t sec[256];
    CHECK(PetIEEE_TestAccess::read_sector(e, 40, 5, sec));
    CHECK(sec[0] == 0xDE && sec[10] == 0xAD && sec[255] == 0xEF);
    // ...and it persisted to the file on disk (split-BAM images stay writable).
    { auto raw = read_file(path); CHECK(raw.size() == 349696);
      CHECK(raw[soff(40, 5) + 0] == 0xDE && raw[soff(40, 5) + 10] == 0xAD); }

    // Highest track/sector is legal; one past it is not.
    CHECK(PetIEEE_TestAccess::cmd(e, "U1:8,0,70,16"));   // T70 has 17 sectors (0..16)
    CHECK(status_code(e) == 0);
    CHECK(!PetIEEE_TestAccess::cmd(e, "U1:8,0,70,17"));
    CHECK(status_code(e) == 66);
    CHECK(!PetIEEE_TestAccess::cmd(e, "U1:8,0,71,0"));    // no track 71
    CHECK(status_code(e) == 66);

    // B-A / B-F on a side-1 block update the split BAM (bitmap in 53/0, count
    // in 18/0) and survive a reparse.
    CHECK(PetIEEE_TestAccess::bam_free(e, 60, 3));
    CHECK(PetIEEE_TestAccess::cmd(e, "B-A:0,60,3"));
    CHECK(status_code(e) == 0);
    CHECK(!PetIEEE_TestAccess::bam_free(e, 60, 3));
    CHECK(PetIEEE_TestAccess::cmd(e, "B-F:0,60,3"));
    CHECK(PetIEEE_TestAccess::bam_free(e, 60, 3));

    // SEQ save + reload round trip (exercises the full write path + split-BAM
    // commit on a 1571 image).
    std::vector<uint8_t> payload(600);
    for (size_t i = 0; i < payload.size(); ++i) payload[i] = (uint8_t)((i * 13 + 7) & 0xFF);
    CHECK(PetIEEE_TestAccess::save_typed(e, "LOGFILE", payload, 0x81));
    CHECK(PetIEEE_TestAccess::open_read(e, "LOGFILE", 2, 1));
    CHECK(PetIEEE_TestAccess::stream_data(e, 2) == payload);

    // Free count drops by the file's block usage after the save.
    std::vector<uint8_t> dir2;
    CHECK(PetIEEE_TestAccess::build_dir(e, dir2, ""));
    CHECK(bytes_contain(dir2, "\"LOGFILE\""));
    CHECK(!bytes_contain(dir2, "1328 BLOCKS FREE"));   // fewer than blank now

    std::error_code ec; std::filesystem::remove(path, ec);
}

// ---------------------------------------------------------------------------
// OPEN-for-write parameter parsing: the shorthand "NAME,W" (write mode, type
// defaulting to SEQ) must arm the writer, not fall through to a failing read.
// This was the save/restore bug. Also covers ",S,W", ",P,W", and ",A" append.
// ---------------------------------------------------------------------------
static std::vector<uint8_t> vbytes(const char* s) {
    return std::vector<uint8_t>((const uint8_t*)s, (const uint8_t*)s + std::strlen(s));
}
static void test_open_write_modes() {
    const std::string path = "d64_test_WRITE.d64";
    CHECK(write_file(path, make_blank_d64()));
    PetIEEE e; CHECK(e.setD64Image(path));

    // The reported bug: OPEN 3,8,3,"SAVENAME,W" (mode only, no ",S").
    PetIEEE_TestAccess::open_ch(e, 3, "SAVENAME,W");
    PetIEEE_TestAccess::put_data(e, 3, vbytes("HELLO SAVE"));
    PetIEEE_TestAccess::close_ch(e, 3);                   // commits (write was armed)
    // Restore: OPEN 3,8,3,"SAVENAME" reads it back as SEQ.
    CHECK(PetIEEE_TestAccess::open_read(e, "SAVENAME", 4, 0));
    CHECK(PetIEEE_TestAccess::stream_data(e, 4) == vbytes("HELLO SAVE"));

    // Explicit ",S,W" still works and stores type SEQ.
    PetIEEE_TestAccess::open_ch(e, 5, "NOTES,S,W");
    PetIEEE_TestAccess::put_data(e, 5, vbytes("SEQDATA"));
    PetIEEE_TestAccess::close_ch(e, 5);
    CHECK(PetIEEE_TestAccess::open_read(e, "NOTES", 6, 1)); // typeFilter 1 = SEQ
    CHECK(PetIEEE_TestAccess::stream_data(e, 6) == vbytes("SEQDATA"));

    // ",P,W" stores a PRG the read path only surfaces under the PRG filter.
    PetIEEE_TestAccess::open_ch(e, 7, "PROG,P,W");
    PetIEEE_TestAccess::put_data(e, 7, vbytes("\x01\x08PRGBODY"));
    PetIEEE_TestAccess::close_ch(e, 7);
    CHECK(PetIEEE_TestAccess::open_read(e, "PROG", 8, 2));  // PRG
    CHECK(!PetIEEE_TestAccess::open_read(e, "PROG", 8, 1)); // not SEQ

    // ",A" append extends an existing SEQ file rather than overwriting it.
    PetIEEE_TestAccess::open_ch(e, 9, "SAVENAME,A");
    PetIEEE_TestAccess::put_data(e, 9, vbytes("+MORE"));
    PetIEEE_TestAccess::close_ch(e, 9);
    CHECK(PetIEEE_TestAccess::open_read(e, "SAVENAME", 10, 0));
    CHECK(PetIEEE_TestAccess::stream_data(e, 10) == vbytes("HELLO SAVE+MORE"));

    // A plain read open ",R" (and no-mode) must NOT be treated as a write.
    CHECK(PetIEEE_TestAccess::open_read(e, "SAVENAME", 11, 0)); // still exists, unchanged
    CHECK(PetIEEE_TestAccess::stream_data(e, 11) == vbytes("HELLO SAVE+MORE"));

    std::error_code ec; std::filesystem::remove(path, ec);
}

// ---------------------------------------------------------------------------
// The five review fixes: @-replace SEQ names, REL rejection, replace reclaims
// old blocks (near-full disk), error-channel reset, B-A hint skips DOS tracks.
// ---------------------------------------------------------------------------
static std::vector<uint8_t> payload(size_t n) {
    std::vector<uint8_t> v(n);
    for (size_t i = 0; i < n; ++i) v[i] = (uint8_t)((i * 7 + 1) & 0xFF);
    return v;
}
static void test_review_fixes() {
    const std::string path = "d64_test_REVIEW.d64";
    CHECK(write_file(path, make_blank_d64()));
    PetIEEE e; CHECK(e.setD64Image(path));

    // #1 @-replace: "@0:SCORES,S,W" replaces SCORES, not create "@0:SCORES".
    PetIEEE_TestAccess::open_ch(e, 3, "SCORES,S,W");
    PetIEEE_TestAccess::put_data(e, 3, vbytes("AAA"));
    PetIEEE_TestAccess::close_ch(e, 3);
    PetIEEE_TestAccess::open_ch(e, 3, "@0:SCORES,S,W");
    PetIEEE_TestAccess::put_data(e, 3, vbytes("BBBB"));
    PetIEEE_TestAccess::close_ch(e, 3);
    CHECK(PetIEEE_TestAccess::open_read(e, "SCORES", 4, 0));
    CHECK(PetIEEE_TestAccess::stream_data(e, 4) == vbytes("BBBB"));
    CHECK(!PetIEEE_TestAccess::open_read(e, "@0:SCORES", 5, 0)); // no garbage name
    std::vector<uint8_t> dir; CHECK(PetIEEE_TestAccess::build_dir(e, dir, "SCORES"));
    CHECK(bytes_contain(dir, "\"SCORES\""));

    // #2 REL open is cleanly rejected (62), not a misleading generic failure.
    PetIEEE_TestAccess::open_ch(e, 6, "RELDATA,L");
    CHECK(status_code(e) == 62);

    // #4 error channel resets to "00, OK" after a full read (was: empty re-read).
    CHECK(status_code(e) == 62);                 // still the REL error
    PetIEEE_TestAccess::ch15_full_read_untalk(e);
    CHECK(status_code(e) == 0);                  // cleared to OK

    std::error_code ec; std::filesystem::remove(path, ec);

    // #3 replace on a near-full disk reuses the old file's blocks. Blank .d64
    // has 664 allocatable blocks. Fill 400, then replace with 500: 500 > the
    // 264 free, but <= 264 + 400 reclaimed, so it must succeed (old code, which
    // allocated before freeing, reported DISK FULL here).
    const std::string p2 = "d64_test_FULL.d64";
    CHECK(write_file(p2, make_blank_d64()));
    PetIEEE f; CHECK(f.setD64Image(p2));
    CHECK(PetIEEE_TestAccess::save_typed(f, "BIG", payload(400 * 254), 0x81));
    CHECK(PetIEEE_TestAccess::save_typed(f, "BIG", payload(500 * 254), 0x81)); // replace
    CHECK(PetIEEE_TestAccess::open_read(f, "BIG", 2, 0));
    CHECK(PetIEEE_TestAccess::stream_data(f, 2).size() == 500u * 254);
    // A genuinely-too-big save is rejected up front (72) and leaves BIG intact.
    CHECK(!PetIEEE_TestAccess::save_typed(f, "HUGE", payload(700 * 254), 0x81));
    CHECK(status_code(f) == 72);
    CHECK(PetIEEE_TestAccess::open_read(f, "BIG", 3, 0));
    CHECK(PetIEEE_TestAccess::stream_data(f, 3).size() == 500u * 254); // uncorrupted
    std::filesystem::remove(p2, ec);

    // #5a B-A's next-free hint skips the DOS track (18). Allocate the last
    // sector of track 17, then re-allocate it: the hint must jump PAST track 18
    // to 19/0, not land on a free track-18 sector.
    const std::string p3 = "d64_test_BAHINT.d64";
    CHECK(write_file(p3, make_blank_d64()));
    PetIEEE g; CHECK(g.setD64Image(p3));
    CHECK(PetIEEE_TestAccess::cmd(g, "B-A:0,17,20"));   // T17 has 21 sectors (0..20)
    CHECK(status_code(g) == 0);
    CHECK(!PetIEEE_TestAccess::cmd(g, "B-A:0,17,20"));  // now in use -> hint
    CHECK(status_code(g) == 65);
    CHECK(PetIEEE_TestAccess::status(g).find("65,NO BLOCK,19,00") != std::string::npos);
    std::filesystem::remove(p3, ec);
}

// Regression fixtures live in a unique directory; guest paths never target user files.
struct ReviewFixture {
    std::filesystem::path dir = std::filesystem::path("d64_review_tests_" +
        std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    ReviewFixture() { std::filesystem::create_directories(dir / "root"); }
    ~ReviewFixture() { std::error_code ec; std::filesystem::remove_all(dir, ec); }
};

static void test_directory_entry_offsets() {
    ReviewFixture f;
    auto img = make_blank_d64();
    for (int i=0; i<8; ++i) {
        auto* entry = img.data() + soff(18,1) + i*32;
        entry[2] = i == 7 ? 0x84 : 0x82;
        entry[3] = 1; entry[4] = (uint8_t)i;
        memset(entry+5,0xA0,16); entry[5] = 'A'+i;
        entry[0x17] = 42; // REL record length relative to the 32-byte slot.
        entry[0x1e] = 7; // Intentionally differs from one-sector chain fallback.
        img[soff(1,i)+1] = 3;
    }
    const auto path = (f.dir / "entries.d64").string();
    CHECK(write_file(path,img)); PetIEEE e; CHECK(e.setD64Image(path));
    std::vector<uint8_t> out;
    CHECK(PetIEEE_TestAccess::build_dir(e,out,""));
    CHECK(bytes_contain(out,"    7 \"A\""));
    CHECK(bytes_contain(out,"REL,42"));
}

static void test_host_paths_stay_in_mount() {
    ReviewFixture f;
    CHECK(write_file((f.dir / "OUTSIDE.prg").string(), {1,4,99}));
    CHECK(write_file((f.dir / "root/SOURCE.prg").string(), {1,4,42}));
    PetIEEE e; e.setHostRoot((f.dir / "root").string());
    std::vector<uint8_t> data; uint16_t addr=0;
    CHECK(PetIEEE_TestAccess::host_load(e,"SOURCE",data,addr));
    CHECK(!PetIEEE_TestAccess::host_load(e,"../OUTSIDE",data,addr));
    CHECK(!PetIEEE_TestAccess::host_load(e,"..\\OUTSIDE",data,addr));
    CHECK(!PetIEEE_TestAccess::host_load(e,std::filesystem::absolute(f.dir / "OUTSIDE.prg").string(),data,addr));
    CHECK(!PetIEEE_TestAccess::cmd(e,"C:../COPY=SOURCE"));
    CHECK(!std::filesystem::exists(f.dir / "COPY.prg"));
    CHECK(!PetIEEE_TestAccess::cmd(e,"R:../MOVED=SOURCE"));
    CHECK(std::filesystem::exists(f.dir / "root/SOURCE.prg"));
    CHECK(e.scratch_host_patterns({"../OUTSIDE"}) <= 0);
    CHECK(std::filesystem::exists(f.dir / "OUTSIDE.prg"));
    PetIEEE_TestAccess::host_save(e,"../SAVED");
    CHECK(!std::filesystem::exists(f.dir / "SAVED.prg"));
    CHECK(PetIEEE_TestAccess::cmd(e,"C:COPY=SOURCE"));
    CHECK(PetIEEE_TestAccess::cmd(e,"R:RENAMED=COPY"));
    CHECK_EQ(e.scratch_host_patterns({"RENAMED"}),1);
}

static void test_failed_disk_writes_are_unchanged() {
    ReviewFixture f;
    const auto path = (f.dir / "disk.d64").string();
    CHECK(write_file(path,make_blank_d64()));
    PetIEEE seed; CHECK(seed.setD64Image(path));
    CHECK(PetIEEE_TestAccess::save_prg(seed,"OLD",{1,4,42}));
    const auto original = PetIEEE_TestAccess::image(seed);
    for (bool unsupported : {true,false}) {
        auto image = original; if (unsupported) image.resize(196608);
        CHECK(write_file(path,image));
        // A directory as the output path guarantees a flush failure without permissions tricks.
        for (const std::string cmd : {"SAVE", "R:NEW=OLD", "S:OLD", "N:EMPTY,00", "V", "B-A:0,1,0", "U2:8,0,1,0"}) {
            PetIEEE e; CHECK(e.setD64Image(path));
            if (!unsupported) PetIEEE_TestAccess::image_path(e,f.dir.string());
            if (cmd == "SAVE") CHECK(!PetIEEE_TestAccess::save_prg(e,"OLD",{1,4,88}));
            else CHECK(!PetIEEE_TestAccess::cmd(e,cmd));
            CHECK(PetIEEE_TestAccess::image(e) == image);
        }
    }
}

static void test_cyclic_disk_chains_are_rejected() {
    ReviewFixture f;
    const auto path = (f.dir / "cycle.d64").string();
    for (bool directory : {true,false}) {
        auto img = make_blank_d64();
        auto* dir = img.data()+soff(18,1);
        if (directory) { dir[0]=18; dir[1]=1; }
        else {
            dir[2]=0x82; dir[3]=1; dir[4]=0;
            memset(dir+5,0xA0,16); dir[5]='X';
            img[soff(1,0)]=1; img[soff(1,0)+1]=0;
        }
        CHECK(write_file(path,img)); PetIEEE e; CHECK(e.setD64Image(path));
        std::vector<uint8_t> out; uint16_t addr=0;
        CHECK(!PetIEEE_TestAccess::load_prg(e,"X",out,addr));
        CHECK(!PetIEEE_TestAccess::open_read(e,"X",2,0));
        CHECK(!PetIEEE_TestAccess::build_dir(e,out,""));
        CHECK(!PetIEEE_TestAccess::cmd(e,"V"));
        CHECK(PetIEEE_TestAccess::image(e) == img);
        CHECK(!PetIEEE_TestAccess::cmd(e,"S:X"));
        CHECK(PetIEEE_TestAccess::image(e) == img);
    }
}

static void test_unicode_host_directory() {
    ReviewFixture f;
    const auto dir=f.dir/std::filesystem::u8path("PET-\xe6\xbc\xa2");
    std::filesystem::create_directory(dir);
    PetIEEE e; e.setHostRoot(dir.u8string());
    PetIEEE_TestAccess::host_save(e,"HELLO");
    std::vector<uint8_t> bytes; uint16_t address=0;
    CHECK(PetIEEE_TestAccess::host_load(e,"H*",bytes,address));
    CHECK_EQ(address,0x401);
    CHECK_EQ(e.scratch_host_patterns({"HELLO"}),1);
}

// ---------------------------------------------------------------------------
// Second batch: data files on the folder drive, atomic image writes, mount
// validation, 40-track geometry, overwrite protection, stale channels, folder
// listing/LOAD agreement.
// ---------------------------------------------------------------------------
static std::string slurp(const std::filesystem::path& p) {
    const auto v = read_file(p.string());
    return std::string(v.begin(), v.end());
}

static void test_folder_data_files() {
    ReviewFixture f;
    const auto root = f.dir / "root";
    PetIEEE e; e.setHostRoot(root.string());
    // OPEN 2,8,2,"SCORES,S,W": PRINT#2 ... CLOSE 2 creates a host file.
    PetIEEE_TestAccess::open_ch(e, 2, "SCORES,S,W");
    CHECK(status_code(e) == 0);
    PetIEEE_TestAccess::put_data(e, 2, vbytes("123\r456\r"));
    CHECK(!std::filesystem::exists(root / "SCORES.seq"));   // not until CLOSE
    PetIEEE_TestAccess::close_ch(e, 2);
    CHECK(slurp(root / "SCORES.seq") == "123\r456\r");
    // Read it back (typed and untyped), and append to it.
    PetIEEE_TestAccess::open_ch(e, 3, "SCORES,S,R");
    CHECK(PetIEEE_TestAccess::stream_data(e, 3) == vbytes("123\r456\r"));
    PetIEEE_TestAccess::open_ch(e, 4, "SCORES");
    CHECK(PetIEEE_TestAccess::stream_data(e, 4) == vbytes("123\r456\r"));
    PetIEEE_TestAccess::open_ch(e, 5, "SCORES,S,A");
    PetIEEE_TestAccess::put_data(e, 5, vbytes("789\r"));
    PetIEEE_TestAccess::close_ch(e, 5);
    CHECK(slurp(root / "SCORES.seq") == "123\r456\r789\r");
    // USR and PRG data files keep their types apart.
    PetIEEE_TestAccess::open_ch(e, 6, "BLOB,U,W");
    PetIEEE_TestAccess::put_data(e, 6, vbytes("U"));
    PetIEEE_TestAccess::close_ch(e, 6);
    PetIEEE_TestAccess::open_ch(e, 7, "CODE,P,W");
    PetIEEE_TestAccess::put_data(e, 7, vbytes("P"));
    PetIEEE_TestAccess::close_ch(e, 7);
    CHECK(std::filesystem::exists(root / "BLOB.usr"));
    CHECK(std::filesystem::exists(root / "CODE.prg"));
    std::vector<uint8_t> dir; CHECK(PetIEEE_TestAccess::build_dir_folder(e, dir, ""));
    CHECK(bytes_contain(dir, "\"SCORES\""));
    CHECK(bytes_contain(dir, "SEQ"));
    CHECK(bytes_contain(dir, "USR"));
    CHECK(bytes_contain(dir, "PRG"));
    // A missing file for read is 62; nothing is created by a read.
    PetIEEE_TestAccess::open_ch(e, 9, "NOSUCH,S,R");
    CHECK(status_code(e) == 62);
    CHECK(!std::filesystem::exists(root / "NOSUCH.seq"));
    // Scratch removes data files too.
    CHECK(PetIEEE_TestAccess::cmd(e, "S:BLOB"));
    CHECK(!std::filesystem::exists(root / "BLOB.usr"));
    // Names cannot leave the folder.
    CHECK(write_file((f.dir / "OUTSIDE.seq").string(), {1}));
    PetIEEE_TestAccess::open_ch(e, 2, "../OUTSIDE,S,W");
    PetIEEE_TestAccess::put_data(e, 2, vbytes("X"));
    PetIEEE_TestAccess::close_ch(e, 2);
    CHECK(slurp(f.dir / "OUTSIDE.seq") == "\x01");
    PetIEEE_TestAccess::open_ch(e, 2, "A:B,S,W");
    CHECK(status_code(e) != 0);
}

static void test_atomic_image_write() {
    ReviewFixture f;
    const auto path = (f.dir / "disk.d64").string();
    CHECK(write_file(path, make_blank_d64()));
    PetIEEE e; CHECK(e.setD64Image(path));
    CHECK(PetIEEE_TestAccess::save_prg(e, "ONE", {1,4,1}));
    CHECK(read_file(path) == PetIEEE_TestAccess::image(e));
    CHECK(!std::filesystem::exists(path + ".tmp~"));           // temp renamed away
    // Make the temporary file impossible to create: the original must survive.
    const auto before = read_file(path);
    std::filesystem::create_directory(path + ".tmp~");
    CHECK(!PetIEEE_TestAccess::save_prg(e, "TWO", {1,4,2}));
    CHECK_EQ(status_code(e), 25);
    CHECK(read_file(path) == before);
    std::filesystem::remove(path + ".tmp~");
    // A rename failure (target is a directory) leaves no temp file behind.
    const auto dirTarget = (f.dir / "asdir").string();
    std::filesystem::create_directory(dirTarget);
    PetIEEE_TestAccess::image_path(e, dirTarget);
    CHECK(!PetIEEE_TestAccess::save_prg(e, "THREE", {1,4,3}));
    CHECK(!std::filesystem::exists(dirTarget + ".tmp~"));
    // Folder saves replace atomically too.
    PetIEEE h; h.setHostRoot((f.dir / "root").string());
    PetIEEE_TestAccess::host_save(h, "KEEP");
    CHECK(!std::filesystem::exists(f.dir / "root/KEEP.prg.tmp~"));
    CHECK(std::filesystem::exists(f.dir / "root/KEEP.prg"));
}

static void test_mount_validation() {
    ReviewFixture f;
    const auto a = (f.dir / "a.d64").string();
    auto imgA = make_blank_d64();
    CHECK(write_file(a, imgA));
    PetIEEE e; CHECK(e.setD64Image(a));
    CHECK(PetIEEE_TestAccess::save_prg(e, "KEEPME", {1,4,9}));
    const auto mounted = PetIEEE_TestAccess::image(e);
    // Wrong sizes, empty and missing files are refused and change nothing.
    const auto junk = (f.dir / "junk.d64").string();
    CHECK(write_file(junk, std::vector<uint8_t>(1000, 0)));
    CHECK(!e.setD64Image(junk));
    CHECK(write_file(junk, std::vector<uint8_t>(174849, 0)));
    CHECK(!e.setD64Image(junk));
    CHECK(write_file(junk, {}));
    CHECK(!e.setD64Image(junk));
    CHECK(!e.setD64Image((f.dir / "missing.d64").string()));
    CHECK(e.isD64Mounted());
    CHECK(e.getD64Image() == a);
    CHECK(PetIEEE_TestAccess::image(e) == mounted);
    std::vector<uint8_t> pay; uint16_t addr = 0;
    CHECK(PetIEEE_TestAccess::load_prg(e, "KEEPME", pay, addr));
    // Every listed size is accepted.
    for (size_t n : {size_t(174848), size_t(175531), size_t(349696), size_t(351062), size_t(196608), size_t(197376)}) {
        auto img = make_blank_d64(); img.resize(n);
        CHECK(write_file(junk, img)); PetIEEE g; CHECK(g.setD64Image(junk));
    }
}

static void test_40_track_geometry() {
    ReviewFixture f;
    auto img = make_blank_d64();
    img.resize(196608, 0);
    // Tracks 36..40 hold 17 sectors each, directly after track 35's 683 sectors.
    auto off40 = [](int t, int s) { return (size_t)((683 + (t - 36) * 17 + s) * 256); };
    uint8_t* ent = img.data() + soff(18, 1) + 2;
    ent[0] = 0x82; ent[1] = 38; ent[2] = 3;
    memset(ent + 3, 0xA0, 16); memcpy(ent + 3, "FAR", 3);
    uint8_t* sec = img.data() + off40(38, 3);
    sec[0] = 0; sec[1] = 5; sec[2] = 0x01; sec[3] = 0x08; sec[4] = 0xAA; sec[5] = 0xBB;
    // A chain that ends on the very last sector of track 40 (sector 16).
    uint8_t* e2 = img.data() + soff(18, 1) + 2 + 32;
    e2[0] = 0x82; e2[1] = 40; e2[2] = 16;
    memset(e2 + 3, 0xA0, 16); memcpy(e2 + 3, "LAST", 4);
    uint8_t* s2 = img.data() + off40(40, 16);
    s2[0] = 0; s2[1] = 4; s2[2] = 0x01; s2[3] = 0x08; s2[4] = 0x77;
    const auto path = (f.dir / "forty.d64").string();
    CHECK(write_file(path, img));
    PetIEEE e; CHECK(e.setD64Image(path));
    std::vector<uint8_t> pay; uint16_t addr = 0;
    CHECK(PetIEEE_TestAccess::load_prg(e, "FAR", pay, addr));
    CHECK_EQ(addr, 0x0801);
    CHECK(pay == (std::vector<uint8_t>{0xAA, 0xBB}));
    CHECK(PetIEEE_TestAccess::load_prg(e, "LAST", pay, addr));
    CHECK(pay == (std::vector<uint8_t>{0x77}));
    uint8_t buf[256];
    CHECK(PetIEEE_TestAccess::read_sector(e, 40, 16, buf));
    CHECK(!PetIEEE_TestAccess::read_sector(e, 40, 17, buf));   // 17 sectors: 0..16
    CHECK(!PetIEEE_TestAccess::read_sector(e, 41, 0, buf));
    // Block commands reach the extra tracks too (U1 used the BAM's 35).
    CHECK(PetIEEE_TestAccess::cmd(e, "U1:5,0,40,16"));
    CHECK_EQ(status_code(e), 0);
    CHECK(PetIEEE_TestAccess::stream_data(e, 5).size() == 256 && PetIEEE_TestAccess::stream_data(e, 5)[4] == 0x77);
    CHECK(!PetIEEE_TestAccess::cmd(e, "U1:5,0,41,0"));
    CHECK_EQ(status_code(e), 66);
    // Still read-only.
    CHECK(!PetIEEE_TestAccess::save_prg(e, "NEW", {1,4,5}));
    CHECK(PetIEEE_TestAccess::image(e) == img);
    // A 35-track image has no tracks 36+ (it used to alias the .d71 mirror).
    PetIEEE g; CHECK(write_file(path, make_blank_d64())); CHECK(g.setD64Image(path));
    CHECK(!PetIEEE_TestAccess::read_sector(g, 36, 0, buf));
}

static void test_overwrite_protection() {
    ReviewFixture f;
    // ---- D64 backend ----
    const auto path = (f.dir / "ow.d64").string();
    CHECK(write_file(path, make_blank_d64()));
    PetIEEE e; CHECK(e.setD64Image(path));
    std::vector<std::string> notices;
    e.setNoticeHandler([&](const std::string& text) { notices.push_back(text); });
    PetIEEE_TestAccess::bus_save(e, "PROG", {1,8,0xAA});
    CHECK_EQ(status_code(e), 0);
    CHECK(notices.empty());                                        // a good SAVE says nothing
    PetIEEE_TestAccess::bus_save(e, "PROG", {1,8,0xBB});          // no "@"
    CHECK_EQ(status_code(e), 63);
    // BASIC prints READY. regardless, so the host is told.
    CHECK_EQ(notices.size(), size_t(1));
    CHECK(!notices.empty() && notices.back().find("63,FILE EXISTS") != std::string::npos &&
          notices.back().find("SAVE\"@0:PROG\",8") != std::string::npos);
    std::vector<uint8_t> pay; uint16_t addr = 0;
    CHECK(PetIEEE_TestAccess::load_prg(e, "PROG", pay, addr));
    CHECK(pay == (std::vector<uint8_t>{0xAA}));                    // old file kept
    PetIEEE_TestAccess::bus_save(e, "@0:PROG", {1,8,0xCC});        // replace
    CHECK_EQ(status_code(e), 0);
    CHECK(PetIEEE_TestAccess::load_prg(e, "PROG", pay, addr));
    CHECK(pay == (std::vector<uint8_t>{0xCC}));
    // OPEN ...,W: refused without "@", channel not armed, nothing committed.
    PetIEEE_TestAccess::open_ch(e, 3, "DATA,S,W");
    PetIEEE_TestAccess::put_data(e, 3, vbytes("ONE"));
    PetIEEE_TestAccess::close_ch(e, 3);
    PetIEEE_TestAccess::open_ch(e, 3, "DATA,S,W");
    CHECK_EQ(status_code(e), 63);
    CHECK(!PetIEEE_TestAccess::write_active(e, 3));
    PetIEEE_TestAccess::put_data(e, 3, vbytes("TWO"));
    PetIEEE_TestAccess::close_ch(e, 3);
    CHECK(PetIEEE_TestAccess::open_read(e, "DATA", 4, 0));
    CHECK(PetIEEE_TestAccess::stream_data(e, 4) == vbytes("ONE"));
    PetIEEE_TestAccess::open_ch(e, 3, "@:DATA,S,W");
    PetIEEE_TestAccess::put_data(e, 3, vbytes("TWO"));
    PetIEEE_TestAccess::close_ch(e, 3);
    CHECK(PetIEEE_TestAccess::open_read(e, "DATA", 4, 0));
    CHECK(PetIEEE_TestAccess::stream_data(e, 4) == vbytes("TWO"));
    // Append to an existing file needs no "@".
    PetIEEE_TestAccess::open_ch(e, 3, "DATA,S,A");
    CHECK_EQ(status_code(e), 0);
    PetIEEE_TestAccess::close_ch(e, 3);
    // Locked files (type bit 6) are neither replaced nor scratched.
    CHECK(PetIEEE_TestAccess::save_prg(e, "LOCK", {1,8,0x11}));
    auto img = read_file(path);
    uint8_t* dir = img.data() + soff(18, 1);
    for (int i = 0; i < 8; ++i)
        if (dir[2 + i*32] == 0x82 && memcmp(dir + 2 + i*32 + 3, "LOCK", 4) == 0) dir[2 + i*32] |= 0x40;
    CHECK(write_file(path, img));
    PetIEEE l; CHECK(l.setD64Image(path));
    PetIEEE_TestAccess::bus_save(l, "@0:LOCK", {1,8,0x22});
    CHECK_EQ(status_code(l), 63);                                  // CBM DOS: name still taken
    PetIEEE_TestAccess::open_ch(l, 3, "@LOCK,S,W");
    CHECK_EQ(status_code(l), 63);
    CHECK(PetIEEE_TestAccess::cmd(l, "S:LOCK"));
    CHECK(PetIEEE_TestAccess::status(l).find("01,FILES SCRATCHED,00") != std::string::npos);
    CHECK(PetIEEE_TestAccess::load_prg(l, "LOCK", pay, addr));
    CHECK(pay == (std::vector<uint8_t>{0x11}));
    CHECK(PetIEEE_TestAccess::image(l) == img);

    // ---- folder backend ----
    const auto root = f.dir / "root";
    PetIEEE h; h.setHostRoot(root.string());
    notices.clear();
    h.setNoticeHandler([&](const std::string& text) { notices.push_back(text); });
    PetIEEE_TestAccess::bus_save(h, "PROG", {1,8,0xAA});
    CHECK(slurp(root / "PROG.prg") == std::string("\x01\x08\xAA", 3));
    PetIEEE_TestAccess::bus_save(h, "PROG", {1,8,0xBB});
    CHECK_EQ(status_code(h), 63);
    CHECK_EQ(notices.size(), size_t(1));
    CHECK(slurp(root / "PROG.prg") == std::string("\x01\x08\xAA", 3));
    PetIEEE_TestAccess::bus_save(h, "@0:PROG", {1,8,0xCC});
    CHECK_EQ(status_code(h), 0);
    CHECK(slurp(root / "PROG.prg") == std::string("\x01\x08\xCC", 3));
    PetIEEE_TestAccess::open_ch(h, 3, "DATA,S,W");
    PetIEEE_TestAccess::put_data(h, 3, vbytes("ONE"));
    PetIEEE_TestAccess::close_ch(h, 3);
    PetIEEE_TestAccess::open_ch(h, 3, "DATA,S,W");
    CHECK_EQ(status_code(h), 63);
    PetIEEE_TestAccess::put_data(h, 3, vbytes("TWO"));
    PetIEEE_TestAccess::close_ch(h, 3);
    CHECK(slurp(root / "DATA.seq") == "ONE");
    PetIEEE_TestAccess::open_ch(h, 3, "@0:DATA,S,W");
    PetIEEE_TestAccess::put_data(h, 3, vbytes("TWO"));
    PetIEEE_TestAccess::close_ch(h, 3);
    CHECK(slurp(root / "DATA.seq") == "TWO");
}

static void test_stale_channels() {
    ReviewFixture f;
    const auto pa = (f.dir / "a.d64").string(), pb = (f.dir / "b.d64").string();
    CHECK(write_file(pa, make_blank_d64()));
    CHECK(write_file(pb, make_blank_d64()));
    // reset(): a file left open for write is dropped, not committed later.
    PetIEEE e; CHECK(e.setD64Image(pa));
    PetIEEE_TestAccess::open_ch(e, 3, "STALE,S,W");
    PetIEEE_TestAccess::put_data(e, 3, vbytes("DATA"));
    CHECK(PetIEEE_TestAccess::write_active(e, 3));
    PetIEEE_TestAccess::bus_cmd(e, 0x28); PetIEEE_TestAccess::bus_cmd(e, 0x6F);
    PetIEEE_TestAccess::bus_data(e, 'S');                          // half-typed command text
    CHECK(!PetIEEE_TestAccess::cmd15_pending(e).empty());
    e.reset();
    CHECK(!PetIEEE_TestAccess::write_active(e, 3));
    CHECK(PetIEEE_TestAccess::cmd15_pending(e).empty());
    CHECK(e.isD64Mounted());                                       // media survives reset
    PetIEEE_TestAccess::close_ch(e, 3);
    CHECK(!PetIEEE_TestAccess::open_read(e, "STALE", 4, 0));
    // Disk swap: the write opened on A must not land on B (or A).
    PetIEEE_TestAccess::open_ch(e, 3, "SWAP,S,W");
    PetIEEE_TestAccess::put_data(e, 3, vbytes("DATA"));
    CHECK(e.setD64Image(pb));
    CHECK(!PetIEEE_TestAccess::write_active(e, 3));
    PetIEEE_TestAccess::close_ch(e, 3);
    CHECK(!PetIEEE_TestAccess::open_read(e, "SWAP", 4, 0));
    PetIEEE a2; CHECK(a2.setD64Image(pa));
    CHECK(!PetIEEE_TestAccess::open_read(a2, "SWAP", 4, 0));
    CHECK(!PetIEEE_TestAccess::open_read(a2, "STALE", 4, 0));
    // Eject drops channels as well.
    PetIEEE_TestAccess::open_ch(e, 3, "EJ,S,W");
    CHECK(e.setD64Image(""));
    CHECK(!PetIEEE_TestAccess::write_active(e, 3));
}

static void test_folder_listing_matches_load() {
    ReviewFixture f;
    const auto root = f.dir / "root";
    auto put = [&](const char* n, std::vector<uint8_t> b) { CHECK(write_file((root / n).string(), b)); };
    put("zed.prg",   {1,8,0x5A});
    put("AAA.d64",   {1,8,1});
    put("BBB.tap",   {1,8,2});
    put("notes.txt", {1,8,3});
    put("mid",       {1,8,0x4D});                                  // extensionless = PRG
    put("ABCDEFGHIJKLMNOPQRST.prg", {1,8,0x4C});                   // 20 characters
    PetIEEE e; e.setHostRoot(root.string());
    std::vector<uint8_t> dir; CHECK(PetIEEE_TestAccess::build_dir_folder(e, dir, ""));
    const std::string txt((const char*)dir.data(), dir.size());
    CHECK(txt.find("AAA") == std::string::npos);                   // .d64/.tap/.txt are not files
    CHECK(txt.find("BBB") == std::string::npos);
    CHECK(txt.find("NOTES") == std::string::npos);
    CHECK(txt.find("\"ABCDEFGHIJKLMNOP\"") != std::string::npos);  // cut to 16
    const size_t pl = txt.find("\"ABCDEFGHIJKLMNOP\""), pm = txt.find("\"MID\""), pz = txt.find("\"ZED\"");
    CHECK(pl != std::string::npos && pm != std::string::npos && pz != std::string::npos);
    CHECK(pl < pm && pm < pz);                                     // sorted by name
    std::vector<uint8_t> pay; uint16_t addr = 0;
    // The name as listed loads the 20-character file; so does the full name.
    CHECK(PetIEEE_TestAccess::host_load(e, "ABCDEFGHIJKLMNOP", pay, addr));
    CHECK(pay == (std::vector<uint8_t>{0x4C}));
    CHECK(PetIEEE_TestAccess::host_load(e, "ABCDEFGHIJKLMNOPQRST", pay, addr));
    CHECK(PetIEEE_TestAccess::host_load(e, "MID", pay, addr));
    CHECK(pay == (std::vector<uint8_t>{0x4D}));
    CHECK(PetIEEE_TestAccess::host_load(e, "ZED", pay, addr));
    CHECK(!PetIEEE_TestAccess::host_load(e, "AAA", pay, addr));
    CHECK(!PetIEEE_TestAccess::host_load(e, "NOTES", pay, addr));
    // "*" is the first listed program, never the .d64 that sorts before it.
    CHECK(PetIEEE_TestAccess::host_load(e, "*", pay, addr));
    CHECK(pay == (std::vector<uint8_t>{0x4C}));
    CHECK(PetIEEE_TestAccess::host_load(e, "M*", pay, addr));
    CHECK(pay == (std::vector<uint8_t>{0x4D}));

    // A dot that is not a known type is part of the name: such files are PRGs
    // listed and loaded under their whole name, as before the typed listing.
    // PC64 containers (.p00) stay out: their header is not a load address.
    put("v1.2",           {1,8,0x12});
    put("invaders.bin",   {1,8,0x1B});
    put("game.p00",       {'C','6','4','F'});
    std::vector<uint8_t> dir2; CHECK(PetIEEE_TestAccess::build_dir_folder(e, dir2, ""));
    const std::string txt2((const char*)dir2.data(), dir2.size());
    CHECK(txt2.find("\"V1.2\"") != std::string::npos);
    CHECK(txt2.find("\"INVADERS.BIN\"") != std::string::npos);
    CHECK(txt2.find("GAME") == std::string::npos);
    CHECK(PetIEEE_TestAccess::host_load(e, "V1.2", pay, addr));
    CHECK(pay == (std::vector<uint8_t>{0x12}));
    CHECK(PetIEEE_TestAccess::host_load(e, "INVADERS.BIN", pay, addr));
    CHECK(pay == (std::vector<uint8_t>{0x1B}));
    CHECK(PetIEEE_TestAccess::host_load(e, "INV*", pay, addr));
    CHECK(pay == (std::vector<uint8_t>{0x1B}));
}

// write_all_file replaces atomically but keeps the file's identity: through a
// symlink the target is written (the link stays a link), and a file with a
// second hard link is updated so both names see the new bytes.
static void test_write_all_file_keeps_links() {
    ReviewFixture f;
    namespace fs = std::filesystem;
    const fs::path real = f.dir / "real.d64", other = f.dir / "other.d64", link = f.dir / "link.d64";
    CHECK(write_file(real.string(), {1, 2, 3}));
    std::error_code ec;
    fs::create_hard_link(real, other, ec);
    if (!ec) {
        CHECK(ieee_helpers::write_all_file(real, {4, 5}));
        CHECK(read_file(other.string()) == (std::vector<uint8_t>{4, 5}));
        CHECK_EQ(fs::hard_link_count(real), uintmax_t(2));
    } else std::puts("NOTE: hard links unavailable here; hard-link check skipped");
    // A relative link target is resolved from the link's own folder.
    fs::create_symlink(real.filename(), link, ec);
    if (!ec) {
        CHECK(ieee_helpers::write_all_file(link, {6}));
        CHECK(fs::is_symlink(fs::symlink_status(link)));
        CHECK(read_file(real.string()) == (std::vector<uint8_t>{6}));
        // A dangling link is refused, not replaced by a new plain file.
        const fs::path dangling = f.dir / "dangling.d64";
        fs::create_symlink("missing.d64", dangling, ec);
        if (!ec) {
            CHECK(!ieee_helpers::write_all_file(dangling, {1}));
            CHECK(fs::is_symlink(fs::symlink_status(dangling)));
        }
    } else std::puts("NOTE: symlinks unavailable here (Windows needs Developer Mode); symlink check skipped");
    // An ordinary file is still replaced, and no temporary file is left behind.
    const fs::path plain = f.dir / "plain.d64";
    CHECK(write_file(plain.string(), {7}));
    CHECK(ieee_helpers::write_all_file(plain, {8, 9}));
    CHECK(read_file(plain.string()) == (std::vector<uint8_t>{8, 9}));
    CHECK(!fs::exists(f.dir / "plain.d64.tmp~"));
}

// ---------------------------------------------------------------------------
// The PET's side of the bus, driven through the public line API in the order
// the BASIC 2 KERNAL (901465-03) does it. Addresses name the ROM routines the
// steps were read from. A write to $E840 updates ATN and NRFD together, as in
// Pet2001IO.
// ---------------------------------------------------------------------------
struct Kernal2Bus {
    PetIEEE& e;
    bool atn = true, nrfd = true;
    bool stalled = false;   // the KERNAL would wait for DAV high forever
    bool eoi = false;       // EOI seen with the last byte received
    void orb(bool a, bool n) { atn = a; nrfd = n; e.ATNout(a); e.NRFDout(n); }
    void send(uint8_t byte) {                       // $F0EE
        e.DAVout(true); e.DIOout((uint8_t)(byte ^ 0xFF));
        e.DAVout(false); e.DAVout(true); e.DIOout(0xFF);
    }
    void command(uint8_t byte) {                    // $F0BC
        orb(atn, true);                             // release NRFD...
        e.NDACout(true);                            // ...and NDAC
        if (!e.DAVin()) { stalled = true; return; } // $F0E4: LDA $E840 / BPL
        orb(false, true);
        send(byte);
    }
    void open(uint8_t sa, const std::string& name) {
        command(0x28); send((uint8_t)(0xF0 | sa)); orb(true, nrfd);
        for (char c : name) send((uint8_t)c);
        command(0x3F); orb(true, nrfd);
    }
    void talk(uint8_t sa) {                         // $F0B6, then $F164
        command(0x48); send((uint8_t)(0x60 | sa));
        orb(atn, false); e.NDACout(false);          // $F146
        orb(true, nrfd);                            // $F12D
    }
    void untalk() { command(0x5F); orb(true, nrfd); } // $F17F
    int get() {                                     // $F18C; -1 = timed out, no byte
        e.NDACout(false);
        orb(atn, true);
        if (e.DAVin()) return -1;
        orb(atn, false);
        eoi = !e.EOIin();
        const int byte = (uint8_t)(e.DIOin() ^ 0xFF);
        e.NDACout(true);
        if (!e.DAVin()) { stalled = true; return -2; }
        e.NDACout(false);
        return byte;
    }
};

// A program that reads a record and leaves the rest for later (INPUT# per
// record, or a GET# loop) hands the bus back with bytes still unread. BASIC 2
// releases NRFD and NDAC before it asserts ATN; the drive took that for
// "ready" plus "accepted", put up another byte and the PET waited for ever.
// With one byte left it did not hang but lost that byte.
static void test_partial_read_hands_the_bus_back() {
    ReviewFixture f;
    const auto root = f.dir / "root";
    CHECK(write_file((root / "DATA.seq").string(), vbytes("1\rXY\r")));
    PetIEEE e; e.setHostRoot(root.string());
    Kernal2Bus pet{ e };
    pet.open(2, "DATA,S,R");
    CHECK(status_code(e) == 0);
    pet.talk(2);                                    // INPUT#2,A
    CHECK_EQ(pet.get(), '1');
    CHECK_EQ(pet.get(), '\r');
    pet.untalk();                                   // three bytes unread
    CHECK(!pet.stalled);
    pet.talk(2); CHECK_EQ(pet.get(), 'X'); CHECK(!pet.eoi); pet.untalk();  // GET#2 (two unread)
    CHECK(!pet.stalled);
    pet.talk(2); CHECK_EQ(pet.get(), 'Y'); CHECK(!pet.eoi); pet.untalk();  // GET#2 (one unread)
    CHECK(!pet.stalled);
    pet.talk(2); CHECK_EQ(pet.get(), '\r'); CHECK(pet.eoi); pet.untalk();  // last byte carries EOI
    CHECK(!pet.stalled);
    pet.talk(2); CHECK_EQ(pet.get(), -1); pet.untalk();                     // nothing left: silence
    CHECK(!pet.stalled);
}

// hostLoadFile / hostSaveFile: LOAD and SAVE by name without bus traffic (the
// PET 2001 BASIC 1 ROM cannot drive the disk itself). The bus must stay idle.
static bool drive_idle(PetIEEE& e) {
    return e.getState() == PetIEEE::STATE_IDLE && e.DAVin() && e.EOIin();
}

static void test_host_load_save_folder() {
    ReviewFixture f;
    const auto root = f.dir / "root";
    CHECK(write_file((root / "hello.prg").string(), { 0x01, 0x04, 0x0A, 0x0B, 0x0C }));
    PetIEEE e; e.setHostRoot(root.string());
    std::vector<uint8_t> pay; uint16_t addr = 0;

    // Missing name: false and 62,FILE NOT FOUND.
    CHECK(!e.hostLoadFile("GHOST", pay, addr));
    CHECK_EQ(status_code(e), 62);
    CHECK(drive_idle(e));

    // By name: the load address comes back separately, the payload without it.
    CHECK(e.hostLoadFile("HELLO", pay, addr));
    CHECK_EQ(addr, 0x0401);
    CHECK(pay == std::vector<uint8_t>({ 0x0A, 0x0B, 0x0C }));
    CHECK_EQ(status_code(e), 0);
    CHECK(drive_idle(e));

    // Directory: "$" and the drive-number form "$0".
    for (const char* dn : { "$", "$0" }) {
        pay.clear(); addr = 0;
        CHECK(e.hostLoadFile(dn, pay, addr));
        CHECK_EQ(addr, 0x0401);
        CHECK(bytes_contain(pay, "HELLO"));
        CHECK(drive_idle(e));
    }
    // A filtered listing that matches nothing still lists, but not HELLO.
    pay.clear();
    CHECK(e.hostLoadFile("$:Z*", pay, addr));
    CHECK(!bytes_contain(pay, "HELLO"));

    // SAVE creates NAME.prg with exactly the given bytes.
    const std::vector<uint8_t> first = { 0x01, 0x04, 0x11, 0x22, 0x33 };
    CHECK(e.hostSaveFile("NEW", first));
    CHECK_EQ(status_code(e), 0);
    CHECK(read_file((root / "NEW.prg").string()) == first);
    CHECK(drive_idle(e));

    // Saved program loads back by name.
    CHECK(e.hostLoadFile("NEW", pay, addr));
    CHECK_EQ(addr, 0x0401);
    CHECK(pay == std::vector<uint8_t>({ 0x11, 0x22, 0x33 }));

    // Second SAVE without "@": 63,FILE EXISTS and the file stays as it was.
    const std::vector<uint8_t> second = { 0x01, 0x04, 0x44, 0x55 };
    CHECK(!e.hostSaveFile("NEW", second));
    CHECK_EQ(status_code(e), 63);
    CHECK(read_file((root / "NEW.prg").string()) == first);
    CHECK(drive_idle(e));

    // "@0:" replaces.
    CHECK(e.hostSaveFile("@0:NEW", second));
    CHECK_EQ(status_code(e), 0);
    CHECK(read_file((root / "NEW.prg").string()) == second);
    CHECK(drive_idle(e));

    // Empty name: 34,SYNTAX ERROR, nothing written.
    CHECK(!e.hostSaveFile("", second));
    CHECK_EQ(status_code(e), 34);
    CHECK(!std::filesystem::exists(root / "UNTITLED.prg"));
    CHECK(!e.hostSaveFile("\"\"", second));
    CHECK_EQ(status_code(e), 34);
    CHECK(drive_idle(e));

    // No folder and no image: not written.
    PetIEEE none;
    CHECK(!none.hostSaveFile("X", first));
    CHECK_EQ(status_code(none), 74);                // DRIVE NOT READY
    CHECK(!none.hostLoadFile("X", pay, addr));
    CHECK_EQ(status_code(none), 62);
    CHECK(drive_idle(none));
}

static void test_host_load_save_d64() {
    ReviewFixture f;
    const auto path = (f.dir / "host.d64").string();
    CHECK(write_file(path, make_blank_d64()));
    PetIEEE e; CHECK(e.setD64Image(path));
    std::vector<uint8_t> pay; uint16_t addr = 0;

    const std::vector<uint8_t> prg = { 0x3C, 0x03, 0xA9, 0x00, 0x60 };
    CHECK(e.hostSaveFile("0:PROG", prg));
    CHECK_EQ(status_code(e), 0);
    CHECK(drive_idle(e));

    CHECK(e.hostLoadFile("PROG", pay, addr));
    CHECK_EQ(addr, 0x033C);
    CHECK(pay == std::vector<uint8_t>({ 0xA9, 0x00, 0x60 }));
    CHECK_EQ(status_code(e), 0);
    CHECK(drive_idle(e));

    pay.clear();
    CHECK(e.hostLoadFile("$", pay, addr));
    CHECK_EQ(addr, 0x0401);
    CHECK(bytes_contain(pay, "PROG"));
    CHECK(drive_idle(e));

    CHECK(!e.hostLoadFile("GHOST", pay, addr));
    CHECK_EQ(status_code(e), 62);
    CHECK(drive_idle(e));

    // No "@": 63, the image keeps the first program.
    CHECK(!e.hostSaveFile("PROG", { 0x01, 0x04, 0xEE }));
    CHECK_EQ(status_code(e), 63);
    CHECK(e.hostLoadFile("PROG", pay, addr));
    CHECK(pay == std::vector<uint8_t>({ 0xA9, 0x00, 0x60 }));

    // The save reached the image file on disk, not only the drive's copy.
    PetIEEE again; CHECK(again.setD64Image(path));
    CHECK(again.hostLoadFile("PROG", pay, addr));
    CHECK_EQ(addr, 0x033C);
    CHECK(pay == std::vector<uint8_t>({ 0xA9, 0x00, 0x60 }));
}

// The tests write their images and folders under relative names. Run them in a
// private folder under the system temp directory, removed afterwards, so a run
// from the source tree or the build folder leaves nothing behind.
// ---------------------------------------------------------------------------
// .d80 (8050, 77 tracks) and .d82 (8250, 154 tracks). Zones of 29/27/25/23
// sectors (the .d82's second side repeats them), header 39/0, directory chain
// from 39/1, BAM blocks 38/0 and 38/3 (plus 38/6 and 38/9 on a .d82), 50 tracks
// and 5 bytes per track in each. The geometry is spelled out here on its own so
// a slip in the emulator shows up as a failure.
// ---------------------------------------------------------------------------
static int spt8(int t) {
    if (t < 1 || t > 154) return 0;
    const int z = t > 77 ? t - 77 : t;
    if (z <= 39) return 29;
    if (z <= 53) return 27;
    if (z <= 64) return 25;
    return 23;
}
static size_t soff8(int t, int s) { int tot = 0; for (int i = 1; i < t; ++i) tot += spt8(i); return (size_t)((tot + s) * 256); }
static const int kBam8Sec[4] = { 0, 3, 6, 9 };

// Blank formatted image as DOS leaves it: track 38 all used, 39/0 and 39/1 used.
static std::vector<uint8_t> make_blank_d8x(bool two, const char* name, const char* id) {
    std::vector<uint8_t> img(two ? 1066496 : 533248, 0);
    const int tracks = two ? 154 : 77, nBlocks = two ? 4 : 2;
    uint8_t* h = img.data() + soff8(39, 0);
    h[0] = 38; h[1] = 0; h[2] = 'C';
    for (int i = 0; i < 16; ++i) h[6 + i] = 0xA0;
    for (int i = 0; name[i]; ++i) h[6 + i] = (uint8_t)name[i];
    h[0x16] = 0xA0; h[0x17] = 0xA0; h[0x18] = (uint8_t)id[0]; h[0x19] = (uint8_t)id[1];
    h[0x1A] = 0xA0; h[0x1B] = '2'; h[0x1C] = 'C';
    for (int i = 0; i < nBlocks; ++i) {
        uint8_t* b = img.data() + soff8(38, kBam8Sec[i]);
        b[0] = (uint8_t)(i + 1 < nBlocks ? 38 : 39); b[1] = (uint8_t)(i + 1 < nBlocks ? kBam8Sec[i + 1] : 1);
        b[2] = 'C';
        const int lo = 1 + 50 * i, hi = (lo + 50 < tracks + 1) ? lo + 50 : tracks + 1;
        b[4] = (uint8_t)lo; b[5] = (uint8_t)hi;
        for (int t = lo; t < hi; ++t) {
            uint32_t bits = (1u << spt8(t)) - 1u;
            if (t == 38) bits = 0;
            if (t == 39) bits &= ~3u;
            int cnt = 0; for (int s = 0; s < 32; ++s) cnt += (bits >> s) & 1;
            const int o = 6 + (t - lo) * 5;
            b[o] = (uint8_t)cnt;
            for (int k = 0; k < 4; ++k) b[o + 1 + k] = (uint8_t)(bits >> (8 * k));
        }
    }
    uint8_t* dir = img.data() + soff8(39, 1);
    dir[0] = 0x00; dir[1] = 0xFF;
    return img;
}

// Put a file on an exact list of sectors: marks them used in the BAM, writes the
// chain and adds directory slot 'slot' (0..7, all in 39/1).
static void add_file_d8x(std::vector<uint8_t>& img, int slot, const char* name, uint8_t type,
                         const std::vector<std::pair<int, int>>& secs, const std::vector<uint8_t>& data) {
    for (size_t k = 0; k < secs.size(); ++k) {
        const int t = secs[k].first, s = secs[k].second;
        uint8_t* b = img.data() + soff8(38, kBam8Sec[(t - 1) / 50]);
        const int o = 6 + ((t - 1) % 50) * 5;
        b[o + 1 + (s >> 3)] &= (uint8_t)~(1u << (s & 7));
        b[o]--;
        uint8_t* p = img.data() + soff8(t, s);
        const size_t pos = k * 254, room = data.size() - pos;
        if (k + 1 < secs.size()) {
            p[0] = (uint8_t)secs[k + 1].first; p[1] = (uint8_t)secs[k + 1].second;
            std::memcpy(p + 2, data.data() + pos, 254);
        } else {
            p[0] = 0; p[1] = (uint8_t)(room + 1);
            std::memcpy(p + 2, data.data() + pos, room);
        }
    }
    uint8_t* e = img.data() + soff8(39, 1) + 2 + slot * 32;
    e[0] = type; e[1] = (uint8_t)secs[0].first; e[2] = (uint8_t)secs[0].second;
    for (int i = 0; i < 16; ++i) e[3 + i] = 0xA0;
    for (int i = 0; name[i]; ++i) e[3 + i] = (uint8_t)name[i];
    e[0x1C] = (uint8_t)(secs.size() & 0xFF); e[0x1D] = (uint8_t)(secs.size() >> 8);
}

static std::vector<uint8_t> pattern_bytes(size_t n, int mul, uint8_t first0, uint8_t first1) {
    std::vector<uint8_t> v(n);
    for (size_t i = 0; i < n; ++i) v[i] = (uint8_t)((i * mul + 11) & 0xFF);
    v[0] = first0; v[1] = first1;   // PRG load address
    return v;
}

// Every sector of a file's chain (empty on a broken or looping chain).
static std::vector<std::pair<int, int>> chain_of(PetIEEE& e, int t, int s) {
    std::vector<std::pair<int, int>> out;
    uint8_t sec[256];
    std::set<std::pair<int, int>> seen;
    while (t != 0) {
        if (!seen.insert({ t, s }).second || !PetIEEE_TestAccess::read_sector(e, t, s, sec)) return {};
        out.push_back({ t, s });
        const int nt = sec[0], ns = sec[1]; t = nt; s = ns;
    }
    return out;
}

static int count_named(PetIEEE& e, const char* name) {
    D64Catalog cat{};
    if (!PetIEEE_TestAccess::parse_bam(e, cat) || !PetIEEE_TestAccess::parse_dir(e, cat)) return -1;
    int n = 0;
    for (const auto& en : cat.entries) if (std::string(en.name) == name && (en.fileType & 0x0F)) ++n;
    return n;
}

static void run_d8x(bool two) {
    const std::string ext = two ? "d82" : "d80";
    const std::string path = "d64_test_d8x." + ext;
    const int total = two ? 4166 : 2083;
    const int blankFree = total - 29 - 2;       // BAM total: track 38, header 39/0 and first directory block 39/1 are used (2052 on a blank 8050)
    const int lastTrack = two ? 154 : 77;

    // HELLO spans three sectors on track 37; SIDE2 sits at the far end of the
    // disk (side two on a .d82) and starts on the last sector of its 23-sector track.
    const auto hello = pattern_bytes(600, 7, 0x01, 0x08);
    const auto side2 = pattern_bytes(300, 13, 0x00, 0x20);
    const int farT = two ? 150 : 70;
    auto img = make_blank_d8x(two, two ? "TEST D82" : "TEST D80", two ? "D2" : "D8");
    add_file_d8x(img, 0, "HELLO", 0x82, { {37, 0}, {37, 5}, {37, 10} }, hello);
    add_file_d8x(img, 1, "SIDE2", 0x82, { {farT, 22}, {farT, 3} }, side2);
    CHECK_EQ((int)img.size(), two ? 1066496 : 533248);
    CHECK(write_file(path, img));

    // --- mount by size; wrong sizes are refused and change nothing ---
    PetIEEE e;
    CHECK(e.setD64Image(path));
    CHECK(PetIEEE_TestAccess::image(e) == img);
    {
        auto bad = img; bad.push_back(0);
        CHECK(write_file("d64_test_d8x_bad.img", bad));
        CHECK(!e.setD64Image("d64_test_d8x_bad.img"));
        bad.resize(img.size() - 256);
        CHECK(write_file("d64_test_d8x_bad.img", bad));
        CHECK(!e.setD64Image("d64_test_d8x_bad.img"));
        CHECK(e.getD64Image() == path);
        CHECK(PetIEEE_TestAccess::image(e) == img);
        std::error_code ec; std::filesystem::remove("d64_test_d8x_bad.img", ec);
    }

    // --- geometry: first/last sector of each zone, one past the end is refused ---
    uint8_t sec[256];
    CHECK(PetIEEE_TestAccess::read_sector(e, 1, 28, sec));
    CHECK(!PetIEEE_TestAccess::read_sector(e, 1, 29, sec));
    CHECK(PetIEEE_TestAccess::read_sector(e, 40, 26, sec));
    CHECK(!PetIEEE_TestAccess::read_sector(e, 40, 27, sec));
    CHECK(PetIEEE_TestAccess::read_sector(e, 54, 24, sec));
    CHECK(!PetIEEE_TestAccess::read_sector(e, 54, 25, sec));
    CHECK(PetIEEE_TestAccess::read_sector(e, 65, 22, sec));
    CHECK(!PetIEEE_TestAccess::read_sector(e, 65, 23, sec));
    CHECK(PetIEEE_TestAccess::read_sector(e, lastTrack, 22, sec));
    CHECK(!PetIEEE_TestAccess::read_sector(e, lastTrack, 23, sec));
    CHECK(!PetIEEE_TestAccess::read_sector(e, lastTrack + 1, 0, sec));
    if (two) {
        CHECK(PetIEEE_TestAccess::read_sector(e, 78, 28, sec));
        CHECK(!PetIEEE_TestAccess::read_sector(e, 78, 29, sec));
        CHECK(PetIEEE_TestAccess::read_sector(e, 117, 26, sec));
        CHECK(!PetIEEE_TestAccess::read_sector(e, 117, 27, sec));
        CHECK(PetIEEE_TestAccess::read_sector(e, 131, 24, sec));
        CHECK(!PetIEEE_TestAccess::read_sector(e, 131, 25, sec));
    }

    // --- directory listing: header, DOS type, entries with block counts, free ---
    std::vector<uint8_t> dir;
    CHECK(PetIEEE_TestAccess::build_dir(e, dir, ""));
    CHECK(bytes_contain(dir, two ? "\"TEST D82" : "\"TEST D80"));
    CHECK(bytes_contain(dir, two ? "D2 2C" : "D8 2C"));
    CHECK(bytes_contain(dir, "    3 \"HELLO\""));
    CHECK(bytes_contain(dir, "    2 \"SIDE2\""));
    CHECK(bytes_contain(dir, "PRG"));
    CHECK_EQ(parse_blocks_free(dir), blankFree - 5);
    std::vector<uint8_t> dirW;
    CHECK(PetIEEE_TestAccess::build_dir(e, dirW, "H*"));
    CHECK(bytes_contain(dirW, "\"HELLO\"") && !bytes_contain(dirW, "SIDE2"));

    // --- LOAD: by name, wildcard, directory, missing file ---
    std::vector<uint8_t> full;
    CHECK(load_full(e, "HELLO", full)); CHECK(full == hello);
    CHECK(load_full(e, "SIDE2", full)); CHECK(full == side2);
    CHECK(load_full(e, "HEL*", full));  CHECK(full == hello);
    CHECK(load_full(e, "S?DE2", full)); CHECK(full == side2);
    CHECK(load_full(e, "*", full));     CHECK(full == hello);       // first entry
    std::vector<uint8_t> pay; uint16_t addr = 0;
    CHECK(e.hostLoadFile("SIDE2", pay, addr));
    CHECK_EQ(addr, 0x2000); CHECK(pay == std::vector<uint8_t>(side2.begin() + 2, side2.end()));
    CHECK_EQ(status_code(e), 0);
    pay.clear();
    CHECK(e.hostLoadFile("$", pay, addr));                          // LOAD"$",8
    CHECK(bytes_contain(pay, "HELLO") && bytes_contain(pay, "SIDE2") && bytes_contain(pay, "2C"));
    CHECK(!e.hostLoadFile("GHOST", pay, addr));
    CHECK_EQ(status_code(e), 62);
    CHECK(!PetIEEE_TestAccess::load_prg(e, "GHOST", pay, addr));

    // --- OPEN/INPUT#/GET# style read (a PRG through a data channel keeps its address) ---
    CHECK(PetIEEE_TestAccess::open_read(e, "HELLO", 2, 2));
    CHECK(PetIEEE_TestAccess::stream_data(e, 2) == hello);
    CHECK(PetIEEE_TestAccess::open_read(e, "SIDE*", 3, 0));
    CHECK(PetIEEE_TestAccess::stream_data(e, 3) == side2);
    CHECK(!PetIEEE_TestAccess::open_read(e, "GHOST", 4, 0));

    // --- the bus sequence loaders use: OPEN name / TALK / $60 / bytes ---
    for (const auto* want : { &hello, &side2 }) {
        Kernal2Bus pet{ e };
        pet.open(0, want == &hello ? "HELLO" : "SIDE2");
        pet.talk(0);
        std::vector<uint8_t> got;
        for (size_t i = 0; i < want->size() + 2; ++i) {
            const int b = pet.get();
            if (b < 0) break;
            got.push_back((uint8_t)b);
            if (pet.eoi) break;
        }
        pet.untalk();
        CHECK(!pet.stalled);
        CHECK(got == *want);
    }

    // --- soft reset reports the 8050/8250 DOS ---
    CHECK(PetIEEE_TestAccess::cmd(e, "UJ"));
    CHECK(PetIEEE_TestAccess::status(e).find(two ? "V2.7 8250" : "V2.7 8050") != std::string::npos);

    // --- SAVE: three sectors, off the BAM/directory tracks, listed and reloaded ---
    const auto prg = pattern_bytes(700, 5, 0x01, 0x04);
    CHECK(PetIEEE_TestAccess::save_prg(e, "NEWPRG", prg));
    CHECK_EQ(status_code(e), 0);
    CHECK(load_full(e, "NEWPRG", full)); CHECK(full == prg);
    {
        D64Catalog cat{};
        CHECK(PetIEEE_TestAccess::parse_bam(e, cat)); CHECK(PetIEEE_TestAccess::parse_dir(e, cat));
        int st = 0, ss = 0, blocks = 0;
        for (const auto& en : cat.entries) if (std::string(en.name) == "NEWPRG") { st = en.startT; ss = en.startS; blocks = en.sizeSectors; }
        const auto chain = chain_of(e, st, ss);
        CHECK_EQ((int)chain.size(), 3);
        CHECK_EQ(blocks, 3);
        for (const auto& ts : chain) CHECK(ts.first != 38 && ts.first != 39);
        for (const auto& ts : chain) CHECK(!PetIEEE_TestAccess::bam_free(e, ts.first, ts.second));
    }
    dir.clear();
    CHECK(PetIEEE_TestAccess::build_dir(e, dir, ""));
    CHECK_EQ(parse_blocks_free(dir), blankFree - 5 - 3);
    CHECK(read_file(path) == PetIEEE_TestAccess::image(e));       // flushed to the host file

    // --- replace keeps one entry and the same free count ---
    CHECK(PetIEEE_TestAccess::save_prg(e, "NEWPRG", hello));
    CHECK_EQ(count_named(e, "NEWPRG"), 1);
    CHECK(load_full(e, "NEWPRG", full)); CHECK(full == hello);
    dir.clear();
    CHECK(PetIEEE_TestAccess::build_dir(e, dir, ""));
    CHECK_EQ(parse_blocks_free(dir), blankFree - 5 - 3);

    // --- SEQ through OPEN/PRINT#/CLOSE, and a long file that must cross tracks ---
    PetIEEE_TestAccess::open_ch(e, 3, "LOG,S,W");
    PetIEEE_TestAccess::put_data(e, 3, vbytes("HELLO 8050"));
    PetIEEE_TestAccess::close_ch(e, 3);
    CHECK(PetIEEE_TestAccess::open_read(e, "LOG", 4, 1));
    CHECK(PetIEEE_TestAccess::stream_data(e, 4) == vbytes("HELLO 8050"));
    std::vector<uint8_t> big(12000);
    for (size_t i = 0; i < big.size(); ++i) big[i] = (uint8_t)((i * 31 + 5) & 0xFF);
    CHECK(PetIEEE_TestAccess::save_typed(e, "BIGSEQ", big, 0x81));
    CHECK(PetIEEE_TestAccess::open_read(e, "BIGSEQ", 5, 1));
    CHECK(PetIEEE_TestAccess::stream_data(e, 5) == big);
    {
        D64Catalog cat{};
        CHECK(PetIEEE_TestAccess::parse_bam(e, cat)); CHECK(PetIEEE_TestAccess::parse_dir(e, cat));
        for (const auto& en : cat.entries) if (std::string(en.name) == "BIGSEQ") {
            const auto chain = chain_of(e, en.startT, en.startS);
            CHECK_EQ((int)chain.size(), (int)((big.size() + 253) / 254));
            std::set<int> tracksUsed;
            for (const auto& ts : chain) { CHECK(ts.first != 38 && ts.first != 39); tracksUsed.insert(ts.first); }
            CHECK(tracksUsed.size() > 1);
        }
    }

    // --- RENAME, COPY, SCRATCH (frees the blocks again) ---
    CHECK(PetIEEE_TestAccess::cmd(e, "R:HI=HELLO"));
    CHECK_EQ(status_code(e), 0);
    CHECK(load_full(e, "HI", full)); CHECK(full == hello);
    CHECK(!load_full(e, "HELLO", full));
    CHECK(PetIEEE_TestAccess::cmd(e, "C:HI2=HI"));
    CHECK(load_full(e, "HI2", full)); CHECK(full == hello);
    dir.clear();
    CHECK(PetIEEE_TestAccess::build_dir(e, dir, ""));
    const int freeBefore = parse_blocks_free(dir);
    PetIEEE_TestAccess::cmd(e, "S:HI2,NEWPRG");
    CHECK(!load_full(e, "HI2", full)); CHECK(!load_full(e, "NEWPRG", full));
    dir.clear();
    CHECK(PetIEEE_TestAccess::build_dir(e, dir, ""));
    CHECK_EQ(parse_blocks_free(dir), freeBefore + 3 + 3);
    CHECK(load_full(e, "HI", full)); CHECK(full == hello);        // untouched

    // --- block commands: U1/U2 on the last track, B-A / B-F, illegal addresses ---
    PetIEEE_TestAccess::open_da(e, 8);
    CHECK(PetIEEE_TestAccess::cmd(e, "U1:8,0," + std::to_string(lastTrack) + ",5"));
    uint8_t* buf = PetIEEE_TestAccess::da_buf(e, 8);
    buf[0] = 0xDE; buf[10] = 0xAD; buf[255] = 0xEF;
    CHECK(PetIEEE_TestAccess::cmd(e, "U2:8,0," + std::to_string(lastTrack) + ",5"));
    CHECK_EQ(status_code(e), 0);
    CHECK(PetIEEE_TestAccess::read_sector(e, lastTrack, 5, sec));
    CHECK(sec[0] == 0xDE && sec[10] == 0xAD && sec[255] == 0xEF);
    { auto raw = read_file(path); CHECK(raw[soff8(lastTrack, 5)] == 0xDE && raw[soff8(lastTrack, 5) + 255] == 0xEF); }
    CHECK(PetIEEE_TestAccess::cmd(e, "U1:8,0," + std::to_string(lastTrack) + ",22"));
    CHECK(!PetIEEE_TestAccess::cmd(e, "U1:8,0," + std::to_string(lastTrack) + ",23"));
    CHECK_EQ(status_code(e), 66);
    CHECK(!PetIEEE_TestAccess::cmd(e, "U1:8,0," + std::to_string(lastTrack + 1) + ",0"));
    CHECK_EQ(status_code(e), 66);
    CHECK(!PetIEEE_TestAccess::cmd(e, "U1:8,0,1,29"));
    CHECK_EQ(status_code(e), 66);
    CHECK(PetIEEE_TestAccess::bam_free(e, 60, 24));
    CHECK(PetIEEE_TestAccess::cmd(e, "B-A:0,60,24"));
    CHECK(!PetIEEE_TestAccess::bam_free(e, 60, 24));
    CHECK(!PetIEEE_TestAccess::cmd(e, "B-A:0,60,24"));            // in use: 65,NO BLOCK
    CHECK_EQ(status_code(e), 65);
    if (two) {                                                     // a side-two block lives in the 38/6 or 38/9 BAM block
        CHECK(PetIEEE_TestAccess::cmd(e, "B-A:0,140,10"));
        CHECK(!PetIEEE_TestAccess::bam_free(e, 140, 10));
        CHECK(PetIEEE_TestAccess::cmd(e, "B-F:0,140,10"));
        CHECK(PetIEEE_TestAccess::bam_free(e, 140, 10));
    }

    // --- VALIDATE reclaims the orphaned block and keeps the files' blocks ---
    CHECK(PetIEEE_TestAccess::cmd(e, "V"));
    CHECK_EQ(status_code(e), 0);
    CHECK(PetIEEE_TestAccess::bam_free(e, 60, 24));
    CHECK(!PetIEEE_TestAccess::bam_free(e, 37, 0));
    CHECK(!PetIEEE_TestAccess::bam_free(e, farT, 22));
    CHECK(load_full(e, "SIDE2", full)); CHECK(full == side2);
    dir.clear();
    CHECK(PetIEEE_TestAccess::build_dir(e, dir, ""));
    CHECK_EQ(parse_blocks_free(dir), freeBefore + 6);

    // --- NEW: fresh header and BAM, files gone, image size kept, drive usable ---
    CHECK(PetIEEE_TestAccess::cmd(e, "N:FRESH,XY"));
    CHECK_EQ(status_code(e), 0);
    dir.clear();
    CHECK(PetIEEE_TestAccess::build_dir(e, dir, ""));
    CHECK(bytes_contain(dir, "\"FRESH") && bytes_contain(dir, "XY 2C"));
    CHECK(!bytes_contain(dir, "HELLO") && !bytes_contain(dir, "SIDE2"));
    CHECK_EQ(parse_blocks_free(dir), blankFree);
    CHECK_EQ((int)PetIEEE_TestAccess::image(e).size(), (int)img.size());
    {
        const auto raw = read_file(path);
        CHECK(raw == PetIEEE_TestAccess::image(e));
        const uint8_t* h = raw.data() + soff8(39, 0);
        CHECK(h[0] == 38 && h[1] == 0 && h[2] == 'C' && h[0x1B] == '2' && h[0x1C] == 'C');
        const uint8_t* b0 = raw.data() + soff8(38, 0);
        CHECK(b0[0] == 38 && b0[1] == 3 && b0[2] == 'C' && b0[4] == 1 && b0[5] == 51);
        CHECK_EQ(b0[6 + 37 * 5], 0);                                  // track 38 fully used
        CHECK_EQ(b0[6 + 38 * 5], 27);                                 // track 39: 39/0 and 39/1 used
        const uint8_t* bl = raw.data() + soff8(38, two ? 9 : 3);
        CHECK(bl[0] == 39 && bl[1] == 1);                             // last BAM block links to the directory
        CHECK_EQ((int)bl[5], two ? 155 : 78);
    }
    CHECK(PetIEEE_TestAccess::save_prg(e, "AGAIN", prg));
    CHECK(load_full(e, "AGAIN", full)); CHECK(full == prg);

    std::error_code ec; std::filesystem::remove(path, ec);
}

static void test_d80_d82() {
    run_d8x(false);
    run_d8x(true);
}

struct ScratchWorkingDirectory {
    std::filesystem::path previous, dir;
    ScratchWorkingDirectory() {
        previous = std::filesystem::current_path();
        dir = std::filesystem::temp_directory_path() /
            ("pet-d64-tests-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
        std::filesystem::create_directories(dir);
        std::filesystem::current_path(dir);
    }
    ~ScratchWorkingDirectory() {
        std::error_code ec;
        std::filesystem::current_path(previous, ec);
        std::filesystem::remove_all(dir, ec);
    }
};

int main(int argc, char** argv) {
    ScratchWorkingDirectory scratch;
    test_partial_read_hands_the_bus_back();
    test_host_load_save_folder();
    test_host_load_save_d64();
    test_unicode_host_directory();
    test_folder_data_files();
    test_atomic_image_write();
    test_mount_validation();
    test_40_track_geometry();
    test_overwrite_protection();
    test_stale_channels();
    test_folder_listing_matches_load();
    test_write_all_file_keeps_links();
    if (argc > 1 && std::string(argv[1]) == "--cycles") {
        test_cyclic_disk_chains_are_rejected();
        return g_fail ? 1 : 0;
    }
    test_directory_entry_offsets();
    test_host_paths_stay_in_mount();
    test_failed_disk_writes_are_unchanged();
    if (argc > 1 && std::string(argv[1]) == "--targeted") return g_fail ? 1 : 0;
    test_cyclic_disk_chains_are_rejected();
    test_block_commands();
    test_typed_open_and_block_alloc();
    test_d71_readwrite();
    test_d80_d82();
    test_open_write_modes();
    test_review_fixes();
    test_dir_d64_golden();
    test_dir_vdrive_format();
    test_lastsector_convention_and_roundtrip();
    test_alloc_covers_18sector_track();
    test_save_name_normalization();
    test_load_wildcard();
    test_dir_filter_and_blocks_free();
    test_cmd_rename();
    test_cmd_copy();
    test_cmd_new();
    test_cmd_validate();
    test_reset_keeps_mounts();
    test_eject_returns_to_vdrive();

    std::printf("\nD64 tests: %d checks, %d failures\n", g_checks, g_fail);
    return g_fail ? 1 : 0;
}
