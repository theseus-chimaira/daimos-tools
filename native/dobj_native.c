#include "dobj_native.h"

#define ND_HALF_MASK 0777777UL

kword_t
nd_lh(kword_t w)
{
        return (w >> 18) & ND_HALF_MASK;
}

kword_t
nd_rh(kword_t w)
{
        return w & ND_HALF_MASK;
}

kword_t
nd_halves(kword_t lh, kword_t rh)
{
        return ((lh & ND_HALF_MASK) << 18) | (rh & ND_HALF_MASK);
}

kword_t
nd_magic(const char *s)
{
        kword_t w;
        unsigned int i;
        unsigned int ch;

        w = 0UL;
        for (i = 0U; i < 6U; ++i) {
                ch = (unsigned int)(unsigned char)s[i];
                if (ch < 040U || ch > 0137U)
                        ch = 040U;
                w = (w << 6) | (kword_t)((ch - 040U) & 077U);
        }
        return w;
}

unsigned int
nd_name_words(unsigned int len)
{
        return (len + 3U) / 4U;
}

unsigned int
nd_strlen(const char *s)
{
        unsigned int n;
        n = 0U;
        while (s[n] != 0)
                ++n;
        return n;
}

int
nd_streq(const char *a, const char *b)
{
        unsigned int i;
        for (i = 0U; a[i] != 0 || b[i] != 0; ++i)
                if (a[i] != b[i])
                        return 0;
        return 1;
}

void
nd_strcpy(char *dst, const char *src)
{
        unsigned int i;
        for (i = 0U;; ++i) {
                dst[i] = src[i];
                if (src[i] == 0)
                        break;
        }
}

int
nd_seek(int fd, kword_t word)
{
        return dsys_seek(fd, word, SYS_SEEK_SET) == word ? 0 : -1;
}

int
nd_read_exact(int fd, kword_t *dst, unsigned int words)
{
        unsigned int done;
        int n;

        done = 0U;
        while (done < words) {
                n = dsys_read_words(fd, &dst[done], words - done);
                if (n <= 0)
                        return -1;
                done += (unsigned int)n;
        }
        return 0;
}

int
nd_write_exact(int fd, const kword_t *src, unsigned int words)
{
        return u_write_words_all(fd, src, words);
}

int
nd_read_at(int fd, kword_t word, kword_t *dst, unsigned int words)
{
        return nd_seek(fd, word) != 0 ? -1 : nd_read_exact(fd, dst, words);
}

int
nd_write_at(int fd, kword_t word, const kword_t *src, unsigned int words)
{
        return nd_seek(fd, word) != 0 ? -1 : nd_write_exact(fd, src, words);
}

int
nd_read_name(int fd, unsigned int len, char *name)
{
        unsigned int pos;
        unsigned int i;
        unsigned int j;
        kword_t w;
        unsigned int v[4];

        if (len > ND_NAME_MAX)
                return -1;
        pos = 0U;
        for (i = 0U; i < nd_name_words(len); ++i) {
                if (nd_read_exact(fd, &w, 1U) != 0)
                        return -1;
                v[0] = (unsigned int)((w >> 27) & 0777UL);
                v[1] = (unsigned int)((w >> 18) & 0777UL);
                v[2] = (unsigned int)((w >> 9) & 0777UL);
                v[3] = (unsigned int)(w & 0777UL);
                for (j = 0U; j < 4U && pos < len; ++j)
                        name[pos++] = (char)(v[j] & 0177U);
        }
        name[len] = 0;
        return 0;
}

int
nd_write_name(int fd, const char *name)
{
        unsigned int len;
        unsigned int pos;
        unsigned int i;
        unsigned int j;
        kword_t w;

        len = nd_strlen(name);
        if (len > ND_NAME_MAX)
                return -1;
        pos = 0U;
        for (i = 0U; i < nd_name_words(len); ++i) {
                w = 0UL;
                for (j = 0U; j < 4U; ++j) {
                        w <<= 9;
                        if (pos < len)
                                w |= (kword_t)((unsigned char)name[pos++] &
                                    0177U);
                }
                if (nd_write_exact(fd, &w, 1U) != 0)
                        return -1;
        }
        return 0;
}

int
nd_obj_parse(int fd, kword_t base, struct nd_obj *obj)
{
        kword_t h[5];
        kword_t off;
        kword_t s0;
        unsigned int i;
        unsigned int len;

        if (nd_read_at(fd, base, h, 5U) != 0 || h[0] != nd_magic("DOBJ1 "))
                return -1;
        obj->base = base;
        obj->text_words = nd_lh(h[1]);
        obj->data_words = nd_rh(h[1]);
        obj->bss_words = nd_lh(h[2]);
        obj->symbol_count = nd_rh(h[2]);
        obj->reloc_count = nd_lh(h[3]);
        obj->entry_symbol = nd_rh(h[4]);
        if (obj->entry_symbol > obj->symbol_count)
                return -1;
        obj->text_off = base + 5UL;
        obj->data_off = obj->text_off + obj->text_words;
        obj->symbol_off = obj->data_off + obj->data_words;
        off = obj->symbol_off;
        for (i = 0U; i < (unsigned int)obj->symbol_count; ++i) {
                if (nd_read_at(fd, off, &s0, 1U) != 0)
                        return -1;
                len = (unsigned int)(nd_lh(s0) & 077UL);
                if (len > ND_NAME_MAX)
                        return -1;
                off += 2UL + (kword_t)nd_name_words(len);
        }
        obj->reloc_off = off;
        obj->object_words = obj->reloc_off - base + obj->reloc_count * 3UL;
        return 0;
}

int
nd_obj_symbol(int fd, const struct nd_obj *obj, unsigned int index,
    struct nd_symbol *sym)
{
        kword_t off;
        kword_t h[2];
        unsigned int i;
        unsigned int len;

        if (index >= (unsigned int)obj->symbol_count)
                return -1;
        off = obj->symbol_off;
        for (i = 0U; i <= index; ++i) {
                if (nd_read_at(fd, off, h, 2U) != 0)
                        return -1;
                len = (unsigned int)(nd_lh(h[0]) & 077UL);
                if (len > ND_NAME_MAX)
                        return -1;
                if (i == index) {
                        sym->kind = (unsigned int)((nd_lh(h[0]) >> 15) & 07UL);
                        sym->sec = (unsigned int)((nd_lh(h[0]) >> 12) & 07UL);
                        sym->value = h[1];
                        if (nd_seek(fd, off + 2UL) != 0 ||
                            nd_read_name(fd, len, sym->name) != 0)
                                return -1;
                        return 0;
                }
                off += 2UL + (kword_t)nd_name_words(len);
        }
        return -1;
}

int
nd_obj_each_symbol(int fd, const struct nd_obj *obj,
    int (*fn)(unsigned int, const struct nd_symbol *, void *), void *arg)
{
        kword_t off;
        kword_t h[2];
        struct nd_symbol sym;
        unsigned int i;
        unsigned int len;

        off = obj->symbol_off;
        for (i = 0U; i < (unsigned int)obj->symbol_count; ++i) {
                if (nd_read_at(fd, off, h, 2U) != 0)
                        return -1;
                len = (unsigned int)(nd_lh(h[0]) & 077UL);
                if (len > ND_NAME_MAX || nd_seek(fd, off + 2UL) != 0)
                        return -1;
                sym.kind = (unsigned int)((nd_lh(h[0]) >> 15) & 07UL);
                sym.sec = (unsigned int)((nd_lh(h[0]) >> 12) & 07UL);
                sym.value = h[1];
                if (nd_read_name(fd, len, sym.name) != 0)
                        return -1;
                if (fn(i, &sym, arg) != 0)
                        return -1;
                off += 2UL + (kword_t)nd_name_words(len);
        }
        return 0;
}

int
nd_obj_reloc(int fd, const struct nd_obj *obj, unsigned int index,
    struct nd_reloc *rel)
{
        kword_t r[3];
        if (index >= (unsigned int)obj->reloc_count ||
            nd_read_at(fd, obj->reloc_off + (kword_t)index * 3UL,
            r, 3U) != 0)
                return -1;
        rel->loc_sec = (unsigned int)((nd_lh(r[0]) >> 15) & 07UL);
        rel->type = (unsigned int)((nd_lh(r[0]) >> 12) & 07UL);
        rel->target_sec = (unsigned int)((nd_lh(r[0]) >> 9) & 07UL);
        rel->offset = nd_rh(r[0]);
        rel->symbol = nd_rh(r[1]);
        rel->addend = r[2];
        return 0;
}

int
nd_addend18(kword_t w, int *value)
{
        kword_t lh;
        kword_t rh;
        lh = nd_lh(w);
        rh = nd_rh(w);
        if (lh == 0UL) {
                *value = (int)rh;
                return 0;
        }
        if (lh == ND_HALF_MASK && (rh & 0400000UL) != 0UL) {
                *value = (int)rh - 01000000;
                return 0;
        }
        return -1;
}

int
nd_arc_header(int fd, kword_t *members, kword_t *index_count)
{
        kword_t h[2];
        if (nd_read_at(fd, 0UL, h, 2U) != 0 || h[0] != nd_magic("DARC1 "))
                return -1;
        *members = nd_lh(h[1]);
        *index_count = nd_rh(h[1]);
        return 0;
}

int
nd_arc_find(int fd, const char *name, kword_t *member_off)
{
        kword_t members;
        kword_t count;
        kword_t h;
        char current[ND_NAME_MAX + 1U];
        unsigned int i;
        unsigned int len;

        if (nd_arc_header(fd, &members, &count) != 0 || nd_seek(fd, 2UL) != 0)
                return -1;
        (void)members;
        for (i = 0U; i < (unsigned int)count; ++i) {
                if (nd_read_exact(fd, &h, 1U) != 0)
                        return -1;
                len = (unsigned int)nd_rh(h);
                if (len > ND_NAME_MAX || nd_read_name(fd, len, current) != 0)
                        return -1;
                if (nd_streq(current, name)) {
                        *member_off = nd_lh(h);
                        return 1;
                }
        }
        return 0;
}

int
nd_copy_words(int infd, kword_t inoff, int outfd, kword_t words)
{
        kword_t buf[ND_IO_WORDS];
        unsigned int n;

        if (nd_seek(infd, inoff) != 0)
                return -1;
        while (words != 0UL) {
                n = words > ND_IO_WORDS ? ND_IO_WORDS : (unsigned int)words;
                if (nd_read_exact(infd, buf, n) != 0 ||
                    nd_write_exact(outfd, buf, n) != 0)
                        return -1;
                words -= (kword_t)n;
        }
        return 0;
}

int
nd_put_name(int fd, const char *name)
{
        unsigned int i;
        for (i = 0U; name[i] != 0; ++i)
                if (u_putc(fd, name[i]) != 0)
                        return -1;
        return 0;
}

int
nd_put_uint(int fd, kword_t value)
{
        return u_put_uint(fd, value);
}

int
nd_put_octal(int fd, kword_t value, unsigned int digits)
{
        return u_put_octal(fd, value, digits);
}
