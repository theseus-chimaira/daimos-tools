/* words2pt.c - encode octal 36-bit words as PDP-6 paper-tape bytes. */

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define WORD_MASK 0777777777777ULL

static unsigned long long parse_word(const char *s)
{
        unsigned long long v;
        int digit;
        int seen;

        v = 0;
        seen = 0;
        while (*s == ' ' || *s == '\t')
                s++;
        while (*s >= '0' && *s <= '7') {
                digit = *s++ - '0';
                if (v > (WORD_MASK >> 3))
                        return ~0ULL;
                v = (v << 3) | (unsigned long long)digit;
                seen = 1;
        }
        while (*s == ' ' || *s == '\t' || *s == '\n' || *s == '\r')
                s++;
        if (!seen || (*s != '\0' && *s != '#'))
                return ~0ULL;
        return v;
}

static void usage(void)
{
        fprintf(stderr, "usage: words2pt -t ptr -p words -o tape\n");
        exit(2);
}

int main(int argc, char **argv)
{
        const char *type = NULL, *payload = NULL, *out_path = NULL;
        FILE *in, *out;
        char line[256];
        int i;

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
        if (type == NULL || strcmp(type, "ptr") != 0 ||
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
                return 1;
        }
        while (fgets(line, sizeof(line), in) != NULL) {
                unsigned long long w;
                unsigned char b[5];
                char *p = line;
                while (*p == ' ' || *p == '\t')
                        p++;
                if (*p == '\0' || *p == '\n' || *p == '#')
                        continue;
                w = parse_word(p);
                if (w == ~0ULL) {
                        fprintf(stderr, "words2pt: bad word: %s", line);
                        return 1;
                }
                b[0] = (unsigned char)((w >> 32) & 0x0f);
                b[1] = (unsigned char)((w >> 24) & 0xff);
                b[2] = (unsigned char)((w >> 16) & 0xff);
                b[3] = (unsigned char)((w >> 8) & 0xff);
                b[4] = (unsigned char)(w & 0xff);
                if (fwrite(b, 1, sizeof(b), out) != sizeof(b)) {
                        perror(out_path);
                        return 1;
                }
        }
        fclose(out);
        fclose(in);
        return 0;
}
