/*
 * mkstream.c - wrap assembled standalone words as an opaque Stage1 stream.
 *
 * Input is the SIMH-style output produced by pdp10-dec-none-as:
 *      deposit ADDRESS WORD
 *
 * The output is an octal word stream:
 *      DAIMON magic, image_words,,entry_offset, opaque image.
 */

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define WORD_MASK 0777777777777ULL
#define HALF_MASK 0777777U
#define DAIMON_MAGIC 0444151555756ULL

static unsigned long parse_octal(const char *s, const char *what)
{
        char *end;
        unsigned long v;

        errno = 0;
        v = strtoul(s, &end, 8);
        if (errno != 0 || end == s || *end != '\0') {
                fprintf(stderr, "mkstream: bad %s: %s\n", what, s);
                exit(1);
        }
        return v;
}

static void usage(void)
{
        fprintf(stderr,
            "usage: mkstream -i rim -o words -b init-base -w init-words\n");
        exit(2);
}

int main(int argc, char **argv)
{
        const char *in_path = NULL;
        const char *out_path = NULL;
        unsigned long init_base = 0;
        unsigned long init_words = 0;
        unsigned long i;
        unsigned long entry_off = 0;
        unsigned long long *init;
        FILE *in;
        FILE *out;
        char line[256];

        for (i = 1; i < (unsigned long)argc; i++) {
                if (strcmp(argv[i], "-i") == 0 && i + 1 < (unsigned long)argc)
                        in_path = argv[++i];
                else if (strcmp(argv[i], "-o") == 0 && i + 1 < (unsigned long)argc)
                        out_path = argv[++i];
                else if (strcmp(argv[i], "-b") == 0 && i + 1 < (unsigned long)argc)
                        init_base = parse_octal(argv[++i], "init base");
                else if (strcmp(argv[i], "-w") == 0 && i + 1 < (unsigned long)argc)
                        init_words = parse_octal(argv[++i], "init words");
                else
                        usage();
        }
        if (in_path == NULL || out_path == NULL || init_base == 0 ||
            init_words == 0 || init_words > HALF_MASK)
                usage();

        init = calloc(init_words, sizeof(init[0]));
        if (init == NULL) {
                perror("mkstream: calloc");
                return 1;
        }

        in = fopen(in_path, "r");
        if (in == NULL) {
            perror(in_path);
            return 1;
        }
        while (fgets(line, sizeof(line), in) != NULL) {
                char op[32], a[64], w[64];
                unsigned long addr;
                unsigned long long word;
                if (sscanf(line, "%31s %63s %63s", op, a, w) != 3)
                        continue;
                if (strcmp(op, "deposit") != 0)
                        continue;
                addr = parse_octal(a, "deposit address");
                word = parse_octal(w, "deposit word") & WORD_MASK;
                if (addr < init_base || addr >= init_base + init_words) {
                        fprintf(stderr,
                            "mkstream: deposit %06lo outside init range %06lo..%06lo\n",
                            addr, init_base, init_base + init_words - 1);
                        return 1;
                }
                init[addr - init_base] = word;
        }
        fclose(in);

        out = fopen(out_path, "w");
        if (out == NULL) {
                perror(out_path);
                return 1;
        }
        fprintf(out, "%012llo\n", DAIMON_MAGIC);
        fprintf(out, "%012llo\n",
            (((unsigned long long)init_words & HALF_MASK) << 18) |
            ((unsigned long long)entry_off & HALF_MASK));
        for (i = 0; i < init_words; i++)
                fprintf(out, "%012llo\n", init[i] & WORD_MASK);
        fclose(out);
        free(init);
        return 0;
}
