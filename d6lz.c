#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define WORD_MASK UINT64_C(0777777777777)
#define HALF_MASK UINT64_C(0777777)
#define DXR_F_COMPRESSED UINT64_C(0100000)
#define D6LZ_WINDOW 128U
#define D6LZ_MIN 3U
#define D6LZ_MAX 130U

static void die(const char *s)
{
    fprintf(stderr, "d6lz: %s\n", s);
    exit(1);
}

static void die_path(const char *path)
{
    fprintf(stderr, "d6lz: %s: %s\n", path, strerror(errno));
    exit(1);
}

static uint64_t get64le(const unsigned char b[8])
{
    uint64_t v = 0;
    unsigned i;
    for (i = 0; i < 8U; ++i)
        v |= (uint64_t)b[i] << (8U * i);
    return v;
}

static void put64le(unsigned char b[8], uint64_t v)
{
    unsigned i;
    for (i = 0; i < 8U; ++i)
        b[i] = (unsigned char)(v >> (8U * i));
}

static uint64_t *read_words(const char *path, size_t *np)
{
    FILE *f;
    long size = 0;
    size_t n, i;
    uint64_t *w;
    unsigned char b[8];

    f = fopen(path, "rb");
    if (f == NULL)
        die_path(path);
    if (fseek(f, 0L, SEEK_END) != 0 || (size = ftell(f)) < 0)
        die_path(path);
    if ((size % 8L) != 0L)
        die("input is not a whole number of 36-bit container words");
    rewind(f);
    n = (size_t)(size / 8L);
    w = n == 0U ? NULL : (uint64_t *)malloc(n * sizeof(*w));
    if (n != 0U && w == NULL)
        die("out of memory");
    for (i = 0; i < n; ++i) {
        if (fread(b, 1U, 8U, f) != 8U)
            die_path(path);
        w[i] = get64le(b);
        if ((w[i] & ~WORD_MASK) != 0U)
            die("input word exceeds 36 bits");
    }
    if (fclose(f) != 0)
        die_path(path);
    *np = n;
    return w;
}

static void write_words(const char *path, const uint64_t *w, size_t n)
{
    FILE *f;
    size_t i;
    unsigned char b[8];

    f = fopen(path, "wb");
    if (f == NULL)
        die_path(path);
    for (i = 0; i < n; ++i) {
        put64le(b, w[i]);
        if (fwrite(b, 1U, 8U, f) != 8U)
            die_path(path);
    }
    if (fclose(f) != 0)
        die_path(path);
}


static uint64_t *read_text_words(const char *path, size_t *np)
{
    FILE *f;
    uint64_t *w = NULL;
    size_t n = 0U;
    size_t cap = 0U;
    char line[128];

    f = fopen(path, "r");
    if (f == NULL)
        die_path(path);
    while (fgets(line, sizeof(line), f) != NULL) {
        char *end;
        unsigned long long v;
        if (strchr(line, '\n') == NULL && !feof(f))
            die("text word line is too long");
        errno = 0;
        v = strtoull(line, &end, 8);
        if (errno != 0 || end == line)
            die("invalid octal text word");
        while (*end == ' ' || *end == '\t' || *end == '\r' || *end == '\n')
            ++end;
        if (*end != '\0' || v > WORD_MASK)
            die("invalid octal text word");
        if (n == cap) {
            size_t newcap = cap == 0U ? 256U : cap * 2U;
            uint64_t *nw = (uint64_t *)realloc(w, newcap * sizeof(*nw));
            if (nw == NULL)
                die("out of memory");
            w = nw;
            cap = newcap;
        }
        w[n++] = (uint64_t)v;
    }
    if (ferror(f))
        die_path(path);
    if (fclose(f) != 0)
        die_path(path);
    *np = n;
    return w;
}

static void write_text_words(const char *path, const uint64_t *w, size_t n)
{
    FILE *f;
    size_t i;

    f = fopen(path, "w");
    if (f == NULL)
        die_path(path);
    for (i = 0U; i < n; ++i)
        if (fprintf(f, "%012llo\n", (unsigned long long)w[i]) < 0)
            die_path(path);
    if (fclose(f) != 0)
        die_path(path);
}

static uint64_t six3(char a, char b, char c)
{
    return ((uint64_t)(a - 040) << 12) |
           ((uint64_t)(b - 040) << 6) |
           (uint64_t)(c - 040);
}

static uint64_t *compress_words(const uint64_t *in, size_t n, size_t *outn)
{
    size_t cap, op, pos;
    uint64_t *out;

    cap = n + (n + 35U) / 36U + 1U;
    out = cap == 0U ? NULL : (uint64_t *)malloc(cap * sizeof(*out));
    if (cap != 0U && out == NULL)
        die("out of memory");
    op = 0U;
    pos = 0U;
    while (pos < n) {
        size_t ctrl_pos = op++;
        uint64_t control = 0U;
        unsigned tok;

        for (tok = 0U; tok < 36U && pos < n; ++tok) {
            size_t best_len = 0U;
            size_t best_dist = 0U;
            size_t max_dist = pos < D6LZ_WINDOW ? pos : D6LZ_WINDOW;
            size_t dist;

            for (dist = 1U; dist <= max_dist; ++dist) {
                size_t len = 0U;
                size_t max_len = n - pos;
                if (max_len > D6LZ_MAX)
                    max_len = D6LZ_MAX;
                while (len < max_len && in[pos + len] == in[pos + len - dist])
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
                out[op++] = ((uint64_t)(best_len - D6LZ_MIN) << 7) |
                    (uint64_t)(best_dist - 1U);
                pos += best_len;
            } else {
                out[op++] = in[pos++];
            }
        }
        out[ctrl_pos] = control;
    }
    *outn = op;
    return out;
}

static void compress_raw(const char *inpath, const char *outpath, int text_mode)
{
    uint64_t *in, *out;
    size_t n, outn;
    in = text_mode ? read_text_words(inpath, &n) : read_words(inpath, &n);
    out = compress_words(in, n, &outn);
    if (text_mode)
        write_text_words(outpath, out, outn);
    else
        write_words(outpath, out, outn);
    free(out);
    free(in);
}

static void compress_framed(const char *inpath, const char *outpath)
{
    uint64_t *in, *payload, *out;
    size_t n, payload_n, i;

    in = read_words(inpath, &n);
    if (n > (size_t)WORD_MASK)
        die("framed input is too large");
    payload = compress_words(in, n, &payload_n);
    out = (uint64_t *)malloc((payload_n + 1U) * sizeof(*out));
    if (out == NULL)
        die("out of memory");
    out[0] = (uint64_t)n;
    for (i = 0U; i < payload_n; ++i)
        out[i + 1U] = payload[i];
    write_words(outpath, out, payload_n + 1U);
    free(out);
    free(payload);
    free(in);
}

static void compress_exec(const char *inpath, const char *outpath)
{
    uint64_t *in, *payload, *out;
    size_t n, payload_n, header = 0, reloc, image, relpos, outn, i;
    uint64_t dxr = six3('D', 'X', 'R');
    uint64_t tx2 = six3('T', 'X', '2');

    in = read_words(inpath, &n);
    if (n < 2U || ((in[0] >> 18) & HALF_MASK) != dxr)
        die("input is not a DXR executable");
    if ((in[1] & DXR_F_COMPRESSED) != 0U)
        die("input executable is already compressed");
    image = (size_t)((in[1] >> 18) & HALF_MASK);
    if (image == 0U)
        die("DXR executable has an empty image");
    reloc = (image + 35U) / 36U;
    if (n == 2U + image + reloc) {
        header = 2U;
    } else if (n == 3U + image + reloc) {
        header = 3U;
        if ((in[2] & HALF_MASK) != tx2 || ((in[2] >> 18) & HALF_MASK) > image)
            die("DXR2 executable has invalid text metadata");
    } else {
        die("DXR executable has inconsistent length");
    }
    relpos = header + image;
    payload = compress_words(in + header, image, &payload_n);
    outn = 3U + payload_n + reloc;
    out = (uint64_t *)malloc(outn * sizeof(*out));
    if (out == NULL)
        die("out of memory");
    out[0] = in[0];
    out[1] = in[1] | DXR_F_COMPRESSED;
    out[2] = header == 3U ? in[2] : tx2;
    for (i = 0U; i < payload_n; ++i)
        out[3U + i] = payload[i];
    for (i = 0U; i < reloc; ++i)
        out[3U + payload_n + i] = in[relpos + i];
    write_words(outpath, out, outn);
    free(out);
    free(payload);
    free(in);
}

static void usage(void)
{
    fprintf(stderr, "usage: d6lz [-t|-f] [-x|-X] input output\n");
    exit(1);
}

int main(int argc, char **argv)
{
    int exec_mode = 0;
    int text_mode = 0;
    int framed_mode = 0;
    int arg = 1;
    while (arg < argc && argv[arg][0] == '-') {
        if (strcmp(argv[arg], "-x") == 0 || strcmp(argv[arg], "-X") == 0)
            exec_mode = 1;
        else if (strcmp(argv[arg], "-t") == 0)
            text_mode = 1;
        else if (strcmp(argv[arg], "-f") == 0)
            framed_mode = 1;
        else
            usage();
        ++arg;
    }
    if (argc - arg != 2 || (exec_mode && (text_mode || framed_mode)) ||
        (text_mode && framed_mode))
        usage();
    if (exec_mode)
        compress_exec(argv[arg], argv[arg + 1]);
    else if (framed_mode)
        compress_framed(argv[arg], argv[arg + 1]);
    else
        compress_raw(argv[arg], argv[arg + 1], text_mode);
    return 0;
}
