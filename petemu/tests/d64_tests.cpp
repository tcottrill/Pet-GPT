// Standalone D64 backend tests. Compiles the PetIEEE disk sources alone.
// Covers the two silent-correctness fixes:
//   #1  d64_alloc_block reaches every sector on 18-sector tracks (25..30).
//   #2  last-sector byte[1] is the CBM "offset of last used byte" (count+1),
//       so save->load round-trips and a full final sector reads 0xFF.
#include <cstdio>
#include <cstdint>
#include <cstdlib>
#include <string>
#include <vector>
#include <set>
#include <fstream>
#include <iterator>
#include <filesystem>

#include "pet2001ieee.h"
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
};

static int status_code(PetIEEE& e) {
    const std::string s = PetIEEE_TestAccess::status(e);
    return (s.size() >= 2) ? std::atoi(s.substr(0, 2).c_str()) : -1;
}

// --- D64 geometry (local copy; mirrors PetIEEE::d64_sectors_per_track) ---
static int spt(int t) { if (t < 1) return 0; if (t <= 17) return 21; if (t <= 24) return 19; if (t <= 30) return 18; if (t <= 35) return 17; return 0; }
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

int main() {
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
