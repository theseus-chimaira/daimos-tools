/* d6fsck.c - read-only structural checker for D6FS V2 disksets. */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define WORD_MASK               0777777777777ULL
#define HALF_MASK               0777777U
#define BLOCK_WORDS             0200U
#define SCAN_LIMIT              0200U
#define MAX_MEMBERS             8U
#define FCB_WORDS               020U
#define SUPER_WORDS             020U
#define DIRENT_WORDS            6U
#define EXTENTS                 7U
#define BITS_PER_MAP_BLOCK      (BLOCK_WORDS * 36U)
#define D6FS_MAGIC              044066263662ULL
#define D6FS_VERSION            2U
#define D6FS_LAYOUT_MAGIC       0442654636222ULL
#define D6FS_LAYOUT_MAGIC_WORD  006U
#define D6FS_LAYOUT_RANGE_WORD  007U
#define D6FS_LAYOUT_SUPER_A     010U
#define D6FS_LAYOUT_SUPER_B     011U
#define D6FS_TYPE_FREE          0U
#define D6FS_TYPE_REG           1U
#define D6FS_TYPE_DIR           2U
#define D6FS_TYPE_SYMLINK       3U
#define EXTENT_LOW_BITS         12U
#define EXTENT_LOW_MASK         07777ULL
#define EXTENT_HIGH_MASK        037ULL
#define HASH_MASK               077777777ULL
#define DIRENT_HASH_SHIFT       12U
#define DIRENT_TYPE_SHIFT       9U
#define DIRENT_TYPE_MASK        07U

struct member {
        FILE *fp;
        char path[512];
        unsigned descriptor;
        unsigned base;
        unsigned blocks;
};

struct super_info {
        uint64_t sequence;
        unsigned state;
        uint64_t fsid[2];
        uint64_t dsid[2];
        unsigned total;
        unsigned root;
        unsigned fcb_start;
        unsigned fcb_count;
        unsigned freemap_start;
        unsigned freemap_blocks;
        unsigned summary_start;
        unsigned summary_blocks;
};

struct fcb_info {
        unsigned type;
        unsigned flags;
        unsigned mode;
        unsigned tail;
        unsigned extents;
        unsigned parent;
        uint64_t size;
};

static struct member members[MAX_MEMBERS];
static unsigned member_count;
static unsigned errors;

static void die(const char *msg)
{
        fprintf(stderr, "d6fsck: %s\n", msg);
        exit(2);
}

static uint64_t get64le(const unsigned char p[8])
{
        uint64_t v;
        unsigned i;

        v = 0;
        for (i = 0; i < 8; ++i)
                v |= (uint64_t)p[i] << (i * 8U);
        return v & WORD_MASK;
}

static int read_phys(struct member *m, unsigned sector,
    uint64_t block[BLOCK_WORDS])
{
        unsigned char raw[8];
        unsigned i;

        if (fseek(m->fp, (long)sector * BLOCK_WORDS * 8L, SEEK_SET) != 0)
                return -1;
        for (i = 0; i < BLOCK_WORDS; ++i) {
                if (fread(raw, 1, 8, m->fp) != 8)
                        return -1;
                block[i] = get64le(raw);
        }
        return 0;
}

static unsigned total_blocks(void)
{
        unsigned i, total;

        total = 0;
        for (i = 0; i < member_count; ++i)
                total += members[i].blocks;
        return total;
}

static int map_logical(unsigned logical, unsigned *memberp, unsigned *blockp)
{
        unsigned floor, next, width, zone, rel, slot, i;

        if (logical >= total_blocks())
                return -1;
        floor = 0;
        for (;;) {
                next = 0;
                width = 0;
                for (i = 0; i < member_count; ++i) {
                        if (members[i].blocks > floor) {
                                ++width;
                                if (next == 0 || members[i].blocks < next)
                                        next = members[i].blocks;
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
        for (i = 0; i < member_count; ++i) {
                if (members[i].blocks <= floor)
                        continue;
                if (slot == 0) {
                        *memberp = i;
                        *blockp = floor + rel;
                        return 0;
                }
                --slot;
        }
        return -1;
}

static int read_logical(unsigned logical, uint64_t block[BLOCK_WORDS])
{
        unsigned mi, local;

        if (map_logical(logical, &mi, &local) != 0)
                return -1;
        return read_phys(&members[mi], members[mi].base + local, block);
}

static void problem(const char *what, unsigned a, unsigned b)
{
        if (b == ~0U)
                fprintf(stderr, "d6fsck: %s: %o\n", what, a);
        else
                fprintf(stderr, "d6fsck: %s: %o/%o\n", what, a, b);
        ++errors;
}

static void scan_member(struct member *m)
{
        uint64_t block[BLOCK_WORDS];
        unsigned s;

        for (s = 0; s < SCAN_LIMIT; ++s) {
                if (read_phys(m, s, block) != 0)
                        die("cannot scan DBOOT descriptor");
                if (block[D6FS_LAYOUT_MAGIC_WORD] != D6FS_LAYOUT_MAGIC)
                        continue;
                m->descriptor = s;
                m->base = (unsigned)((block[D6FS_LAYOUT_RANGE_WORD] >> 18) & HALF_MASK);
                m->blocks = (unsigned)(block[D6FS_LAYOUT_RANGE_WORD] & HALF_MASK);
                return;
        }
        die("D6FS root-layout descriptor not found");
}

static void open_members(const char *dir, unsigned n, unsigned *sap,
    unsigned *sbp)
{
        uint64_t block[BLOCK_WORDS];
        unsigned i, sa, sb;

        member_count = n;
        sa = sb = 0;
        for (i = 0; i < n; ++i) {
                int len;

                len = snprintf(members[i].path, sizeof(members[i].path),
                    "%s/dsk%u.dsk", dir, i);
                if (len < 0 || (size_t)len >= sizeof(members[i].path))
                        die("disk path too long");
                members[i].fp = fopen(members[i].path, "rb");
                if (members[i].fp == NULL)
                        die("cannot open disk member");
                scan_member(&members[i]);
                if (read_phys(&members[i], members[i].descriptor, block) != 0)
                        die("cannot reread DBOOT descriptor");
                if (i == 0) {
                        sa = (unsigned)block[D6FS_LAYOUT_SUPER_A];
                        sb = (unsigned)block[D6FS_LAYOUT_SUPER_B];
                } else if (sa != (unsigned)block[D6FS_LAYOUT_SUPER_A] ||
                    sb != (unsigned)block[D6FS_LAYOUT_SUPER_B])
                        die("members disagree on superblock locations");
        }
        if (sa == sb || sa >= total_blocks() || sb >= total_blocks())
                die("invalid superblock locations");
        *sap = sa;
        *sbp = sb;
}

static int range_ok(unsigned start, unsigned count, unsigned total)
{
        return count != 0 && start < total && count <= total - start;
}

static int super_decode(const uint64_t sb[SUPER_WORDS], unsigned disk_blocks,
    struct super_info *s)
{
        if ((sb[0] & ~077ULL) != (D6FS_MAGIC & ~077ULL) ||
            (sb[0] & 077ULL) != D6FS_VERSION || sb[2] > 1 ||
            sb[7] == 0 || sb[7] > disk_blocks || sb[012] == 0 ||
            sb[010] >= sb[012])
                return -1;
        s->sequence = sb[1];
        s->state = (unsigned)sb[2];
        s->fsid[0] = sb[3]; s->fsid[1] = sb[4];
        s->dsid[0] = sb[5]; s->dsid[1] = sb[6];
        s->total = (unsigned)sb[7];
        s->root = (unsigned)sb[010];
        s->fcb_start = (unsigned)sb[011];
        s->fcb_count = (unsigned)sb[012];
        s->freemap_start = (unsigned)sb[013];
        s->freemap_blocks = (unsigned)sb[014];
        s->summary_start = (unsigned)sb[015];
        s->summary_blocks = (unsigned)sb[016];
        if (!range_ok(s->fcb_start,
            (s->fcb_count * FCB_WORDS + BLOCK_WORDS - 1) / BLOCK_WORDS,
            s->total) || !range_ok(s->freemap_start, s->freemap_blocks, s->total) ||
            !range_ok(s->summary_start, s->summary_blocks, s->total))
                return -1;
        if (sb[017] != 0)
                return -1;
        return 0;
}

static int same_identity(const struct super_info *a, const struct super_info *b)
{
        return a->fsid[0] == b->fsid[0] && a->fsid[1] == b->fsid[1] &&
            a->dsid[0] == b->dsid[0] && a->dsid[1] == b->dsid[1] &&
            a->total == b->total && a->root == b->root &&
            a->fcb_start == b->fcb_start && a->fcb_count == b->fcb_count &&
            a->freemap_start == b->freemap_start &&
            a->freemap_blocks == b->freemap_blocks &&
            a->summary_start == b->summary_start &&
            a->summary_blocks == b->summary_blocks;
}

static unsigned extent_high(uint64_t word, unsigned e)
{
        return (unsigned)((word >> (e * 5U)) & EXTENT_HIGH_MASK);
}

static int fcb_decode(const uint64_t f[FCB_WORDS], unsigned total,
    unsigned fcb_count, struct fcb_info *i)
{
        unsigned e;
        uint64_t capacity;

        i->type = (unsigned)((f[0] >> 33) & 07U);
        i->flags = (unsigned)((f[0] >> 24) & 0777U);
        i->mode = (unsigned)((f[0] >> 12) & 07777U);
        i->tail = (unsigned)((f[0] >> 8) & 017U);
        i->extents = (unsigned)((f[0] >> 4) & 017U);
        i->size = f[2];
        i->parent = (unsigned)((f[4] >> 18) & HALF_MASK);
        if (i->type > D6FS_TYPE_SYMLINK || i->tail > 4 || i->extents > EXTENTS ||
            (f[0] & 017U) != 0 || (f[4] & HALF_MASK) != 0 ||
            f[015] != 0 || f[016] != 0 || f[017] != 0)
                return -1;
        if (i->type == D6FS_TYPE_FREE)
                return i->extents == 0 && i->size == 0 ? 0 : -1;
        if (i->parent >= fcb_count)
                return -1;
        capacity = 0;
        for (e = 0; e < EXTENTS; ++e) {
                uint64_t run, count;
                unsigned start, blocks;

                run = f[006 + e];
                if (e >= i->extents) {
                        if (run != 0 || extent_high(f[005], e) != 0)
                                return -1;
                        continue;
                }
                start = (unsigned)(run >> EXTENT_LOW_BITS);
                count = ((uint64_t)extent_high(f[005], e) << EXTENT_LOW_BITS) |
                    (run & EXTENT_LOW_MASK);
                blocks = (unsigned)(count + 1);
                if (start >= total || blocks > total - start)
                        return -1;
                capacity += (uint64_t)blocks * BLOCK_WORDS;
        }
        return i->size <= capacity ? 0 : -1;
}

static int get_bit(const uint64_t *map, unsigned bit)
{
        return (map[bit / 36U] & ((uint64_t)1U << (35U - bit % 36U))) != 0;
}

static void mark_range(unsigned char *used, unsigned total, unsigned start,
    unsigned count, unsigned owner)
{
        unsigned b;

        for (b = 0; b < count; ++b) {
                unsigned p = start + b;
                if (p >= total) {
                        problem("block outside filesystem", p, ~0U);
                        continue;
                }
                if (used[p] != 0 && used[p] != owner)
                        problem("cross-linked block", p, ~0U);
                else
                        used[p] = (unsigned char)owner;
        }
}

static int read_file_word(const struct super_info *s, const uint64_t *fcbs,
    unsigned fi, uint64_t off, uint64_t *vp)
{
        const uint64_t *f = fcbs + (size_t)fi * FCB_WORDS;
        unsigned e;
        uint64_t left;

        left = off;
        for (e = 0; e < EXTENTS; ++e) {
                uint64_t run, count, words;
                unsigned start, blocks, logical, wi;
                uint64_t block[BLOCK_WORDS];

                if (e >= (unsigned)((f[0] >> 4) & 017U))
                        break;
                run = f[006 + e];
                start = (unsigned)(run >> EXTENT_LOW_BITS);
                count = ((uint64_t)extent_high(f[005], e) << EXTENT_LOW_BITS) |
                    (run & EXTENT_LOW_MASK);
                blocks = (unsigned)(count + 1);
                words = (uint64_t)blocks * BLOCK_WORDS;
                if (left >= words) {
                        left -= words;
                        continue;
                }
                logical = start + (unsigned)(left / BLOCK_WORDS);
                wi = (unsigned)(left % BLOCK_WORDS);
                if (logical >= s->total || read_logical(logical, block) != 0)
                        return -1;
                *vp = block[wi];
                return 0;
        }
        return -1;
}

static uint64_t dir_hash(const uint64_t name[4])
{
        uint64_t h, w;
        int last;
        unsigned i, chars, c;

        last = -1;
        for (i = 0; i < 24; ++i) {
                w = name[i / 6U];
                c = (unsigned)((w >> (30U - (i % 6U) * 6U)) & 077U);
                if (c != 0)
                        last = (int)i;
        }
        chars = last < 0 ? 0U : (unsigned)last + 1U;
        h = 0;
        for (i = 0; i < chars; ++i) {
                w = name[i / 6U];
                c = (unsigned)(((w >> (30U - (i % 6U) * 6U)) & 077U) + 040U);
                h = ((h << 5) ^ (h >> 19) ^ (c & 0377U)) & HASH_MASK;
        }
        return h;
}

static void check_filesystem(unsigned sa, unsigned sb)
{
        uint64_t ba[BLOCK_WORDS], bb[BLOCK_WORDS];
        struct super_info ai, bi, *s;
        int av, bv;
        unsigned selected, total, fcb_blocks, i, b;
        uint64_t *fcbs, *freemap, *summary;
        struct fcb_info *infos;
        unsigned char *used, *reachable;
        unsigned *refs, *queue;
        unsigned qh, qt;

        if (read_logical(sa, ba) != 0 || read_logical(sb, bb) != 0)
                die("cannot read superblocks");
        av = super_decode(ba, total_blocks(), &ai) == 0;
        bv = super_decode(bb, total_blocks(), &bi) == 0;
        if (!av) problem("invalid superblock", sa, ~0U);
        if (!bv) problem("invalid superblock", sb, ~0U);
        if (!av && !bv)
                die("no structurally valid superblock");
        if (av && bv && !same_identity(&ai, &bi))
                problem("superblocks disagree", sa, sb);
        if (av && bv && ai.sequence == bi.sequence && memcmp(ba, bb, SUPER_WORDS * sizeof(uint64_t)) != 0)
                problem("equal-sequence superblocks differ", sa, sb);
        if (!bv || (av && ai.sequence >= bi.sequence)) {
                s = &ai; selected = sa;
        } else {
                s = &bi; selected = sb;
        }
        if (s->total != total_blocks())
                problem("superblock total differs from diskset", s->total, total_blocks());
        if (s->state != 0)
                fprintf(stderr, "d6fsck: note: selected superblock is DIRTY\n");

        total = s->total;
        fcb_blocks = (s->fcb_count * FCB_WORDS + BLOCK_WORDS - 1) / BLOCK_WORDS;
        fcbs = calloc((size_t)s->fcb_count * FCB_WORDS, sizeof(*fcbs));
        infos = calloc(s->fcb_count, sizeof(*infos));
        freemap = calloc((size_t)s->freemap_blocks * BLOCK_WORDS, sizeof(*freemap));
        summary = calloc((size_t)s->summary_blocks * BLOCK_WORDS, sizeof(*summary));
        used = calloc(total, 1);
        reachable = calloc(s->fcb_count, 1);
        refs = calloc(s->fcb_count, sizeof(*refs));
        queue = calloc(s->fcb_count, sizeof(*queue));
        if (fcbs == NULL || infos == NULL || freemap == NULL || summary == NULL ||
            used == NULL || reachable == NULL || refs == NULL || queue == NULL)
                die("out of memory");

        for (b = 0; b < fcb_blocks; ++b) {
                uint64_t block[BLOCK_WORDS];
                unsigned words = BLOCK_WORDS;
                if (read_logical(s->fcb_start + b, block) != 0)
                        die("cannot read FCB table");
                if ((b + 1) * BLOCK_WORDS > s->fcb_count * FCB_WORDS)
                        words = s->fcb_count * FCB_WORDS - b * BLOCK_WORDS;
                memcpy(fcbs + (size_t)b * BLOCK_WORDS, block, words * sizeof(uint64_t));
        }
        for (b = 0; b < s->freemap_blocks; ++b)
                if (read_logical(s->freemap_start + b,
                    freemap + (size_t)b * BLOCK_WORDS) != 0)
                        die("cannot read free map");
        for (b = 0; b < s->summary_blocks; ++b)
                if (read_logical(s->summary_start + b,
                    summary + (size_t)b * BLOCK_WORDS) != 0)
                        die("cannot read free summary");

        mark_range(used, total, 0, (sa > sb ? sa : sb) + 1U, 1);
        mark_range(used, total, s->fcb_start, fcb_blocks, 1);
        mark_range(used, total, s->freemap_start, s->freemap_blocks, 1);
        mark_range(used, total, s->summary_start, s->summary_blocks, 1);

        for (i = 0; i < s->fcb_count; ++i) {
                uint64_t *f = fcbs + (size_t)i * FCB_WORDS;
                unsigned e;
                if (fcb_decode(f, total, s->fcb_count, &infos[i]) != 0) {
                        problem("invalid FCB", i, ~0U);
                        continue;
                }
                if (infos[i].type == D6FS_TYPE_FREE)
                        continue;
                for (e = 0; e < infos[i].extents; ++e) {
                        uint64_t run = f[006 + e];
                        uint64_t count = ((uint64_t)extent_high(f[005], e) << EXTENT_LOW_BITS) |
                            (run & EXTENT_LOW_MASK);
                        mark_range(used, total, (unsigned)(run >> EXTENT_LOW_BITS),
                            (unsigned)(count + 1), i + 2U);
                }
        }
        if (infos[s->root].type != D6FS_TYPE_DIR || infos[s->root].parent != s->root)
                problem("invalid root FCB", s->root, ~0U);

        reachable[s->root] = 1;
        queue[qt = 0] = s->root;
        qt = 1;
        qh = 0;
        while (qh < qt) {
                unsigned di = queue[qh++];
                uint64_t off;
                if (infos[di].type != D6FS_TYPE_DIR)
                        continue;
                if ((infos[di].size % DIRENT_WORDS) != 0) {
                        problem("directory size not multiple of dirent", di, ~0U);
                        continue;
                }
                for (off = 0; off < infos[di].size; off += DIRENT_WORDS) {
                        uint64_t ent[DIRENT_WORDS], name[4], hash;
                        unsigned j, child, type;
                        for (j = 0; j < DIRENT_WORDS; ++j)
                                if (read_file_word(s, fcbs, di, off + j, &ent[j]) != 0) {
                                        problem("cannot read directory entry", di, (unsigned)(off / DIRENT_WORDS));
                                        break;
                                }
                        if (j != DIRENT_WORDS)
                                continue;
                        for (j = 0; j < 4; ++j) name[j] = ent[j];
                        hash = (ent[4] >> DIRENT_HASH_SHIFT) & HASH_MASK;
                        type = (unsigned)((ent[4] >> DIRENT_TYPE_SHIFT) & DIRENT_TYPE_MASK);
                        child = (unsigned)((ent[5] >> 18) & HALF_MASK);
                        if ((ent[5] & HALF_MASK) != 0 || child >= s->fcb_count ||
                            child == s->root || infos[child].type == D6FS_TYPE_FREE) {
                                problem("invalid directory child", di, child);
                                continue;
                        }
                        if (type != infos[child].type)
                                problem("directory cached type mismatch", di, child);
                        if (hash != dir_hash(name))
                                problem("directory hash mismatch", di, child);
                        if (infos[child].parent != di)
                                problem("FCB parent mismatch", child, di);
                        ++refs[child];
                        if (!reachable[child]) {
                                reachable[child] = 1;
                                if (qt < s->fcb_count)
                                        queue[qt++] = child;
                        }
                }
        }

        for (i = 0; i < s->fcb_count; ++i) {
                if (infos[i].type == D6FS_TYPE_FREE)
                        continue;
                if (i == s->root) {
                        if (refs[i] != 0)
                                problem("root referenced by directory", i, refs[i]);
                } else if (refs[i] != 1)
                        problem("FCB reference count", i, refs[i]);
                if (!reachable[i])
                        problem("unreachable FCB", i, ~0U);
        }

        for (i = 0; i < total; ++i) {
                unsigned mbi = i / BITS_PER_MAP_BLOCK;
                unsigned bit = i % BITS_PER_MAP_BLOCK;
                int allocated = get_bit(freemap + (size_t)mbi * BLOCK_WORDS, bit);
                if ((used[i] != 0) != allocated)
                        problem(used[i] != 0 ? "referenced block marked free" : "allocated leaked block", i, ~0U);
        }
        for (i = 0; i < s->freemap_blocks; ++i) {
                unsigned first = i * BITS_PER_MAP_BLOCK;
                unsigned limit = first + BITS_PER_MAP_BLOCK;
                unsigned p;
                int has_free = 0;
                int summary_bit;
                if (limit > total) limit = total;
                for (p = first; p < limit; ++p) {
                        unsigned bit = p - first;
                        if (!get_bit(freemap + (size_t)i * BLOCK_WORDS, bit)) {
                                has_free = 1;
                                break;
                        }
                }
                summary_bit = get_bit(summary, i);
                if (summary_bit != has_free)
                        problem("free-summary mismatch", i, ~0U);
        }

        fprintf(stderr, "d6fsck: selected superblock %o seq=%llo state=%s\n",
            selected, (unsigned long long)s->sequence,
            s->state == 0 ? "CLEAN" : "DIRTY");
        if (errors == 0)
                fprintf(stderr, "d6fsck: %u blocks, %u FCBs: clean\n",
                    total, s->fcb_count);

        free(queue); free(refs); free(reachable); free(used);
        free(summary); free(freemap); free(infos); free(fcbs);
}

static void usage(void)
{
        fprintf(stderr, "usage: d6fsck -n members -d diskdir\n");
        exit(2);
}

int main(int argc, char **argv)
{
        const char *dir = NULL;
        unsigned n = 0, sa, sb, i;
        int a;

        for (a = 1; a < argc; ++a) {
                if (strcmp(argv[a], "-n") == 0 && a + 1 < argc)
                        n = (unsigned)strtoul(argv[++a], NULL, 0);
                else if (strcmp(argv[a], "-d") == 0 && a + 1 < argc)
                        dir = argv[++a];
                else
                        usage();
        }
        if (dir == NULL || n == 0 || n > MAX_MEMBERS)
                usage();
        open_members(dir, n, &sa, &sb);
        check_filesystem(sa, sb);
        for (i = 0; i < member_count; ++i)
                fclose(members[i].fp);
        return errors == 0 ? 0 : 1;
}
