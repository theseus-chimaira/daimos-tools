/* mkrim.c - make a PDP-6 executable RIM paper tape. */

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>

#define WORD_MASK 0777777777777UL
#define ADDR_MASK 0777777UL
#define DATAI_PTR 0710440000000UL
#define JRST_WORD 0254000000000UL

static void
write_word(FILE *out, unsigned long word)
{
        int shift;

        word &= WORD_MASK;
        for (shift = 30; shift >= 0; shift -= 6) {
                int ch;

                ch = (int)((word >> shift) & 077UL) | 0200;
                if (putc(ch, out) == EOF) {
                        perror("paper tape");
                        exit(1);
                }
        }
}

static int
parse_octal(const char *s, unsigned long *value)
{
        char *end;
        unsigned long v;

        errno = 0;
        v = strtoul(s, &end, 8);
        if (errno != 0 || end == s)
                return 0;
        while (*end == ' ' || *end == '\t')
                end++;
        if (*end != '\0' && *end != '\n' && *end != '\r')
                return 0;
        *value = v;
        return 1;
}

int
main(int argc, char **argv)
{
        FILE *in;
        FILE *out;
        char line[256];
        unsigned long entry;
        int have_entry;

        if (argc != 3) {
                fprintf(stderr, "usage: %s assembler-rim output-tape\n",
                    argv[0]);
                return 2;
        }
        in = fopen(argv[1], "r");
        if (in == NULL) {
                perror(argv[1]);
                return 1;
        }
        out = fopen(argv[2], "wb");
        if (out == NULL) {
                perror(argv[2]);
                fclose(in);
                return 1;
        }

        entry = 0;
        have_entry = 0;
        while (fgets(line, sizeof(line), in) != NULL) {
                char a[64];
                char w[64];
                unsigned long addr;
                unsigned long word;

                if (sscanf(line, "deposit %63s %63s", a, w) == 2) {
                        if (!parse_octal(a, &addr) ||
                            !parse_octal(w, &word) ||
                            addr > ADDR_MASK || word > WORD_MASK) {
                                fprintf(stderr, "%s: bad deposit: %s",
                                    argv[1], line);
                                return 1;
                        }
                        write_word(out, DATAI_PTR | addr);
                        write_word(out, word);
                } else if (sscanf(line, "go %63s", a) == 1) {
                        if (!parse_octal(a, &entry) || entry > ADDR_MASK) {
                                fprintf(stderr, "%s: bad entry: %s",
                                    argv[1], line);
                                return 1;
                        }
                        have_entry = 1;
                }
        }
        if (ferror(in)) {
                perror(argv[1]);
                return 1;
        }
        if (!have_entry) {
                fprintf(stderr, "%s: no entry point\n", argv[1]);
                return 1;
        }

        /* Transfer to the Stage1 entry after all deposits. */
        write_word(out, JRST_WORD | entry);
        fclose(in);
        if (fclose(out) == EOF) {
                perror(argv[2]);
                return 1;
        }
        return 0;
}
