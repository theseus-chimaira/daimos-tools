#include "d6lz-codec.h"

#include <stdlib.h>

#define D6LZ_WORD_MASK UINT64_C(0777777777777)
#define D6LZ_WINDOW 128U
#define D6LZ_MIN 3U
#define D6LZ_MAX 130U
#define D6LZ_DESC_MASK UINT64_C(037777)

int
d6lz_codec_compress(const uint64_t *in, size_t n,
    uint64_t **outp, size_t *outn)
{
        size_t cap;
        size_t op;
        size_t pos;
        uint64_t *out;

        if (outp == NULL || outn == NULL || (n != 0U && in == NULL))
                return -1;
        cap = n + (n + 35U) / 36U + 1U;
        out = cap == 0U ? NULL : (uint64_t *)malloc(cap * sizeof(*out));
        if (cap != 0U && out == NULL)
                return -1;
        op = 0U;
        pos = 0U;
        while (pos < n) {
                size_t ctrl_pos;
                uint64_t control;
                unsigned int tok;

                ctrl_pos = op++;
                control = 0U;
                for (tok = 0U; tok < 36U && pos < n; ++tok) {
                        size_t best_len;
                        size_t best_dist;
                        size_t max_dist;
                        size_t dist;

                        best_len = 0U;
                        best_dist = 0U;
                        max_dist = pos < D6LZ_WINDOW ? pos : D6LZ_WINDOW;
                        for (dist = 1U; dist <= max_dist; ++dist) {
                                size_t len;
                                size_t max_len;

                                len = 0U;
                                max_len = n - pos;
                                if (max_len > D6LZ_MAX)
                                        max_len = D6LZ_MAX;
                                while (len < max_len &&
                                    in[pos + len] == in[pos + len - dist])
                                        ++len;
                                if (len >= D6LZ_MIN && len > best_len) {
                                        best_len = len;
                                        best_dist = dist;
                                        if (best_len == D6LZ_MAX)
                                                break;
                                }
                        }
                        if (best_len >= D6LZ_MIN) {
                                control |= UINT64_C(1) << (35U - tok);
                                out[op++] =
                                    ((uint64_t)(best_len - D6LZ_MIN) << 7) |
                                    (uint64_t)(best_dist - 1U);
                                pos += best_len;
                        } else {
                                if ((in[pos] & ~D6LZ_WORD_MASK) != 0U) {
                                        free(out);
                                        return -1;
                                }
                                out[op++] = in[pos++];
                        }
                }
                out[ctrl_pos] = control;
        }
        *outp = out;
        *outn = op;
        return 0;
}

int
d6lz_codec_decompress(const uint64_t *in, size_t n,
    uint64_t *out, size_t outn)
{
        size_t ip;
        size_t op;
        uint64_t control;
        unsigned int tokens;

        if ((n != 0U && in == NULL) || (outn != 0U && out == NULL))
                return -1;
        ip = 0U;
        op = 0U;
        control = 0U;
        tokens = 0U;
        while (op < outn) {
                uint64_t token;

                if (tokens == 0U) {
                        if (ip >= n)
                                return -1;
                        control = in[ip++];
                        tokens = 36U;
                }
                if (ip >= n)
                        return -1;
                token = in[ip++];
                if ((control & (UINT64_C(1) << 35)) != 0U) {
                        size_t distance;
                        size_t length;
                        size_t i;

                        if ((token & ~D6LZ_DESC_MASK) != 0U)
                                return -1;
                        distance = (size_t)(token & UINT64_C(0177)) + 1U;
                        length = (size_t)((token >> 7) & UINT64_C(0177)) +
                            D6LZ_MIN;
                        if (distance > op || length > outn - op)
                                return -1;
                        for (i = 0U; i < length; ++i) {
                                out[op] = out[op - distance];
                                ++op;
                        }
                } else {
                        if ((token & ~D6LZ_WORD_MASK) != 0U)
                                return -1;
                        out[op++] = token;
                }
                control = (control << 1) & D6LZ_WORD_MASK;
                --tokens;
        }
        return ip == n ? 0 : -1;
}
