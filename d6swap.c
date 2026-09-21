#define _POSIX_C_SOURCE 200809L
/* d6swap.c - plan, inspect and safely resize equal per-member swap tails. */

#include "d6maint.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

static void die(const char *msg)
{
        fprintf(stderr, "d6swap: %s\n", msg);
        exit(1);
}


static unsigned tail_for_ram(uint64_t ram_words, unsigned members)
{
        uint64_t denom, blocks;

        if (ram_words == 0 || members == 0)
                return 0;
        denom = (uint64_t)D6M_BLOCK_WORDS * members;
        blocks = (ram_words + denom - 1U) / denom;
        if (blocks > D6M_HALF_MASK)
                return 0;
        return (unsigned)blocks;
}

static void show(const struct d6m_set *set)
{
        unsigned i;
        uint64_t total_words;

        total_words = (uint64_t)set->layout.swap_tail_blocks * set->members *
            D6M_BLOCK_WORDS;
        printf("members=%u tail-blocks/member=%o total-swap-blocks=%o total-swap-words=%llo\n",
            set->members, set->layout.swap_tail_blocks,
            set->layout.swap_tail_blocks * set->members,
            (unsigned long long)total_words);
        for (i = 0; i < set->members; ++i)
                printf("member=%u swap-physical=%o..%o d6fs-base=%o d6fs-blocks=%o\n",
                    i, set->member[i].sectors - set->layout.swap_tail_blocks,
                    set->member[i].sectors - 1U, set->member[i].base,
                    set->member[i].blocks);
}

static void write_word(FILE *fp, uint64_t v)
{
        unsigned char raw[8];
        unsigned i;
        for (i = 0; i < 8; ++i)
                raw[i] = (unsigned char)((v >> (i * 8U)) & 0377U);
        if (fwrite(raw, 1, 8, fp) != 8)
                die("cannot write temporary logical image");
}

static uint64_t read_word(FILE *fp)
{
        unsigned char raw[8];
        uint64_t v = 0;
        unsigned i;
        if (fread(raw, 1, 8, fp) != 8)
                die("cannot read temporary logical image");
        for (i = 0; i < 8; ++i)
                v |= (uint64_t)raw[i] << (i * 8U);
        return v & D6M_WORD_MASK;
}

static void save_logical_image(struct d6m_set *set, FILE *tmp, unsigned total)
{
        uint64_t block[D6M_BLOCK_WORDS];
        unsigned l, w;
        for (l = 0; l < total; ++l) {
                if (d6m_read(set, l, block) != 0)
                        die("cannot snapshot logical diskset before swap resize");
                for (w = 0; w < D6M_BLOCK_WORDS; ++w)
                        write_word(tmp, block[w]);
        }
        if (fflush(tmp) != 0)
                die("cannot flush temporary logical image");
        rewind(tmp);
}

static void update_descriptor(struct d6m_set *set, unsigned mi,
    unsigned new_blocks, unsigned new_tail)
{
        uint64_t block[D6M_BLOCK_WORDS];
        if (d6m_read_phys(set, mi, set->member[mi].descriptor, block) != 0)
                die("cannot read DBOOT descriptor for swap resize");
        block[D6M_LAYOUT_RANGE_WORD] =
            ((uint64_t)set->member[mi].base << 18) | new_blocks;
        block[D6M_LAYOUT_SWAP_TAIL] = new_tail;
        if (d6m_write_phys(set, mi, set->member[mi].descriptor, block) != 0)
                die("cannot update DBOOT descriptor for swap resize");
}

static void restore_logical_image(struct d6m_set *set, FILE *tmp,
    unsigned old_total, unsigned new_total)
{
        uint64_t block[D6M_BLOCK_WORDS];
        unsigned l, w, copy;

        copy = old_total < new_total ? old_total : new_total;
        rewind(tmp);
        for (l = 0; l < copy; ++l) {
                for (w = 0; w < D6M_BLOCK_WORDS; ++w)
                        block[w] = read_word(tmp);
                if (d6m_write(set, l, block) != 0)
                        die("cannot restore logical diskset after swap resize");
        }
        memset(block, 0, sizeof(block));
        for (l = copy; l < new_total; ++l)
                if (d6m_write(set, l, block) != 0)
                        die("cannot initialize enlarged D6FS logical range");
}

static void load_freemap(struct d6m_set *set, const struct d6m_super *s,
    uint64_t **fmp, uint64_t **sump)
{
        uint64_t *fm, *sum;
        unsigned b;
        fm = calloc((size_t)s->freemap_blocks * D6M_BLOCK_WORDS, sizeof(*fm));
        sum = calloc((size_t)s->summary_blocks * D6M_BLOCK_WORDS, sizeof(*sum));
        if (fm == NULL || sum == NULL)
                die("out of memory");
        for (b = 0; b < s->freemap_blocks; ++b)
                if (d6m_read(set, s->freemap_start + b,
                    fm + (size_t)b * D6M_BLOCK_WORDS) != 0)
                        die("cannot read free map");
        *fmp = fm;
        *sump = sum;
}

static void resize(const char *argv0, const char *dir, unsigned n,
    unsigned new_tail)
{
        struct d6m_set oldset, newset;
        struct d6m_super super;
        uint64_t selected_block[D6M_BLOCK_WORDS], *fm, *sum;
        uint64_t block[D6M_BLOCK_WORDS], raw[D6M_SUPER_WORDS];
        unsigned selected, old_total, new_total, i, b, new_map_blocks;
        unsigned old_tail;
        unsigned new_blocks[D6M_MAX_MEMBERS];
        char err[256], tmppath[1024];
        const char *tmpdir;
        FILE *tmp;

        if (d6m_run_fsck(argv0, dir, n, 0) != 0)
                die("pre-resize d6fsck failed");
        /* Compact first so a shrink has the best chance of leaving the high
         * logical tail free. */
        {
                char tool[1024], nbuf[32];
                const char *slash = strrchr(argv0, '/');
                int rc;
                if (slash != NULL) {
                        size_t prefix = (size_t)(slash - argv0 + 1);
                        if (prefix + strlen("packfs") + 1U > sizeof(tool))
                                die("tool path too long");
                        memcpy(tool, argv0, prefix);
                        strcpy(tool + prefix, "packfs");
                } else
                        strcpy(tool, "packfs");
                (void)snprintf(nbuf, sizeof(nbuf), "%u", n);
                {
                        pid_t pid = fork();
                        int status;
                        if (pid < 0)
                                die("cannot fork packfs");
                        if (pid == 0) {
                                execl(tool, tool, "-n", nbuf, "-d", dir,
                                    "--in-place", (char *)NULL);
                                _exit(127);
                        }
                        if (waitpid(pid, &status, 0) != pid ||
                            !WIFEXITED(status) || (rc = WEXITSTATUS(status)) != 0)
                                die("packfs failed before swap resize");
                }
        }
        if (d6m_open(&oldset, dir, n, 1, err, sizeof(err)) != 0)
                die(err);
        if (d6m_select_super(&oldset, &super, &selected, selected_block,
            err, sizeof(err)) != 0)
                die(err);
        old_total = super.total;
        old_tail = oldset.layout.swap_tail_blocks;
        new_total = 0;
        for (i = 0; i < n; ++i) {
                if (new_tail >= oldset.member[i].sectors - oldset.member[i].base)
                        die("requested swap tail leaves no D6FS blocks on a member");
                new_blocks[i] = oldset.member[i].sectors -
                    oldset.member[i].base - new_tail;
                new_total += new_blocks[i];
        }
        new_map_blocks = (new_total + D6M_BITS_PER_MAP_BLOCK - 1U) /
            D6M_BITS_PER_MAP_BLOCK;
        if (new_map_blocks != super.freemap_blocks)
                die("resize crosses free-map capacity boundary; reformat with the planned swap size");
        load_freemap(&oldset, &super, &fm, &sum);
        if (new_total < old_total)
                for (i = new_total; i < old_total; ++i) {
                        unsigned mbi = i / D6M_BITS_PER_MAP_BLOCK;
                        unsigned bit = i % D6M_BITS_PER_MAP_BLOCK;
                        if (d6m_get_bit(fm + (size_t)mbi * D6M_BLOCK_WORDS, bit))
                                die("cannot shrink: allocated logical blocks remain in the removed tail");
                }

        tmpdir = getenv("TMPDIR");
        if (tmpdir == NULL || *tmpdir == '\0')
                die("TMPDIR must be set for safe swap resize");
        if (snprintf(tmppath, sizeof(tmppath), "%s/d6swap-resize-v2-%ld.img",
            tmpdir, (long)getpid()) >= (int)sizeof(tmppath))
                die("TMPDIR path too long");
        tmp = fopen(tmppath, "w+b");
        if (tmp == NULL)
                die(strerror(errno));
        save_logical_image(&oldset, tmp, old_total);

        for (i = 0; i < n; ++i)
                update_descriptor(&oldset, i, new_blocks[i], new_tail);
        d6m_close(&oldset);
        if (d6m_open(&newset, dir, n, 1, err, sizeof(err)) != 0)
                die(err);
        restore_logical_image(&newset, tmp, old_total, new_total);
        fclose(tmp);
        unlink(tmppath);

        /* Update allocation bounds and both superblock copies only after all
         * logical data has been restored through the new stripe mapping. */
        if (new_total > old_total)
                for (i = old_total; i < new_total; ++i) {
                        unsigned mbi = i / D6M_BITS_PER_MAP_BLOCK;
                        unsigned bit = i % D6M_BITS_PER_MAP_BLOCK;
                        d6m_set_bit(fm + (size_t)mbi * D6M_BLOCK_WORDS, bit, 0);
                }
        super.total = new_total;
        super.swap_start = new_tail == 0U ? 0U : new_total;
        super.swap_blocks = new_tail * n;
        super.sequence = (super.sequence + 1U) & D6M_WORD_MASK;
        super.state = D6M_STATE_CLEAN;
        d6m_rebuild_summary(&super, fm, sum);
        for (b = 0; b < super.freemap_blocks; ++b)
                if (d6m_write(&newset, super.freemap_start + b,
                    fm + (size_t)b * D6M_BLOCK_WORDS) != 0)
                        die("cannot update free map after swap resize");
        for (b = 0; b < super.summary_blocks; ++b)
                if (d6m_write(&newset, super.summary_start + b,
                    sum + (size_t)b * D6M_BLOCK_WORDS) != 0)
                        die("cannot update free summary after swap resize");
        if (d6m_super_encode(raw, &super) != 0)
                die("cannot encode resized superblock");
        for (i = 0; i < D6M_BLOCK_WORDS; ++i)
                block[i] = 0;
        for (i = 0; i < D6M_SUPER_WORDS; ++i)
                block[i] = raw[i];
        if (d6m_write(&newset, newset.layout.super_a, block) != 0 ||
            d6m_write(&newset, newset.layout.super_b, block) != 0)
                die("cannot publish resized superblocks");
        d6m_close(&newset);
        free(sum); free(fm);
        if (d6m_run_fsck(argv0, dir, n, 0) != 0)
                die("post-resize d6fsck failed");
        fprintf(stderr,
            "d6swap: resized tail/member %o -> %o blocks; D6FS %o -> %o blocks\n",
            old_tail, new_tail, old_total, new_total);
}


static void migrate_reservation(const char *argv0, const char *dir, unsigned n)
{
        struct d6m_set set;
        struct d6m_super super;
        uint64_t selected_block[D6M_BLOCK_WORDS];
        uint64_t block[D6M_BLOCK_WORDS];
        uint64_t raw[D6M_SUPER_WORDS];
        unsigned selected;
        unsigned tail_blocks;
        unsigned i;
        char err[256];

        if (d6m_run_fsck(argv0, dir, n, 0) != 0)
                die("pre-migration d6fsck failed");
        if (d6m_open(&set, dir, n, 1, err, sizeof(err)) != 0)
                die(err);
        if (d6m_select_super(&set, &super, &selected, selected_block,
            err, sizeof(err)) != 0)
                die(err);
        tail_blocks = set.layout.swap_tail_blocks * set.members;
        if (tail_blocks == 0U) {
                d6m_close(&set);
                die("media has no legacy swap tail to migrate");
        }
        if (super.swap_blocks != 0U) {
                if (super.swap_start != super.total ||
                    super.swap_blocks != tail_blocks) {
                        d6m_close(&set);
                        die("existing D6FS swap reservation disagrees with DBOOT tail");
                }
                d6m_close(&set);
                fprintf(stderr, "d6swap: swap reservation already authoritative\n");
                return;
        }
        if (super.swap_start != 0U) {
                d6m_close(&set);
                die("zero-length swap reservation has nonzero start");
        }
        super.swap_start = super.total;
        super.swap_blocks = tail_blocks;
        super.sequence = (super.sequence + 1U) & D6M_WORD_MASK;
        super.state = D6M_STATE_CLEAN;
        if (d6m_super_encode(raw, &super) != 0) {
                d6m_close(&set);
                die("cannot encode migrated superblock");
        }
        memcpy(block, selected_block, sizeof(block));
        for (i = 0; i < D6M_SUPER_WORDS; ++i)
                block[i] = raw[i];
        if (d6m_write(&set, set.layout.super_a, block) != 0 ||
            d6m_write(&set, set.layout.super_b, block) != 0) {
                d6m_close(&set);
                die("cannot publish migrated swap reservation");
        }
        d6m_close(&set);
        if (d6m_run_fsck(argv0, dir, n, 0) != 0)
                die("post-migration d6fsck failed");
        fprintf(stderr,
            "d6swap: migrated legacy tail to D6FS swap reservation start=%o blocks=%o\n",
            super.swap_start, super.swap_blocks);
}

static void usage(void)
{
        fprintf(stderr,
            "usage: d6swap -n members -d diskdir --show\n"
            "       d6swap -n members -d diskdir --plan --ram-words words\n"
            "       d6swap -n members -d diskdir --resize --ram-words words "
            "--output newdiskdir\n"
            "       d6swap -n members -d diskdir --resize-tail blocks "
            "--output newdiskdir\n"
            "       d6swap -n members -d diskdir --migrate-reservation\n");
        exit(2);
}

int main(int argc, char **argv)
{
        const char *dir = NULL, *output = NULL;
        unsigned n = 0, tail = 0;
        uint64_t ram_words = 0;
        int show_mode = 0, plan = 0, do_resize = 0, migrate = 0, a;
        char err[256];
        struct d6m_set set;

        for (a = 1; a < argc; ++a) {
                if (strcmp(argv[a], "-n") == 0 && a + 1 < argc)
                        n = (unsigned)strtoul(argv[++a], NULL, 0);
                else if (strcmp(argv[a], "-d") == 0 && a + 1 < argc)
                        dir = argv[++a];
                else if (strcmp(argv[a], "--show") == 0)
                        show_mode = 1;
                else if (strcmp(argv[a], "--plan") == 0)
                        plan = 1;
                else if (strcmp(argv[a], "--resize") == 0)
                        do_resize = 1;
                else if (strcmp(argv[a], "--migrate-reservation") == 0)
                        migrate = 1;
                else if (strcmp(argv[a], "--ram-words") == 0 && a + 1 < argc)
                        ram_words = strtoull(argv[++a], NULL, 0);
                else if (strcmp(argv[a], "--resize-tail") == 0 && a + 1 < argc) {
                        do_resize = 1;
                        tail = (unsigned)strtoul(argv[++a], NULL, 0);
                } else if (strcmp(argv[a], "--output") == 0 && a + 1 < argc)
                        output = argv[++a];
                else
                        usage();
        }
        if (dir == NULL || n == 0 || n > D6M_MAX_MEMBERS ||
            show_mode + plan + do_resize + migrate != 1)
                usage();
        if ((plan || (do_resize && tail == 0)) && ram_words == 0)
                usage();
        if ((do_resize && output == NULL) || (!do_resize && output != NULL))
                usage();
        if (migrate && (ram_words != 0 || tail != 0U))
                usage();
        if (ram_words != 0) {
                tail = tail_for_ram(ram_words, n);
                if (tail == 0)
                        die("RAM-based swap plan is outside representable range");
        }
        if (plan) {
                printf("policy=1x-RAM ram-words=%llo members=%u tail-blocks/member=%o "
                    "total-swap-words=%llo\n", (unsigned long long)ram_words, n,
                    tail, (unsigned long long)tail * n * D6M_BLOCK_WORDS);
                return 0;
        }
        if (show_mode) {
                if (d6m_open(&set, dir, n, 0, err, sizeof(err)) != 0)
                        die(err);
                show(&set);
                d6m_close(&set);
                return 0;
        }
        if (migrate) {
                migrate_reservation(argv[0], dir, n);
                return 0;
        }
        if (d6m_clone_diskset(dir, output, n, err, sizeof(err)) != 0)
                die(err);
        resize(argv[0], output, n, tail);
        return 0;
}
