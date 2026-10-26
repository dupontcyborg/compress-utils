// Differential test: our first-party Snappy core (src/algorithms/snappy/core)
// vs the reference google/snappy (C++), which is compiled here only, from
// third_party/snappy-oracle, and never ships.
//
// What it proves:
//   * the oracle's output decodes with ours to the original bytes;
//   * on every input, valid or not, ours and the oracle agree on accept or
//     reject and, when they accept, on every output byte. Inputs are the
//     oracle's own streams under bit flips, truncations, splices and header
//     edits, plus streams generated element by element with boundary offsets,
//     lengths and every legal (also non-minimal) encoding;
//   * ours never touches memory outside its buffers: input and output live
//     against PROT_NONE guard pages (POSIX), so any out-of-bounds access
//     faults without a sanitizer, and a canary checks the capacity slack.
//
// This links the core directly, not compress_utils_obj, so there is no symbol
// clash with the oracle's C++ `snappy::` namespace. SNAPPY_DIFF_ITERS scales
// the number of mutated/generated cases (default suits a CTest run).

#include <snappy.h>  // google/snappy C++ API (from third_party/snappy-oracle)

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "snappy_core.h"

#if defined(__unix__) || defined(__APPLE__)
#  include <sys/mman.h>
#  include <unistd.h>
#  define HAVE_GUARD_PAGES 1
#endif

namespace {

uint64_t g_st = 0x9E3779B97F4A7C15ULL;
uint64_t rnd() {
    g_st ^= g_st << 13;
    g_st ^= g_st >> 7;
    g_st ^= g_st << 17;
    return g_st;
}
uint32_t rnd32() { return static_cast<uint32_t>(rnd() >> 16); }
uint32_t below(uint32_t n) { return n ? rnd32() % n : 0; }

int g_pass = 0, g_fail = 0;
void fail(const char* what, const char* label, size_t n) {
    if (g_fail < 20) printf("  FAIL [%s] %s (input %zu bytes)\n", label, what, n);
    g_fail++;
}

// ---- guarded buffers ------------------------------------------------------
// An arena of kArena usable bytes between two PROT_NONE pages. A buffer is
// placed flush against the end (catches over-reads/writes) or the start
// (catches reads before the buffer: copy offsets reaching before the output).
constexpr size_t kArena = 8u << 20;

struct Arena {
    uint8_t* base = nullptr;  // first usable byte
    Arena() {
#ifdef HAVE_GUARD_PAGES
        size_t page = static_cast<size_t>(sysconf(_SC_PAGESIZE));
        size_t total = kArena + 2 * page;
        void* m = mmap(nullptr, total, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
        if (m == MAP_FAILED) { perror("mmap"); exit(2); }
        mprotect(m, page, PROT_NONE);
        mprotect(static_cast<uint8_t*>(m) + page + kArena, page, PROT_NONE);
        base = static_cast<uint8_t*>(m) + page;
#else
        base = static_cast<uint8_t*>(malloc(kArena));
#endif
    }
    uint8_t* place(size_t n, bool at_end) const {
        if (n > kArena) { fprintf(stderr, "arena too small\n"); exit(2); }
        return at_end ? base + kArena - n : base;
    }
};

Arena g_in, g_out;

// ---- oracle / ours --------------------------------------------------------
struct Result {
    bool ok = false;
    std::string bytes;
};

Result oracle_decode(const std::string& in, size_t max_len) {
    Result r;
    size_t ul = 0;
    if (!snappy::GetUncompressedLength(in.data(), in.size(), &ul)) return r;
    if (ul > max_len) return r;  // callers screen these separately
    r.bytes.resize(ul);
    char none = 0;  // never a null destination, even for an empty block
    r.ok = snappy::RawUncompress(in.data(), in.size(), ul ? &r.bytes[0] : &none);
    if (!r.ok) r.bytes.clear();
    return r;
}

// Decode with the exact capacity (so the output sits flush against a guard
// page when at_end) plus, optionally, a canary check with extra capacity.
Result ours_decode(const std::string& in, size_t ul, bool in_at_end, bool out_at_end) {
    Result r;
    uint8_t* ib = g_in.place(in.size(), in_at_end);
    memcpy(ib, in.data(), in.size());
    uint8_t* ob = g_out.place(ul, out_at_end);
    size_t n = 0;
    snappy_core_status_t st = snappy_core_decode(ib, in.size(), ob, ul, &n);
    r.ok = st == SNAPPY_CORE_OK;
    if (r.ok) {
        if (n != ul) { r.ok = false; fail("length mismatch on success", "ours", in.size()); }
        else r.bytes.assign(reinterpret_cast<char*>(ob), ul);
    }
    return r;
}

// Extra capacity must never be written past the declared length.
bool canary_ok(const std::string& in, size_t ul) {
    constexpr size_t kSlack = 160;
    std::vector<uint8_t> in_copy(in.begin(), in.end());
    std::vector<uint8_t> out(ul + kSlack, 0xA5);
    size_t n = 0;
    snappy_core_status_t st = snappy_core_decode(in_copy.data(), in_copy.size(), out.data(), out.size(), &n);
    for (size_t i = ul; i < out.size(); i++)
        if (out[i] != 0xA5) return false;
    (void)st;
    return true;
}

// One differential case. `label` names the generator for failure reports.
void diff_case(const std::string& in, const char* label) {
    size_t ul = 0;
    bool o_len = snappy::GetUncompressedLength(in.data(), in.size(), &ul);
    size_t our_len = 0;
    snappy_core_status_t ls = snappy_core_uncompressed_length(
        reinterpret_cast<const uint8_t*>(in.data()), in.size(), &our_len);
    if (o_len != (ls == SNAPPY_CORE_OK) || (o_len && ul != our_len)) {
        fail("header disagreement", label, in.size());
        return;
    }
    if (!o_len) {
        // Ours must reject too (any capacity).
        uint8_t dummy[8];
        size_t n;
        if (snappy_core_decode(reinterpret_cast<const uint8_t*>(in.data()), in.size(), dummy, sizeof dummy, &n) ==
            SNAPPY_CORE_OK)
            fail("accepted a bad header", label, in.size());
        g_pass++;
        return;
    }
    constexpr size_t kMax = 6u << 20;
    if (ul > kMax) {
        // Too big to run the oracle on; ours must refuse a small capacity.
        uint8_t dummy[8];
        size_t n;
        if (snappy_core_decode(reinterpret_cast<const uint8_t*>(in.data()), in.size(), dummy, sizeof dummy, &n) !=
            SNAPPY_CORE_OUTPUT_FULL)
            fail("did not report OUTPUT_FULL", label, in.size());
        g_pass++;
        return;
    }
    Result ref = oracle_decode(in, kMax);
    Result got = ours_decode(in, ul, rnd() & 1, rnd() & 1);
    if (ref.ok != got.ok) fail(ref.ok ? "rejected a valid block" : "accepted an invalid block", label, in.size());
    else if (ref.ok && ref.bytes != got.bytes) fail("output differs", label, in.size());
    else if (!canary_ok(in, ul)) fail("wrote past the declared length", label, in.size());
    else g_pass++;
    // A capacity below the declared length is always refused, nothing written.
    if (ul > 0) {
        std::vector<uint8_t> small(ul - 1 + 1);
        size_t n = 0;
        if (snappy_core_decode(reinterpret_cast<const uint8_t*>(in.data()), in.size(), small.data(), ul - 1, &n) !=
            SNAPPY_CORE_OUTPUT_FULL)
            fail("capacity below declared length not refused", label, in.size());
    }
}

std::string oracle_compress(const std::string& d);

// Outbound: our compress, with the input and the output (sized to exactly the
// documented bound) against guard pages, then the oracle and our decoder must
// both return the original, and the size must hold up against the oracle's.
void encode_case(const std::string& data, const char* label) {
    size_t bound = snappy_core_max_compressed_length(data.size());
    if (bound != snappy::MaxCompressedLength(data.size())) { fail("bound differs from the oracle's", label, data.size()); return; }
    bool in_end = rnd() & 1, out_end = rnd() & 1;
    uint8_t* ib = g_in.place(data.size(), in_end);
    memcpy(ib, data.data(), data.size());
    uint8_t* ob = g_out.place(bound, out_end);
    size_t n = 0;
    snappy_core_status_t st = snappy_core_encode(ib, data.size(), ob, bound, &n);
    if (st != SNAPPY_CORE_OK) { fail("encode failed", label, data.size()); return; }
    if (n > bound) { fail("output exceeds the bound", label, data.size()); return; }
    std::string comp(reinterpret_cast<char*>(ob), n);
    std::string back;
    if (!snappy::Uncompress(comp.data(), comp.size(), &back) || back != data) {
        fail("the oracle could not decode our output", label, data.size());
        return;
    }
    Result mine = ours_decode(comp, data.size(), rnd() & 1, rnd() & 1);
    if (!mine.ok || mine.bytes != data) { fail("our decoder could not decode our output", label, data.size()); return; }
    std::string ref = oracle_compress(data);
    // Match quality: within 3% (+ a small constant) of the oracle's size.
    if (comp.size() > ref.size() + ref.size() / 33 + 32) {
        fail("compressed size well above the oracle's", label, data.size());
        return;
    }
    // A capacity below the bound is refused.
    if (bound > 0 && snappy_core_encode(ib, data.size(), ob, bound - 1, &n) != SNAPPY_CORE_OUTPUT_FULL)
        fail("capacity below the bound not refused", label, data.size());
    g_pass++;
}

// ---- inputs ---------------------------------------------------------------
std::string gen_data(size_t n, int mode) {
    std::string b(n, '\0');
    for (size_t j = 0; j < n; j++) {
        switch (mode) {
            case 0: b[j] = static_cast<char>(rnd()); break;
            case 1: b[j] = static_cast<char>(j & 0x0F); break;
            case 2: b[j] = static_cast<char>(below(32) ? 'x' : rnd()); break;
            case 3: b[j] = static_cast<char>("abcdefgh"[(j + (j >> 5)) % 8]); break;
            case 4: b[j] = 0; break;
            case 5: {  // text-ish with repeats
                static const char* w[] = {"the ", "quick ", "brown ", "fox ", "jumps ", "over ", "lazy ", "dog\n", "snappy "};
                const char* s = w[below(9)];
                size_t k = 0;
                while (s[k] && j < n) b[j++] = s[k++];
                j--;
                break;
            }
            default: b[j] = static_cast<char>(j % 251); break;
        }
    }
    return b;
}

std::string oracle_compress(const std::string& d) {
    std::string c;
    snappy::Compress(d.data(), d.size(), &c);
    return c;
}

void put_varint(std::string& s, uint64_t v) {
    while (v >= 0x80) { s.push_back(static_cast<char>(v | 0x80)); v >>= 7; }
    s.push_back(static_cast<char>(v));
}

// ---- element-by-element stream generator -----------------------------------
// Emits literal/copy elements in every legal encoding, with offsets and
// lengths drawn from valid values and from just-out-of-range ones.
struct Gen {
    std::string body;
    std::string produced;  // what a correct decoder would have produced so far
    bool valid = true;

    void literal(size_t len, int enc) {  // enc: 0 minimal, 1..4 force N length bytes
        if (len == 0) return;
        uint64_t l = len - 1;
        int need = l < 60 ? 0 : l < 256 ? 1 : l < 65536 ? 2 : l < (1u << 24) ? 3 : 4;
        int nb = enc > need ? enc : need;
        if (nb == 0) {
            body.push_back(static_cast<char>(l << 2));
        } else {
            body.push_back(static_cast<char>((59 + nb) << 2));
            for (int i = 0; i < nb; i++) body.push_back(static_cast<char>(l >> (8 * i)));
        }
        for (size_t i = 0; i < len; i++) {
            char ch = static_cast<char>(rnd());
            body.push_back(ch);
            produced.push_back(ch);
        }
    }
    // type 1, 2 or 4 (the tag's 2-bit type is 1, 2, 3).
    void copy(int type, uint64_t offset, size_t len) {
        size_t have = produced.size();
        bool legal = offset >= 1 && offset <= have;
        if (!legal) valid = false;
        if (type == 1) {
            len = len < 4 ? 4 : len > 11 ? 11 : len;
            offset &= 2047;
            body.push_back(static_cast<char>(1 | ((len - 4) << 2) | ((offset >> 8) << 5)));
            body.push_back(static_cast<char>(offset));
        } else if (type == 2) {
            len = len < 1 ? 1 : len > 64 ? 64 : len;
            offset &= 0xFFFF;
            body.push_back(static_cast<char>(2 | ((len - 1) << 2)));
            body.push_back(static_cast<char>(offset));
            body.push_back(static_cast<char>(offset >> 8));
        } else {
            len = len < 1 ? 1 : len > 64 ? 64 : len;
            offset &= 0xFFFFFFFFu;
            body.push_back(static_cast<char>(3 | ((len - 1) << 2)));
            for (int i = 0; i < 4; i++) body.push_back(static_cast<char>(offset >> (8 * i)));
        }
        if (offset == 0 || offset > have) { valid = false; return; }
        for (size_t i = 0; i < len; i++) produced.push_back(produced[produced.size() - offset]);
    }
};

std::string gen_stream(int* kind) {
    Gen g;
    int elems = 1 + below(40);
    size_t big = below(8) == 0;
    bool adversarial = below(3) == 0;  // only then: out-of-range offsets
    for (int i = 0; i < elems; i++) {
        if (g.produced.empty() || below(3) == 0) {
            static const size_t lens[] = {1, 2, 5, 15, 16, 17, 31, 32, 33, 59, 60, 61, 62, 63, 64, 65, 100, 255, 256, 257, 1000, 65535, 65536, 70000};
            size_t len = below(4) ? 1 + below(70) : lens[below(sizeof lens / sizeof *lens)];
            if (!big && len > 5000) len = 5000;
            g.literal(len, below(4) == 0 ? 1 + below(4) : 0);
        } else {
            size_t have = g.produced.size();
            uint64_t off;
            uint32_t pick = below(adversarial ? 14 : 10);
            switch (pick) {
                case 0: off = 1; break;
                case 1: off = have; break;
                case 2: off = 1 + below(16); break;  // short, overlapping
                case 3: off = 1 + below(64); break;
                case 10: off = have + 1; break;      // just out of range
                case 11: off = 0; break;
                case 12: off = rnd() & 0xFFFFFFFFu; break;
                case 13: off = have + below(100); break;
                default: off = 1 + below(static_cast<uint32_t>(have)); break;
            }
            if (off > have && !adversarial) off = have;
            int type = 1 + below(3);
            if (type == 1 && off > 2047 && below(2)) type = 2;
            if (type == 2 && off > 65535 && below(2)) type = 3;
            g.copy(type, off, 1 + below(66));
            if (!g.valid && below(3) == 0) break;
        }
    }
    std::string out;
    size_t declared = g.produced.size();
    switch (adversarial ? below(6) : 99) {
        case 0: declared += 1 + below(3); g.valid = false; break;
        case 1: if (declared) { declared -= 1 + below(declared < 3 ? declared : 3); g.valid = false; } break;
        default: break;
    }
    put_varint(out, declared);
    // Non-canonical varint: pad with a redundant continuation byte.
    if (below(30) == 0 && !out.empty()) {
        out.back() = static_cast<char>(out.back() | 0x80);
        out.push_back(0);
    }
    out += g.body;
    if (below(25) == 0) out.push_back(static_cast<char>(rnd()));  // trailing garbage
    *kind = g.valid ? 1 : 0;
    return out;
}

// ---- mutation ---------------------------------------------------------------
std::string mutate(std::string s, const std::string& other) {
    int ops = 1 + below(3);
    while (ops--) {
        if (s.empty()) { s.push_back(static_cast<char>(rnd())); continue; }
        switch (below(9)) {
            case 0: s[below(s.size())] ^= static_cast<char>(1u << below(8)); break;
            case 1: s[below(s.size())] = static_cast<char>(rnd()); break;
            case 2: s.resize(below(s.size() + 1)); break;  // truncate
            case 3: s.insert(below(s.size() + 1), 1, static_cast<char>(rnd())); break;
            case 4: s.erase(below(s.size()), 1); break;
            case 5: {  // splice a chunk of another stream
                if (other.empty()) break;
                size_t a = below(other.size()), n = 1 + below(16);
                s.insert(below(s.size() + 1), other.substr(a, n));
                break;
            }
            case 6: s[0] = static_cast<char>(rnd()); break;  // header byte
            case 7: s += static_cast<char>(rnd()); break;
            default: {  // flip within the first 8 bytes (header/first tag)
                size_t i = below(s.size() < 8 ? static_cast<uint32_t>(s.size()) : 8);
                s[i] ^= static_cast<char>(1u << below(8));
            }
        }
    }
    return s;
}

}  // namespace

int main() {
    const char* e = getenv("SNAPPY_DIFF_ITERS");
    long iters = e ? atol(e) : 60000;

    // Inbound: oracle compress -> our decode, on structured and random data.
    for (int i = 0; i < 700; i++) {
        size_t n = below(4) == 0 ? below(300) : below(200000);
        if (i < 16) n = i;  // tiny sizes, incl. 0
        std::string d = gen_data(n, i % 7);
        std::string c = oracle_compress(d);
        Result got = ours_decode(c, d.size(), i & 1, (i >> 1) & 1);
        if (!got.ok || got.bytes != d) fail("could not decode the oracle's output", "inbound", c.size());
        else if (!canary_ok(c, d.size())) fail("wrote past the declared length", "inbound", c.size());
        else g_pass++;
    }
    int inbound_fail = g_fail;

    // Outbound: every length 0..1100 in every data mode, the 64 KiB fragment
    // boundaries, and larger random/structured inputs.
    for (size_t n = 0; n <= 1100; n++)
        for (int mode = 0; mode < 7; mode++) encode_case(gen_data(n, mode), "outbound-small");
    {
        static const size_t sizes[] = {65534, 65535, 65536, 65537, 65551, 65552, 131071, 131072, 131073, 196608, 200000, 300001};
        for (size_t n : sizes)
            for (int mode = 0; mode < 7; mode++) encode_case(gen_data(n, mode), "outbound-boundary");
    }
    for (long i = 0; i < iters / 100; i++) {
        size_t n = below(4) == 0 ? below(300) : below(400000);
        encode_case(gen_data(n, below(7)), "outbound-random");
    }
    int outbound_fail = g_fail - inbound_fail;

    // Generated element streams (some valid, many deliberately invalid).
    int valid_seen = 0, invalid_seen = 0;
    for (long i = 0; i < iters; i++) {
        int kind;
        std::string s = gen_stream(&kind);
        (kind ? valid_seen : invalid_seen)++;
        diff_case(s, "generated");
    }

    // Mutated oracle streams.
    std::vector<std::string> pool;
    for (int i = 0; i < 64; i++) pool.push_back(oracle_compress(gen_data(below(3000) + 1, i % 7)));
    for (long i = 0; i < iters; i++) {
        const std::string& base = pool[below(pool.size())];
        diff_case(mutate(base, pool[below(pool.size())]), "mutated");
    }

    // Every 1-2 byte stream and sampled 5-byte headers: varint rules.
    for (int a = 0; a < 256; a++) {
        std::string s(1, static_cast<char>(a));
        diff_case(s, "header1");
        for (int b = 0; b < 256; b++) {
            std::string t = s + static_cast<char>(b);
            diff_case(t, "header2");
        }
    }
    for (int i = 0; i < 20000; i++) {
        std::string s;
        size_t n = 1 + below(6);
        for (size_t k = 0; k < n; k++) s.push_back(static_cast<char>(rnd()));
        diff_case(s, "header-rand");
    }
    diff_case("", "empty");

    printf("snappy core differential: %d passed, %d failed (%d inbound, %d outbound failures); generated %d valid / %d invalid\n",
           g_pass, g_fail, inbound_fail, outbound_fail, valid_seen, invalid_seen);
    return g_fail ? 1 : 0;
}
