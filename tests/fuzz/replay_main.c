/*
 * replay_main.c — runs saved fuzz inputs through the harness once each,
 * without libFuzzer, so regressions run in every build on every compiler.
 *
 * Linked with fuzz_decompress.c into fuzz_replay. Usage:
 *   ./fuzz_replay input1 [input2 ...]
 * Exits nonzero if an input can't be read. A harness failure aborts, as it
 * does under libFuzzer.
 */

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size);

static int replay(const char* path) {
    FILE* f = fopen(path, "rb");
    if (!f) {
        fprintf(stderr, "fuzz_replay: cannot open %s\n", path);
        return 1;
    }
    size_t cap = 4096, len = 0;
    uint8_t* buf = malloc(cap);
    size_t n;
    while (buf && (n = fread(buf + len, 1, cap - len, f)) > 0) {
        len += n;
        if (len == cap) {
            uint8_t* grown = realloc(buf, cap * 2);
            if (!grown) {
                free(buf);
                buf = NULL;
                break;
            }
            buf = grown;
            cap *= 2;
        }
    }
    int failed = !buf || ferror(f);
    fclose(f);
    if (failed) {
        fprintf(stderr, "fuzz_replay: cannot read %s\n", path);
        free(buf);
        return 1;
    }
    printf("%s (%zu bytes)\n", path, len);
    fflush(stdout);
    LLVMFuzzerTestOneInput(buf, len);
    free(buf);
    return 0;
}

int main(int argc, char** argv) {
    int failed = 0;
    for (int i = 1; i < argc; i++) failed |= replay(argv[i]);
    printf("replayed %d input(s)\n", argc - 1);
    return failed;
}
