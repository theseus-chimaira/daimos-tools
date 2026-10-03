#include "dobj_native.h"

#define NL_MAX_OBJECTS       48U
#define NL_MAX_ARCHIVES       8U
#define NL_MAX_DEFS         384U
#define NL_DEF_NAME_CHARS   6144U
#define NL_MAX_LOCAL_SYMS     96U
#define NL_LOCAL_NAME_CHARS 2048U
#define NL_MAX_IMAGE_WORDS  040000UL
#define NL_MAX_RELMAP_WORDS   512U

#define NL_HALF_MASK        0777777UL
#define NL_DXR_BSS_MASK     0077777UL
#define NL_DXR_F_PURE       0200000UL
#define NL_DXR_F_RT_REQUIRED 0400000UL

struct nl_object {
        kword_t path[U_PATH_WORDS];
        struct nd_obj obj;
        kword_t text_base;
        kword_t data_base;
        kword_t bss_base;
};

struct nl_archive {
        kword_t path[U_PATH_WORDS];
};

struct nl_def {
        unsigned int name_off;
        unsigned int object;
        unsigned int sec;
        kword_t value;
};

struct nl_lsym {
        unsigned int name_off;
        unsigned int kind;
        unsigned int sec;
        kword_t value;
};

struct nl_veneer {
        const char *name;
        unsigned int opcode;
        unsigned int index;
};

static struct nl_object nl_objects[NL_MAX_OBJECTS];
static struct nl_archive nl_archives[NL_MAX_ARCHIVES];
static struct nl_def nl_defs[NL_MAX_DEFS];
static char nl_def_names[NL_DEF_NAME_CHARS];
static struct nl_lsym nl_local[NL_MAX_LOCAL_SYMS];
static char nl_local_names[NL_LOCAL_NAME_CHARS];
static kword_t nl_relmap[NL_MAX_RELMAP_WORDS];
static unsigned int nl_object_count;
static unsigned int nl_archive_count;
static unsigned int nl_def_count;
static unsigned int nl_def_name_used;
static unsigned int nl_local_count;
static unsigned int nl_local_name_used;
static kword_t nl_image_base;
static kword_t nl_text_words;
static kword_t nl_data_words;
static kword_t nl_bss_words;
static int nl_relax;

static const struct nl_veneer nl_veneers[] = {
        { "dsys_exit",        0040U, 1U },
        { "dsys_open",        0041U, 1U },
        { "dsys_close",       0042U, 1U },
        { "dsys_chdir",       0045U, 1U },
        { "dsys_getcwd",      0046U, 1U },
        { "dsys_stat",        0047U, 1U },
        { "dsys_dirread",     0050U, 1U },
        { "dsys_unlink",      0052U, 1U },
        { "dsys_rename",      0053U, 1U },
        { "dsys_truncate",    0054U, 1U },
        { "dsys_read_words",  0055U, 1U },
        { "dsys_write_words", 0056U, 1U },
        { "dsys_procinfo",    0057U, 1U },
        { "dsys_meminfo",     0060U, 1U },
        { "dsys_readchar",    0061U, 1U },
        { "dsys_writechar",   0062U, 1U },
        { "dsys_halt",        0063U, 0U },
        { "dsys_chmod",       0064U, 1U },
        { "dsys_dtfs_format", 0065U, 1U },
        { "dsys_dtfs_mount",  0066U, 1U },
        { "dsys_unmount",     0067U, 1U },
        { "dsys_flock",       0070U, 1U },
        { "dsys_dup",         0071U, 1U },
        { "dsys_symlink",     0072U, 1U },
        { "dsys_nice",        0073U, 1U },
        { "dsys_run",         0074U, 1U },
        { "dsys_wait",        0075U, 1U },
        { "dsys_getpid",      0076U, 0U },
        { "dsys_procctl",     0077U, 1U }
};

static int
s6_copy(kword_t *dst, const kword_t *src)
{
        unsigned int chars;
        unsigned int words;
        unsigned int i;
        chars = (unsigned int)(src[0] & 0777777UL);
        words = 1U + (chars + 5U) / 6U;
        if (words > U_PATH_WORDS)
                return -1;
        for (i = 0U; i < U_PATH_WORDS; ++i)
                dst[i] = i < words ? src[i] : 0UL;
        return 0;
}

static int
s6_same(const kword_t *a, const kword_t *b)
{
        unsigned int chars;
        unsigned int words;
        unsigned int i;
        if (a[0] != b[0])
                return 0;
        chars = (unsigned int)(a[0] & 0777777UL);
        words = 1U + (chars + 5U) / 6U;
        for (i = 0U; i < words; ++i)
                if (a[i] != b[i])
                        return 0;
        return 1;
}

static int
parse_octal(const kword_t *arg, kword_t *vp)
{
        unsigned int n;
        unsigned int i;
        unsigned int wi;
        unsigned int sh;
        unsigned int ch;
        kword_t v;
        n = (unsigned int)(arg[0] & 0777777UL);
        if (n == 0U || n > 6U)
                return -1;
        v = 0UL;
        for (i = 0U; i < n; ++i) {
                wi = 1U + i / 6U;
                sh = 30U - (i % 6U) * 6U;
                ch = (unsigned int)(((arg[wi] >> sh) & 077UL) + 040U);
                if (ch < '0' || ch > '7')
                        return -1;
                v = (v << 3) | (kword_t)(ch - '0');
        }
        if (v > NL_HALF_MASK)
                return -1;
        *vp = v;
        return 0;
}

static int
diag_path(const char *msg, const kword_t *path)
{
        return u_puts(2, "DLINK: ") != 0 || u_puts(2, msg) != 0 ||
            (path != 0 && (u_puts(2, ": ") != 0 || u_put_s6(2, path) != 0)) ||
            u_crlf(2) != 0 ? -1 : 0;
}

static const char *
def_name(unsigned int i)
{
        return &nl_def_names[nl_defs[i].name_off];
}

static const char *
local_name(unsigned int i)
{
        return &nl_local_names[nl_local[i].name_off];
}

static int
find_def(const char *name)
{
        unsigned int i;
        for (i = 0U; i < nl_def_count; ++i)
                if (nd_streq(def_name(i), name))
                        return (int)i;
        return -1;
}

static const struct nl_veneer *
find_veneer(const char *name)
{
        unsigned int i;
        for (i = 0U; i < sizeof(nl_veneers) / sizeof(nl_veneers[0]); ++i)
                if (nd_streq(nl_veneers[i].name, name))
                        return &nl_veneers[i];
        return 0;
}

static int
store_name(char *arena, unsigned int cap, unsigned int *used,
    const char *name, unsigned int *offp)
{
        unsigned int len;
        unsigned int i;
        len = nd_strlen(name) + 1U;
        if (*used + len > cap)
                return -1;
        *offp = *used;
        for (i = 0U; i < len; ++i)
                arena[*used + i] = name[i];
        *used += len;
        return 0;
}

struct add_def_ctx { unsigned int object; };

static int
add_def_symbol(unsigned int index, const struct nd_symbol *sym, void *arg)
{
        struct add_def_ctx *ctx;
        struct nl_def *d;
        (void)index;
        if (sym->kind != ND_SYM_DEF)
                return 0;
        if (find_def(sym->name) >= 0 || nl_def_count >= NL_MAX_DEFS)
                return -1;
        ctx = (struct add_def_ctx *)arg;
        d = &nl_defs[nl_def_count];
        if (store_name(nl_def_names, NL_DEF_NAME_CHARS, &nl_def_name_used,
            sym->name, &d->name_off) != 0)
                return -1;
        d->object = ctx->object;
        d->sec = sym->sec;
        d->value = sym->value;
        nl_def_count++;
        return 0;
}

static int
add_defs(unsigned int oi)
{
        struct add_def_ctx ctx;
        int fd;
        int rc;
        fd = dsys_open(nl_objects[oi].path, SYS_O_RDONLY);
        if (fd < 0)
                return -1;
        ctx.object = oi;
        rc = nd_obj_each_symbol(fd, &nl_objects[oi].obj, add_def_symbol, &ctx);
        if (dsys_close(fd) != 0)
                rc = -1;
        return rc;
}

static int
object_loaded(const kword_t *path, kword_t base)
{
        unsigned int i;
        for (i = 0U; i < nl_object_count; ++i)
                if (nl_objects[i].obj.base == base &&
                    s6_same(nl_objects[i].path, path))
                        return 1;
        return 0;
}

static int
add_object(const kword_t *path, kword_t base)
{
        int fd;
        unsigned int oi;
        if (nl_object_count >= NL_MAX_OBJECTS || object_loaded(path, base))
                return object_loaded(path, base) ? 0 : -1;
        oi = nl_object_count;
        if (s6_copy(nl_objects[oi].path, path) != 0)
                return -1;
        fd = dsys_open(nl_objects[oi].path, SYS_O_RDONLY);
        if (fd < 0 || nd_obj_parse(fd, base, &nl_objects[oi].obj) != 0) {
                if (fd >= 0) (void)dsys_close(fd);
                return -1;
        }
        if (dsys_close(fd) != 0)
                return -1;
        nl_object_count++;
        if (add_defs(oi) != 0)
                return -1;
        return 1;
}

static int
add_archive(const kword_t *path)
{
        int fd;
        kword_t members;
        kword_t index_count;
        if (nl_archive_count >= NL_MAX_ARCHIVES ||
            s6_copy(nl_archives[nl_archive_count].path, path) != 0)
                return -1;
        fd = dsys_open(nl_archives[nl_archive_count].path, SYS_O_RDONLY);
        if (fd < 0 || nd_arc_header(fd, &members, &index_count) != 0) {
                if (fd >= 0) (void)dsys_close(fd);
                return -1;
        }
        (void)members;
        (void)index_count;
        if (dsys_close(fd) != 0)
                return -1;
        nl_archive_count++;
        return 0;
}

static int
load_local(unsigned int oi)
{
        struct nd_symbol sym;
        unsigned int i;
        unsigned int off;
        int fd;
        nl_local_count = 0U;
        nl_local_name_used = 0U;
        if (nl_objects[oi].obj.symbol_count > NL_MAX_LOCAL_SYMS)
                return -1;
        fd = dsys_open(nl_objects[oi].path, SYS_O_RDONLY);
        if (fd < 0)
                return -1;
        for (i = 0U; i < (unsigned int)nl_objects[oi].obj.symbol_count; ++i) {
                if (nd_obj_symbol(fd, &nl_objects[oi].obj, i, &sym) != 0 ||
                    store_name(nl_local_names, NL_LOCAL_NAME_CHARS,
                    &nl_local_name_used, sym.name, &off) != 0) {
                        (void)dsys_close(fd);
                        return -1;
                }
                nl_local[i].name_off = off;
                nl_local[i].kind = sym.kind;
                nl_local[i].sec = sym.sec;
                nl_local[i].value = sym.value;
                nl_local_count++;
        }
        return dsys_close(fd) == 0 ? 0 : -1;
}

static int
try_extract(const char *name)
{
        unsigned int ai;
        int fd;
        int found;
        kword_t off;
        for (ai = 0U; ai < nl_archive_count; ++ai) {
                fd = dsys_open(nl_archives[ai].path, SYS_O_RDONLY);
                if (fd < 0)
                        return -1;
                found = nd_arc_find(fd, name, &off);
                if (dsys_close(fd) != 0)
                        return -1;
                if (found < 0)
                        return -1;
                if (found > 0) {
                        if (object_loaded(nl_archives[ai].path, off + 1UL))
                                return 0;
                        return add_object(nl_archives[ai].path, off + 1UL);
                }
        }
        return 0;
}

static int
resolve_archives(void)
{
        int changed;
        unsigned int oi;
        unsigned int si;
        int rc;
        do {
                changed = 0;
                for (oi = 0U; oi < nl_object_count && !changed; ++oi) {
                        if (load_local(oi) != 0)
                                return -1;
                        for (si = 0U; si < nl_local_count; ++si) {
                                const char *name;
                                if (nl_local[si].kind != ND_SYM_UNDEF)
                                        continue;
                                name = local_name(si);
                                if (find_def(name) >= 0 ||
                                    (nl_relax && find_veneer(name) != 0))
                                        continue;
                                rc = try_extract(name);
                                if (rc < 0)
                                        return -1;
                                if (rc > 0) {
                                        changed = 1;
                                        break;
                                }
                        }
                }
        } while (changed);
        for (oi = 0U; oi < nl_object_count; ++oi) {
                if (load_local(oi) != 0)
                        return -1;
                for (si = 0U; si < nl_local_count; ++si)
                        if (nl_local[si].kind == ND_SYM_UNDEF &&
                            find_def(local_name(si)) < 0 &&
                            !(nl_relax && find_veneer(local_name(si)) != 0)) {
                                (void)u_puts(2, "DLINK: UNDEFINED SYMBOL: ");
                                (void)nd_put_name(2, local_name(si));
                                (void)u_crlf(2);
                                return -1;
                        }
        }
        return 0;
}

static int
layout(void)
{
        unsigned int i;
        kword_t t;
        kword_t d;
        kword_t b;
        t = d = b = 0UL;
        for (i = 0U; i < nl_object_count; ++i) {
                nl_objects[i].text_base = t;
                t += nl_objects[i].obj.text_words;
        }
        for (i = 0U; i < nl_object_count; ++i) {
                nl_objects[i].data_base = t + d;
                d += nl_objects[i].obj.data_words;
        }
        for (i = 0U; i < nl_object_count; ++i) {
                nl_objects[i].bss_base = t + d + b;
                b += nl_objects[i].obj.bss_words;
        }
        if (t + d > NL_MAX_IMAGE_WORDS || t + d + b > 040000UL)
                return -1;
        nl_text_words = t;
        nl_data_words = d;
        nl_bss_words = b;
        return 0;
}

static kword_t
section_base(unsigned int oi, unsigned int sec)
{
        kword_t off;
        if (sec == ND_SEC_TEXT) off = nl_objects[oi].text_base;
        else if (sec == ND_SEC_DATA) off = nl_objects[oi].data_base;
        else if (sec == ND_SEC_BSS) off = nl_objects[oi].bss_base;
        else off = 0UL;
        return nl_image_base + off;
}

static int
def_address(unsigned int di, kword_t *addr, int *relative)
{
        struct nl_def *d;
        kword_t off;
        d = &nl_defs[di];
        if (nd_lh(d->value) != 0UL)
                return -1;
        off = nd_rh(d->value);
        if (d->sec == ND_SEC_ABS) {
                *addr = off;
                *relative = 0;
                return 0;
        }
        if (d->sec < ND_SEC_TEXT || d->sec > ND_SEC_BSS)
                return -1;
        *addr = section_base(d->object, d->sec) + off;
        *relative = 1;
        return *addr <= NL_HALF_MASK ? 0 : -1;
}

static void
set_relbit(kword_t off)
{
        unsigned int wi;
        unsigned int bit;
        wi = (unsigned int)(off / 36UL);
        bit = (unsigned int)(off % 36UL);
        nl_relmap[wi] |= 1UL << (35U - bit);
}

static int
opcode_writes_ea(unsigned int op)
{
        if (op == 0136U || op == 0202U || op == 0203U ||
            op == 0206U || op == 0207U || op == 0212U ||
            op == 0213U || op == 0216U || op == 0217U ||
            op == 0222U || op == 0223U || op == 0226U ||
            op == 0227U || op == 0232U || op == 0233U ||
            op == 0236U || op == 0237U || op == 0250U ||
            op == 0262U || op == 0264U || op == 0266U ||
            op == 0272U || op == 0273U || op == 0276U || op == 0277U)
                return 1;
        if (op >= 0350U && op <= 0357U) return 1;
        if (op >= 0370U && op <= 0377U) return 1;
        if (op >= 0400U && op <= 0477U && (op & 3U) >= 2U) return 1;
        if (op >= 0500U && op <= 0577U && (op & 3U) >= 2U) return 1;
        return 0;
}

static int
definite_text_write(kword_t w)
{
        kword_t lh;
        kword_t ea;
        unsigned int op;
        lh = nd_lh(w);
        op = (unsigned int)((lh >> 9) & 0777UL);
        if (((lh >> 4) & 1UL) != 0UL || (lh & 017UL) != 0UL ||
            !opcode_writes_ea(op))
                return 0;
        ea = nd_rh(w);
        return ea >= nl_image_base && ea < nl_image_base + nl_text_words;
}

static int
apply_reloc(unsigned int oi, const struct nd_reloc *r, kword_t global_off,
    kword_t *word)
{
        int add;
        kword_t target;
        int relative;
        const char *name;
        const struct nl_veneer *v;
        int di;
        kword_t lh;

        if (nd_addend18(r->addend, &add) != 0)
                return -1;
        if (r->type == ND_RELOC_LOCAL_RH18 ||
            r->type == ND_RELOC_LOCAL_LH18) {
                if (r->target_sec < ND_SEC_TEXT || r->target_sec > ND_SEC_BSS)
                        return -1;
                target = section_base(oi, r->target_sec);
                relative = nl_image_base == 0UL;
        } else if (r->type == ND_RELOC_SYMBOL_RH18 ||
            r->type == ND_RELOC_SYMBOL_LH18) {
                if (r->symbol == 0UL || r->symbol > nl_local_count)
                        return -1;
                name = local_name((unsigned int)r->symbol - 1U);
                v = nl_relax ? find_veneer(name) : 0;
                if (v != 0 && r->type == ND_RELOC_SYMBOL_RH18 && add == 0) {
                        lh = nd_lh(*word);
                        if (((lh >> 9) & 0777UL) == 0260UL &&
                            ((lh >> 5) & 017UL) == 017UL &&
                            ((lh >> 4) & 1UL) == 0UL && (lh & 017UL) == 0UL &&
                            nd_rh(*word) == 0UL) {
                                *word = nd_halves(((kword_t)v->opcode << 9) |
                                    (kword_t)v->index, 0UL);
                                return 0;
                        }
                }
                di = find_def(name);
                if (di < 0 || def_address((unsigned int)di, &target,
                    &relative) != 0)
                        return -1;
                if (nl_image_base != 0UL && relative)
                        relative = 0;
        } else {
                return -1;
        }
        if ((int)target + add < 0 || (kword_t)((int)target + add) > NL_HALF_MASK)
                return -1;
        if (r->type == ND_RELOC_LOCAL_LH18 ||
            r->type == ND_RELOC_SYMBOL_LH18) {
                *word = nd_halves((kword_t)((int)target + add), nd_rh(*word));
        } else {
                *word = nd_halves(nd_lh(*word), (kword_t)((int)target + add));
                if (relative)
                        set_relbit(global_off);
        }
        return 0;
}

static int
stream_section(unsigned int oi, unsigned int sec, int outfd,
    int *text_write)
{
        struct nd_obj *o;
        struct nd_reloc rel;
        kword_t buf[ND_IO_WORDS];
        kword_t words;
        kword_t section_off;
        kword_t global_base;
        kword_t pos;
        unsigned int ri;
        unsigned int n;
        unsigned int j;
        int have;
        int fd;
        int rfd;

        o = &nl_objects[oi].obj;
        words = sec == ND_SEC_TEXT ? o->text_words : o->data_words;
        section_off = sec == ND_SEC_TEXT ? o->text_off : o->data_off;
        global_base = sec == ND_SEC_TEXT ? nl_objects[oi].text_base :
            nl_objects[oi].data_base;
        if (load_local(oi) != 0)
                return -1;
        fd = dsys_open(nl_objects[oi].path, SYS_O_RDONLY);
        rfd = dsys_open(nl_objects[oi].path, SYS_O_RDONLY);
        if (fd < 0 || rfd < 0) {
                if (fd >= 0) (void)dsys_close(fd);
                if (rfd >= 0) (void)dsys_close(rfd);
                return -1;
        }
        if (nd_seek(fd, section_off) != 0)
                goto bad;
        ri = 0U;
        have = 0;
        while (ri < (unsigned int)o->reloc_count) {
                if (nd_obj_reloc(rfd, o, ri, &rel) != 0)
                        goto bad;
                if (rel.loc_sec >= sec) { have = 1; break; }
                ri++;
        }
        pos = 0UL;
        while (pos < words) {
                n = words - pos > ND_IO_WORDS ? ND_IO_WORDS :
                    (unsigned int)(words - pos);
                if (nd_read_exact(fd, buf, n) != 0)
                        goto bad;
                for (j = 0U; j < n; ++j) {
                        kword_t off = pos + (kword_t)j;
                        while (have && rel.loc_sec == sec && rel.offset == off) {
                                if (apply_reloc(oi, &rel, global_base + off,
                                    &buf[j]) != 0)
                                        goto bad;
                                ri++;
                                if (ri >= (unsigned int)o->reloc_count) {
                                        have = 0;
                                        break;
                                }
                                if (nd_obj_reloc(rfd, o, ri, &rel) != 0)
                                        goto bad;
                        }
                        if (have && rel.loc_sec == sec && rel.offset < off)
                                goto bad;
                        if (sec == ND_SEC_TEXT && definite_text_write(buf[j]))
                                *text_write = 1;
                }
                if (nd_write_exact(outfd, buf, n) != 0)
                        goto bad;
                pos += (kword_t)n;
        }
        if (have && rel.loc_sec == sec)
                goto bad;
        if (dsys_close(fd) != 0 || dsys_close(rfd) != 0)
                return -1;
        return 0;
bad:
        (void)dsys_close(fd);
        (void)dsys_close(rfd);
        return -1;
}

static int
entry_offset(kword_t *entryp)
{
        unsigned int oi;
        struct nd_symbol sym;
        int fd;
        int di;
        int relative;
        kword_t addr;
        for (oi = 0U; oi < nl_object_count; ++oi) {
                if (nl_objects[oi].obj.entry_symbol == 0UL)
                        continue;
                fd = dsys_open(nl_objects[oi].path, SYS_O_RDONLY);
                if (fd < 0)
                        return -1;
                if (nd_obj_symbol(fd, &nl_objects[oi].obj,
                    (unsigned int)nl_objects[oi].obj.entry_symbol - 1U,
                    &sym) != 0 || dsys_close(fd) != 0)
                        return -1;
                di = find_def(sym.name);
                if (di < 0 || def_address((unsigned int)di, &addr,
                    &relative) != 0)
                        return -1;
                if (addr < nl_image_base ||
                    addr - nl_image_base >= nl_text_words + nl_data_words)
                        return -1;
                *entryp = addr - nl_image_base;
                return 0;
        }
        *entryp = 0UL;
        return 0;
}

static int
write_dxr(const kword_t *path, int pure_request, int rt_required)
{
        kword_t h[3];
        kword_t entry;
        kword_t iw;
        kword_t rw;
        kword_t flags;
        unsigned int i;
        int fd;
        int text_write;

        iw = nl_text_words + nl_data_words;
        rw = (iw + 35UL) / 36UL;
        if (rw > NL_MAX_RELMAP_WORDS || entry_offset(&entry) != 0)
                return -1;
        for (i = 0U; i < NL_MAX_RELMAP_WORDS; ++i)
                nl_relmap[i] = 0UL;
        fd = dsys_open((kword_t *)path,
            SYS_O_WRONLY | SYS_O_CREAT | SYS_O_TRUNC);
        if (fd < 0)
                return -1;
        flags = rt_required ? NL_DXR_F_RT_REQUIRED : 0UL;
        h[0] = nd_magic("DXR2  ");
        h[0] = nd_halves(nd_lh(h[0]), entry);
        h[1] = nd_halves(iw, (nl_bss_words & NL_DXR_BSS_MASK) | flags);
        h[2] = nd_halves(nl_text_words, nd_lh(nd_magic("TX2   ")));
        if (nd_write_exact(fd, h, 3U) != 0)
                goto bad;
        text_write = 0;
        for (i = 0U; i < nl_object_count; ++i)
                if (stream_section(i, ND_SEC_TEXT, fd, &text_write) != 0)
                        goto bad;
        for (i = 0U; i < nl_object_count; ++i)
                if (stream_section(i, ND_SEC_DATA, fd, &text_write) != 0)
                        goto bad;
        if (nd_write_exact(fd, nl_relmap, (unsigned int)rw) != 0)
                goto bad;
        if (pure_request) {
                if (text_write) {
                        (void)diag_path("--PURE CONTRADICTED BY TEXT WRITE", 0);
                        goto bad;
                }
                flags |= NL_DXR_F_PURE;
                h[1] = nd_halves(iw,
                    (nl_bss_words & NL_DXR_BSS_MASK) | flags);
                if (nd_write_at(fd, 1UL, &h[1], 1U) != 0)
                        goto bad;
        }
        if (dsys_close(fd) != 0)
                return -1;
        return 0;
bad:
        (void)dsys_close(fd);
        (void)dsys_unlink((kword_t *)path);
        return -1;
}

static int
write_map(const kword_t *path)
{
        unsigned int i;
        kword_t addr;
        int relative;
        int fd;
        int rc;
        fd = dsys_open((kword_t *)path,
            SYS_O_WRONLY | SYS_O_CREAT | SYS_O_TRUNC);
        if (fd < 0 || u_text_sink_attach(fd) != 0) {
                if (fd >= 0) (void)dsys_close(fd);
                return -1;
        }
        rc = 0;
        for (i = 0U; i < nl_def_count; ++i) {
                if (def_address(i, &addr, &relative) != 0 ||
                    nd_put_name(fd, def_name(i)) != 0 || u_putc(fd, ' ') != 0 ||
                    nd_put_octal(fd, addr, 6U) != 0 || u_crlf(fd) != 0) {
                        rc = -1;
                        break;
                }
        }
        if (u_text_sink_detach() != 0 || dsys_close(fd) != 0)
                rc = -1;
        if (rc != 0)
                (void)dsys_unlink((kword_t *)path);
        return rc;
}

static int
usage(void)
{
        (void)u_puts(2, "USAGE: DLINK -O OUT.DXR [-M MAP] [-B OCTAL] [--PURE] [--RT-REQUIRED] [--DAIMOS-UUO-RELAX] INPUT.DOBJ|LIBRARY.DARC ...");
        (void)u_crlf(2);
        return 1;
}

int
main(int argc, kword_t **argv)
{
        kword_t *out;
        kword_t *map;
        kword_t magic;
        kword_t h;
        unsigned int first;
        unsigned int i;
        int pure_request;
        int rt_required;
        int fd;
        int rc;

        out = 0;
        map = 0;
        nl_image_base = 0UL;
        nl_relax = 0;
        pure_request = 0;
        rt_required = 0;
        first = 1U;
        while (first < (unsigned int)argc) {
                if (u_s6_eq(argv[first], "-O") && first + 1U < (unsigned int)argc) {
                        out = argv[first + 1U]; first += 2U;
                } else if (u_s6_eq(argv[first], "-M") && first + 1U < (unsigned int)argc) {
                        map = argv[first + 1U]; first += 2U;
                } else if (u_s6_eq(argv[first], "-B") && first + 1U < (unsigned int)argc) {
                        if (parse_octal(argv[first + 1U], &nl_image_base) != 0)
                                return usage();
                        first += 2U;
                } else if (u_s6_eq(argv[first], "--PURE")) {
                        pure_request = 1; first++;
                } else if (u_s6_eq(argv[first], "--RT-REQUIRED")) {
                        rt_required = 1; first++;
                } else if (u_s6_eq(argv[first], "--DAIMOS-UUO-RELAX")) {
                        nl_relax = 1; first++;
                } else {
                        break;
                }
        }
        if (out == 0 || first >= (unsigned int)argc)
                return usage();
        for (i = first; i < (unsigned int)argc; ++i) {
                fd = dsys_open(argv[i], SYS_O_RDONLY);
                if (fd < 0 || nd_read_at(fd, 0UL, &magic, 1U) != 0) {
                        if (fd >= 0) (void)dsys_close(fd);
                        (void)diag_path("CANNOT READ INPUT", argv[i]);
                        return 1;
                }
                if (dsys_close(fd) != 0)
                        return 1;
                if (magic == nd_magic("DOBJ1 ")) {
                        rc = add_object(argv[i], 0UL);
                } else if (magic == nd_magic("DARC1 ")) {
                        rc = add_archive(argv[i]);
                } else {
                        rc = -1;
                }
                if (rc < 0) {
                        (void)diag_path("INVALID OR EXCESS INPUT", argv[i]);
                        return 1;
                }
        }
        if (resolve_archives() != 0 || layout() != 0 ||
            write_dxr(out, pure_request, rt_required) != 0) {
                (void)diag_path("LINK FAILED", 0);
                return 1;
        }
        if (map != 0 && write_map(map) != 0) {
                (void)dsys_unlink(out);
                (void)diag_path("MAP WRITE FAILED", map);
                return 1;
        }
        /* Read back the header once so truncation/write failures cannot be
         * mistaken for a successful native link. */
        fd = dsys_open(out, SYS_O_RDONLY);
        if (fd < 0 || nd_read_at(fd, 0UL, &h, 1U) != 0 ||
            nd_lh(h) != nd_lh(nd_magic("DXR2  "))) {
                if (fd >= 0) (void)dsys_close(fd);
                return 1;
        }
        return dsys_close(fd) == 0 ? 0 : 1;
}
