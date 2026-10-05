/*
 * mkbootbanner.c - generate DAIMOS PDP-6 boot-display banner words.
 *
 * The kernel consumes the generated arrays during MINIT only.  Keeping the
 * encoding here removes fixed banner construction code from the target image.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>

#define DPY_MODE_PARAM 0U
#define DPY_MODE_POINT 1U
#define DPY_MODE_CHAR 3U
#define DPY_SPACE 040U
#define DPY_ESC 037U

static const unsigned long long wcnsls_title[] = {
    0364306143076ULL, 0164307743061ULL, 0371020410237ULL,
    0216726543061ULL, 0164306143056ULL, 0174101602076ULL, 0ULL
};

static const unsigned long long wcnsls_digits[] = {
    0164316563056ULL, 0043020410216ULL, 0164204210437ULL,
    0360205602076ULL, 0021452276102ULL, 0374103602076ULL,
    0164103643056ULL, 0370210420410ULL, 0164305643056ULL,
    0164305702056ULL
};

static void usage(void)
{
    fprintf(stderr, "usage: mkbootbanner -v VERSION -o FILE\n");
    exit(2);
}

static unsigned int dpy_param_mode(unsigned int mode)
{
    return (mode & 07U) << 13;
}

static unsigned int dpy_point_coord(unsigned int yflag, unsigned int coord,
    unsigned int next_mode)
{
    unsigned int inst;

    inst = (next_mode & 07U) << 13;
    if (yflag != 0U)
        inst |= 0200000U;
    inst |= coord & 01777U;
    return inst & 0777777U;
}

static unsigned int dpy_char3(unsigned int c0, unsigned int c1,
    unsigned int c2)
{
    return ((c0 & 077U) << 12) | ((c1 & 077U) << 6) | (c2 & 077U);
}

static unsigned long long dpy_inst(unsigned int left, unsigned int right)
{
    return (((unsigned long long)left & 0777777ULL) << 18) |
        ((unsigned long long)right & 0777777ULL);
}

static unsigned int dpy_code(unsigned int ch)
{
    if (ch >= (unsigned int)'A' && ch <= (unsigned int)'Z')
        return ch - (unsigned int)'A' + 1U;
    if (ch >= 040U && ch <= 077U)
        return ch;
    return 077U;
}

static unsigned long long wcnsls_glyph(unsigned int ch)
{
    if (ch >= (unsigned int)'0' && ch <= (unsigned int)'9')
        return wcnsls_digits[ch - (unsigned int)'0'];
    if (ch == (unsigned int)'V')
        return 0214306142504ULL;
    if (ch == (unsigned int)'.')
        return 0000000000306ULL;
    if (ch == (unsigned int)'-')
        return 0000003700000ULL;
    return 0ULL;
}

static void emit_word(FILE *f, unsigned long long word)
{
    fprintf(f, "        .word 0%012llo\n", word & 0777777777777ULL);
}

static void emit_dpy(FILE *f, const char *version_text)
{
    const char *title = "DAIMOS ";
    unsigned int codes[6];
    unsigned int count = 0U;
    unsigned int char_mode = 0U;
    size_t i;
    const char *parts[2];
    int part;

    fprintf(f, "        .data\n");
    fprintf(f, "        .globl minit_dpy_banner_words\n");
    fprintf(f, "        .globl minit_dpy_banner_words_end\n");
    fprintf(f, "minit_dpy_banner_words:\n");
    emit_word(f, dpy_inst(dpy_param_mode(DPY_MODE_POINT),
        dpy_point_coord(0U, 0240U, DPY_MODE_POINT)));
    emit_word(f, dpy_inst(dpy_point_coord(1U, 01000U, DPY_MODE_PARAM),
        dpy_param_mode(DPY_MODE_PARAM)));

    parts[0] = title;
    parts[1] = version_text;
    for (part = 0; part < 2; ++part) {
        for (i = 0U; parts[part][i] != '\0'; ++i) {
            unsigned int need;
            codes[count++] = dpy_code((unsigned char)parts[part][i]);
            need = char_mode == 0U ? 3U : 6U;
            if (count == need) {
                if (char_mode == 0U) {
                    emit_word(f, dpy_inst(dpy_param_mode(DPY_MODE_CHAR),
                        dpy_char3(codes[0], codes[1], codes[2])));
                    char_mode = 1U;
                } else {
                    emit_word(f, dpy_inst(
                        dpy_char3(codes[0], codes[1], codes[2]),
                        dpy_char3(codes[3], codes[4], codes[5])));
                }
                count = 0U;
            }
        }
    }
    /* Type 342 remains in CHAR mode until it sees ESC.  A boot banner used
     * only once did not need to care, but the resident DPY driver replays the
     * same five-word program.  Terminate every generated stream in PARAM mode
     * so the first word of the next refresh is decoded as a PARAM instruction
     * rather than as three character codes. */
    while (count < 5U)
        codes[count++] = DPY_SPACE;
    codes[count++] = DPY_ESC;
    emit_word(f, dpy_inst(dpy_char3(codes[0], codes[1], codes[2]),
        dpy_char3(codes[3], codes[4], codes[5])));
    fprintf(f, "minit_dpy_banner_words_end:\n");
}

static void emit_wcnsls(FILE *f, const char *version_text)
{
    size_t i;

    fprintf(f, "        .globl minit_wcnsls_banner_glyphs\n");
    fprintf(f, "        .globl minit_wcnsls_banner_glyphs_end\n");
    fprintf(f, "minit_wcnsls_banner_glyphs:\n");
    for (i = 0U; i < sizeof(wcnsls_title) / sizeof(wcnsls_title[0]); ++i)
        emit_word(f, wcnsls_title[i]);
    for (i = 0U; version_text[i] != '\0'; ++i)
        emit_word(f, wcnsls_glyph((unsigned char)version_text[i]));
    fprintf(f, "minit_wcnsls_banner_glyphs_end:\n");
}

int main(int argc, char **argv)
{
    const char *version = NULL;
    const char *output = NULL;
    char version_text[128];
    FILE *f;
    int i;

    for (i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "-v") == 0) {
            if (++i >= argc)
                usage();
            version = argv[i];
        } else if (strcmp(argv[i], "-o") == 0) {
            if (++i >= argc)
                usage();
            output = argv[i];
        } else {
            usage();
        }
    }
    if (version == NULL || output == NULL)
        usage();
    if (snprintf(version_text, sizeof(version_text), "V%s  ", version) < 0 ||
        strlen(version_text) >= sizeof(version_text) - 1U) {
        fprintf(stderr, "mkbootbanner: version too long\n");
        return 1;
    }

    f = fopen(output, "w");
    if (f == NULL) {
        fprintf(stderr, "mkbootbanner: %s: %s\n", output, strerror(errno));
        return 1;
    }
    fprintf(f, "; generated by mkbootbanner - do not edit\n");
    emit_dpy(f, version_text);
    emit_wcnsls(f, version_text);
    if (fclose(f) != 0) {
        fprintf(stderr, "mkbootbanner: %s: %s\n", output, strerror(errno));
        return 1;
    }
    return 0;
}
