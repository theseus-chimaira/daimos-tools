#include "dobj.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int save_object(const char *name, struct dobj_object *o)
{
    FILE *f;
    int rc;
    f = fopen(name, "wb");
    if (f == NULL) return -1;
    rc = dobj_write(f, o);
    if (fclose(f) != 0) rc = -1;
    return rc;
}

static void make_main(struct dobj_object *o)
{
    memset(o, 0, sizeof(*o));
    o->text_words = 2UL;
    o->symbol_count = 2UL;
    o->reloc_count = 1UL;
    o->entry_symbol = 1UL;
    o->text = (struct dobj_word *)calloc(2U, sizeof(*o->text));
    o->symbols = (struct dobj_symbol *)calloc(2U, sizeof(*o->symbols));
    o->relocs = (struct dobj_reloc *)calloc(1U, sizeof(*o->relocs));
    o->text[0] = dobj_word_halves(0254000UL, 0UL);
    o->text[1] = dobj_word_halves(0254000UL, 0UL);
    strcpy(o->symbols[0].name, "main");
    o->symbols[0].kind = DOBJ_SYM_DEF;
    o->symbols[0].sec = DOBJ_SEC_TEXT;
    o->symbols[0].value = dobj_word_halves(0UL, 0UL);
    strcpy(o->symbols[1].name, "__helper");
    o->symbols[1].kind = DOBJ_SYM_UNDEF;
    o->symbols[1].sec = DOBJ_SEC_ABS;
    o->relocs[0].loc_sec = DOBJ_SEC_TEXT;
    o->relocs[0].type = DOBJ_RELOC_SYMBOL_RH18;
    o->relocs[0].offset = 0UL;
    o->relocs[0].symbol = 2UL;
    o->relocs[0].addend = dobj_word_halves(0UL, 0UL);
}

static void make_helper(struct dobj_object *o, const char *name,
                        unsigned long marker)
{
    memset(o, 0, sizeof(*o));
    o->text_words = 2UL;
    o->symbol_count = 1UL;
    o->reloc_count = 1UL;
    o->text = (struct dobj_word *)calloc(2U, sizeof(*o->text));
    o->symbols = (struct dobj_symbol *)calloc(1U, sizeof(*o->symbols));
    o->relocs = (struct dobj_reloc *)calloc(1U, sizeof(*o->relocs));
    o->text[0] = dobj_word_halves(marker, 0UL);
    o->text[1] = dobj_word_halves(0254000UL, 0UL);
    strcpy(o->symbols[0].name, name);
    o->symbols[0].kind = DOBJ_SYM_DEF;
    o->symbols[0].sec = DOBJ_SEC_TEXT;
    o->symbols[0].value = dobj_word_halves(0UL, 0UL);
    o->relocs[0].loc_sec = DOBJ_SEC_TEXT;
    o->relocs[0].type = DOBJ_RELOC_LOCAL_RH18;
    o->relocs[0].target_sec = DOBJ_SEC_TEXT;
    o->relocs[0].offset = 1UL;
    o->relocs[0].addend = dobj_word_halves(0UL, 0UL);
}

static void make_plain(struct dobj_object *o, unsigned long marker)
{
    memset(o, 0, sizeof(*o));
    o->text_words = 1UL;
    o->text = (struct dobj_word *)calloc(1U, sizeof(*o->text));
    if (o->text != NULL)
        o->text[0] = dobj_word_halves(marker, 0UL);
}

static int check_many_inputs(void)
{
    enum { MANY_COUNT = 300 };
    struct dobj_object o;
    char name[64];
    char *cmd;
    size_t cap;
    size_t used;
    int i;
    int rc;
    FILE *f;
    struct dobj_word w;

    cap = 64U + (size_t)MANY_COUNT * 32U;
    cmd = (char *)malloc(cap);
    if (cmd == NULL)
        return -1;
    strcpy(cmd, "./dlink -o tests/many.dxr");
    used = strlen(cmd);
    for (i = 0; i < MANY_COUNT; i++) {
        sprintf(name, "tests/many-%03d.dobj", i);
        make_plain(&o, (unsigned long)i);
        if (o.text == NULL || save_object(name, &o) != 0) {
            dobj_free(&o);
            free(cmd);
            return -1;
        }
        dobj_free(&o);
        if (used + strlen(name) + 2U >= cap) {
            free(cmd);
            return -1;
        }
        cmd[used++] = ' ';
        strcpy(cmd + used, name);
        used += strlen(name);
    }
    rc = system(cmd);
    free(cmd);
    if (rc != 0)
        return -1;
    f = fopen("tests/many.dxr", "rb");
    if (f == NULL || dobj_read_word(f, &w) != 0 ||
        dobj_read_word(f, &w) != 0 || w.lh != (unsigned long)MANY_COUNT) {
        if (f != NULL) fclose(f);
        return -1;
    }
    fclose(f);
    for (i = 0; i < MANY_COUNT; i++) {
        sprintf(name, "tests/many-%03d.dobj", i);
        remove(name);
    }
    remove("tests/many.dxr");
    return 0;
}

static int check_dxr(const char *name)
{
    FILE *f;
    struct dobj_word w;
    struct dobj_word image[4];
    struct dobj_word map;
    int i;
    f = fopen(name, "rb");
    if (f == NULL) return -1;
    if (dobj_read_word(f, &w) != 0 || w.rh != 0UL) goto bad;
    if (dobj_read_word(f, &w) != 0 || w.lh != 4UL || w.rh != 0UL) goto bad;
    for (i = 0; i < 4; i++) if (dobj_read_word(f, &image[i]) != 0) goto bad;
    if (dobj_read_word(f, &map) != 0) goto bad;
    fclose(f);
    if (image[0].rh != 2UL) return -1;
    if (image[3].rh != 2UL) return -1;
    if ((map.lh & 0440000UL) != 0440000UL) return -1;
    return 0;
bad:
    fclose(f);
    return -1;
}

int main(void)
{
    long add;
    struct dobj_object main_o, helper_o, unused_o;
    int rc;

    if (dobj_word_addend18(dobj_word_halves(0UL, 0777777UL), &add) != 0 ||
        add != 0777777L) {
        fprintf(stderr, "dobj-test: positive RH18 addend contract failed\n");
        return 1;
    }
    if (dobj_word_addend18(dobj_word_halves(0777777UL, 0777777UL), &add) != 0 ||
        add != -1L) {
        fprintf(stderr, "dobj-test: negative RH18 addend contract failed\n");
        return 1;
    }
    make_main(&main_o);
    make_helper(&helper_o, "__helper", 0123456UL);
    make_helper(&unused_o, "__unused", 0654321UL);
    if (save_object("tests/main.dobj", &main_o) != 0 ||
        save_object("tests/helper.dobj", &helper_o) != 0 ||
        save_object("tests/unused.dobj", &unused_o) != 0) return 1;
    rc = system("./darc -o tests/libtest.darc tests/helper.dobj tests/unused.dobj");
    if (rc != 0) {
        fprintf(stderr, "dobj-test: darc archive creation failed\n");
        return 1;
    }
    rc = system("./dlink -o tests/out.dxr -M tests/out.map tests/main.dobj tests/libtest.darc");
    if (rc != 0 || check_dxr("tests/out.dxr") != 0) {
        fprintf(stderr, "dobj-test: link/archive/local relocation contract failed\n");
        return 1;
    }
    {
        FILE *mf;
        char line[160];
        int saw_main;
        int saw_helper;
        int saw_unused;

        saw_main = saw_helper = saw_unused = 0;
        mf = fopen("tests/out.map", "r");
        if (mf == NULL) return 1;
        while (fgets(line, sizeof(line), mf) != NULL) {
            if (strncmp(line, "main ", 5U) == 0) saw_main = 1;
            if (strncmp(line, "__helper ", 9U) == 0) saw_helper = 1;
            if (strncmp(line, "__unused ", 9U) == 0) saw_unused = 1;
        }
        fclose(mf);
        if (!saw_main || !saw_helper || saw_unused) {
            fprintf(stderr, "dobj-test: linker map/archive selection contract failed\n");
            return 1;
        }
    }
    rc = system("./dlink -o tests/bad-undef.dxr tests/main.dobj >/dev/null 2>&1");
    if (rc == 0) {
        fprintf(stderr, "dobj-test: unresolved symbol was accepted\n");
        return 1;
    }
    rc = system("./dlink -o tests/bad-dup.dxr tests/helper.dobj tests/helper.dobj >/dev/null 2>&1");
    if (rc == 0) {
        fprintf(stderr, "dobj-test: duplicate global was accepted\n");
        return 1;
    }
    if (check_many_inputs() != 0) {
        fprintf(stderr, "dobj-test: dynamic input capacity contract failed\n");
        return 1;
    }
    printf("DOBJ1/DARC1 linker contract passed\n");
    dobj_free(&main_o);
    dobj_free(&helper_o);
    dobj_free(&unused_o);
    return 0;
}
