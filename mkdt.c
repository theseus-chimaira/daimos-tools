/* mkdt.c - write a PDP-6 18-bit SIMH DECtape image. */

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define WORD_MASK 0777777777777ULL

static void put32(FILE *f, unsigned long v)
{
        unsigned char b[4];

        b[0] = (unsigned char)(v & 0xffUL);
        b[1] = (unsigned char)((v >> 8) & 0xffUL);
        b[2] = (unsigned char)((v >> 16) & 0xffUL);
        b[3] = (unsigned char)((v >> 24) & 0xffUL);
        if (fwrite(b, 1, sizeof(b), f) != sizeof(b)) {
                perror("mkdt: write");
                exit(1);
        }
}

static unsigned long long parse_word(const char *s)
{
        char *end;
        unsigned long long v;

        errno = 0;
        v = strtoull(s, &end, 8);
        while (*end == ' ' || *end == '\t' || *end == '\r' || *end == '\n')
                end++;
        if (errno != 0 || end == s || *end != '\0' || (v & ~WORD_MASK) != 0)
                return ~0ULL;
        return v;
}

static void usage(void)
{
        fprintf(stderr, "usage: mkdt -t dtc -p words -o image\n");
        exit(2);
}

int main(int argc, char **argv)
{
        const char *type;
        const char *payload;
        const char *out_path;
        FILE *in;
        FILE *out;
        char line[256];
        int i;
        unsigned int words;

        type = NULL;
        payload = NULL;
        out_path = NULL;
        words = 0;
        for (i = 1; i < argc; i++) {
                if (strcmp(argv[i], "-t") == 0 && i + 1 < argc)
                        type = argv[++i];
                else if (strcmp(argv[i], "-p") == 0 && i + 1 < argc)
                        payload = argv[++i];
                else if (strcmp(argv[i], "-o") == 0 && i + 1 < argc)
                        out_path = argv[++i];
                else
                        usage();
        }
        if (type == NULL || strcmp(type, "dtc") != 0 ||
            payload == NULL || out_path == NULL)
                usage();

        in = fopen(payload, "r");
        if (in == NULL) {
                perror(payload);
                return 1;
        }
        out = fopen(out_path, "wb");
        if (out == NULL) {
                perror(out_path);
                fclose(in);
                return 1;
        }
        while (fgets(line, sizeof(line), in) != NULL) {
                unsigned long long w;
                char *p;

                p = line;
                while (*p == ' ' || *p == '\t')
                        p++;
                if (*p == '\0' || *p == '\n' || *p == '#')
                        continue;
                w = parse_word(p);
                if (w == ~0ULL) {
                        fprintf(stderr, "mkdt: bad word: %s", line);
                        fclose(out);
                        fclose(in);
                        return 1;
                }
                put32(out, (w >> 18) & 0777777UL);
                put32(out, w & 0777777UL);
                words++;
        }
        while ((words & 0177U) != 0) {
                put32(out, 0);
                put32(out, 0);
                words++;
        }
        if (fclose(out) == EOF) {
                perror(out_path);
                fclose(in);
                return 1;
        }
        fclose(in);
        return 0;
}
