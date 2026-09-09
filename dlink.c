#include "dobj.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <limits.h>

#define HALF_MASK 0777777UL
#define DXR_BSS_MASK 0177777UL
#define DXR_F_PURE   0200000UL
#define DXR_F_IMPURE 0400000UL

#define PURITY_UNKNOWN 0
#define PURITY_PURE    1
#define PURITY_IMPURE  2
struct link_object {
    struct dobj_object obj;
    unsigned long text_base;
    unsigned long data_base;
    unsigned long bss_base;
};

struct link_archive {
    FILE *file;
    struct dobj_archive ar;
    unsigned long *loaded_offsets;
    int loaded_count;
    int loaded_cap;
};

struct global_def {
    char name[DOBJ_NAME_MAX + 1];
    int object_index;
    unsigned long symbol_index;
};

struct linker {
    unsigned long image_base;
    struct link_object *objects;
    int object_count;
    int object_cap;
    struct link_archive *archives;
    int archive_count;
    int archive_cap;
    struct global_def *defs;
    int def_count;
    int def_cap;
    int daimos_uuo_relax;
};


static int opcode_writes_ea(unsigned long op)
{
    if (op == 0136UL || op == 0202UL || op == 0203UL ||
        op == 0206UL || op == 0207UL || op == 0212UL ||
        op == 0213UL || op == 0216UL || op == 0217UL ||
        op == 0222UL || op == 0223UL || op == 0226UL ||
        op == 0227UL || op == 0232UL || op == 0233UL ||
        op == 0236UL || op == 0237UL || op == 0250UL ||
        op == 0262UL || op == 0264UL || op == 0266UL ||
        op == 0272UL || op == 0273UL || op == 0276UL ||
        op == 0277UL)
        return 1;
    if (op >= 0350UL && op <= 0357UL)
        return 1;
    if (op >= 0370UL && op <= 0377UL)
        return 1;
    if (op >= 0400UL && op <= 0477UL && (op & 3UL) >= 2UL)
        return 1;
    if (op >= 0500UL && op <= 0577UL && (op & 3UL) >= 2UL)
        return 1;
    return 0;
}

/*
 * Prove only the easy case: an instruction with an unindexed, non-indirect
 * effective address which is a memory-writing operand in the final TEXT
 * range.  Failure to find such an instruction is deliberately not a proof
 * of purity; computed/indirect stores and block/byte operations can hide a
 * text write.
 */
static int definite_text_write(const struct dobj_word *image,
                               unsigned long text_words,
                               unsigned long *where)
{
    unsigned long i;
    for (i = 0UL; i < text_words; i++) {
        unsigned long op, indirect, index, ea;
        op = (image[i].lh >> 9) & 0777UL;
        indirect = (image[i].lh >> 4) & 1UL;
        index = image[i].lh & 017UL;
        ea = image[i].rh & HALF_MASK;
        if (!indirect && index == 0UL && ea < text_words &&
            opcode_writes_ea(op)) {
            if (where != NULL)
                *where = i;
            return 1;
        }
    }
    return 0;
}

static void *grow_array(void *p, int *cap, size_t elem_size)
{
    int newcap;
    void *q;

    if (*cap > INT_MAX / 2)
        return NULL;
    newcap = *cap == 0 ? 8 : *cap * 2;
    if ((size_t)newcap > ((size_t)-1) / elem_size)
        return NULL;
    q = realloc(p, (size_t)newcap * elem_size);
    if (q == NULL)
        return NULL;
    *cap = newcap;
    return q;
}

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
    if (l->object_count == l->object_cap) {
        void *p;
        p = grow_array(l->objects, &l->object_cap, sizeof(l->objects[0]));
        if (p == NULL)
            return -1;
        l->objects = (struct link_object *)p;
    }
    l->objects[l->object_count].obj = *obj;
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
            if (l->def_count == l->def_cap) {
                void *p;
                p = grow_array(l->defs, &l->def_cap, sizeof(l->defs[0]));
                if (p == NULL) {
                    fprintf(stderr, "dlink: out of memory for globals\n");
                    return -1;
                }
                l->defs = (struct global_def *)p;
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
        if (l->archives[ai].loaded_count == l->archives[ai].loaded_cap) {
            void *p;
            p = grow_array(l->archives[ai].loaded_offsets,
                           &l->archives[ai].loaded_cap,
                           sizeof(l->archives[ai].loaded_offsets[0]));
            if (p == NULL)
                return -1;
            l->archives[ai].loaded_offsets = (unsigned long *)p;
        }
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


struct daimos_uuo_veneer {
    const char *name;
    unsigned long opcode;
    unsigned long index;
};

/*
 * Only veneers whose complete implementation is exactly one monitor UUO
 * followed by POPJ are listed here.  Helpers which alter arguments before
 * trapping (for example mkdir, dtfs_check and write_nonets) are intentionally
 * absent: relaxing those would change the C ABI semantics.
 */
static const struct daimos_uuo_veneer daimos_uuo_veneers[] = {
    { "dsys_exit",        0040UL, 1UL },
    { "dsys_open",        0041UL, 1UL },
    { "dsys_close",       0042UL, 1UL },
    { "dsys_write_chars", 0043UL, 1UL },
    { "dsys_chdir",       0045UL, 1UL },
    { "dsys_getcwd",      0046UL, 1UL },
    { "dsys_stat",        0047UL, 1UL },
    { "dsys_dirread",     0050UL, 1UL },
    { "dsys_unlink",      0052UL, 1UL },
    { "dsys_rename",      0053UL, 1UL },
    { "dsys_truncate",    0054UL, 1UL },
    { "dsys_read_words",  0055UL, 1UL },
    { "dsys_write_words", 0056UL, 1UL },
    { "dsys_procinfo",    0057UL, 1UL },
    { "dsys_meminfo",     0060UL, 1UL },
    { "dsys_readchar",    0061UL, 1UL },
    { "dsys_writechar",   0062UL, 1UL },
    { "dsys_halt",        0063UL, 0UL },
    { "dsys_chmod",       0064UL, 1UL },
    { "dsys_dtfs_format", 0065UL, 1UL },
    { "dsys_dtfs_mount",  0066UL, 1UL },
    { "dsys_unmount",     0067UL, 1UL },
    { "dsys_flock",       0070UL, 1UL },
    { "dsys_dup",         0071UL, 1UL },
    { "dsys_symlink",     0072UL, 1UL },
    { "dsys_nice",        0073UL, 1UL }
};

static const struct daimos_uuo_veneer *daimos_uuo_veneer(const char *name)
{
    size_t i;
    for (i = 0U; i < sizeof(daimos_uuo_veneers) / sizeof(daimos_uuo_veneers[0]); i++)
        if (strcmp(name, daimos_uuo_veneers[i].name) == 0)
            return &daimos_uuo_veneers[i];
    return NULL;
}

static int direct_pushj17_reloc(const struct dobj_object *o,
                                const struct dobj_reloc *r,
                                const char *name)
{
    const struct dobj_symbol *u;
    unsigned long lh;
    long add;

    if (r->loc_sec != DOBJ_SEC_TEXT || r->type != DOBJ_RELOC_SYMBOL_RH18 ||
        r->offset >= o->text_words || r->symbol == 0UL ||
        r->symbol > o->symbol_count)
        return 0;
    u = &o->symbols[r->symbol - 1UL];
    if (u->kind != DOBJ_SYM_UNDEF || strcmp(u->name, name) != 0)
        return 0;
    if (dobj_word_addend18(r->addend, &add) != 0 || add != 0L)
        return 0;
    lh = o->text[r->offset].lh & HALF_MASK;
    if (((lh >> 9) & 0777UL) != 0260UL ||
        ((lh >> 5) & 017UL) != 017UL ||
        ((lh >> 4) & 1UL) != 0UL || (lh & 017UL) != 0UL ||
        o->text[r->offset].rh != 0UL)
        return 0;
    return 1;
}

/*
 * A missing veneer definition is safe only when every reference to that
 * symbol is an exact direct PUSHJ 17 call which this linker can replace.
 * Any address-taking, indexing, indirection or nonzero addend forces normal
 * symbol resolution and therefore preserves a callable veneer.
 */
static int symbol_fully_daimos_uuo_relaxable(struct linker *l,
                                              const char *name)
{
    int oi;
    int seen;

    if (!l->daimos_uuo_relax || daimos_uuo_veneer(name) == NULL)
        return 0;
    seen = 0;
    for (oi = 0; oi < l->object_count; oi++) {
        struct dobj_object *o;
        unsigned long ri;
        o = &l->objects[oi].obj;
        for (ri = 0UL; ri < o->reloc_count; ri++) {
            struct dobj_reloc *r;
            struct dobj_symbol *u;
            r = &o->relocs[ri];
            if (r->symbol == 0UL || r->symbol > o->symbol_count)
                continue;
            u = &o->symbols[r->symbol - 1UL];
            if (u->kind != DOBJ_SYM_UNDEF || strcmp(u->name, name) != 0)
                continue;
            seen = 1;
            if (!direct_pushj17_reloc(o, r, name))
                return 0;
        }
    }
    return seen;
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
                if (symbol_fully_daimos_uuo_relaxable(l,
                                                       o->symbols[si].name))
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
                find_def(l, o->symbols[si].name) < 0 &&
                !symbol_fully_daimos_uuo_relaxable(l,
                                                    o->symbols[si].name)) {
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

static unsigned long section_offset(const struct link_object *o, int sec)
{
    if (sec == DOBJ_SEC_TEXT) return o->text_base;
    if (sec == DOBJ_SEC_DATA) return o->data_base;
    if (sec == DOBJ_SEC_BSS) return o->bss_base;
    return 0UL;
}

static unsigned long section_base(const struct linker *l,
                                  const struct link_object *o, int sec)
{
    return l->image_base + section_offset(o, sec);
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
    *addr = section_base(l, o, s->sec) + s->value.rh;
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


static void set_mres_reloc(struct dobj_word *map, unsigned long off,
                           unsigned long code)
{
    unsigned long wi;
    unsigned long slot;
    unsigned long shift;

    wi = off / 18UL;
    slot = off % 18UL;
    if (slot < 9UL) {
        shift = 16UL - slot * 2UL;
        map[wi].lh |= (code & 3UL) << shift;
    } else {
        shift = 16UL - (slot - 9UL) * 2UL;
        map[wi].rh |= (code & 3UL) << shift;
    }
}

static int apply_relocs(struct linker *l, struct dobj_word *image,
                        struct dobj_word *relmap, struct dobj_word *mresmap)
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
            if (r->type == DOBJ_RELOC_LOCAL_RH18 ||
                r->type == DOBJ_RELOC_LOCAL_LH18) {
                if (r->target_sec < DOBJ_SEC_TEXT || r->target_sec > DOBJ_SEC_BSS)
                    return -1;
                target = section_base(l, lo, r->target_sec);
                relative = l->image_base == 0UL;
            } else if (r->type == DOBJ_RELOC_SYMBOL_RH18 ||
                       r->type == DOBJ_RELOC_SYMBOL_LH18) {
                int di;
                struct dobj_symbol *u;
                if (r->symbol == 0UL || r->symbol > lo->obj.symbol_count)
                    return -1;
                const struct daimos_uuo_veneer *dv;
                u = &lo->obj.symbols[r->symbol - 1UL];
                dv = l->daimos_uuo_relax ? daimos_uuo_veneer(u->name) : NULL;
                if (dv != NULL && direct_pushj17_reloc(&lo->obj, r, u->name)) {
                    image[loc].lh = ((dv->opcode & 0777UL) << 9) |
                                    (dv->index & 017UL);
                    image[loc].rh = 0UL;
                    continue;
                }
                di = find_def(l, u->name);
                if (di < 0 || symbol_address(l, di, &target, &relative) != 0)
                    return -1;
                if (l->image_base != 0UL && relative)
                    relative = 0;
            } else {
                fprintf(stderr, "dlink: unsupported relocation type\n");
                return -1;
            }
            if ((long)target + add < 0L || (unsigned long)((long)target + add) > HALF_MASK) {
                fprintf(stderr, "dlink: RH18 relocation overflow\n");
                return -1;
            }
            if (r->type == DOBJ_RELOC_LOCAL_LH18 ||
                r->type == DOBJ_RELOC_SYMBOL_LH18) {
                image[loc].lh = (unsigned long)((long)target + add) & HALF_MASK;
                if (relative && mresmap != NULL)
                    set_mres_reloc(mresmap, loc, 2UL);
            } else {
                image[loc].rh = (unsigned long)((long)target + add) & HALF_MASK;
                if (relative) {
                    if (relmap != NULL)
                        set_reloc_bit(relmap, loc);
                    if (mresmap != NULL)
                        set_mres_reloc(mresmap, loc, 1UL);
                }
            }
        }
    }
    return 0;
}

static int write_map(const char *name, struct linker *l)
{
    FILE *f;
    int i;

    f = fopen(name, "w");
    if (f == NULL)
        return -1;
    for (i = 0; i < l->def_count; i++) {
        unsigned long addr;
        int relative;

        if (symbol_address(l, i, &addr, &relative) != 0 ||
            fprintf(f, "%-32s %06lo\n", l->defs[i].name, addr) < 0) {
            fclose(f);
            remove(name);
            return -1;
        }
    }
    if (fclose(f) != 0) {
        remove(name);
        return -1;
    }
    return 0;
}

static int write_dxr(const char *name, struct linker *l, int purity_request)
{
    unsigned long tw, dw, bw, iw, rw;
    struct dobj_word *image;
    struct dobj_word *relmap;
    struct dobj_word h;
    unsigned long pos;
    unsigned long entry;
    unsigned long flags;
    unsigned long text_write_at;
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
    if (apply_relocs(l, image, relmap, NULL) != 0) {
        free(image); free(relmap); return -1;
    }
    flags = 0UL;
    if (definite_text_write(image, tw, &text_write_at)) {
        flags = DXR_F_IMPURE;
        if (purity_request == PURITY_PURE) {
            fprintf(stderr,
                    "dlink: --pure contradicted by definite TEXT write at %06lo\n",
                    text_write_at);
            free(image); free(relmap); return -1;
        }
    } else if (purity_request == PURITY_IMPURE) {
        flags = DXR_F_IMPURE;
    } else if (purity_request == PURITY_PURE) {
        flags = DXR_F_PURE;
    }
    entry = l->image_base;
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
    if (entry < l->image_base || entry - l->image_base >= iw) {
        fprintf(stderr, "dlink: entry point is outside linked image\n");
        free(image); free(relmap); return -1;
    }
    entry -= l->image_base;
    f = fopen(name, "wb");
    if (f == NULL) { free(image); free(relmap); return -1; }
    h = sixbit_word("DXR2  ");
    h.rh = entry;
    if (dobj_write_word(f, h) != 0 ||
        dobj_write_word(f, dobj_word_halves(iw, (bw & DXR_BSS_MASK) | flags)) != 0 ||
        dobj_write_word(f, dobj_word_halves(tw, sixbit_word("TX2   ").lh)) != 0) goto bad;
    for (pos = 0UL; pos < iw; pos++) if (dobj_write_word(f, image[pos]) != 0) goto bad;
    for (pos = 0UL; pos < rw; pos++) if (dobj_write_word(f, relmap[pos]) != 0) goto bad;
    if (fclose(f) != 0) { remove(name); free(image); free(relmap); return -1; }
    free(image); free(relmap); return 0;
bad:
    fclose(f); remove(name); free(image); free(relmap); return -1;
}

static int write_mres_object(const char *name, const char *package_symbol,
                             const char **exports, int export_count,
                             struct linker *l)
{
    unsigned long tw, dw, bw, iw, rw, ew, pw;
    struct dobj_word *image;
    struct dobj_word *mresmap;
    struct dobj_object out;
    unsigned long i;
    FILE *f;
    int rc;

    if (l->image_base != 0UL) {
        fprintf(stderr, "dlink: MRES output requires link base 0\n");
        return -1;
    }
    if (package_symbol == NULL || package_symbol[0] == '\0' ||
        strlen(package_symbol) > DOBJ_NAME_MAX) {
        fprintf(stderr, "dlink: invalid MRES package symbol\n");
        return -1;
    }
    if (layout(l, &tw, &dw, &bw) != 0)
        return -1;
    iw = tw + dw;
    rw = (iw + 17UL) / 18UL;
    ew = ((unsigned long)export_count + 1UL) / 2UL;
    pw = 3UL + ew + iw + rw;
    image = (struct dobj_word *)calloc((size_t)iw, sizeof(*image));
    mresmap = (struct dobj_word *)calloc((size_t)rw, sizeof(*mresmap));
    if ((iw != 0UL && image == NULL) || (rw != 0UL && mresmap == NULL)) {
        free(image);
        free(mresmap);
        return -1;
    }
    for (i = 0UL; i < (unsigned long)l->object_count; i++) {
        unsigned long j;
        struct link_object *lo = &l->objects[i];
        for (j = 0UL; j < lo->obj.text_words; j++)
            image[lo->text_base + j] = lo->obj.text[j];
        for (j = 0UL; j < lo->obj.data_words; j++)
            image[lo->data_base + j] = lo->obj.data[j];
    }
    if (apply_relocs(l, image, NULL, mresmap) != 0) {
        free(image);
        free(mresmap);
        return -1;
    }
    memset(&out, 0, sizeof(out));
    out.data_words = pw;
    out.data = (struct dobj_word *)calloc((size_t)pw, sizeof(*out.data));
    out.symbol_count = 1UL;
    out.symbols = (struct dobj_symbol *)calloc(1U, sizeof(*out.symbols));
    if (out.data == NULL || out.symbols == NULL) {
        free(image);
        free(mresmap);
        dobj_free(&out);
        return -1;
    }
    out.data[0] = sixbit_word("MRES1 ");
    out.data[1] = dobj_word_halves(iw, bw);
    out.data[2] = dobj_word_halves(rw, (unsigned long)export_count);
    for (i = 0UL; i < (unsigned long)export_count; i++) {
        int di;
        int relative;
        unsigned long addr;
        unsigned long wi;
        di = find_def(l, exports[i]);
        if (di < 0 || symbol_address(l, di, &addr, &relative) != 0 ||
            !relative || addr >= iw + bw) {
            fprintf(stderr, "dlink: invalid MRES export: %s\n", exports[i]);
            free(image);
            free(mresmap);
            dobj_free(&out);
            return -1;
        }
        wi = 3UL + i / 2UL;
        if ((i & 1UL) == 0UL)
            out.data[wi].lh = addr;
        else
            out.data[wi].rh = addr;
    }
    for (i = 0UL; i < iw; i++)
        out.data[3UL + ew + i] = image[i];
    for (i = 0UL; i < rw; i++)
        out.data[3UL + ew + iw + i] = mresmap[i];
    strcpy(out.symbols[0].name, package_symbol);
    out.symbols[0].kind = DOBJ_SYM_DEF;
    out.symbols[0].sec = DOBJ_SEC_DATA;
    out.symbols[0].value = dobj_word_halves(0UL, 0UL);
    f = fopen(name, "wb");
    if (f == NULL) {
        perror(name);
        free(image);
        free(mresmap);
        dobj_free(&out);
        return -1;
    }
    rc = dobj_write(f, &out);
    if (fclose(f) != 0)
        rc = -1;
    if (rc != 0)
        remove(name);
    free(image);
    free(mresmap);
    dobj_free(&out);
    return rc;
}

static int add_abs_map(struct linker *l, const char *name)
{
    FILE *f;
    struct dobj_object obj;
    char sym[DOBJ_NAME_MAX + 1];
    unsigned long value;
    unsigned long cap;

    memset(&obj, 0, sizeof(obj));
    cap = 0UL;
    f = fopen(name, "r");
    if (f == NULL) {
        perror(name);
        return -1;
    }
    while (fscanf(f, "%63s %lo", sym, &value) == 2) {
        struct dobj_symbol *nv;
        if (value > HALF_MASK) {
            fprintf(stderr, "dlink: absolute map symbol out of range: %s\n", sym);
            fclose(f);
            dobj_free(&obj);
            return -1;
        }
        if (obj.symbol_count == cap) {
            unsigned long ncap = cap == 0UL ? 32UL : cap * 2UL;
            nv = (struct dobj_symbol *)realloc(obj.symbols,
                    (size_t)ncap * sizeof(*nv));
            if (nv == NULL) {
                fclose(f);
                dobj_free(&obj);
                return -1;
            }
            obj.symbols = nv;
            cap = ncap;
        }
        memset(&obj.symbols[obj.symbol_count], 0,
               sizeof(obj.symbols[obj.symbol_count]));
        strcpy(obj.symbols[obj.symbol_count].name, sym);
        obj.symbols[obj.symbol_count].kind = DOBJ_SYM_DEF;
        obj.symbols[obj.symbol_count].sec = DOBJ_SEC_ABS;
        obj.symbols[obj.symbol_count].value.rh = value;
        obj.symbol_count++;
    }
    if (ferror(f)) {
        fclose(f);
        dobj_free(&obj);
        return -1;
    }
    fclose(f);
    if (add_object(l, &obj) != 0) {
        dobj_free(&obj);
        return -1;
    }
    return 0;
}

static void cleanup(struct linker *l)
{
    int i;
    for (i = 0; i < l->object_count; i++)
        dobj_free(&l->objects[i].obj);
    for (i = 0; i < l->archive_count; i++) {
        dobj_archive_close(&l->archives[i].ar);
        fclose(l->archives[i].file);
        free(l->archives[i].loaded_offsets);
    }
    free(l->objects);
    free(l->archives);
    free(l->defs);
}

static void usage(void)
{
    fprintf(stderr, "usage: dlink -o out.dxr [--pure|--impure] [--daimos-uuo-relax] [-M out.map] [-b octal] [-A map] [-R out.dobj -N symbol [-X export] ...] input.dobj|library.darc ...\n");
}

int main(int argc, char **argv)
{
    struct linker l;
    const char *out;
    const char *map;
    const char *abs_maps[32];
    const char *mres_out;
    const char *mres_symbol;
    const char *mres_exports[32];
    int abs_map_count;
    int mres_export_count;
    int i;
    int first;
    int rc;
    int purity_request;

    memset(&l, 0, sizeof(l));
    out = NULL;
    map = NULL;
    abs_map_count = 0;
    mres_out = NULL;
    mres_symbol = NULL;
    mres_export_count = 0;
    l.image_base = 0UL;
    l.daimos_uuo_relax = 0;
    purity_request = PURITY_UNKNOWN;
    first = 1;
    while (first < argc && argv[first][0] == '-') {
        if (strcmp(argv[first], "-o") == 0 && first + 1 < argc) {
            out = argv[first + 1];
            first += 2;
        } else if (strcmp(argv[first], "--pure") == 0) {
            if (purity_request == PURITY_IMPURE) { usage(); return 1; }
            purity_request = PURITY_PURE;
            first++;
        } else if (strcmp(argv[first], "--impure") == 0) {
            if (purity_request == PURITY_PURE) { usage(); return 1; }
            purity_request = PURITY_IMPURE;
            first++;
        } else if (strcmp(argv[first], "--daimos-uuo-relax") == 0) {
            l.daimos_uuo_relax = 1;
            first++;
        } else if (strcmp(argv[first], "-M") == 0 && first + 1 < argc) {
            map = argv[first + 1];
            first += 2;
        } else if (strcmp(argv[first], "-b") == 0 && first + 1 < argc) {
            char *endp;
            unsigned long v = strtoul(argv[first + 1], &endp, 8);
            if (*endp != '\0' || v > HALF_MASK) { usage(); return 1; }
            l.image_base = v;
            first += 2;
        } else if (strcmp(argv[first], "-A") == 0 && first + 1 < argc) {
            if (abs_map_count >= 32) { usage(); return 1; }
            abs_maps[abs_map_count++] = argv[first + 1];
            first += 2;
        } else if (strcmp(argv[first], "-R") == 0 && first + 1 < argc) {
            mres_out = argv[first + 1];
            first += 2;
        } else if (strcmp(argv[first], "-N") == 0 && first + 1 < argc) {
            mres_symbol = argv[first + 1];
            first += 2;
        } else if (strcmp(argv[first], "-X") == 0 && first + 1 < argc) {
            if (mres_export_count >= 32) { usage(); return 1; }
            mres_exports[mres_export_count++] = argv[first + 1];
            first += 2;
        } else {
            usage();
            return 1;
        }
    }
    if (out == NULL || first >= argc ||
        ((mres_out == NULL) != (mres_symbol == NULL))) { usage(); return 1; }
    rc = 1;
    for (i = 0; i < abs_map_count; i++)
        if (add_abs_map(&l, abs_maps[i]) != 0) goto done;
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
            if (l.archive_count == l.archive_cap) {
                void *p;
                p = grow_array(l.archives, &l.archive_cap, sizeof(l.archives[0]));
                if (p == NULL) { fclose(f); goto done; }
                l.archives = (struct link_archive *)p;
            }
            a = &l.archives[l.archive_count];
            memset(a, 0, sizeof(*a));
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
    if (write_dxr(out, &l, purity_request) != 0) {
        fprintf(stderr, "dlink: link failed\n");
        goto done;
    }
    if (map != NULL && write_map(map, &l) != 0) {
        fprintf(stderr, "dlink: cannot write map\n");
        remove(out);
        goto done;
    }
    if (mres_out != NULL &&
        write_mres_object(mres_out, mres_symbol, mres_exports,
                          mres_export_count, &l) != 0) {
        fprintf(stderr, "dlink: cannot write MRES package object\n");
        remove(out);
        if (map != NULL) remove(map);
        goto done;
    }
    rc = 0;
done:
    cleanup(&l);
    return rc;
}
