#define _POSIX_C_SOURCE 200809L

/* d6fsck.c - read-only structural checker for D6FS V2 disksets. */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

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
#define D6FS_LAYOUT_SWAP_TAIL   012U
#define D6FS_LAYOUT_BOOTSTREAM_BLOCKS 013U
#define D6FS_LAYOUT_LOGSTORE_START 014U
#define D6FS_LAYOUT_LOGSTORE_BLOCKS 015U
#define D6FS_LAYOUT_BADMAP_START 016U
#define D6FS_LAYOUT_BADMAP_BLOCKS 017U
#define D6FS_BADMAP_MAGIC       0442642414422ULL
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
#define RES_START_SHIFT         12U
#define RES_LEN_LOW_MASK        07777ULL
#define RES_LEN_HIGH_MASK       07777ULL
#define RES_SWAP_HI_SHIFT       24U
#define RES_LOG_HI_SHIFT        12U
#define RES_RESERVED_MASK       07777ULL
#define LOGICAL_BLOCK_LIMIT     (1U << 24)

struct member {
        FILE *fp;
        char path[512];
        unsigned descriptor;
        unsigned base;
        unsigned blocks;
        unsigned sectors;
};

struct layout_info {
        unsigned bootstream_blocks;
        unsigned logstore_start;
        unsigned logstore_blocks;
        unsigned badmap_start;
        unsigned badmap_blocks;
        unsigned swap_tail_blocks;
};

static struct layout_info disk_layout;

#ifdef D6FSCK_TEST_FAULTS
static unsigned test_fault_write;
static unsigned test_write_count;

static void test_fault_after_durable_write(void)
{
        const char *s;

        if (test_fault_write == 0U) {
                s = getenv("D6FSCK_TEST_ABORT_WRITE");
                if (s != NULL && *s != '\0')
                        test_fault_write = (unsigned)strtoul(s, NULL, 0);
                if (test_fault_write == 0U)
                        test_fault_write = ~0U;
        }
        ++test_write_count;
        if (test_write_count == test_fault_write)
                _exit(99);
}
#endif

struct super_info {
        uint64_t sequence;
        unsigned state;
        uint64_t fsid[2];
        unsigned swap_start;
        unsigned swap_blocks;
        unsigned log_start;
        unsigned log_blocks;
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
static unsigned repairs;
static int repair_mode;
static uint64_t badmap_entries[BLOCK_WORDS - 4U];
static unsigned badmap_entry_count;

struct dir_patch {
        unsigned dir;
        uint64_t offset;
        uint64_t value;
};

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

static int remap_phys(unsigned *memberp, unsigned *sectorp)
{
        uint64_t source;
        unsigned i;

        source = ((uint64_t)*memberp << 16) | *sectorp;
        for (i = 0; i < badmap_entry_count; ++i) {
                uint64_t word = badmap_entries[i];
                uint64_t key = (word >> 18) & HALF_MASK;
                uint64_t spare;
                if (key > source)
                        break;
                if (key != source)
                        continue;
                spare = word & HALF_MASK;
                *memberp = (unsigned)((spare >> 16) & 03U);
                *sectorp = (unsigned)(spare & 0177777U);
                if (*memberp >= member_count || *sectorp >= members[*memberp].sectors)
                        return -1;
                break;
        }
        return 0;
}

static int read_logical(unsigned logical, uint64_t block[BLOCK_WORDS])
{
        unsigned mi, local;

        unsigned sector;
        if (map_logical(logical, &mi, &local) != 0)
                return -1;
        sector = members[mi].base + local;
        if (remap_phys(&mi, &sector) != 0)
                return -1;
        return read_phys(&members[mi], sector, block);
}

static void put64le(unsigned char p[8], uint64_t v)
{
        unsigned i;

        v &= WORD_MASK;
        for (i = 0; i < 8; ++i)
                p[i] = (unsigned char)((v >> (i * 8U)) & 0377U);
}

static int write_phys(struct member *m, unsigned sector,
    const uint64_t block[BLOCK_WORDS])
{
        unsigned char raw[8];
        unsigned i;

        if (!repair_mode || fseek(m->fp, (long)sector * BLOCK_WORDS * 8L,
            SEEK_SET) != 0)
                return -1;
        for (i = 0; i < BLOCK_WORDS; ++i) {
                put64le(raw, block[i]);
                if (fwrite(raw, 1, 8, m->fp) != 8)
                        return -1;
        }
        if (fflush(m->fp) != 0 || fsync(fileno(m->fp)) != 0)
                return -1;
#ifdef D6FSCK_TEST_FAULTS
        test_fault_after_durable_write();
#endif
        return 0;
}

static int write_logical(unsigned logical, const uint64_t block[BLOCK_WORDS])
{
        unsigned mi, local;

        unsigned sector;
        if (map_logical(logical, &mi, &local) != 0)
                return -1;
        sector = members[mi].base + local;
        if (remap_phys(&mi, &sector) != 0)
                return -1;
        return write_phys(&members[mi], sector, block);
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
        long bytes;
        unsigned s;

        if (fseek(m->fp, 0L, SEEK_END) != 0)
                die("invalid disk member length");
        bytes = ftell(m->fp);
        if (bytes < 0 || bytes % (long)(BLOCK_WORDS * 8U) != 0)
                die("invalid disk member length");
        m->sectors = (unsigned)(bytes / (long)(BLOCK_WORDS * 8U));
        rewind(m->fp);
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
                members[i].fp = fopen(members[i].path, repair_mode ? "r+b" : "rb");
                if (members[i].fp == NULL)
                        die("cannot open disk member");
                scan_member(&members[i]);
                if (read_phys(&members[i], members[i].descriptor, block) != 0)
                        die("cannot reread DBOOT descriptor");
                if (i == 0) {
                        sa = (unsigned)block[D6FS_LAYOUT_SUPER_A];
                        sb = (unsigned)block[D6FS_LAYOUT_SUPER_B];
                        disk_layout.swap_tail_blocks =
                            (unsigned)block[D6FS_LAYOUT_SWAP_TAIL];
                        disk_layout.bootstream_blocks =
                            (unsigned)block[D6FS_LAYOUT_BOOTSTREAM_BLOCKS];
                        disk_layout.logstore_start =
                            (unsigned)block[D6FS_LAYOUT_LOGSTORE_START];
                        disk_layout.logstore_blocks =
                            (unsigned)block[D6FS_LAYOUT_LOGSTORE_BLOCKS];
                        disk_layout.badmap_start =
                            (unsigned)block[D6FS_LAYOUT_BADMAP_START];
                        disk_layout.badmap_blocks =
                            (unsigned)block[D6FS_LAYOUT_BADMAP_BLOCKS];
                } else if (sa != (unsigned)block[D6FS_LAYOUT_SUPER_A] ||
                    sb != (unsigned)block[D6FS_LAYOUT_SUPER_B] ||
                    disk_layout.swap_tail_blocks !=
                    (unsigned)block[D6FS_LAYOUT_SWAP_TAIL] ||
                    disk_layout.bootstream_blocks !=
                    (unsigned)block[D6FS_LAYOUT_BOOTSTREAM_BLOCKS] ||
                    disk_layout.logstore_start !=
                    (unsigned)block[D6FS_LAYOUT_LOGSTORE_START] ||
                    disk_layout.logstore_blocks !=
                    (unsigned)block[D6FS_LAYOUT_LOGSTORE_BLOCKS] ||
                    disk_layout.badmap_start !=
                    (unsigned)block[D6FS_LAYOUT_BADMAP_START] ||
                    disk_layout.badmap_blocks !=
                    (unsigned)block[D6FS_LAYOUT_BADMAP_BLOCKS])
                        die("members disagree on reserved layout");
        }
        if (sa == sb || sa >= total_blocks() || sb >= total_blocks())
                die("invalid superblock locations");
        if (disk_layout.bootstream_blocks != 0U ||
            disk_layout.logstore_start != 0U || disk_layout.logstore_blocks != 0U ||
            disk_layout.badmap_start != 0U || disk_layout.badmap_blocks != 0U) {
                if (disk_layout.logstore_start != disk_layout.bootstream_blocks ||
                    disk_layout.badmap_start != disk_layout.logstore_start +
                    disk_layout.logstore_blocks ||
                    sa != disk_layout.badmap_start + disk_layout.badmap_blocks ||
                    sb != sa + 1U)
                        die("invalid reserved-range layout");
        } else {
                disk_layout.bootstream_blocks = sa;
                disk_layout.logstore_start = sa;
                disk_layout.logstore_blocks = 0U;
                disk_layout.badmap_start = sa;
                disk_layout.badmap_blocks = 0U;
                fprintf(stderr, "d6fsck: note: legacy layout without explicit reserved ranges\n");
        }
        for (i = 0; i < n; ++i)
                if (members[i].base + members[i].blocks +
                    disk_layout.swap_tail_blocks > members[i].sectors)
                        die("D6FS/swap layout exceeds a member image");
        badmap_entry_count = 0U;
        if (disk_layout.badmap_blocks != 0U) {
                unsigned mi, local, count;
                if (disk_layout.badmap_blocks != 1U ||
                    map_logical(disk_layout.badmap_start, &mi, &local) != 0 ||
                    read_phys(&members[mi], members[mi].base + local, block) != 0 ||
                    block[0] != D6FS_BADMAP_MAGIC)
                        die("invalid BADMAP metadata");
                count = (unsigned)block[2];
                if (count > BLOCK_WORDS - 4U)
                        die("BADMAP count exceeds v1 capacity");
                memcpy(badmap_entries, block + 4U, count * sizeof(*badmap_entries));
                for (i = 0; i < count; ++i) {
                        uint64_t word = badmap_entries[i];
                        uint64_t source = (word >> 18) & HALF_MASK;
                        uint64_t replacement = word & HALF_MASK;
                        unsigned sm = (unsigned)((source >> 16) & 03U);
                        unsigned rm = (unsigned)((replacement >> 16) & 03U);
                        unsigned ss = (unsigned)(source & 0177777U);
                        unsigned rs = (unsigned)(replacement & 0177777U);
                        if (sm >= member_count || rm >= member_count ||
                            ss >= members[sm].sectors || rs >= members[rm].sectors ||
                            source == replacement ||
                            (i != 0U && ((badmap_entries[i - 1U] >> 18) & HALF_MASK) >= source))
                                die("invalid/unsorted BADMAP entry");
                }
                badmap_entry_count = count;
        }
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
        uint64_t high;
        unsigned fcb_blocks;

        if ((sb[0] & ~077ULL) != (D6FS_MAGIC & ~077ULL) ||
            (sb[0] & 077ULL) != D6FS_VERSION || sb[2] > 1 ||
            sb[7] == 0 || sb[7] > disk_blocks || sb[7] > LOGICAL_BLOCK_LIMIT ||
            sb[012] == 0 || sb[010] >= sb[012])
                return -1;
        high = sb[017];
        if ((high & RES_RESERVED_MASK) != 0)
                return -1;
        s->sequence = sb[1];
        s->state = (unsigned)sb[2];
        s->fsid[0] = sb[3]; s->fsid[1] = sb[4];
        s->swap_start = (unsigned)(sb[5] >> RES_START_SHIFT);
        s->swap_blocks = (unsigned)((((high >> RES_SWAP_HI_SHIFT) &
            RES_LEN_HIGH_MASK) << 12U) | (sb[5] & RES_LEN_LOW_MASK));
        s->log_start = (unsigned)(sb[6] >> RES_START_SHIFT);
        s->log_blocks = (unsigned)((((high >> RES_LOG_HI_SHIFT) &
            RES_LEN_HIGH_MASK) << 12U) | (sb[6] & RES_LEN_LOW_MASK));
        s->total = (unsigned)sb[7];
        s->root = (unsigned)sb[010];
        s->fcb_start = (unsigned)sb[011];
        s->fcb_count = (unsigned)sb[012];
        s->freemap_start = (unsigned)sb[013];
        s->freemap_blocks = (unsigned)sb[014];
        s->summary_start = (unsigned)sb[015];
        s->summary_blocks = (unsigned)sb[016];
        fcb_blocks = (s->fcb_count * FCB_WORDS + BLOCK_WORDS - 1U) / BLOCK_WORDS;
        if (!range_ok(s->fcb_start, fcb_blocks, s->total) ||
            !range_ok(s->freemap_start, s->freemap_blocks, s->total) ||
            s->summary_blocks != 1U ||
            !range_ok(s->summary_start, s->summary_blocks, s->total) ||
            (s->swap_blocks == 0U ? s->swap_start != 0U :
            (s->swap_start != s->total ||
            s->swap_blocks > LOGICAL_BLOCK_LIMIT - s->swap_start)) ||
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

static int sequence_newer(uint64_t a, uint64_t b)
{
        uint64_t delta;

        delta = (a - b) & WORD_MASK;
        return delta != 0 && delta < (1ULL << 35);
}

static int same_identity(const struct super_info *a, const struct super_info *b)
{
        return a->fsid[0] == b->fsid[0] && a->fsid[1] == b->fsid[1];
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
        if (i->type > D6FS_TYPE_SYMLINK ||
            (i->type == D6FS_TYPE_SYMLINK ? i->tail > 6 : i->tail > 4) ||
            i->extents > EXTENTS ||
            (f[0] & 017U) != 0 || (f[4] & HALF_MASK) != 0 ||
            f[015] != 0 || f[016] != 0 || f[017] != 0)
                return -1;
        if (i->type == D6FS_TYPE_FREE)
                return i->extents == 0 && i->size == 0 ? 0 : -1;
        if (i->parent >= fcb_count)
                return -1;
        capacity = 0;
        {
                unsigned previous_end = 0U;
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
                if (start >= total || blocks > total - start ||
                    (e != 0U && start < previous_end))
                        return -1;
                previous_end = start + blocks;
                capacity += (uint64_t)blocks * BLOCK_WORDS;
                }
        }
        return i->size <= capacity ? 0 : -1;
}

static int get_bit(const uint64_t *map, unsigned bit)
{
        return (map[bit / 36U] & ((uint64_t)1U << (35U - bit % 36U))) != 0;
}

static void set_bit(uint64_t *map, unsigned bit, int value)
{
        uint64_t mask;

        mask = (uint64_t)1U << (35U - bit % 36U);
        if (value)
                map[bit / 36U] |= mask;
        else
                map[bit / 36U] &= ~mask;
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

static int write_file_word(const struct super_info *s, const uint64_t *fcbs,
    unsigned fi, uint64_t off, uint64_t value)
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
                block[wi] = value & WORD_MASK;
                return write_logical(logical, block);
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

static int write_fcb_table(const struct super_info *s, const uint64_t *fcbs)
{
        unsigned fcb_blocks, b;

        fcb_blocks = (s->fcb_count * FCB_WORDS + BLOCK_WORDS - 1) / BLOCK_WORDS;
        for (b = 0; b < fcb_blocks; ++b) {
                uint64_t block[BLOCK_WORDS];
                unsigned words, i;

                memset(block, 0, sizeof(block));
                words = BLOCK_WORDS;
                if ((b + 1) * BLOCK_WORDS > s->fcb_count * FCB_WORDS)
                        words = s->fcb_count * FCB_WORDS - b * BLOCK_WORDS;
                for (i = 0; i < words; ++i)
                        block[i] = fcbs[(size_t)b * BLOCK_WORDS + i];
                if (write_logical(s->fcb_start + b, block) != 0)
                        return -1;
        }
        return 0;
}

static int write_map_blocks(unsigned start, unsigned blocks, const uint64_t *map)
{
        unsigned b;

        for (b = 0; b < blocks; ++b)
                if (write_logical(start + b, map + (size_t)b * BLOCK_WORDS) != 0)
                        return -1;
        return 0;
}

static void rebuild_summary(const struct super_info *s, const uint64_t *freemap,
    uint64_t *summary)
{
        unsigned i;

        memset(summary, 0, (size_t)s->summary_blocks * BLOCK_WORDS * sizeof(*summary));
        for (i = 0; i < s->freemap_blocks; ++i) {
                unsigned first, limit, p;
                int has_free;

                first = i * BITS_PER_MAP_BLOCK;
                limit = first + BITS_PER_MAP_BLOCK;
                if (limit > s->total)
                        limit = s->total;
                has_free = 0;
                for (p = first; p < limit; ++p)
                        if (!get_bit(freemap + (size_t)i * BLOCK_WORDS,
                            p - first)) {
                                has_free = 1;
                                break;
                        }
                set_bit(summary, i, has_free);
        }
}

static unsigned char **load_badmap(void)
{
        unsigned char **bad;
        unsigned i;

        bad = calloc(member_count, sizeof(*bad));
        if (bad == NULL)
                die("out of memory");
        for (i = 0; i < member_count; ++i) {
                bad[i] = calloc(members[i].blocks, 1);
                if (bad[i] == NULL)
                        die("out of memory");
        }
        for (i = 0; i < badmap_entry_count; ++i) {
                uint64_t word = badmap_entries[i];
                uint64_t source = (word >> 18) & HALF_MASK;
                uint64_t spare = word & HALF_MASK;
                unsigned sm = (unsigned)((source >> 16) & 03U);
                unsigned ss = (unsigned)(source & 0177777U);
                unsigned rm = (unsigned)((spare >> 16) & 03U);
                unsigned rs = (unsigned)(spare & 0177777U);
                if (sm >= member_count || rm >= member_count ||
                    ss >= members[sm].sectors || rs >= members[rm].sectors ||
                    rs < members[rm].base + members[rm].blocks +
                    disk_layout.swap_tail_blocks) {
                        problem("invalid BADMAP remap", i, ~0U);
                        continue;
                }
                if (i != 0U &&
                    ((badmap_entries[i - 1U] >> 18) & HALF_MASK) >= source)
                        problem("unsorted/duplicate BADMAP source", i, ~0U);
                if (ss >= members[sm].base &&
                    ss < members[sm].base + members[sm].blocks)
                        bad[sm][ss - members[sm].base] = 1;
        }
        return bad;
}

static void free_badmap(unsigned char **bad)
{
        unsigned i;

        if (bad == NULL)
                return;
        for (i = 0; i < member_count; ++i)
                free(bad[i]);
        free(bad);
}

static int publish_next_super(unsigned sa, unsigned sb, unsigned selected,
    const uint64_t selected_block[BLOCK_WORDS], unsigned state,
    unsigned *new_selected, uint64_t new_block[BLOCK_WORDS])
{
        unsigned target;

        memcpy(new_block, selected_block, BLOCK_WORDS * sizeof(*new_block));
        new_block[1] = (new_block[1] + 1U) & WORD_MASK;
        new_block[2] = state;
        target = selected == sa ? sb : sa;
        if (write_logical(target, new_block) != 0)
                return -1;
        if (new_selected != NULL)
                *new_selected = target;
        return 0;
}

static void stage_dir_patch(struct dir_patch **patches, unsigned *count,
    unsigned *capacity, unsigned dir, uint64_t offset, uint64_t value)
{
        struct dir_patch *next;
        unsigned ncap;

        if (*count == *capacity) {
                ncap = *capacity == 0U ? 8U : *capacity * 2U;
                next = realloc(*patches, (size_t)ncap * sizeof(*next));
                if (next == NULL)
                        die("out of memory staging directory repairs");
                *patches = next;
                *capacity = ncap;
        }
        (*patches)[*count].dir = dir;
        (*patches)[*count].offset = offset;
        (*patches)[*count].value = value;
        ++*count;
}

static int check_filesystem(unsigned sa, unsigned sb)
{
        uint64_t ba[BLOCK_WORDS], bb[BLOCK_WORDS];
        struct super_info ai, bi, *s;
        int av, bv, ambiguous, changed_fcbs, changed_dirs, changed_map;
        unsigned selected, total, fcb_blocks, i, b, planned;
        unsigned dir_patch_count, dir_patch_capacity;
        struct dir_patch *dir_patches;
        uint64_t *fcbs, *freemap, *summary;
        struct fcb_info *infos;
        unsigned char *used, *reachable, *crosslinked;
        unsigned char **bad_phys;
        unsigned *refs, *ref_parent, *queue;
        unsigned qh, qt;
        uint64_t *selected_block;

        ambiguous = changed_fcbs = changed_dirs = changed_map = 0;
        planned = dir_patch_count = dir_patch_capacity = 0U;
        dir_patches = NULL;
        if (read_logical(sa, ba) != 0 || read_logical(sb, bb) != 0)
                die("cannot read superblocks");
        av = super_decode(ba, total_blocks(), &ai) == 0;
        bv = super_decode(bb, total_blocks(), &bi) == 0;
        if (!av) problem("invalid superblock", sa, ~0U);
        if (!bv) problem("invalid superblock", sb, ~0U);
        if (!av && !bv)
                die("no structurally valid superblock");
        if (av && bv && !same_identity(&ai, &bi)) {
                problem("superblocks disagree", sa, sb);
                ambiguous = 1;
        }
        if (av && bv && ai.sequence == bi.sequence &&
            memcmp(ba, bb, SUPER_WORDS * sizeof(uint64_t)) != 0) {
                problem("equal-sequence superblocks differ", sa, sb);
                ambiguous = 1;
        }
        if (!bv) {
                s = &ai; selected = sa; selected_block = ba;
        } else if (!av) {
                s = &bi; selected = sb; selected_block = bb;
        } else if (ai.sequence == bi.sequence ||
            sequence_newer(ai.sequence, bi.sequence)) {
                s = &ai; selected = sa; selected_block = ba;
        } else if (sequence_newer(bi.sequence, ai.sequence)) {
                s = &bi; selected = sb; selected_block = bb;
        } else {
                problem("ambiguous superblock sequence distance", sa, sb);
                ambiguous = 1;
                s = &ai; selected = sa; selected_block = ba;
        }
        if (s->total != total_blocks()) {
                problem("superblock total differs from diskset", s->total, total_blocks());
                ambiguous = 1;
        }
        if (s->state != 0)
                problem("selected superblock DIRTY", selected, ~0U);
        if (s->swap_blocks != 0U &&
            s->swap_blocks > disk_layout.swap_tail_blocks * member_count) {
                problem("swap reservation exceeds raw tail capacity",
                    s->swap_blocks, disk_layout.swap_tail_blocks * member_count);
                ambiguous = 1;
        }

        total = s->total;
        fcb_blocks = (s->fcb_count * FCB_WORDS + BLOCK_WORDS - 1) / BLOCK_WORDS;
        fcbs = calloc((size_t)s->fcb_count * FCB_WORDS, sizeof(*fcbs));
        infos = calloc(s->fcb_count, sizeof(*infos));
        freemap = calloc((size_t)s->freemap_blocks * BLOCK_WORDS, sizeof(*freemap));
        summary = calloc((size_t)s->summary_blocks * BLOCK_WORDS, sizeof(*summary));
        used = calloc(total, 1);
        crosslinked = calloc(total, 1);
        reachable = calloc(s->fcb_count, 1);
        refs = calloc(s->fcb_count, sizeof(*refs));
        ref_parent = malloc((size_t)s->fcb_count * sizeof(*ref_parent));
        queue = calloc(s->fcb_count, sizeof(*queue));
        if (fcbs == NULL || infos == NULL || freemap == NULL || summary == NULL ||
            used == NULL || crosslinked == NULL || reachable == NULL || refs == NULL ||
            ref_parent == NULL || queue == NULL)
                die("out of memory");
        for (i = 0; i < s->fcb_count; ++i)
                ref_parent[i] = ~0U;
        bad_phys = load_badmap();

        for (b = 0; b < fcb_blocks; ++b) {
                uint64_t block[BLOCK_WORDS];
                unsigned words = BLOCK_WORDS;
                if (read_logical(s->fcb_start + b, block) != 0)
                        die("cannot read FCB table");
                if ((b + 1) * BLOCK_WORDS > s->fcb_count * FCB_WORDS)
                        words = s->fcb_count * FCB_WORDS - b * BLOCK_WORDS;
                memcpy(fcbs + (size_t)b * BLOCK_WORDS, block,
                    words * sizeof(uint64_t));
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
        if (s->log_blocks != 0U)
                mark_range(used, total, s->log_start, s->log_blocks, 1);
        for (i = 0; i < s->fcb_count; ++i) {
                uint64_t *f = fcbs + (size_t)i * FCB_WORDS;
                unsigned e;
                if (fcb_decode(f, total, s->fcb_count, &infos[i]) != 0) {
                        problem("invalid FCB", i, ~0U);
                        ambiguous = 1;
                        continue;
                }
                if (infos[i].type == D6FS_TYPE_FREE)
                        continue;
                for (e = 0; e < infos[i].extents; ++e) {
                        uint64_t run, count;
                        unsigned start, blocks, p;
                        run = f[006 + e];
                        count = ((uint64_t)extent_high(f[005], e) << EXTENT_LOW_BITS) |
                            (run & EXTENT_LOW_MASK);
                        start = (unsigned)(run >> EXTENT_LOW_BITS);
                        blocks = (unsigned)(count + 1);
                        for (p = start; p < start + blocks; ++p) {
                                if (p >= total) {
                                        problem("block outside filesystem", p, ~0U);
                                        ambiguous = 1;
                                        continue;
                                }
                                if (used[p] != 0 && used[p] != i + 2U) {
                                        problem("cross-linked block", p, ~0U);
                                        crosslinked[p] = 1;
                                        ambiguous = 1;
                                } else
                                        used[p] = (unsigned char)(i + 2U);
                        }
                }
        }
        if (infos[s->root].type != D6FS_TYPE_DIR || infos[s->root].parent != s->root) {
                problem("invalid root FCB", s->root, ~0U);
                ambiguous = 1;
        }

        reachable[s->root] = 1;
        queue[0] = s->root; qt = 1; qh = 0;
        while (qh < qt) {
                unsigned di = queue[qh++];
                uint64_t off;
                if (infos[di].type != D6FS_TYPE_DIR)
                        continue;
                if ((infos[di].size % DIRENT_WORDS) != 0) {
                        problem("directory size not multiple of dirent", di, ~0U);
                        ambiguous = 1;
                        continue;
                }
                for (off = 0; off < infos[di].size; off += DIRENT_WORDS) {
                        uint64_t ent[DIRENT_WORDS], name[4], hash, want_hash, want_meta;
                        unsigned j, child, type;
                        for (j = 0; j < DIRENT_WORDS; ++j)
                                if (read_file_word(s, fcbs, di, off + j, &ent[j]) != 0) {
                                        problem("cannot read directory entry", di,
                                            (unsigned)(off / DIRENT_WORDS));
                                        ambiguous = 1;
                                        break;
                                }
                        if (j != DIRENT_WORDS)
                                continue;
                        for (j = 0; j < DIRENT_WORDS; ++j)
                                if (ent[j] != 0)
                                        break;
                        if (j == DIRENT_WORDS)
                                continue;
                        for (j = 0; j < 4; ++j) name[j] = ent[j];
                        hash = (ent[4] >> DIRENT_HASH_SHIFT) & HASH_MASK;
                        type = (unsigned)((ent[4] >> DIRENT_TYPE_SHIFT) & DIRENT_TYPE_MASK);
                        child = (unsigned)((ent[5] >> 18) & HALF_MASK);
                        if ((ent[5] & HALF_MASK) != 0 || child >= s->fcb_count ||
                            child == s->root || infos[child].type == D6FS_TYPE_FREE) {
                                problem("invalid directory child", di, child);
                                ambiguous = 1;
                                continue;
                        }
                        want_hash = dir_hash(name);
                        want_meta = (want_hash << DIRENT_HASH_SHIFT) |
                            ((uint64_t)infos[child].type << DIRENT_TYPE_SHIFT);
                        if (type != infos[child].type || hash != want_hash) {
                                problem(type != infos[child].type ?
                                    "directory cached type mismatch" : "directory hash mismatch",
                                    di, child);
                                if (repair_mode) {
                                        stage_dir_patch(&dir_patches,
                                            &dir_patch_count, &dir_patch_capacity,
                                            di, off + 4U, want_meta);
                                        ++planned;
                                        changed_dirs = 1;
                                }
                        }
                        ++refs[child];
                        if (ref_parent[child] == ~0U)
                                ref_parent[child] = di;
                        else if (ref_parent[child] != di)
                                ambiguous = 1;
                        if (!reachable[child]) {
                                reachable[child] = 1;
                                if (qt < s->fcb_count)
                                        queue[qt++] = child;
                        }
                }
        }

        for (i = 0; i < s->fcb_count; ++i) {
                uint64_t *f;
                if (infos[i].type == D6FS_TYPE_FREE)
                        continue;
                if (i == s->root) {
                        if (refs[i] != 0) {
                                problem("root referenced by directory", i, refs[i]);
                                ambiguous = 1;
                        }
                } else if (refs[i] != 1) {
                        problem("FCB reference count", i, refs[i]);
                        if (refs[i] > 1)
                                ambiguous = 1;
                }
                if (reachable[i] && i != s->root && refs[i] == 1 &&
                    infos[i].parent != ref_parent[i]) {
                        problem("FCB parent mismatch", i, ref_parent[i]);
                        if (repair_mode) {
                                f = fcbs + (size_t)i * FCB_WORDS;
                                f[4] = (uint64_t)ref_parent[i] << 18;
                                infos[i].parent = ref_parent[i];
                                changed_fcbs = 1;
                                ++planned;
                        }
                }
                if (!reachable[i]) {
                        unsigned e;
                        int unique;
                        problem("unreachable FCB", i, ~0U);
                        unique = refs[i] == 0;
                        f = fcbs + (size_t)i * FCB_WORDS;
                        for (e = 0; unique && e < infos[i].extents; ++e) {
                                uint64_t run, count;
                                unsigned start, blocks, p;
                                run = f[006 + e];
                                count = ((uint64_t)extent_high(f[005], e) << EXTENT_LOW_BITS) |
                                    (run & EXTENT_LOW_MASK);
                                start = (unsigned)(run >> EXTENT_LOW_BITS);
                                blocks = (unsigned)(count + 1);
                                for (p = start; p < start + blocks; ++p)
                                        if (p >= total || crosslinked[p] || used[p] != i + 2U) {
                                                unique = 0;
                                                break;
                                        }
                        }
                        if (repair_mode && unique) {
                                for (e = 0; e < infos[i].extents; ++e) {
                                        uint64_t run, count;
                                        unsigned start, blocks, p;
                                        run = f[006 + e];
                                        count = ((uint64_t)extent_high(f[005], e) << EXTENT_LOW_BITS) |
                                            (run & EXTENT_LOW_MASK);
                                        start = (unsigned)(run >> EXTENT_LOW_BITS);
                                        blocks = (unsigned)(count + 1);
                                        for (p = start; p < start + blocks; ++p)
                                                used[p] = 0;
                                }
                                memset(f, 0, FCB_WORDS * sizeof(*f));
                                infos[i].type = D6FS_TYPE_FREE;
                                changed_fcbs = 1;
                                ++planned;
                                fprintf(stderr, "d6fsck: reclaimed orphan FCB %o\n", i);
                        } else if (!unique)
                                ambiguous = 1;
                }
        }

        for (i = 0; i < total; ++i) {
                unsigned mbi = i / BITS_PER_MAP_BLOCK;
                unsigned bit = i % BITS_PER_MAP_BLOCK;
                int allocated = get_bit(freemap + (size_t)mbi * BLOCK_WORDS, bit);
                int wanted = used[i] != 0;
                if (wanted != allocated) {
                        problem(wanted ? "referenced block marked free" :
                            "allocated leaked block", i, ~0U);
                        if (repair_mode && !crosslinked[i]) {
                                set_bit(freemap + (size_t)mbi * BLOCK_WORDS, bit, wanted);
                                changed_map = 1;
                                ++planned;
                        }
                }
        }
        {
                uint64_t *rebuilt;
                rebuilt = calloc((size_t)s->summary_blocks * BLOCK_WORDS,
                    sizeof(*rebuilt));
                if (rebuilt == NULL)
                        die("out of memory");
                rebuild_summary(s, freemap, rebuilt);
                if (memcmp(summary, rebuilt,
                    (size_t)s->summary_blocks * BLOCK_WORDS * sizeof(*summary)) != 0) {
                        problem("free-summary mismatch", 0, ~0U);
                        if (repair_mode) {
                                memcpy(summary, rebuilt,
                                    (size_t)s->summary_blocks * BLOCK_WORDS * sizeof(*summary));
                                changed_map = 1;
                                ++planned;
                        }
                }
                free(rebuilt);
        }

        fprintf(stderr, "d6fsck: selected superblock %o seq=%llo state=%s\n",
            selected, (unsigned long long)s->sequence,
            s->state == 0 ? "CLEAN" : "DIRTY");

        if (repair_mode && !ambiguous) {
                int metadata_changed;

                metadata_changed = changed_fcbs || changed_dirs || changed_map;
                if (metadata_changed) {
                        uint64_t dirty_block[BLOCK_WORDS];
                        uint64_t clean_block[BLOCK_WORDS];
                        unsigned dirty_selected;

                        /* Never expose a partially repaired filesystem under
                         * an older CLEAN superblock.  Publish a newer DIRTY
                         * copy before the first metadata write, then publish
                         * an even newer CLEAN copy only after every staged
                         * deterministic repair is durable. */
                        if (publish_next_super(sa, sb, selected, selected_block,
                            1U, &dirty_selected, dirty_block) != 0)
                                die("cannot publish DIRTY state before repair");
                        ++repairs;
                        for (i = 0U; i < dir_patch_count; ++i)
                                if (write_file_word(s, fcbs,
                                    dir_patches[i].dir,
                                    dir_patches[i].offset,
                                    dir_patches[i].value) != 0)
                                        die("cannot write repaired directory metadata");
                        if (changed_fcbs && write_fcb_table(s, fcbs) != 0)
                                die("cannot write repaired FCB table");
                        if (changed_map) {
                                if (write_map_blocks(s->freemap_start,
                                    s->freemap_blocks, freemap) != 0 ||
                                    write_map_blocks(s->summary_start,
                                    s->summary_blocks, summary) != 0)
                                        die("cannot write repaired allocation metadata");
                        }
                        if (publish_next_super(sa, sb, dirty_selected,
                            dirty_block, 0U, NULL, clean_block) != 0)
                                die("cannot publish CLEAN state after repair");
                        ++repairs;
                        repairs += planned;
                } else if (!av || !bv || s->state != 0U) {
                        uint64_t clean_block[BLOCK_WORDS];
                        unsigned clean_selected;

                        if (publish_next_super(sa, sb, selected, selected_block,
                            0U, &clean_selected, clean_block) != 0)
                                die("cannot repair superblock redundancy/state");
                        ++repairs;
                        fprintf(stderr,
                            "d6fsck: repaired superblock copy %o (seq=%llo CLEAN)\n",
                            clean_selected,
                            (unsigned long long)clean_block[1]);
                }
        } else if (repair_mode && ambiguous)
                fprintf(stderr, "d6fsck: ambiguous corruption remains; no repairs written\n");

        if (errors == 0)
                fprintf(stderr, "d6fsck: %u blocks, %u FCBs: clean\n",
                    total, s->fcb_count);

        free_badmap(bad_phys);
        free(dir_patches);
        free(queue); free(ref_parent); free(refs); free(reachable);
        free(crosslinked); free(used); free(summary); free(freemap);
        free(infos); free(fcbs);
        return ambiguous ? -1 : 0;
}

static void usage(void)
{
        fprintf(stderr, "usage: d6fsck [-r] -n members -d diskdir\n");
        exit(2);
}

static void close_members(void)
{
        unsigned i;

        for (i = 0; i < member_count; ++i) {
                if (members[i].fp != NULL)
                        fclose(members[i].fp);
                members[i].fp = NULL;
        }
        member_count = 0;
}

int main(int argc, char **argv)
{
        const char *dir = NULL;
        unsigned n = 0, sa, sb;
        int a, requested_repair;

        requested_repair = 0;
        for (a = 1; a < argc; ++a) {
                if (strcmp(argv[a], "-r") == 0)
                        requested_repair = 1;
                else if (strcmp(argv[a], "-n") == 0 && a + 1 < argc)
                        n = (unsigned)strtoul(argv[++a], NULL, 0);
                else if (strcmp(argv[a], "-d") == 0 && a + 1 < argc)
                        dir = argv[++a];
                else
                        usage();
        }
        if (dir == NULL || n == 0 || n > MAX_MEMBERS)
                usage();

        repair_mode = requested_repair;
        errors = repairs = 0;
        open_members(dir, n, &sa, &sb);
        (void)check_filesystem(sa, sb);
        close_members();

        if (requested_repair && repairs != 0) {
                unsigned repaired = repairs;
                fprintf(stderr, "d6fsck: %u deterministic repairs written; verifying\n",
                    repaired);
                repair_mode = 0;
                errors = repairs = 0;
                open_members(dir, n, &sa, &sb);
                (void)check_filesystem(sa, sb);
                close_members();
                if (errors == 0) {
                        fprintf(stderr, "d6fsck: repair verification: clean\n");
                        return 0;
                }
                fprintf(stderr, "d6fsck: repair verification failed with %u error(s)\n",
                    errors);
                return 1;
        }
        return errors == 0 ? 0 : 1;
}
