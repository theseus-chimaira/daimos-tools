/* mktap.c - write PDP-6 36-bit words as a SIMH magnetic-tape record. */

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define WORD_MASK 0777777777777ULL
#define MTC_RECORD_BYTES 32766U

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

static unsigned char
mtc_odd_parity_v1(unsigned char value)
{
        unsigned int bits;
        unsigned int ones;

        value &= 077U;
        bits = value;
        ones = 0U;
        while (bits != 0U) {
                ones += bits & 1U;
                bits >>= 1;
        }
        if ((ones & 1U) == 0U)
                value |= 0100U;
        return value;
}

static int
write_record(FILE *out, const unsigned char *data, size_t used)
{
        if (used == 0U)
                return 0;
        put32(out, (unsigned long)used);
        if (fwrite(data, 1, used, out) != used)
                return -1;
        if ((used & 1U) != 0U && fputc(0, out) == EOF)
                return -1;
        put32(out, (unsigned long)used);
        return 0;
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
        unsigned char *data;
        size_t used;
        char line[256];
        int i;

        type = payload = out_path = NULL;
        data = NULL;
        used = 0U;
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
        out = fopen(out_path, "wb");
        if (out == NULL) {
                perror(out_path);
                fclose(in);
                return 1;
        }
        data = (unsigned char *)malloc(MTC_RECORD_BYTES);
        if (data == NULL) {
                perror("mktap: malloc");
                fclose(in);
                fclose(out);
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
                        fclose(out);
                        free(data);
                        return 1;
                }
                if (used + 6U > MTC_RECORD_BYTES) {
                        if (write_record(out, data, used) != 0) {
                                perror("mktap: write");
                                fclose(in);
                                fclose(out);
                                free(data);
                                return 1;
                        }
                        used = 0U;
                }
                data[used++] = mtc_odd_parity_v1((unsigned char)(w >> 30));
                data[used++] = mtc_odd_parity_v1((unsigned char)(w >> 24));
                data[used++] = mtc_odd_parity_v1((unsigned char)(w >> 18));
                data[used++] = mtc_odd_parity_v1((unsigned char)(w >> 12));
                data[used++] = mtc_odd_parity_v1((unsigned char)(w >> 6));
                data[used++] = mtc_odd_parity_v1((unsigned char)w);
        }
        fclose(in);
        if (write_record(out, data, used) != 0) {
                perror("mktap: write");
                fclose(out);
                free(data);
                return 1;
        }
        /* One tape mark terminates the boot file after the final data record. */
        put32(out, 0);
        if (fclose(out) != 0) {
                perror(out_path);
                free(data);
                return 1;
        }
        free(data);
        return 0;
}
