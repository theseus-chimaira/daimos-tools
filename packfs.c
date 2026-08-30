/* packfs.c - offline D6FS V2 extent compactor. */

#include "d6maint.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

struct image {
        long spool_off;
        unsigned blocks;
        unsigned old_extents;
        unsigned new_extents;
        unsigned start[D6M_EXTENTS];
        unsigned count[D6M_EXTENTS];
};

static void die(const char *msg)
{
        fprintf(stderr, "packfs: %s\n", msg);
        exit(1);
}

static unsigned fcb_type(const uint64_t *f)
{
        return (unsigned)((f[0] >> 33) & 07U);
}

static unsigned fcb_extents(const uint64_t *f)
{
        return (unsigned)((f[0] >> 4) & 017U);
}

static uint64_t fcb_size(const uint64_t *f)
{
        return f[2];
}

static int spool_blocks(struct d6m_set *set, const uint64_t *f,
    struct image *im, FILE *spool)
{
        uint64_t block[D6M_BLOCK_WORDS];
        unsigned e, out;

        im->old_extents = fcb_extents(f);
        im->blocks = (unsigned)((fcb_size(f) + D6M_BLOCK_WORDS - 1U) /
            D6M_BLOCK_WORDS);
        im->spool_off = ftell(spool);
        if (im->spool_off < 0)
                return -1;
        if (im->blocks == 0)
                return 0;
        out = 0;
        for (e = 0; e < im->old_extents && out < im->blocks; ++e) {
                unsigned start, blocks, b;
                if (d6m_extent_decode(f, e, &start, &blocks) != 0)
                        return -1;
                for (b = 0; b < blocks && out < im->blocks; ++b, ++out) {
                        if (d6m_read(set, start + b, block) != 0 ||
                            fwrite(block, sizeof(*block), D6M_BLOCK_WORDS,
                            spool) != D6M_BLOCK_WORDS)
                                return -1;
                }
        }
        return out == im->blocks ? 0 : -1;
}

static void mark_fcb_blocks(unsigned char *owner, unsigned total,
    const uint64_t *f, unsigned tag)
{
        unsigned e, ne;

        ne = fcb_extents(f);
        for (e = 0; e < ne; ++e) {
                unsigned start, blocks, b;
                if (d6m_extent_decode(f, e, &start, &blocks) != 0)
                        die("invalid extent after pre-check");
                if (start >= total || blocks > total - start)
                        die("extent outside filesystem after pre-check");
                for (b = 0; b < blocks; ++b)
                        owner[start + b] = (unsigned char)tag;
        }
}

static unsigned find_run(const unsigned char *reserved,
    const unsigned char *newused, unsigned total, unsigned from, unsigned need)
{
        unsigned p, start, len;

        start = len = 0;
        for (p = from; p < total; ++p) {
                if (reserved[p] || newused[p]) {
                        len = 0;
                        continue;
                }
                if (len == 0)
                        start = p;
                ++len;
                if (len >= need)
                        return start;
        }
        return ~0U;
}

static int allocate_image(struct image *im, const unsigned char *reserved,
    unsigned char *newused, unsigned total, unsigned data_start,
    unsigned *cursorp)
{
        unsigned remain, cursor, start, p, len;

        im->new_extents = 0;
        if (im->blocks == 0)
                return 0;
        cursor = *cursorp < data_start ? data_start : *cursorp;

        /* Prefer one contiguous run, preserving monotonically packed order. */
        start = find_run(reserved, newused, total, cursor, im->blocks);
        if (start != ~0U) {
                im->start[0] = start;
                im->count[0] = im->blocks;
                im->new_extents = 1;
                for (p = 0; p < im->blocks; ++p)
                        newused[start + p] = 1;
                *cursorp = start + im->blocks;
                return 0;
        }

        remain = im->blocks;
        p = cursor;
        while (remain != 0 && p < total) {
                while (p < total && (reserved[p] || newused[p]))
                        ++p;
                if (p >= total)
                        break;
                start = p;
                len = 0;
                while (p < total && !reserved[p] && !newused[p] && len < remain) {
                        ++p;
                        ++len;
                }
                if (len == 0)
                        continue;
                if (im->new_extents >= D6M_EXTENTS)
                        return -1;
                im->start[im->new_extents] = start;
                im->count[im->new_extents] = len;
                ++im->new_extents;
                while (len-- != 0)
                        newused[start++] = 1;
                remain -= im->count[im->new_extents - 1U];
        }
        if (remain != 0)
                return -1;
        *cursorp = p;
        return 0;
}

static int write_image(struct d6m_set *set, const struct image *im,
    FILE *spool)
{
        uint64_t block[D6M_BLOCK_WORDS];
        unsigned e, b, in;

        if (fseek(spool, im->spool_off, SEEK_SET) != 0)
                return -1;
        in = 0;
        for (e = 0; e < im->new_extents; ++e)
                for (b = 0; b < im->count[e]; ++b, ++in) {
                        if (fread(block, sizeof(*block), D6M_BLOCK_WORDS,
                            spool) != D6M_BLOCK_WORDS ||
                            d6m_write(set, im->start[e] + b, block) != 0)
                                return -1;
                }
        return in == im->blocks ? 0 : -1;
}

static void encode_image(uint64_t *f, const struct image *im)
{
        unsigned e;

        f[0] &= ~((uint64_t)017U << 4);
        f[0] |= (uint64_t)im->new_extents << 4;
        f[005] = 0;
        for (e = 0; e < D6M_EXTENTS; ++e)
                f[006 + e] = 0;
        for (e = 0; e < im->new_extents; ++e)
                if (d6m_extent_set(f, e, im->start[e], im->count[e]) != 0)
                        die("new extent cannot be encoded");
}

static int write_fcbs(struct d6m_set *set, const struct d6m_super *s,
    const uint64_t *fcbs)
{
        unsigned blocks, b;

        blocks = (s->fcb_count * D6M_FCB_WORDS + D6M_BLOCK_WORDS - 1U) /
            D6M_BLOCK_WORDS;
        for (b = 0; b < blocks; ++b) {
                uint64_t block[D6M_BLOCK_WORDS];
                unsigned words;
                memset(block, 0, sizeof(block));
                words = D6M_BLOCK_WORDS;
                if ((b + 1U) * D6M_BLOCK_WORDS > s->fcb_count * D6M_FCB_WORDS)
                        words = s->fcb_count * D6M_FCB_WORDS -
                            b * D6M_BLOCK_WORDS;
                memcpy(block, fcbs + (size_t)b * D6M_BLOCK_WORDS,
                    words * sizeof(*block));
                if (d6m_write(set, s->fcb_start + b, block) != 0)
                        return -1;
        }
        return 0;
}

static int write_maps(struct d6m_set *set, const struct d6m_super *s,
    const uint64_t *freemap, const uint64_t *summary)
{
        unsigned b;
        for (b = 0; b < s->freemap_blocks; ++b)
                if (d6m_write(set, s->freemap_start + b,
                    freemap + (size_t)b * D6M_BLOCK_WORDS) != 0)
                        return -1;
        for (b = 0; b < s->summary_blocks; ++b)
                if (d6m_write(set, s->summary_start + b,
                    summary + (size_t)b * D6M_BLOCK_WORDS) != 0)
                        return -1;
        return 0;
}

static void usage(void)
{
        fprintf(stderr,
            "usage: packfs -n members -d diskdir --output newdiskdir "
            "[--reserve-low block] [--limit-total blocks]\n"
            "       packfs -n members -d private-diskdir --in-place "
            "[--reserve-low block] [--limit-total blocks]\n");
        exit(2);
}

int main(int argc, char **argv)
{
        const char *dir = NULL, *output = NULL;
        unsigned n = 0, i, b, selected, total, fcb_blocks, data_start, cursor;
        unsigned reserve_low = 0, limit_total = 0, alloc_total;
        unsigned before_extents = 0, after_extents = 0, moved = 0;
        int a, in_place = 0;
        char err[256];
        struct d6m_set set;
        struct d6m_super super;
        uint64_t super_block[D6M_BLOCK_WORDS];
        uint64_t dirty_block[D6M_BLOCK_WORDS];
        struct d6m_super dirty_super;
        unsigned dirty_selected;
        uint64_t *fcbs, *freemap, *summary;
        unsigned char *owner, *reserved, *newused;
        struct image *images;
        FILE *spool;
        const char *tmpdir;
        char spool_path[1024];

        for (a = 1; a < argc; ++a) {
                if (strcmp(argv[a], "-n") == 0 && a + 1 < argc)
                        n = (unsigned)strtoul(argv[++a], NULL, 0);
                else if (strcmp(argv[a], "-d") == 0 && a + 1 < argc)
                        dir = argv[++a];
                else if (strcmp(argv[a], "--output") == 0 && a + 1 < argc)
                        output = argv[++a];
                else if (strcmp(argv[a], "--in-place") == 0)
                        in_place = 1;
                else if (strcmp(argv[a], "--reserve-low") == 0 && a + 1 < argc)
                        reserve_low = (unsigned)strtoul(argv[++a], NULL, 0);
                else if (strcmp(argv[a], "--limit-total") == 0 && a + 1 < argc)
                        limit_total = (unsigned)strtoul(argv[++a], NULL, 0);
                else
                        usage();
        }
        if (dir == NULL || n == 0 || n > D6M_MAX_MEMBERS ||
            (output == NULL) == !in_place)
                usage();
        if (output != NULL) {
                if (d6m_clone_diskset(dir, output, n, err, sizeof(err)) != 0)
                        die(err);
                dir = output;
        }
        tmpdir = getenv("TMPDIR");
        if (tmpdir == NULL || *tmpdir == '\0')
                die("TMPDIR must be set for disk-backed compaction staging");
        if (snprintf(spool_path, sizeof(spool_path),
            "%s/packfs-data-v2-%ld.bin", tmpdir, (long)getpid()) >=
            (int)sizeof(spool_path))
                die("TMPDIR path too long");
        spool = fopen(spool_path, "w+b");
        if (spool == NULL)
                die("cannot create compaction staging file");

        if (d6m_run_fsck(argv[0], dir, n, 0) != 0)
                die("pre-pack d6fsck failed; repair the filesystem first");
        if (d6m_open(&set, dir, n, 1, err, sizeof(err)) != 0)
                die(err);
        if (d6m_select_super(&set, &super, &selected, super_block,
            err, sizeof(err)) != 0)
                die(err);
        total = super.total;
        fcb_blocks = (super.fcb_count * D6M_FCB_WORDS + D6M_BLOCK_WORDS - 1U) /
            D6M_BLOCK_WORDS;
        data_start = super.summary_start + super.summary_blocks;
        if (reserve_low > data_start)
                data_start = reserve_low;
        alloc_total = limit_total != 0U ? limit_total : total;
        if (data_start >= alloc_total || alloc_total > total)
                die("invalid compaction reservation/limit");
        fcbs = calloc((size_t)super.fcb_count * D6M_FCB_WORDS, sizeof(*fcbs));
        freemap = calloc((size_t)super.freemap_blocks * D6M_BLOCK_WORDS,
            sizeof(*freemap));
        summary = calloc((size_t)super.summary_blocks * D6M_BLOCK_WORDS,
            sizeof(*summary));
        owner = calloc(total, 1);
        reserved = calloc(total, 1);
        newused = calloc(total, 1);
        images = calloc(super.fcb_count, sizeof(*images));
        if (fcbs == NULL || freemap == NULL || summary == NULL || owner == NULL ||
            reserved == NULL || newused == NULL || images == NULL)
                die("out of memory");

        for (b = 0; b < fcb_blocks; ++b) {
                uint64_t block[D6M_BLOCK_WORDS];
                unsigned words = D6M_BLOCK_WORDS;
                if (d6m_read(&set, super.fcb_start + b, block) != 0)
                        die("cannot read FCB table");
                if ((b + 1U) * D6M_BLOCK_WORDS > super.fcb_count * D6M_FCB_WORDS)
                        words = super.fcb_count * D6M_FCB_WORDS - b * D6M_BLOCK_WORDS;
                memcpy(fcbs + (size_t)b * D6M_BLOCK_WORDS, block,
                    words * sizeof(*block));
        }
        for (b = 0; b < super.freemap_blocks; ++b)
                if (d6m_read(&set, super.freemap_start + b,
                    freemap + (size_t)b * D6M_BLOCK_WORDS) != 0)
                        die("cannot read free map");

        for (i = 0; i <= (set.layout.super_a > set.layout.super_b ?
            set.layout.super_a : set.layout.super_b) && i < total; ++i)
                owner[i] = 1;
        for (i = super.fcb_start; i < super.fcb_start + fcb_blocks; ++i)
                owner[i] = 1;
        for (i = super.freemap_start;
            i < super.freemap_start + super.freemap_blocks; ++i)
                owner[i] = 1;
        for (i = super.summary_start;
            i < super.summary_start + super.summary_blocks; ++i)
                owner[i] = 1;

        for (i = 0; i < super.fcb_count; ++i) {
                uint64_t *f = fcbs + (size_t)i * D6M_FCB_WORDS;
                unsigned type = fcb_type(f);
                if (type == D6M_TYPE_FREE)
                        continue;
                if (type > D6M_TYPE_SYMLINK)
                        die("invalid FCB type after pre-check");
                mark_fcb_blocks(owner, total, f, i + 2U);
                if (spool_blocks(&set, f, &images[i], spool) != 0)
                        die("cannot stage file contents");
                before_extents += images[i].old_extents;
        }

        /* Any allocated block not owned by an FCB is maintenance-reserved
         * (normally a physical-bad-sector exclusion).  Preserve it. */
        for (i = 0; i < total; ++i) {
                unsigned mbi = i / D6M_BITS_PER_MAP_BLOCK;
                unsigned bit = i % D6M_BITS_PER_MAP_BLOCK;
                int allocated = d6m_get_bit(freemap +
                    (size_t)mbi * D6M_BLOCK_WORDS, bit);
                if (owner[i] == 1 || (owner[i] == 0 && allocated))
                        reserved[i] = 1;
        }

        cursor = data_start;
        for (i = 0; i < super.fcb_count; ++i) {
                uint64_t *f = fcbs + (size_t)i * D6M_FCB_WORDS;
                unsigned type = fcb_type(f), old_start = ~0U;
                if (type == D6M_TYPE_FREE)
                        continue;
                if (images[i].old_extents != 0)
                        (void)d6m_extent_decode(f, 0, &old_start, &b);
                if (allocate_image(&images[i], reserved, newused, alloc_total,
                    data_start, &cursor) != 0)
                        die("cannot compact within seven extents per file");
                if (images[i].new_extents != 0 && old_start != images[i].start[0])
                        moved += images[i].blocks;
                after_extents += images[i].new_extents;
        }

        if (d6m_begin_dirty(&set, &super, selected, super_block, &dirty_super,
            &dirty_selected, dirty_block) != 0)
                die("cannot publish DIRTY state before compaction");
        for (i = 0; i < super.fcb_count; ++i) {
                uint64_t *f = fcbs + (size_t)i * D6M_FCB_WORDS;
                if (fcb_type(f) == D6M_TYPE_FREE)
                        continue;
                if (write_image(&set, &images[i], spool) != 0)
                        die("cannot write compacted file data");
                encode_image(f, &images[i]);
        }

        memset(freemap, 0, (size_t)super.freemap_blocks * D6M_BLOCK_WORDS *
            sizeof(*freemap));
        for (i = 0; i < total; ++i)
                if (reserved[i] || newused[i]) {
                        unsigned mbi = i / D6M_BITS_PER_MAP_BLOCK;
                        unsigned bit = i % D6M_BITS_PER_MAP_BLOCK;
                        d6m_set_bit(freemap + (size_t)mbi * D6M_BLOCK_WORDS,
                            bit, 1);
                }
        d6m_rebuild_summary(&super, freemap, summary);
        if (write_fcbs(&set, &super, fcbs) != 0 ||
            write_maps(&set, &super, freemap, summary) != 0)
                die("cannot write compacted metadata");
        if (d6m_publish_clean(&set, &dirty_super, dirty_selected, dirty_block,
            NULL) != 0)
                die("cannot publish CLEAN compacted superblock");
        d6m_close(&set);

        if (fclose(spool) != 0)
                die("cannot close compaction staging file");
        (void)unlink(spool_path);
        free(images); free(newused); free(reserved); free(owner);
        free(summary); free(freemap); free(fcbs);

        if (d6m_run_fsck(argv[0], dir, n, 0) != 0)
                die("post-pack d6fsck failed");
        fprintf(stderr,
            "packfs: clean; extents %u -> %u, moved %u block(s)\n",
            before_extents, after_extents, moved);
        return 0;
}
