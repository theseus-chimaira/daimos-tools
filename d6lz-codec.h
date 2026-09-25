#ifndef PDP10_TOOLS_D6LZ_CODEC_H
#define PDP10_TOOLS_D6LZ_CODEC_H

#include <stddef.h>
#include <stdint.h>

int d6lz_codec_compress(const uint64_t *in, size_t n,
    uint64_t **outp, size_t *outn);
int d6lz_codec_decompress(const uint64_t *in, size_t n,
    uint64_t *out, size_t outn);

#endif
