#define _POSIX_C_SOURCE 200809L
#include "d6maint.h"

#include <errno.h>
#include <sys/stat.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

static uint64_t get64le(const unsigned char p[8])
{
        uint64_t v;
        unsigned i;

        v = 0;
        for (i = 0; i < 8; ++i)
                v |= (uint64_t)p[i] << (i * 8U);
        return v & D6M_WORD_MASK;
}

static void put64le(unsigned char p[8], uint64_t v)
{
        unsigned i;

        v &= D6M_WORD_MASK;
        for (i = 0; i < 8; ++i)
                p[i] = (unsigned char)((v >> (i * 8U)) & 0377U);
}

static void seterr(char *err, size_t n, const char *text)
{
        if (err == NULL || n == 0)
                return;
        (void)snprintf(err, n, "%s", text);
}


int d6m_clone_diskset(const char *srcdir, const char *dstdir,
    unsigned members, char *err, size_t errlen)
{
        unsigned char *buf;
        unsigned i;
        char src[1024], dst[1024];

        if (srcdir == NULL || dstdir == NULL || members == 0 ||
            members > D6M_MAX_MEMBERS) {
                seterr(err, errlen, "invalid diskset clone arguments");
                return -1;
        }
        if (mkdir(dstdir, 0777) != 0) {
                seterr(err, errlen,
                    errno == EEXIST ? "output directory already exists" :
                    "cannot create output directory");
                return -1;
        }
        buf = malloc(16384U);
        if (buf == NULL) {
                seterr(err, errlen, "out of memory cloning diskset");
                return -1;
        }
        for (i = 0; i < members; ++i) {
                FILE *in, *out;
                size_t nr;

                if (snprintf(src, sizeof(src), "%s/dsk%u.dsk", srcdir, i) >=
                    (int)sizeof(src) ||
                    snprintf(dst, sizeof(dst), "%s/dsk%u.dsk", dstdir, i) >=
                    (int)sizeof(dst)) {
                        free(buf);
                        seterr(err, errlen, "disk member path too long");
                        return -1;
                }
                in = fopen(src, "rb");
                if (in == NULL) {
                        free(buf);
                        seterr(err, errlen, "cannot open source disk member");
                        return -1;
                }
                out = fopen(dst, "wb");
                if (out == NULL) {
                        fclose(in);
                        free(buf);
                        seterr(err, errlen, "cannot create output disk member");
                        return -1;
                }
                while ((nr = fread(buf, 1, 16384U, in)) != 0)
                        if (fwrite(buf, 1, nr, out) != nr) {
                                fclose(out);
                                fclose(in);
                                free(buf);
                                seterr(err, errlen, "cannot copy disk member");
                                return -1;
                        }
                if (ferror(in) || fflush(out) != 0 ||
                    fsync(fileno(out)) != 0 || fclose(out) != 0 ||
                    fclose(in) != 0) {
                        free(buf);
                        seterr(err, errlen, "cannot finish disk member copy");
                        return -1;
                }
        }
        free(buf);
        return 0;
}

int d6m_read_phys(const struct d6m_set *set, unsigned member,
    unsigned sector, uint64_t block[D6M_BLOCK_WORDS])
{
        unsigned char raw[8];
        unsigned i;
        FILE *fp;

        if (set == NULL || block == NULL || member >= set->members ||
            sector >= set->member[member].sectors)
                return -1;
        fp = set->member[member].fp;
        if (fseek(fp, (long)sector * D6M_BLOCK_WORDS * 8L, SEEK_SET) != 0)
                return -1;
        for (i = 0; i < D6M_BLOCK_WORDS; ++i) {
                if (fread(raw, 1, 8, fp) != 8)
                        return -1;
                block[i] = get64le(raw);
        }
        return 0;
}

int d6m_write_phys(struct d6m_set *set, unsigned member,
    unsigned sector, const uint64_t block[D6M_BLOCK_WORDS])
{
        unsigned char raw[8];
        unsigned i;
        FILE *fp;

        if (set == NULL || !set->writable || block == NULL ||
            member >= set->members || sector >= set->member[member].sectors)
                return -1;
        fp = set->member[member].fp;
        if (fseek(fp, (long)sector * D6M_BLOCK_WORDS * 8L, SEEK_SET) != 0)
                return -1;
        for (i = 0; i < D6M_BLOCK_WORDS; ++i) {
                put64le(raw, block[i]);
                if (fwrite(raw, 1, 8, fp) != 8)
                        return -1;
        }
        return fflush(fp) == 0 ? 0 : -1;
}

unsigned d6m_total_blocks(const struct d6m_set *set)
{
        unsigned i, total;

        if (set == NULL)
                return 0;
        total = 0;
        for (i = 0; i < set->members; ++i)
                total += set->member[i].blocks;
        return total;
}

int d6m_map(const struct d6m_set *set, unsigned logical,
    unsigned *memberp, unsigned *localp)
{
        unsigned floor, next, width, zone, rel, slot, i;

        if (set == NULL || memberp == NULL || localp == NULL ||
            logical >= d6m_total_blocks(set))
                return -1;
        floor = 0;
        for (;;) {
                next = 0;
                width = 0;
                for (i = 0; i < set->members; ++i) {
                        if (set->member[i].blocks > floor) {
                                ++width;
                                if (next == 0 || set->member[i].blocks < next)
                                        next = set->member[i].blocks;
                        }
                }
                if (next == 0 || width == 0)
                        return -1;
                zone = (next - floor) * width;
                if (logical < zone)
                        break;
                logical -= zone;
                floor = next;
        }
        slot = logical % width;
        rel = logical / width;
        for (i = 0; i < set->members; ++i) {
                if (set->member[i].blocks <= floor)
                        continue;
                if (slot == 0) {
                        *memberp = i;
                        *localp = floor + rel;
                        return 0;
                }
                --slot;
        }
        return -1;
}

int d6m_inverse_map(const struct d6m_set *set, unsigned member, unsigned local,
    unsigned *logicalp)
{
        unsigned floor, next, width, base, slot, i;

        if (set == NULL || logicalp == NULL || member >= set->members ||
            local >= set->member[member].blocks)
                return -1;
        floor = 0;
        base = 0;
        for (;;) {
                next = 0;
                width = 0;
                slot = 0;
                for (i = 0; i < set->members; ++i) {
                        if (set->member[i].blocks <= floor)
                                continue;
                        if (i < member)
                                ++slot;
                        ++width;
                        if (next == 0 || set->member[i].blocks < next)
                                next = set->member[i].blocks;
                }
                if (next == 0 || width == 0)
                        return -1;
                if (set->member[member].blocks > floor && local >= floor &&
                    local < next) {
                        *logicalp = base + (local - floor) * width + slot;
                        return 0;
                }
                base += (next - floor) * width;
                floor = next;
        }
}

static int d6m_remap_phys(const struct d6m_set *set, unsigned *memberp,
    unsigned *sectorp)
{
        uint64_t source;
        unsigned i;

        if (set == NULL || memberp == NULL || sectorp == NULL ||
            *memberp >= set->members || *sectorp > D6M_BADMAP_BLOCK_MASK)
                return -1;
        source = ((uint64_t)*memberp << D6M_BADMAP_MEMBER_SHIFT) | *sectorp;
        for (i = 0; i < set->badmap_count; ++i) {
                uint64_t word = set->badmap[i];
                uint64_t key = (word >> 18) & D6M_HALF_MASK;
                uint64_t replacement;
                if (key > source)
                        break;
                if (key != source)
                        continue;
                replacement = word & D6M_HALF_MASK;
                *memberp = (unsigned)((replacement >> D6M_BADMAP_MEMBER_SHIFT) &
                    D6M_BADMAP_MEMBER_MASK);
                *sectorp = (unsigned)(replacement & D6M_BADMAP_BLOCK_MASK);
                if (*memberp >= set->members ||
                    *sectorp >= set->member[*memberp].sectors)
                        return -1;
                break;
        }
        return 0;
}

int d6m_read(const struct d6m_set *set, unsigned logical,
    uint64_t block[D6M_BLOCK_WORDS])
{
        unsigned m, local, sector;

        if (d6m_map(set, logical, &m, &local) != 0)
                return -1;
        sector = set->member[m].base + local;
        if (d6m_remap_phys(set, &m, &sector) != 0)
                return -1;
        return d6m_read_phys(set, m, sector, block);
}

int d6m_write(struct d6m_set *set, unsigned logical,
    const uint64_t block[D6M_BLOCK_WORDS])
{
        unsigned m, local, sector;

        if (d6m_map(set, logical, &m, &local) != 0)
                return -1;
        sector = set->member[m].base + local;
        if (d6m_remap_phys(set, &m, &sector) != 0)
                return -1;
        return d6m_write_phys(set, m, sector, block);
}

static int scan_member(struct d6m_set *set, unsigned mi, char *err, size_t errlen)
{
        struct d6m_member *m;
        uint64_t block[D6M_BLOCK_WORDS];
        long bytes;
        unsigned s;

        m = &set->member[mi];
        if (fseek(m->fp, 0L, SEEK_END) != 0 || (bytes = ftell(m->fp)) < 0 ||
            bytes % (long)(D6M_BLOCK_WORDS * 8U) != 0) {
                seterr(err, errlen, "invalid disk member length");
                return -1;
        }
        m->sectors = (unsigned)(bytes / (long)(D6M_BLOCK_WORDS * 8U));
        rewind(m->fp);
        for (s = 0; s < D6M_SCAN_LIMIT && s < m->sectors; ++s) {
                if (d6m_read_phys(set, mi, s, block) != 0) {
                        seterr(err, errlen, "cannot scan DBOOT descriptor");
                        return -1;
                }
                if (block[D6M_LAYOUT_MAGIC_WORD] != D6M_LAYOUT_MAGIC)
                        continue;
                m->descriptor = s;
                m->base = (unsigned)((block[D6M_LAYOUT_RANGE_WORD] >> 18) &
                    D6M_HALF_MASK);
                m->blocks = (unsigned)(block[D6M_LAYOUT_RANGE_WORD] &
                    D6M_HALF_MASK);
                return 0;
        }
        seterr(err, errlen, "D6FS root-layout descriptor not found");
        return -1;
}

int d6m_open(struct d6m_set *set, const char *dir, unsigned members,
    int writable, char *err, size_t errlen)
{
        uint64_t block[D6M_BLOCK_WORDS];
        unsigned i;

        if (set == NULL || dir == NULL || members == 0 ||
            members > D6M_MAX_MEMBERS) {
                seterr(err, errlen, "invalid diskset arguments");
                return -1;
        }
        memset(set, 0, sizeof(*set));
        set->members = members;
        set->writable = writable != 0;
        for (i = 0; i < members; ++i) {
                int len;
                struct d6m_member *m = &set->member[i];
                len = snprintf(m->path, sizeof(m->path), "%s/dsk%u.dsk", dir, i);
                if (len < 0 || (size_t)len >= sizeof(m->path)) {
                        seterr(err, errlen, "disk path too long");
                        d6m_close(set);
                        return -1;
                }
                m->fp = fopen(m->path, writable ? "r+b" : "rb");
                if (m->fp == NULL) {
                        seterr(err, errlen, strerror(errno));
                        d6m_close(set);
                        return -1;
                }
                if (scan_member(set, i, err, errlen) != 0 ||
                    d6m_read_phys(set, i, m->descriptor, block) != 0) {
                        d6m_close(set);
                        return -1;
                }
                if (i == 0) {
                        set->layout.super_a = (unsigned)block[D6M_LAYOUT_SUPER_A];
                        set->layout.super_b = (unsigned)block[D6M_LAYOUT_SUPER_B];
                        set->layout.swap_tail_blocks =
                            (unsigned)block[D6M_LAYOUT_SWAP_TAIL];
                        set->layout.bootstream_blocks =
                            (unsigned)block[D6M_LAYOUT_BOOTSTREAM];
                        set->layout.logstore_start =
                            (unsigned)block[D6M_LAYOUT_LOGSTORE_START];
                        set->layout.logstore_blocks =
                            (unsigned)block[D6M_LAYOUT_LOGSTORE_BLOCKS];
                        set->layout.badmap_start =
                            (unsigned)block[D6M_LAYOUT_BADMAP_START];
                        set->layout.badmap_blocks =
                            (unsigned)block[D6M_LAYOUT_BADMAP_BLOCKS];
                } else if (set->layout.super_a !=
                    (unsigned)block[D6M_LAYOUT_SUPER_A] ||
                    set->layout.super_b != (unsigned)block[D6M_LAYOUT_SUPER_B] ||
                    set->layout.swap_tail_blocks !=
                    (unsigned)block[D6M_LAYOUT_SWAP_TAIL] ||
                    set->layout.bootstream_blocks !=
                    (unsigned)block[D6M_LAYOUT_BOOTSTREAM] ||
                    set->layout.logstore_start !=
                    (unsigned)block[D6M_LAYOUT_LOGSTORE_START] ||
                    set->layout.logstore_blocks !=
                    (unsigned)block[D6M_LAYOUT_LOGSTORE_BLOCKS] ||
                    set->layout.badmap_start !=
                    (unsigned)block[D6M_LAYOUT_BADMAP_START] ||
                    set->layout.badmap_blocks !=
                    (unsigned)block[D6M_LAYOUT_BADMAP_BLOCKS]) {
                        seterr(err, errlen, "members disagree on D6FS layout");
                        d6m_close(set);
                        return -1;
                }
        }
        if (set->layout.super_a == set->layout.super_b ||
            set->layout.super_a >= d6m_total_blocks(set) ||
            set->layout.super_b >= d6m_total_blocks(set)) {
                seterr(err, errlen, "invalid superblock locations");
                d6m_close(set);
                return -1;
        }
        set->layout.explicit_ranges = set->layout.bootstream_blocks != 0U ||
            set->layout.logstore_start != 0U || set->layout.logstore_blocks != 0U ||
            set->layout.badmap_start != 0U || set->layout.badmap_blocks != 0U;
        if (set->layout.explicit_ranges) {
                if (set->layout.logstore_start != set->layout.bootstream_blocks ||
                    set->layout.badmap_start != set->layout.logstore_start +
                    set->layout.logstore_blocks ||
                    set->layout.super_a != set->layout.badmap_start +
                    set->layout.badmap_blocks ||
                    set->layout.super_b != set->layout.super_a + 1U) {
                        seterr(err, errlen, "invalid reserved-range layout");
                        d6m_close(set);
                        return -1;
                }
        } else {
                set->layout.bootstream_blocks = set->layout.super_a;
                set->layout.logstore_start = set->layout.super_a;
                set->layout.badmap_start = set->layout.super_a;
        }
        for (i = 0; i < members; ++i) {
                struct d6m_member *m = &set->member[i];
                if (m->blocks == 0 || m->base >= m->sectors ||
                    m->blocks > m->sectors - m->base ||
                    set->layout.swap_tail_blocks > m->sectors - m->base - m->blocks) {
                        seterr(err, errlen, "D6FS/swap range outside member");
                        d6m_close(set);
                        return -1;
                }
        }
        if (set->layout.badmap_blocks != 0U) {
                uint64_t *words;
                uint64_t previous = 0;
                unsigned capacity, count, b, copied;
                size_t nwords;
                if (set->layout.badmap_blocks != 1U) {
                        seterr(err, errlen, "BADMAP v1 requires one metadata block");
                        d6m_close(set);
                        return -1;
                }
                nwords = D6M_BLOCK_WORDS;
                words = calloc(nwords, sizeof(*words));
                if (words == NULL) {
                        seterr(err, errlen, "out of memory loading badmap");
                        d6m_close(set);
                        return -1;
                }
                for (b = 0; b < set->layout.badmap_blocks; ++b) {
                        unsigned lm, ll;
                        if (d6m_map(set, set->layout.badmap_start + b, &lm, &ll) != 0 ||
                            d6m_read_phys(set, lm, set->member[lm].base + ll,
                            words + (size_t)b * D6M_BLOCK_WORDS) != 0) {
                                free(words);
                                seterr(err, errlen, "cannot read badmap");
                                d6m_close(set);
                                return -1;
                        }
                }
                capacity = (unsigned)nwords - D6M_BADMAP_HEADER_WORDS;
                count = words[0] == D6M_BADMAP_MAGIC ? (unsigned)words[2] : 0U;
                if (words[0] != D6M_BADMAP_MAGIC || count > capacity) {
                        free(words);
                        seterr(err, errlen, "invalid badmap");
                        d6m_close(set);
                        return -1;
                }
                if (count != 0U) {
                        set->badmap = calloc(count, sizeof(*set->badmap));
                        if (set->badmap == NULL) {
                                free(words);
                                seterr(err, errlen, "out of memory loading badmap entries");
                                d6m_close(set);
                                return -1;
                        }
                }
                copied = 0U;
                for (b = D6M_BADMAP_HEADER_WORDS; b < (unsigned)nwords && copied < count; ++b) {
                        uint64_t word = words[b];
                        uint64_t source = (word >> 18) & D6M_HALF_MASK;
                        uint64_t replacement = word & D6M_HALF_MASK;
                        unsigned sm = (unsigned)((source >> D6M_BADMAP_MEMBER_SHIFT) &
                            D6M_BADMAP_MEMBER_MASK);
                        unsigned rm = (unsigned)((replacement >> D6M_BADMAP_MEMBER_SHIFT) &
                            D6M_BADMAP_MEMBER_MASK);
                        unsigned ss = (unsigned)(source & D6M_BADMAP_BLOCK_MASK);
                        unsigned rs = (unsigned)(replacement & D6M_BADMAP_BLOCK_MASK);
                        if (sm >= set->members || rm >= set->members ||
                            ss >= set->member[sm].sectors ||
                            rs >= set->member[rm].sectors || source == replacement ||
                            (copied != 0U && source <= previous)) {
                                free(words);
                                seterr(err, errlen, "invalid/unsorted BADMAP entry");
                                d6m_close(set);
                                return -1;
                        }
                        set->badmap[copied++] = word;
                        previous = source;
                }
                set->badmap_count = copied;
                free(words);
        }
        return 0;
}

void d6m_close(struct d6m_set *set)
{
        unsigned i;

        if (set == NULL)
                return;
        for (i = 0; i < set->members; ++i) {
                if (set->member[i].fp != NULL)
                        fclose(set->member[i].fp);
                set->member[i].fp = NULL;
        }
        free(set->badmap);
        set->badmap = NULL;
        set->badmap_count = 0U;
        set->members = 0;
}

static int range_ok(unsigned start, unsigned count, unsigned total)
{
        return count != 0 && start < total && count <= total - start;
}

int d6m_super_decode(const uint64_t sb[D6M_SUPER_WORDS], unsigned disk_blocks,
    struct d6m_super *s)
{
        uint64_t high;
        unsigned fcb_blocks;

        if (sb == NULL || s == NULL ||
            (sb[0] & ~077ULL) != (D6M_MAGIC & ~077ULL) ||
            (sb[0] & 077ULL) != D6M_VERSION || sb[2] > D6M_STATE_DIRTY ||
            sb[7] == 0 || sb[7] > disk_blocks || sb[7] > D6M_LOGICAL_BLOCK_LIMIT ||
            sb[012] == 0 || sb[010] >= sb[012])
                return -1;
        high = sb[017];
        if ((high & D6M_RES_RESERVED_MASK) != 0)
                return -1;
        s->sequence = sb[1];
        s->state = (unsigned)sb[2];
        s->fsid[0] = sb[3]; s->fsid[1] = sb[4];
        s->swap_start = (unsigned)(sb[5] >> D6M_RES_START_SHIFT);
        s->swap_blocks = (unsigned)((((high >> D6M_RES_SWAP_HI_SHIFT) &
            D6M_RES_LEN_HIGH_MASK) << 12U) | (sb[5] & D6M_RES_LEN_LOW_MASK));
        s->log_start = (unsigned)(sb[6] >> D6M_RES_START_SHIFT);
        s->log_blocks = (unsigned)((((high >> D6M_RES_LOG_HI_SHIFT) &
            D6M_RES_LEN_HIGH_MASK) << 12U) | (sb[6] & D6M_RES_LEN_LOW_MASK));
        s->total = (unsigned)sb[7];
        s->root = (unsigned)sb[010];
        s->fcb_start = (unsigned)sb[011];
        s->fcb_count = (unsigned)sb[012];
        s->freemap_start = (unsigned)sb[013];
        s->freemap_blocks = (unsigned)sb[014];
        s->summary_start = (unsigned)sb[015];
        s->summary_blocks = (unsigned)sb[016];
        fcb_blocks = (s->fcb_count * D6M_FCB_WORDS + D6M_BLOCK_WORDS - 1U) /
            D6M_BLOCK_WORDS;
        if (!range_ok(s->fcb_start, fcb_blocks, s->total) ||
            !range_ok(s->freemap_start, s->freemap_blocks, s->total) ||
            s->summary_blocks != 1U ||
            !range_ok(s->summary_start, s->summary_blocks, s->total) ||
            (s->swap_blocks == 0U ? s->swap_start != 0U :
            (s->swap_start != s->total ||
            s->swap_blocks > D6M_LOGICAL_BLOCK_LIMIT - s->swap_start)) ||
            (s->log_blocks == 0U ? s->log_start != 0U :
            !range_ok(s->log_start, s->log_blocks, s->total)))
                return -1;
        if (s->log_blocks != 0U &&
            ((s->log_start < s->fcb_start + fcb_blocks &&
            s->fcb_start < s->log_start + s->log_blocks) ||
            (s->log_start < s->freemap_start + s->freemap_blocks &&
            s->freemap_start < s->log_start + s->log_blocks) ||
            (s->log_start < s->summary_start + s->summary_blocks &&
            s->summary_start < s->log_start + s->log_blocks)))
                return -1;
        return 0;
}

int d6m_super_encode(uint64_t raw[D6M_SUPER_WORDS], const struct d6m_super *s)
{
        uint64_t high;

        if (raw == NULL || s == NULL || s->swap_start >= D6M_LOGICAL_BLOCK_LIMIT ||
            s->log_start >= D6M_LOGICAL_BLOCK_LIMIT ||
            s->swap_blocks >= D6M_LOGICAL_BLOCK_LIMIT ||
            s->log_blocks >= D6M_LOGICAL_BLOCK_LIMIT)
                return -1;
        memset(raw, 0, D6M_SUPER_WORDS * sizeof(*raw));
        raw[0] = (D6M_MAGIC & ~077ULL) | D6M_VERSION;
        raw[1] = s->sequence & D6M_WORD_MASK;
        raw[2] = s->state;
        raw[3] = s->fsid[0]; raw[4] = s->fsid[1];
        raw[5] = ((uint64_t)s->swap_start << D6M_RES_START_SHIFT) |
            (s->swap_blocks & D6M_RES_LEN_LOW_MASK);
        raw[6] = ((uint64_t)s->log_start << D6M_RES_START_SHIFT) |
            (s->log_blocks & D6M_RES_LEN_LOW_MASK);
        high = (((uint64_t)(s->swap_blocks >> 12U) & D6M_RES_LEN_HIGH_MASK) <<
            D6M_RES_SWAP_HI_SHIFT) |
            (((uint64_t)(s->log_blocks >> 12U) & D6M_RES_LEN_HIGH_MASK) <<
            D6M_RES_LOG_HI_SHIFT);
        raw[7] = s->total;
        raw[010] = s->root;
        raw[011] = s->fcb_start;
        raw[012] = s->fcb_count;
        raw[013] = s->freemap_start;
        raw[014] = s->freemap_blocks;
        raw[015] = s->summary_start;
        raw[016] = s->summary_blocks;
        raw[017] = high;
        return 0;
}

static int sequence_newer(uint64_t a, uint64_t b)
{
        uint64_t delta;

        delta = (a - b) & D6M_WORD_MASK;
        return delta != 0 && delta < (1ULL << 35);
}

static int same_identity(const struct d6m_super *a, const struct d6m_super *b)
{
        return a->fsid[0] == b->fsid[0] && a->fsid[1] == b->fsid[1];
}

int d6m_select_super(const struct d6m_set *set, struct d6m_super *s,
    unsigned *selected, uint64_t selected_block[D6M_BLOCK_WORDS],
    char *err, size_t errlen)
{
        uint64_t a[D6M_BLOCK_WORDS], b[D6M_BLOCK_WORDS];
        struct d6m_super ai, bi;
        int av, bv;

        if (set == NULL || s == NULL || selected == NULL || selected_block == NULL ||
            d6m_read(set, set->layout.super_a, a) != 0 ||
            d6m_read(set, set->layout.super_b, b) != 0) {
                seterr(err, errlen, "cannot read superblocks");
                return -1;
        }
        av = d6m_super_decode(a, d6m_total_blocks(set), &ai) == 0;
        bv = d6m_super_decode(b, d6m_total_blocks(set), &bi) == 0;
        if (!av && !bv) {
                seterr(err, errlen, "no structurally valid superblock");
                return -1;
        }
        if (av && bv && !same_identity(&ai, &bi)) {
                seterr(err, errlen, "superblock identities disagree");
                return -1;
        }
        if (av && bv && ai.sequence == bi.sequence &&
            memcmp(a, b, D6M_SUPER_WORDS * sizeof(uint64_t)) != 0) {
                seterr(err, errlen, "equal-sequence superblocks differ");
                return -1;
        }
        if (!bv || (av && (ai.sequence == bi.sequence ||
            sequence_newer(ai.sequence, bi.sequence)))) {
                *s = ai;
                *selected = set->layout.super_a;
                memcpy(selected_block, a, sizeof(a));
        } else if (!av || sequence_newer(bi.sequence, ai.sequence)) {
                *s = bi;
                *selected = set->layout.super_b;
                memcpy(selected_block, b, sizeof(b));
        } else {
                seterr(err, errlen, "ambiguous superblock sequence distance");
                return -1;
        }
        if (s->swap_blocks != 0U &&
            s->swap_blocks > set->layout.swap_tail_blocks * set->members) {
                seterr(err, errlen, "swap reservation exceeds raw tail capacity");
                return -1;
        }
        return 0;
}

unsigned d6m_extent_high(uint64_t word, unsigned e)
{
        return (unsigned)((word >> (e * 5U)) & D6M_EXTENT_HIGH_MASK);
}

int d6m_extent_decode(const uint64_t fcb[D6M_FCB_WORDS], unsigned e,
    unsigned *startp, unsigned *blocksp)
{
        uint64_t run, count;

        if (fcb == NULL || startp == NULL || blocksp == NULL || e >= D6M_EXTENTS)
                return -1;
        run = fcb[006 + e];
        count = ((uint64_t)d6m_extent_high(fcb[005], e) <<
            D6M_EXTENT_LOW_BITS) | (run & D6M_EXTENT_LOW_MASK);
        *startp = (unsigned)(run >> D6M_EXTENT_LOW_BITS);
        *blocksp = (unsigned)(count + 1U);
        return 0;
}

int d6m_extent_set(uint64_t fcb[D6M_FCB_WORDS], unsigned e,
    unsigned start, unsigned blocks)
{
        uint64_t count, mask;
        unsigned high;

        if (fcb == NULL || e >= D6M_EXTENTS || blocks == 0)
                return -1;
        count = (uint64_t)blocks - 1U;
        if ((count >> (D6M_EXTENT_LOW_BITS + 5U)) != 0)
                return -1;
        high = (unsigned)((count >> D6M_EXTENT_LOW_BITS) & D6M_EXTENT_HIGH_MASK);
        fcb[006 + e] = ((uint64_t)start << D6M_EXTENT_LOW_BITS) |
            (count & D6M_EXTENT_LOW_MASK);
        mask = (uint64_t)D6M_EXTENT_HIGH_MASK << (e * 5U);
        fcb[005] = (fcb[005] & ~mask) | ((uint64_t)high << (e * 5U));
        return 0;
}

int d6m_get_bit(const uint64_t *map, unsigned bit)
{
        return (map[bit / 36U] & ((uint64_t)1U << (35U - bit % 36U))) != 0;
}

void d6m_set_bit(uint64_t *map, unsigned bit, int value)
{
        uint64_t mask = (uint64_t)1U << (35U - bit % 36U);
        if (value)
                map[bit / 36U] |= mask;
        else
                map[bit / 36U] &= ~mask;
}

void d6m_rebuild_summary(const struct d6m_super *s, const uint64_t *freemap,
    uint64_t *summary)
{
        unsigned i;

        memset(summary, 0, (size_t)s->summary_blocks * D6M_BLOCK_WORDS *
            sizeof(*summary));
        for (i = 0; i < s->freemap_blocks; ++i) {
                unsigned first, limit, p;
                int has_free = 0;
                first = i * D6M_BITS_PER_MAP_BLOCK;
                limit = first + D6M_BITS_PER_MAP_BLOCK;
                if (limit > s->total)
                        limit = s->total;
                for (p = first; p < limit; ++p)
                        if (!d6m_get_bit(freemap +
                            (size_t)i * D6M_BLOCK_WORDS, p - first)) {
                                has_free = 1;
                                break;
                        }
                d6m_set_bit(summary, i, has_free);
        }
}

int d6m_begin_dirty(struct d6m_set *set, const struct d6m_super *old,
    unsigned selected, const uint64_t old_block[D6M_BLOCK_WORDS],
    struct d6m_super *dirty, unsigned *dirty_selected,
    uint64_t dirty_block[D6M_BLOCK_WORDS])
{
        struct d6m_super next;
        uint64_t raw[D6M_SUPER_WORDS];
        unsigned target, i;

        if (set == NULL || old == NULL || old_block == NULL || dirty == NULL ||
            dirty_selected == NULL || dirty_block == NULL || !set->writable ||
            old->state != D6M_STATE_CLEAN)
                return -1;
        next = *old;
        next.sequence = (next.sequence + 1U) & D6M_WORD_MASK;
        next.state = D6M_STATE_DIRTY;
        memcpy(dirty_block, old_block, D6M_BLOCK_WORDS * sizeof(*dirty_block));
        if (d6m_super_encode(raw, &next) != 0)
                return -1;
        for (i = 0; i < D6M_SUPER_WORDS; ++i)
                dirty_block[i] = raw[i];
        target = selected == set->layout.super_a ? set->layout.super_b :
            set->layout.super_a;
        if (d6m_write(set, target, dirty_block) != 0)
                return -1;
        *dirty = next;
        *dirty_selected = target;
        return 0;
}

int d6m_publish_clean(struct d6m_set *set, const struct d6m_super *old,
    unsigned selected, const uint64_t old_block[D6M_BLOCK_WORDS],
    struct d6m_super *new_super)
{
        uint64_t block[D6M_BLOCK_WORDS], raw[D6M_SUPER_WORDS];
        struct d6m_super next;
        unsigned target, i;

        if (set == NULL || old == NULL || old_block == NULL || !set->writable)
                return -1;
        next = *old;
        next.sequence = (next.sequence + 1U) & D6M_WORD_MASK;
        next.state = D6M_STATE_CLEAN;
        memcpy(block, old_block, sizeof(block));
        if (d6m_super_encode(raw, &next) != 0)
                return -1;
        for (i = 0; i < D6M_SUPER_WORDS; ++i)
                block[i] = raw[i];
        target = selected == set->layout.super_a ? set->layout.super_b :
            set->layout.super_a;
        if (d6m_write(set, target, block) != 0)
                return -1;
        if (new_super != NULL)
                *new_super = next;
        return 0;
}

int d6m_run_fsck(const char *argv0, const char *dir, unsigned members,
    int repair)
{
        char tool[1024], nbuf[32];
        const char *slash;
        pid_t pid;
        int status;

        if (argv0 == NULL || dir == NULL)
                return -1;
        slash = strrchr(argv0, '/');
        if (slash != NULL) {
                size_t prefix = (size_t)(slash - argv0 + 1);
                if (prefix + strlen("d6fsck") + 1U > sizeof(tool))
                        return -1;
                memcpy(tool, argv0, prefix);
                strcpy(tool + prefix, "d6fsck");
        } else
                strcpy(tool, "d6fsck");
        (void)snprintf(nbuf, sizeof(nbuf), "%u", members);
        pid = fork();
        if (pid < 0)
                return -1;
        if (pid == 0) {
                if (repair)
                        execl(tool, tool, "-r", "-n", nbuf, "-d", dir,
                            (char *)NULL);
                else
                        execl(tool, tool, "-n", nbuf, "-d", dir, (char *)NULL);
                _exit(127);
        }
        if (waitpid(pid, &status, 0) != pid)
                return -1;
        return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}
