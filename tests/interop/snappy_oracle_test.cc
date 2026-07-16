// Differential test: our pure-C Snappy (andikleen/snappy-c, via the cu_* C ABI)
// vs the reference google/snappy (C++), which is vendored at
// third_party/snappy-oracle FOR THIS TEST ONLY and never ships in the release
// library. Proves the wire formats are mutually decodable in both directions —
// the check that guards against a wire-format divergence after the codec swap.
//
// Uses google's C++ API (snappy::Compress/Uncompress, namespaced) rather than
// its C API, so there is no symbol clash with andikleen's C `snappy_*` symbols
// that are linked into compress_utils_obj.

#include <snappy.h>  // google/snappy C++ API (from third_party/snappy-oracle)

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>

#include "compress_utils.h"  // our C ABI

namespace {

uint64_t g_st = 0x243F6A8885A308D3ULL;
uint8_t rb() {
    g_st = g_st * 6364136223846793005ULL + 1442695040888963407ULL;
    return static_cast<uint8_t>(g_st >> 33);
}

int g_pass = 0, g_fail = 0;

// our compress via the C ABI
bool cu_snappy_compress(const std::string& in, std::string& out) {
    size_t cap = cu_compress_bound(in.size(), CU_ALGO_SNAPPY);
    out.resize(cap);
    size_t olen = cap;
    cu_status_t s = cu_compress(CU_ALGO_SNAPPY,
                                reinterpret_cast<const uint8_t*>(in.data()), in.size(),
                                reinterpret_cast<uint8_t*>(&out[0]), &olen, 5);
    if (s != CU_OK) return false;
    out.resize(olen);
    return true;
}

// our decompress via the C ABI (size-hint + one-shot)
bool cu_snappy_decompress(const std::string& in, std::string& out) {
    size_t need = 0;
    if (cu_decompress_size_hint(CU_ALGO_SNAPPY,
                                reinterpret_cast<const uint8_t*>(in.data()), in.size(),
                                &need) != CU_OK)
        return false;
    out.resize(need);
    size_t olen = need;
    uint8_t dummy;
    uint8_t* dst = need ? reinterpret_cast<uint8_t*>(&out[0]) : &dummy;
    cu_status_t s = cu_decompress(CU_ALGO_SNAPPY,
                                  reinterpret_cast<const uint8_t*>(in.data()), in.size(),
                                  dst, &olen);
    if (s != CU_OK) return false;
    out.resize(olen);
    return true;
}

void check(const std::string& data, const char* label) {
    bool ok = true;
    std::string ours, ref;

    // outbound: our compress -> google decode
    if (!cu_snappy_compress(data, ours)) { printf("  [%s] our compress FAILED\n", label); ok = false; }
    else {
        std::string dec;
        if (!snappy::Uncompress(ours.data(), ours.size(), &dec) || dec != data) {
            printf("  [%s] outbound FAILED (google could not decode our output)\n", label);
            ok = false;
        }
    }

    // inbound: google compress -> our decode
    snappy::Compress(data.data(), data.size(), &ref);
    std::string dec2;
    if (!cu_snappy_decompress(ref, dec2) || dec2 != data) {
        printf("  [%s] inbound FAILED (we could not decode google's output)\n", label);
        ok = false;
    }

    if (ok) g_pass++; else g_fail++;
}

}  // namespace

int main() {
    check("", "empty");
    check("A", "one");
    check("the quick brown fox jumps over the lazy dog", "text");
    check(std::string(200000, 'Z'), "runZ");

    for (int i = 0; i < 500; i++) {
        size_t n = (static_cast<size_t>(rb()) | (static_cast<size_t>(rb()) << 8)) % 60000u;
        std::string b(n, '\0');
        int mode = i % 3;
        for (size_t j = 0; j < n; j++)
            b[j] = mode == 0 ? static_cast<char>(rb())
                 : mode == 1 ? static_cast<char>(j & 0x0F)
                             : static_cast<char>(rb() < 32 ? rb() : 'x');
        char lbl[32];
        snprintf(lbl, sizeof lbl, "rand#%d(%zu)", i, n);
        check(b, lbl);
    }

    printf("snappy oracle differential: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
