#include "dobj.h"

#include <stdlib.h>
#include <string.h>

#define HALF_MASK 0777777UL
#define DOBJ_MAGIC_TEXT "DOBJ1 "
#define DARC_MAGIC_TEXT "DARC1 "

static struct dobj_word sixbit_word(const char *s)
{
    struct dobj_word w;
    unsigned long v[6];
    int i;

    for (i = 0; i < 6; i++) {
        unsigned int ch;
        ch = (unsigned int)(unsigned char)s[i];
        if (ch < 040U || ch > 0137U)
            ch = 040U;
        v[i] = (unsigned long)(ch - 040U) & 077UL;
    }
    w.lh = (v[0] << 12) | (v[1] << 6) | v[2];
    w.rh = (v[3] << 12) | (v[4] << 6) | v[5];
    return w;
}

static int word_equal(struct dobj_word a, struct dobj_word b)
{
    return a.lh == b.lh && a.rh == b.rh;
}

void dobj_word_zero(struct dobj_word *w)
{
    w->lh = 0UL;
    w->rh = 0UL;
}

struct dobj_word dobj_word_halves(unsigned long lh, unsigned long rh)
{
    struct dobj_word w;
    w.lh = lh & HALF_MASK;
    w.rh = rh & HALF_MASK;
    return w;
}

int dobj_word_is_zero(struct dobj_word w)
{
    return w.lh == 0UL && w.rh == 0UL;
}

int dobj_word_addend18(struct dobj_word w, long *value)
{
    unsigned long rh;
    rh = w.rh & HALF_MASK;
    if (w.lh == 0UL) {
        *value = (long)rh;
        return 0;
    }
    if (w.lh == HALF_MASK && (rh & 0400000UL) != 0UL) {
        *value = (long)rh - 01000000L;
        return 0;
    }
    return -1;
}

int dobj_read_word(FILE *f, struct dobj_word *w)
{
    unsigned char b[8];
    unsigned long lo;
    unsigned long hi;

    if (fread(b, 1U, 8U, f) != 8U)
        return -1;
    lo = (unsigned long)b[0] |
         ((unsigned long)b[1] << 8) |
         ((unsigned long)b[2] << 16) |
         ((unsigned long)b[3] << 24);
    hi = (unsigned long)b[4] |
         ((unsigned long)b[5] << 8) |
         ((unsigned long)b[6] << 16) |
         ((unsigned long)b[7] << 24);
    if ((hi & ~017UL) != 0UL)
        return -1;
    w->rh = lo & HALF_MASK;
    w->lh = ((lo >> 18) | (hi << 14)) & HALF_MASK;
    return 0;
}

int dobj_write_word(FILE *f, struct dobj_word w)
{
    unsigned char b[8];
    unsigned long lo;
    unsigned long hi;

    w.lh &= HALF_MASK;
    w.rh &= HALF_MASK;
    lo = w.rh | ((w.lh & 037777UL) << 18);
    hi = w.lh >> 14;
    b[0] = (unsigned char)(lo & 0377UL);
    b[1] = (unsigned char)((lo >> 8) & 0377UL);
    b[2] = (unsigned char)((lo >> 16) & 0377UL);
    b[3] = (unsigned char)((lo >> 24) & 0377UL);
    b[4] = (unsigned char)(hi & 0377UL);
    b[5] = (unsigned char)((hi >> 8) & 0377UL);
    b[6] = (unsigned char)((hi >> 16) & 0377UL);
    b[7] = (unsigned char)((hi >> 24) & 0377UL);
    return fwrite(b, 1U, 8U, f) == 8U ? 0 : -1;
}

static unsigned long name_words(unsigned long len)
{
    return (len + 3UL) / 4UL;
}

static int write_name(FILE *f, const char *name)
{
    unsigned long len;
    unsigned long i;

    len = (unsigned long)strlen(name);
    for (i = 0UL; i < name_words(len); i++) {
        struct dobj_word w;
        unsigned long j;
        dobj_word_zero(&w);
        for (j = 0UL; j < 4UL; j++) {
            unsigned long p;
            unsigned int ch;
            p = i * 4UL + j;
            if (p >= len)
                break;
            ch = (unsigned int)(unsigned char)name[p] & 0177U;
            if (j == 0UL)
                w.lh |= ((unsigned long)ch) << 9;
            else if (j == 1UL)
                w.lh |= (unsigned long)ch;
            else if (j == 2UL)
                w.rh |= ((unsigned long)ch) << 9;
            else
                w.rh |= (unsigned long)ch;
        }
        if (dobj_write_word(f, w) != 0)
            return -1;
    }
    return 0;
}

static int read_name(FILE *f, char *name, unsigned long len)
{
    unsigned long i;
    unsigned long pos;

    if (len > DOBJ_NAME_MAX)
        return -1;
    pos = 0UL;
    for (i = 0UL; i < name_words(len); i++) {
        struct dobj_word w;
        unsigned long v[4];
        int j;
        if (dobj_read_word(f, &w) != 0)
            return -1;
        v[0] = (w.lh >> 9) & 0777UL;
        v[1] = w.lh & 0777UL;
        v[2] = (w.rh >> 9) & 0777UL;
        v[3] = w.rh & 0777UL;
        for (j = 0; j < 4 && pos < len; j++)
            name[pos++] = (char)(v[j] & 0177UL);
    }
    name[len] = 0;
    return 0;
}

static void object_zero(struct dobj_object *obj)
{
    memset(obj, 0, sizeof(*obj));
}

void dobj_free(struct dobj_object *obj)
{
    free(obj->text);
    free(obj->data);
    free(obj->symbols);
    free(obj->relocs);
    object_zero(obj);
}

static void *xarray(unsigned long n, size_t size)
{
    if (n == 0UL)
        return NULL;
    if (n > 1000000UL)
        return NULL;
    return calloc((size_t)n, size);
}

int dobj_read(FILE *f, struct dobj_object *obj)
{
    struct dobj_word h[5];
    struct dobj_word magic;
    unsigned long i;

    object_zero(obj);
    for (i = 0UL; i < 5UL; i++)
        if (dobj_read_word(f, &h[i]) != 0)
            return -1;
    magic = sixbit_word(DOBJ_MAGIC_TEXT);
    if (!word_equal(h[0], magic))
        return -1;
    obj->text_words = h[1].lh;
    obj->data_words = h[1].rh;
    obj->bss_words = h[2].lh;
    obj->symbol_count = h[2].rh;
    obj->reloc_count = h[3].lh;
    obj->entry_symbol = h[4].rh;
    obj->text = (struct dobj_word *)xarray(obj->text_words, sizeof(*obj->text));
    obj->data = (struct dobj_word *)xarray(obj->data_words, sizeof(*obj->data));
    obj->symbols = (struct dobj_symbol *)xarray(obj->symbol_count, sizeof(*obj->symbols));
    obj->relocs = (struct dobj_reloc *)xarray(obj->reloc_count, sizeof(*obj->relocs));
    if ((obj->text_words && !obj->text) || (obj->data_words && !obj->data) ||
        (obj->symbol_count && !obj->symbols) || (obj->reloc_count && !obj->relocs)) {
        dobj_free(obj);
        return -1;
    }
    for (i = 0UL; i < obj->text_words; i++)
        if (dobj_read_word(f, &obj->text[i]) != 0) goto bad;
    for (i = 0UL; i < obj->data_words; i++)
        if (dobj_read_word(f, &obj->data[i]) != 0) goto bad;
    for (i = 0UL; i < obj->symbol_count; i++) {
        struct dobj_word s0;
        unsigned long len;
        if (dobj_read_word(f, &s0) != 0 ||
            dobj_read_word(f, &obj->symbols[i].value) != 0) goto bad;
        obj->symbols[i].kind = (int)((s0.lh >> 15) & 07UL);
        obj->symbols[i].sec = (int)((s0.lh >> 12) & 07UL);
        len = s0.lh & 077UL;
        if (read_name(f, obj->symbols[i].name, len) != 0) goto bad;
    }
    for (i = 0UL; i < obj->reloc_count; i++) {
        struct dobj_word r0, r1;
        if (dobj_read_word(f, &r0) != 0 || dobj_read_word(f, &r1) != 0 ||
            dobj_read_word(f, &obj->relocs[i].addend) != 0) goto bad;
        obj->relocs[i].loc_sec = (int)((r0.lh >> 15) & 07UL);
        obj->relocs[i].type = (int)((r0.lh >> 12) & 07UL);
        obj->relocs[i].target_sec = (int)((r0.lh >> 9) & 07UL);
        obj->relocs[i].offset = r0.rh;
        obj->relocs[i].symbol = r1.rh;
    }
    if (obj->entry_symbol > obj->symbol_count)
        goto bad;
    return 0;
bad:
    dobj_free(obj);
    return -1;
}

int dobj_write(FILE *f, const struct dobj_object *obj)
{
    struct dobj_word h[5];
    unsigned long i;

    h[0] = sixbit_word(DOBJ_MAGIC_TEXT);
    h[1] = dobj_word_halves(obj->text_words, obj->data_words);
    h[2] = dobj_word_halves(obj->bss_words, obj->symbol_count);
    h[3] = dobj_word_halves(obj->reloc_count, 0UL);
    h[4] = dobj_word_halves(0UL, obj->entry_symbol);
    for (i = 0UL; i < 5UL; i++)
        if (dobj_write_word(f, h[i]) != 0) return -1;
    for (i = 0UL; i < obj->text_words; i++)
        if (dobj_write_word(f, obj->text[i]) != 0) return -1;
    for (i = 0UL; i < obj->data_words; i++)
        if (dobj_write_word(f, obj->data[i]) != 0) return -1;
    for (i = 0UL; i < obj->symbol_count; i++) {
        struct dobj_word s0;
        unsigned long len;
        len = (unsigned long)strlen(obj->symbols[i].name);
        if (len > DOBJ_NAME_MAX) return -1;
        s0 = dobj_word_halves(((unsigned long)obj->symbols[i].kind << 15) |
                              ((unsigned long)obj->symbols[i].sec << 12) | len,
                              0UL);
        if (dobj_write_word(f, s0) != 0 ||
            dobj_write_word(f, obj->symbols[i].value) != 0 ||
            write_name(f, obj->symbols[i].name) != 0) return -1;
    }
    for (i = 0UL; i < obj->reloc_count; i++) {
        struct dobj_word r0, r1;
        r0 = dobj_word_halves(((unsigned long)obj->relocs[i].loc_sec << 15) |
                              ((unsigned long)obj->relocs[i].type << 12) |
                              ((unsigned long)obj->relocs[i].target_sec << 9),
                              obj->relocs[i].offset);
        r1 = dobj_word_halves(0UL, obj->relocs[i].symbol);
        if (dobj_write_word(f, r0) != 0 || dobj_write_word(f, r1) != 0 ||
            dobj_write_word(f, obj->relocs[i].addend) != 0) return -1;
    }
    return 0;
}

static int probe_magic(FILE *f, const char *text)
{
    long pos;
    struct dobj_word got;
    struct dobj_word want;
    int ok;

    pos = ftell(f);
    if (pos < 0L) return 0;
    ok = dobj_read_word(f, &got) == 0;
    if (fseek(f, pos, SEEK_SET) != 0) return 0;
    if (!ok) return 0;
    want = sixbit_word(text);
    return word_equal(got, want);
}

int dobj_is_object(FILE *f) { return probe_magic(f, DOBJ_MAGIC_TEXT); }
int dobj_is_archive(FILE *f) { return probe_magic(f, DARC_MAGIC_TEXT); }

static unsigned long object_word_count(const struct dobj_object *obj)
{
    unsigned long n;
    unsigned long i;
    n = 5UL + obj->text_words + obj->data_words + obj->reloc_count * 3UL;
    for (i = 0UL; i < obj->symbol_count; i++)
        n += 2UL + name_words((unsigned long)strlen(obj->symbols[i].name));
    return n;
}

int dobj_archive_write(FILE *out, struct dobj_object *objects,
                       unsigned long count)
{
    unsigned long index_count;
    unsigned long index_words;
    unsigned long member_base;
    unsigned long member_off;
    unsigned long i, j;
    struct dobj_word h;

    index_count = 0UL;
    index_words = 0UL;
    for (i = 0UL; i < count; i++) {
        for (j = 0UL; j < objects[i].symbol_count; j++) {
            if (objects[i].symbols[j].kind != DOBJ_SYM_DEF)
                continue;
            index_count++;
            index_words += 1UL + name_words((unsigned long)strlen(objects[i].symbols[j].name));
        }
    }
    if (index_count > HALF_MASK || count > HALF_MASK)
        return -1;
    if (dobj_write_word(out, sixbit_word(DARC_MAGIC_TEXT)) != 0 ||
        dobj_write_word(out, dobj_word_halves(count, index_count)) != 0)
        return -1;
    member_base = 2UL + index_words;
    member_off = member_base;
    for (i = 0UL; i < count; i++) {
        for (j = 0UL; j < objects[i].symbol_count; j++) {
            unsigned long len;
            if (objects[i].symbols[j].kind != DOBJ_SYM_DEF)
                continue;
            len = (unsigned long)strlen(objects[i].symbols[j].name);
            h = dobj_word_halves(member_off, len);
            if (dobj_write_word(out, h) != 0 ||
                write_name(out, objects[i].symbols[j].name) != 0)
                return -1;
        }
        member_off += 1UL + object_word_count(&objects[i]);
    }
    for (i = 0UL; i < count; i++) {
        h = dobj_word_halves(0UL, object_word_count(&objects[i]));
        if (dobj_write_word(out, h) != 0 || dobj_write(out, &objects[i]) != 0)
            return -1;
    }
    return 0;
}

int dobj_archive_open(FILE *f, struct dobj_archive *ar)
{
    struct dobj_word h0, h1;
    unsigned long i;

    memset(ar, 0, sizeof(*ar));
    if (fseek(f, 0L, SEEK_SET) != 0 || dobj_read_word(f, &h0) != 0 ||
        dobj_read_word(f, &h1) != 0 ||
        !word_equal(h0, sixbit_word(DARC_MAGIC_TEXT)))
        return -1;
    ar->file = f;
    ar->index_count = h1.rh;
    ar->index = (struct dobj_archive_index *)xarray(ar->index_count, sizeof(*ar->index));
    if (ar->index_count && !ar->index)
        return -1;
    for (i = 0UL; i < ar->index_count; i++) {
        struct dobj_word ih;
        unsigned long len;
        if (dobj_read_word(f, &ih) != 0) goto bad;
        ar->index[i].member_offset = ih.lh;
        len = ih.rh;
        if (read_name(f, ar->index[i].name, len) != 0) goto bad;
    }
    return 0;
bad:
    dobj_archive_close(ar);
    return -1;
}

void dobj_archive_close(struct dobj_archive *ar)
{
    free(ar->index);
    ar->index = NULL;
    ar->index_count = 0UL;
    ar->file = NULL;
}

int dobj_archive_find(const struct dobj_archive *ar, const char *name,
                      unsigned long *member_offset)
{
    unsigned long i;
    for (i = 0UL; i < ar->index_count; i++) {
        if (strcmp(ar->index[i].name, name) == 0) {
            *member_offset = ar->index[i].member_offset;
            return 1;
        }
    }
    return 0;
}

int dobj_archive_read_member(const struct dobj_archive *ar,
                             unsigned long member_offset,
                             struct dobj_object *obj)
{
    struct dobj_word mh;
    long pos;
    pos = (long)member_offset * 8L;
    if (fseek(ar->file, pos, SEEK_SET) != 0 || dobj_read_word(ar->file, &mh) != 0)
        return -1;
    return dobj_read(ar->file, obj);
}
