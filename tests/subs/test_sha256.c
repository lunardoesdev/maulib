#include "mewnet_subs_internal.h"

#include <stdio.h>
#include <string.h>

static void hex(const uint8_t* data, size_t len, char* out) {
    static const char* digits = "0123456789abcdef";
    for (size_t i = 0; i < len; i++) {
        out[i * 2] = digits[data[i] >> 4];
        out[i * 2 + 1] = digits[data[i] & 0x0f];
    }
    out[len * 2] = '\0';
}

static int check(const char* input, const char* expected) {
    uint8_t hash[32];
    char actual[65];
    subs_sha256(input, strlen(input), hash);
    hex(hash, sizeof(hash), actual);
    if (strcmp(actual, expected) != 0) {
        fprintf(stderr, "sha256(%s) = %s, expected %s\n", input, actual, expected);
        return 1;
    }
    return 0;
}

int main(void) {
    int failures = 0;
    failures += check("", "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
    failures += check("abc", "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    failures += check("The quick brown fox jumps over the lazy dog",
                      "d7a8fbb307d7809469ca9abcb0082e4f8d5651e46d3cdb762d02d0bf37c9e592");
    if (failures == 0) {
        printf("sha256 tests passed\n");
    }
    return failures == 0 ? 0 : 1;
}
