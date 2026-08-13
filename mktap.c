/* mktap.c - write PDP-6 36-bit words as a SIMH magnetic-tape record. */

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define WORD_MASK 0777777777777ULL

static void
put32(FILE *f, unsigned long v)
{
        unsigned char b[4];

        b[0] = (unsigned char)(v & 0xffUL);
        b[1] = (unsigned char)((v >> 8) & 0xffUL);
        b[2] = (unsigned char)((v >> 16) & 0xffUL);
        b[3] = (unsigned char)((v >> 24) & 0xffUL);
        if (fwrite(b, 1, 4, f) != 4) {
                perror("mktap: write");
                exit(1);
        }
}

static unsigned long long
parse_word(const char *s, int *ok)
{
        unsigned long long v;
        int digit;

        v = 0;
        *ok = 0;
        while (*s == ' ' || *s == '\t')
                s++;
        if (*s == '\0' || *s == '\n' || *s == '#')
                return 0;
        while (*s >= '0' && *s <= '7') {
                digit = *s++ - '0';
                if (v > (WORD_MASK >> 3))
                        return 0;
                v = (v << 3) | (unsigned long long)digit;
        }
        while (*s == ' ' || *s == '\t')
                s++;
        if (*s != '\0' && *s != '\n' && *s != '#')
                return 0;
        if (v > WORD_MASK)
                return 0;
        *ok = 1;
        return v;
}

static void
usage(void)
{
        fprintf(stderr, "usage: mktap -t mtc -p words -o tape\n");
        exit(2);
}

int
main(int argc, char **argv)
{
        const char *type, *payload, *out_path;
        FILE *in, *out;
        unsigned char *data, *new_data;
        size_t used, cap;
        char line[256];
        int i;

        type = payload = out_path = NULL;
        data = NULL;
        used = cap = 0;
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
        if (type == NULL || strcmp(type, "mtc") != 0 ||
            payload == NULL || out_path == NULL)
                usage();
        in = fopen(payload, "r");
        if (in == NULL) {
                perror(payload);
                return 1;
        }
        while (fgets(line, sizeof(line), in) != NULL) {
                unsigned long long w;
                int ok;

                w = parse_word(line, &ok);
                if (!ok) {
                        char *p = line;
                        while (*p == ' ' || *p == '\t')
                                p++;
                        if (*p == '\0' || *p == '\n' || *p == '#')
                                continue;
                        fprintf(stderr, "mktap: bad word: %s", line);
                        fclose(in);
                        free(data);
                        return 1;
                }
                if (used + 6 > cap) {
                        size_t new_cap = cap ? cap * 2 : 1024;
                        while (new_cap < used + 6)
                                new_cap *= 2;
                        new_data = (unsigned char *)realloc(data, new_cap);
                        if (new_data == NULL) {
                                perror("mktap: realloc");
                                fclose(in);
                                free(data);
                                return 1;
                        }
                        data = new_data;
                        cap = new_cap;
                }
                data[used++] = (unsigned char)(((w >> 30) & 077));
                data[used++] = (unsigned char)(((w >> 24) & 077));
                data[used++] = (unsigned char)(((w >> 18) & 077));
                data[used++] = (unsigned char)(((w >> 12) & 077));
                data[used++] = (unsigned char)(((w >> 6) & 077));
                data[used++] = (unsigned char)((w & 077));
        }
        fclose(in);

        out = fopen(out_path, "wb");
        if (out == NULL) {
                perror(out_path);
                free(data);
                return 1;
        }
        put32(out, (unsigned long)used);
        if (used != 0 && fwrite(data, 1, used, out) != used) {
                perror("mktap: write");
                fclose(out);
                free(data);
                return 1;
        }
        if (used & 1)
                fputc(0, out);
        put32(out, (unsigned long)used);
        put32(out, 0);
        if (fclose(out) != 0) {
                perror(out_path);
                free(data);
                return 1;
        }
        free(data);
        return 0;
}
