#include "mewnet_subs_internal.h"

#include <stdio.h>
#include <string.h>

static int decode_check(const char* input, const char* expected) {
    char output[256];
    memset(output, 0, sizeof(output));
    if (subs_b64_decode_string(input, output, sizeof(output)) != 0) {
        fprintf(stderr, "decode failed: %s\n", input);
        return 1;
    }
    if (strcmp(output, expected) != 0) {
        fprintf(stderr, "decode %s = %s, expected %s\n", input, output, expected);
        return 1;
    }
    return 0;
}

typedef struct {
    char output[256];
    size_t used;
} chunk_ctx;

static void chunk_out(void* user, const uint8_t* data, size_t len) {
    chunk_ctx* ctx = (chunk_ctx*)user;
    for (size_t i = 0; i < len && ctx->used + 1 < sizeof(ctx->output); i++) {
        ctx->output[ctx->used++] = (char)data[i];
    }
    ctx->output[ctx->used] = '\0';
}

static int chunk_test(void) {
    const char* input = "dmxlc3M6Ly8xMjM0NTY3OC1hYmNkLWVmMDAtMTIzNC01Njc4OWFiY2RlZjBAZXhhbXBsZS5jb206NDQzP3NlY3VyaXR5PXRscyN0ZXN0";
    const char* expected = "vless://12345678-abcd-ef00-1234-56789abcdef0@example.com:443?security=tls#test";
    for (size_t chunk = 1; chunk <= strlen(input); chunk++) {
        subs_b64 state;
        subs_b64_init(&state);
        chunk_ctx ctx;
        memset(&ctx, 0, sizeof(ctx));
        for (size_t offset = 0; offset < strlen(input); offset += chunk) {
            size_t len = strlen(input) - offset;
            if (len > chunk) len = chunk;
            if (subs_b64_feed(&state, (const uint8_t*)input + offset, len, chunk_out, &ctx) != 0) {
                fprintf(stderr, "chunked feed failed at chunk %zu\n", chunk);
                return 1;
            }
        }
        if (subs_b64_finish(&state, chunk_out, &ctx) != 0) {
            fprintf(stderr, "chunked finish failed at chunk %zu\n", chunk);
            return 1;
        }
        if (strcmp(ctx.output, expected) != 0) {
            fprintf(stderr, "chunk %zu output mismatch: %s\n", chunk, ctx.output);
            return 1;
        }
    }
    return 0;
}

int main(void) {
    int failures = 0;
    failures += decode_check("TQ==", "M");
    failures += decode_check("TWE=", "Ma");
    failures += decode_check("TWFu", "Man");
    failures += decode_check("SGVsbG8sIHdvcmxkIQ==", "Hello, world!");
    failures += decode_check("SGVsbG8sIHdvcmxkIQ", "Hello, world!");
    failures += decode_check("aGVsbG8td29ybGQ_", "hello-world?");
    failures += chunk_test();
    if (failures == 0) {
        printf("base64 tests passed\n");
    }
    return failures == 0 ? 0 : 1;
}
