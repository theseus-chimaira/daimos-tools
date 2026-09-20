#include "tsfs-format.h"

#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>

uint64_t
tsfs_sixbit_word(const char text[6])
{
        uint64_t word;
        unsigned int i;
        unsigned int ch;

        word = 0;
        for (i = 0; i < 6U; ++i) {
                ch = (unsigned char)text[i];
                if (ch < 040U || ch > 0137U)
                        return TSFS_WORD_MASK;
                word = (word << 6) | (uint64_t)(ch - 040U);
        }
        return word & TSFS_WORD_MASK;
}

uint64_t
tsfs_checksum36(const uint64_t *words, size_t count, size_t zero_word)
{
        uint64_t sum;
        uint64_t word;
        size_t i;

        sum = 0;
        for (i = 0; i < count; ++i) {
                word = i == zero_word ? 0 : words[i] & TSFS_WORD_MASK;
                sum = ((sum << 1) | (sum >> 35)) & TSFS_WORD_MASK;
                sum ^= word;
                sum = (sum + 1) & TSFS_WORD_MASK;
        }
        return sum;
}

static int
read_u32(FILE *f, unsigned long *vp)
{
        unsigned char b[4];

        if (fread(b, 1, sizeof(b), f) != sizeof(b))
                return -1;
        *vp = (unsigned long)b[0] |
            ((unsigned long)b[1] << 8) |
            ((unsigned long)b[2] << 16) |
            ((unsigned long)b[3] << 24);
        return 0;
}

static int
write_u32(FILE *f, unsigned long value)
{
        unsigned char b[4];

        b[0] = (unsigned char)(value & 0xffUL);
        b[1] = (unsigned char)((value >> 8) & 0xffUL);
        b[2] = (unsigned char)((value >> 16) & 0xffUL);
        b[3] = (unsigned char)((value >> 24) & 0xffUL);
        return fwrite(b, 1, sizeof(b), f) == sizeof(b) ? 0 : -1;
}

int
tsfs_read_image_block(const char *path, unsigned int block,
    uint64_t words[TSFS_BLOCK_WORDS])
{
        FILE *f;
        uint64_t offset;
        unsigned long hi;
        unsigned long lo;
        unsigned int i;

        if (path == NULL || words == NULL || block >= TSFS_BLOCK_COUNT)
                return -1;
        f = fopen(path, "rb");
        if (f == NULL)
                return -1;
        offset = (uint64_t)block * TSFS_BLOCK_WORDS * 8U;
        if (offset > (uint64_t)LONG_MAX || fseek(f, (long)offset, SEEK_SET) != 0) {
                fclose(f);
                return -1;
        }
        for (i = 0; i < TSFS_BLOCK_WORDS; ++i) {
                if (read_u32(f, &hi) != 0 || read_u32(f, &lo) != 0 ||
                    (hi & ~0777777UL) != 0 || (lo & ~0777777UL) != 0) {
                        fclose(f);
                        return -1;
                }
                words[i] = ((uint64_t)hi << 18) | (uint64_t)lo;
        }
        fclose(f);
        return 0;
}

int
tsfs_write_image(const char *path, const uint64_t *words, size_t word_count)
{
        FILE *f;
        size_t i;
        uint64_t word;

        if (path == NULL || words == NULL)
                return -1;
        f = fopen(path, "wb");
        if (f == NULL)
                return -1;
        for (i = 0; i < word_count; ++i) {
                word = words[i] & TSFS_WORD_MASK;
                if (write_u32(f, (unsigned long)((word >> 18) & TSFS_HALF_MASK)) != 0 ||
                    write_u32(f, (unsigned long)(word & TSFS_HALF_MASK)) != 0) {
                        fclose(f);
                        return -1;
                }
        }
        if (fclose(f) == EOF)
                return -1;
        return 0;
}
