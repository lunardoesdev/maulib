#include "milib_subs_internal.h"

#include <ctype.h>
#include <string.h>

static int b64_value(uint8_t c) {
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+' || c == '-') return 62;
    if (c == '/' || c == '_') return 63;
    return -1;
}

void subs_b64_init(subs_b64* state) {
    memset(state, 0, sizeof(*state));
}

static void b64_emit(subs_b64* state, subs_b64_out out, void* user) {
    uint32_t value = ((uint32_t)state->q[0] << 18) |
                     ((uint32_t)state->q[1] << 12) |
                     ((uint32_t)state->q[2] << 6) |
                     (uint32_t)state->q[3];
    uint8_t bytes[3] = {
        (uint8_t)(value >> 16),
        (uint8_t)(value >> 8),
        (uint8_t)value,
    };
    int emit = 3;
    if (state->pad == 1) {
        emit = 2;
    } else if (state->pad == 2) {
        emit = 1;
    }
    if (emit > 0 && out != NULL) {
        out(user, bytes, (size_t)emit);
    }
    int had_padding = state->pad > 0;
    state->n = 0;
    state->pad = 0;
    if (had_padding) {
        state->finished = 1;
    }
}

int subs_b64_feed(subs_b64* state, const uint8_t* data, size_t len, subs_b64_out out, void* user) {
    if (state->finished) {
        for (size_t i = 0; i < len; i++) {
            if (!isspace(data[i])) {
                return -1;
            }
        }
        return 0;
    }
    for (size_t i = 0; i < len; i++) {
        uint8_t c = data[i];
        if (isspace(c)) {
            continue;
        }
        if (c == '=') {
            if (state->n < 2) {
                return -1;
            }
            state->pad++;
            if (state->pad > 2) {
                return -1;
            }
            state->q[state->n++] = 0;
        } else {
            if (state->pad > 0) {
                return -1;
            }
            int value = b64_value(c);
            if (value < 0) {
                return -1;
            }
            state->q[state->n++] = (uint8_t)value;
        }
        if (state->n == 4) {
            b64_emit(state, out, user);
        }
    }
    return 0;
}

int subs_b64_finish(subs_b64* state, subs_b64_out out, void* user) {
    if (state->finished) {
        return 0;
    }
    state->finished = 1;
    if (state->n == 0) {
        return 0;
    }
    if (state->pad > 0) {
        return -1;
    }
    if (state->n == 2) {
        uint8_t byte = (uint8_t)((state->q[0] << 2) | (state->q[1] >> 4));
        if (out != NULL) out(user, &byte, 1);
        state->n = 0;
        return 0;
    }
    if (state->n == 3) {
        uint8_t bytes[2] = {
            (uint8_t)((state->q[0] << 2) | (state->q[1] >> 4)),
            (uint8_t)((state->q[1] << 4) | (state->q[2] >> 2)),
        };
        if (out != NULL) out(user, bytes, 2);
        state->n = 0;
        return 0;
    }
    return -1;
}

typedef struct b64_string_ctx {
    char* output;
    size_t size;
    size_t used;
    int truncated;
} b64_string_ctx;

static void b64_string_out(void* user, const uint8_t* data, size_t len) {
    b64_string_ctx* ctx = (b64_string_ctx*)user;
    for (size_t i = 0; i < len; i++) {
        if (ctx->used + 1 < ctx->size) {
            ctx->output[ctx->used++] = (char)data[i];
        } else {
            ctx->truncated = 1;
        }
    }
}

int subs_b64_decode_string(const char* input, char* output, size_t output_size) {
    if (output_size == 0) {
        return -1;
    }
    subs_b64 state;
    subs_b64_init(&state);
    b64_string_ctx ctx = {output, output_size, 0, 0};
    if (subs_b64_feed(&state, (const uint8_t*)input, strlen(input), b64_string_out, &ctx) != 0) {
        output[0] = '\0';
        return -1;
    }
    if (subs_b64_finish(&state, b64_string_out, &ctx) != 0) {
        output[0] = '\0';
        return -1;
    }
    output[ctx.used] = '\0';
    return ctx.truncated ? 1 : 0;
}
