#include "dobj.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define HALF_MASK 0777777UL
#define MAX_INPUTS 256

struct link_object {
    struct dobj_object obj;
    unsigned long text_base;
    unsigned long data_base;
    unsigned long bss_base;
    int selected;
};

struct link_archive {
    FILE *file;
    struct dobj_archive ar;
    unsigned long loaded_offsets[MAX_INPUTS];
    int loaded_count;
};

struct global_def {
    char name[DOBJ_NAME_MAX + 1];
    int object_index;
    unsigned long symbol_index;
};

struct linker {
    struct link_object objects[MAX_INPUTS];
    int object_count;
    struct link_archive archives[MAX_INPUTS];
    int archive_count;
    struct global_def defs[MAX_INPUTS * 16];
    int def_count;
};

static struct dobj_word sixbit_word(const char *s)
{
    struct dobj_word w;
    unsigned long v[6];
    int i;
    for (i = 0; i < 6; i++) {
        unsigned int ch;
        ch = (unsigned int)(unsigned char)s[i];
        if (ch < 040U || ch > 0137U) ch = 040U;
        v[i] = (unsigned long)(ch - 040U) & 077UL;
    }
    w.lh = (v[0] << 12) | (v[1] << 6) | v[2];
    w.rh = (v[3] << 12) | (v[4] << 6) | v[5];
    return w;
}

static int add_object(struct linker *l, struct dobj_object *obj)
{
    if (l->object_count >= MAX_INPUTS)
        return -1;
    l->objects[l->object_count].obj = *obj;
    l->objects[l->object_count].selected = 1;
    memset(obj, 0, sizeof(*obj));
    l->object_count++;
    return 0;
}

static int find_def(const struct linker *l, const char *name)
{
    int i;
    for (i = 0; i < l->def_count; i++)
        if (strcmp(l->defs[i].name, name) == 0)
            return i;
    return -1;
}

static int rebuild_defs(struct linker *l)
{
    int oi;
    l->def_count = 0;
    for (oi = 0; oi < l->object_count; oi++) {
        struct dobj_object *o;
        unsigned long si;
        o = &l->objects[oi].obj;
        for (si = 0UL; si < o->symbol_count; si++) {
            struct dobj_symbol *s;
            s = &o->symbols[si];
            if (s->kind != DOBJ_SYM_DEF)
                continue;
            if (find_def(l, s->name) >= 0) {
                fprintf(stderr, "dlink: duplicate global: %s\n", s->name);
                return -1;
            }
            if (l->def_count >= (int)(sizeof(l->defs) / sizeof(l->defs[0]))) {
                fprintf(stderr, "dlink: too many globals\n");
                return -1;
            }
            strcpy(l->defs[l->def_count].name, s->name);
            l->defs[l->def_count].object_index = oi;
            l->defs[l->def_count].symbol_index = si;
            l->def_count++;
        }
    }
    return 0;
}

static int archive_offset_loaded(const struct link_archive *a,
                                 unsigned long off)
{
    int i;
    for (i = 0; i < a->loaded_count; i++)
        if (a->loaded_offsets[i] == off)
            return 1;
    return 0;
}

static int try_extract(struct linker *l, const char *name)
{
    int ai;
    for (ai = 0; ai < l->archive_count; ai++) {
        unsigned long off;
        struct dobj_object obj;
        if (!dobj_archive_find(&l->archives[ai].ar, name, &off))
            continue;
        if (archive_offset_loaded(&l->archives[ai], off))
            return 0;
        if (l->archives[ai].loaded_count >= MAX_INPUTS)
            return -1;
        if (dobj_archive_read_member(&l->archives[ai].ar, off, &obj) != 0) {
            fprintf(stderr, "dlink: bad archive member for %s\n", name);
            return -1;
        }
        if (add_object(l, &obj) != 0) {
            dobj_free(&obj);
            return -1;
        }
        l->archives[ai].loaded_offsets[l->archives[ai].loaded_count++] = off;
        return 1;
    }
    return 0;
}

static int resolve_archives(struct linker *l)
{
    int changed;
    do {
        int oi;
        changed = 0;
        if (rebuild_defs(l) != 0)
            return -1;
        for (oi = 0; oi < l->object_count; oi++) {
            struct dobj_object *o;
            unsigned long si;
            o = &l->objects[oi].obj;
            for (si = 0UL; si < o->symbol_count; si++) {
                int r;
                if (o->symbols[si].kind != DOBJ_SYM_UNDEF)
                    continue;
                if (find_def(l, o->symbols[si].name) >= 0)
                    continue;
                r = try_extract(l, o->symbols[si].name);
                if (r < 0)
                    return -1;
                if (r > 0) {
                    changed = 1;
                    break;
                }
            }
            if (changed)
                break;
        }
    } while (changed);
    if (rebuild_defs(l) != 0)
        return -1;
    return 0;
}

static int check_undefined(struct linker *l)
{
    int oi;
    for (oi = 0; oi < l->object_count; oi++) {
        struct dobj_object *o;
        unsigned long si;
        o = &l->objects[oi].obj;
        for (si = 0UL; si < o->symbol_count; si++) {
            if (o->symbols[si].kind == DOBJ_SYM_UNDEF &&
                find_def(l, o->symbols[si].name) < 0) {
                fprintf(stderr, "dlink: undefined symbol: %s\n",
                        o->symbols[si].name);
                return -1;
            }
        }
    }
    return 0;
}

static int layout(struct linker *l, unsigned long *text_words,
                  unsigned long *data_words, unsigned long *bss_words)
{
    unsigned long t, d, b;
    int i;
    t = d = b = 0UL;
    for (i = 0; i < l->object_count; i++) {
        l->objects[i].text_base = t;
        t += l->objects[i].obj.text_words;
    }
    for (i = 0; i < l->object_count; i++) {
        l->objects[i].data_base = t + d;
        d += l->objects[i].obj.data_words;
    }
    for (i = 0; i < l->object_count; i++) {
        l->objects[i].bss_base = t + d + b;
        b += l->objects[i].obj.bss_words;
    }
    if (t > HALF_MASK || d > HALF_MASK || b > HALF_MASK ||
        t + d > HALF_MASK || t + d + b > HALF_MASK) {
        fprintf(stderr, "dlink: image exceeds 18-bit address space\n");
        return -1;
    }
    *text_words = t;
    *data_words = d;
    *bss_words = b;
    return 0;
}

static unsigned long section_base(const struct link_object *o, int sec)
{
    if (sec == DOBJ_SEC_TEXT) return o->text_base;
    if (sec == DOBJ_SEC_DATA) return o->data_base;
    if (sec == DOBJ_SEC_BSS) return o->bss_base;
    return 0UL;
}

static int symbol_address(struct linker *l, int def_index,
                          unsigned long *addr, int *relative)
{
    struct global_def *d;
    struct link_object *o;
    struct dobj_symbol *s;
    d = &l->defs[def_index];
    o = &l->objects[d->object_index];
    s = &o->obj.symbols[d->symbol_index];
    if (s->sec == DOBJ_SEC_ABS) {
        if (s->value.lh != 0UL || s->value.rh > HALF_MASK)
            return -1;
        *addr = s->value.rh;
        *relative = 0;
        return 0;
    }
    if (s->sec < DOBJ_SEC_TEXT || s->sec > DOBJ_SEC_BSS ||
        s->value.lh != 0UL)
        return -1;
    *addr = section_base(o, s->sec) + s->value.rh;
    *relative = 1;
    return *addr <= HALF_MASK ? 0 : -1;
}

static void set_reloc_bit(struct dobj_word *map, unsigned long off)
{
    unsigned long wi;
    unsigned long bit;
    wi = off / 36UL;
    bit = off % 36UL;
    if (bit < 18UL)
        map[wi].lh |= 1UL << (17UL - bit);
    else
        map[wi].rh |= 1UL << (35UL - bit);
}

static int apply_relocs(struct linker *l, struct dobj_word *image,
                        struct dobj_word *relmap)
{
    int oi;
    for (oi = 0; oi < l->object_count; oi++) {
        struct link_object *lo;
        unsigned long ri;
        lo = &l->objects[oi];
        for (ri = 0UL; ri < lo->obj.reloc_count; ri++) {
            struct dobj_reloc *r;
            unsigned long loc;
            unsigned long target;
            int relative;
            long add;
            r = &lo->obj.relocs[ri];
            if (r->loc_sec == DOBJ_SEC_TEXT) {
                if (r->offset >= lo->obj.text_words) return -1;
                loc = lo->text_base + r->offset;
            } else if (r->loc_sec == DOBJ_SEC_DATA) {
                if (r->offset >= lo->obj.data_words) return -1;
                loc = lo->data_base + r->offset;
            } else {
                fprintf(stderr, "dlink: relocation location is not stored section\n");
                return -1;
            }
            if (dobj_word_addend18(r->addend, &add) != 0) {
                fprintf(stderr, "dlink: relocation addend is outside DOBJ1 RH18 range\n");
                return -1;
            }
            if (r->type == DOBJ_RELOC_LOCAL_RH18) {
                if (r->target_sec < DOBJ_SEC_TEXT || r->target_sec > DOBJ_SEC_BSS)
                    return -1;
                target = section_base(lo, r->target_sec);
                relative = 1;
            } else if (r->type == DOBJ_RELOC_SYMBOL_RH18) {
                int di;
                struct dobj_symbol *u;
                if (r->symbol == 0UL || r->symbol > lo->obj.symbol_count)
                    return -1;
                u = &lo->obj.symbols[r->symbol - 1UL];
                di = find_def(l, u->name);
                if (di < 0 || symbol_address(l, di, &target, &relative) != 0)
                    return -1;
            } else {
                fprintf(stderr, "dlink: unsupported relocation type\n");
                return -1;
            }
            if ((long)target + add < 0L || (unsigned long)((long)target + add) > HALF_MASK) {
                fprintf(stderr, "dlink: RH18 relocation overflow\n");
                return -1;
            }
            image[loc].rh = (unsigned long)((long)target + add) & HALF_MASK;
            if (relative)
                set_reloc_bit(relmap, loc);
        }
    }
    return 0;
}

static int write_dxr(const char *name, struct linker *l)
{
    unsigned long tw, dw, bw, iw, rw;
    struct dobj_word *image;
    struct dobj_word *relmap;
    struct dobj_word h;
    unsigned long pos;
    unsigned long entry;
    int i;
    FILE *f;

    if (layout(l, &tw, &dw, &bw) != 0)
        return -1;
    iw = tw + dw;
    rw = (iw + 35UL) / 36UL;
    image = (struct dobj_word *)calloc((size_t)iw, sizeof(*image));
    relmap = (struct dobj_word *)calloc((size_t)rw, sizeof(*relmap));
    if ((iw && image == NULL) || (rw && relmap == NULL)) {
        free(image); free(relmap); return -1;
    }
    for (i = 0; i < l->object_count; i++) {
        unsigned long j;
        for (j = 0UL; j < l->objects[i].obj.text_words; j++)
            image[l->objects[i].text_base + j] = l->objects[i].obj.text[j];
        for (j = 0UL; j < l->objects[i].obj.data_words; j++)
            image[l->objects[i].data_base + j] = l->objects[i].obj.data[j];
    }
    if (apply_relocs(l, image, relmap) != 0) {
        free(image); free(relmap); return -1;
    }
    entry = 0UL;
    for (i = 0; i < l->object_count; i++) {
        struct dobj_object *o;
        if (l->objects[i].obj.entry_symbol == 0UL) continue;
        o = &l->objects[i].obj;
        if (o->entry_symbol > o->symbol_count) continue;
        {
            int di;
            int rel;
            di = find_def(l, o->symbols[o->entry_symbol - 1UL].name);
            if (di < 0 || symbol_address(l, di, &entry, &rel) != 0) {
                free(image); free(relmap); return -1;
            }
        }
        break;
    }
    f = fopen(name, "wb");
    if (f == NULL) { free(image); free(relmap); return -1; }
    h = sixbit_word("DXR1  ");
    h.rh = entry;
    if (dobj_write_word(f, h) != 0 ||
        dobj_write_word(f, dobj_word_halves(iw, bw)) != 0) goto bad;
    for (pos = 0UL; pos < iw; pos++) if (dobj_write_word(f, image[pos]) != 0) goto bad;
    for (pos = 0UL; pos < rw; pos++) if (dobj_write_word(f, relmap[pos]) != 0) goto bad;
    if (fclose(f) != 0) { remove(name); free(image); free(relmap); return -1; }
    free(image); free(relmap); return 0;
bad:
    fclose(f); remove(name); free(image); free(relmap); return -1;
}

static void cleanup(struct linker *l)
{
    int i;
    for (i = 0; i < l->object_count; i++)
        dobj_free(&l->objects[i].obj);
    for (i = 0; i < l->archive_count; i++) {
        dobj_archive_close(&l->archives[i].ar);
        fclose(l->archives[i].file);
    }
}

static void usage(void)
{
    fprintf(stderr, "usage: dlink -o out.dxr input.dobj|library.darc ...\n");
}

int main(int argc, char **argv)
{
    struct linker l;
    const char *out;
    int i;
    int first;
    int rc;

    memset(&l, 0, sizeof(l));
    out = NULL;
    first = 1;
    if (argc > 2 && strcmp(argv[1], "-o") == 0) {
        out = argv[2];
        first = 3;
    }
    if (out == NULL || first >= argc) { usage(); return 1; }
    rc = 1;
    for (i = first; i < argc; i++) {
        FILE *f;
        f = fopen(argv[i], "rb");
        if (f == NULL) { perror(argv[i]); goto done; }
        if (dobj_is_object(f)) {
            struct dobj_object obj;
            if (fseek(f, 0L, SEEK_SET) != 0 || dobj_read(f, &obj) != 0 ||
                add_object(&l, &obj) != 0) {
                fclose(f); goto done;
            }
            fclose(f);
        } else if (dobj_is_archive(f)) {
            struct link_archive *a;
            if (l.archive_count >= MAX_INPUTS) { fclose(f); goto done; }
            a = &l.archives[l.archive_count];
            a->file = f;
            if (dobj_archive_open(f, &a->ar) != 0) { fclose(f); goto done; }
            l.archive_count++;
        } else {
            fprintf(stderr, "dlink: unknown input format: %s\n", argv[i]);
            fclose(f); goto done;
        }
    }
    if (resolve_archives(&l) != 0 || check_undefined(&l) != 0)
        goto done;
    if (write_dxr(out, &l) != 0) {
        fprintf(stderr, "dlink: link failed\n");
        goto done;
    }
    rc = 0;
done:
    cleanup(&l);
    return rc;
}
