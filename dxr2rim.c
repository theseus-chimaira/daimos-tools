/* dxr2rim.c - convert DAIMOS DXR V1 to PDP-6 RIM tape. */

#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>

#define WORD_MASK 0777777777777UL
#define HALF_MASK 0777777UL
#define DXR_MAGIC 0447062UL
#define DATAI_PTR 0710440000000UL
#define JRST_WORD 0254000000000UL

static void
usage(const char *name)
{
        fprintf(stderr, "usage: %s -b base input.dxr output.pt\n", name);
}

static int
parse_octal(const char *s, unsigned long *value)
{
        char *end;
        unsigned long v;

        errno = 0;
        v = strtoul(s, &end, 8);
        if (errno != 0 || end == s || *end != '\0' || v > HALF_MASK)
                return 0;
        *value = v;
        return 1;
}

static int
read_word(FILE *in, unsigned long *word)
{
        unsigned long v;
        int i;
        int ch;

        v = 0;
        for (i = 0; i < 8; i++) {
                ch = getc(in);
                if (ch == EOF) {
                        if (i == 0)
                                return 0;
                        fprintf(stderr, "dxr2rim: truncated DXR word\n");
                        return -1;
                }
                v |= ((unsigned long)(unsigned char)ch) << (i * 8);
        }
        if ((v & ~WORD_MASK) != 0) {
                fprintf(stderr, "dxr2rim: invalid DXR container word\n");
                return -1;
        }
        *word = v;
        return 1;
}

static int
write_word(FILE *out, unsigned long word)
{
        int shift;

        word &= WORD_MASK;
        for (shift = 30; shift >= 0; shift -= 6) {
                if (putc((int)((word >> shift) & 077UL) | 0200, out) == EOF)
                        return -1;
        }
        return 0;
}

int
main(int argc, char **argv)
{
        FILE *in;
        FILE *out;
        unsigned long *image;
        unsigned long *reloc;
        unsigned long base;
        unsigned long header0;
        unsigned long header1;
        unsigned long entry;
        unsigned long image_words;
        unsigned long bss_words;
        unsigned long reloc_words;
        unsigned long i;
        unsigned long bit;
        int rc;

        if (sizeof(unsigned long) < 8) {
                fprintf(stderr, "dxr2rim: 64-bit unsigned long required\n");
                return 2;
        }
        if (argc != 5 || argv[1][0] != '-' || argv[1][1] != 'b' ||
            argv[1][2] != '\0' || !parse_octal(argv[2], &base)) {
                usage(argv[0]);
                return 2;
        }
        in = fopen(argv[3], "rb");
        if (in == NULL) {
                perror(argv[3]);
                return 1;
        }
        rc = read_word(in, &header0);
        if (rc != 1 || read_word(in, &header1) != 1) {
                fprintf(stderr, "%s: short DXR header\n", argv[3]);
                fclose(in);
                return 1;
        }
        if (((header0 >> 18) & HALF_MASK) != DXR_MAGIC) {
                fprintf(stderr, "%s: bad DXR magic\n", argv[3]);
                fclose(in);
                return 1;
        }
        entry = header0 & HALF_MASK;
        image_words = (header1 >> 18) & HALF_MASK;
        bss_words = header1 & HALF_MASK;
        (void)bss_words;
        if (image_words == 0 || entry >= image_words ||
            base + image_words > HALF_MASK + 1UL) {
                fprintf(stderr, "%s: invalid DXR image bounds\n", argv[3]);
                fclose(in);
                return 1;
        }
        reloc_words = (image_words + 35UL) / 36UL;
        image = (unsigned long *)malloc(image_words * sizeof(*image));
        reloc = (unsigned long *)malloc(reloc_words * sizeof(*reloc));
        if (image == NULL || reloc == NULL) {
                fprintf(stderr, "dxr2rim: out of memory\n");
                free(image);
                free(reloc);
                fclose(in);
                return 2;
        }
        for (i = 0; i < image_words; i++) {
                if (read_word(in, &image[i]) != 1) {
                        fprintf(stderr, "%s: truncated DXR image\n", argv[3]);
                        free(image);
                        free(reloc);
                        fclose(in);
                        return 1;
                }
        }
        for (i = 0; i < reloc_words; i++) {
                if (read_word(in, &reloc[i]) != 1) {
                        fprintf(stderr, "%s: truncated DXR relocation map\n", argv[3]);
                        free(image);
                        free(reloc);
                        fclose(in);
                        return 1;
                }
        }
        if (read_word(in, &header0) != 0) {
                fprintf(stderr, "%s: trailing data after DXR image\n", argv[3]);
                free(image);
                free(reloc);
                fclose(in);
                return 1;
        }
        fclose(in);

        for (i = 0; i < image_words; i++) {
                bit = i % 36UL;
                if ((reloc[i / 36UL] & (1UL << (35UL - bit))) != 0)
                        image[i] = (image[i] & ~HALF_MASK) |
                            (((image[i] & HALF_MASK) + base) & HALF_MASK);
        }

        out = fopen(argv[4], "wb");
        if (out == NULL) {
                perror(argv[4]);
                free(image);
                free(reloc);
                return 1;
        }
        for (i = 0; i < image_words; i++) {
                if (write_word(out, DATAI_PTR | ((base + i) & HALF_MASK)) != 0 ||
                    write_word(out, image[i]) != 0) {
                        perror(argv[4]);
                        fclose(out);
                        free(image);
                        free(reloc);
                        return 1;
                }
        }
        if (write_word(out, JRST_WORD | ((base + entry) & HALF_MASK)) != 0 ||
            fclose(out) == EOF) {
                perror(argv[4]);
                free(image);
                free(reloc);
                return 1;
        }
        free(image);
        free(reloc);
        return 0;
}
