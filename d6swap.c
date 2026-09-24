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

static void show(struct d6m_set *set)
{
        struct d6m_super super;
        uint64_t selected_block[D6M_BLOCK_WORDS];
        unsigned selected, i;
        uint64_t total_words;
        char err[256];

        if (d6m_select_super(set, &super, &selected, selected_block,
            err, sizeof(err)) != 0)
                die(err);
        total_words = (uint64_t)set->layout.swap_tail_blocks * set->members *
            D6M_BLOCK_WORDS;
        printf("members=%u tail-blocks/member=%o total-swap-blocks=%o "
            "total-swap-words=%llo reservation=%o+%o logstore=%o+%o\n",
            set->members, set->layout.swap_tail_blocks,
            set->layout.swap_tail_blocks * set->members,
            (unsigned long long)total_words, super.swap_start,
            super.swap_blocks, super.log_start, super.log_blocks);
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

static int tail_map(const struct d6m_set *set, unsigned tail_blocks,
    unsigned logical, unsigned *memberp, unsigned *sectorp)
{
        unsigned member, rel;

        if (set == NULL || memberp == NULL || sectorp == NULL ||
            tail_blocks == 0U || set->members == 0U ||
            logical >= tail_blocks * set->members)
                return -1;
        member = logical % set->members;
        rel = logical / set->members;
        if (tail_blocks > set->member[member].sectors ||
            rel >= tail_blocks)
                return -1;
        *memberp = member;
        *sectorp = set->member[member].sectors - tail_blocks + rel;
        return 0;
}

static void save_swap_image(struct d6m_set *set, FILE *tmp, unsigned tail_blocks)
{
        uint64_t block[D6M_BLOCK_WORDS];
        unsigned l, w, mi, sector, total;

        total = tail_blocks * set->members;
        for (l = 0U; l < total; ++l) {
                if (tail_map(set, tail_blocks, l, &mi, &sector) != 0 ||
                    d6m_read_phys(set, mi, sector, block) != 0)
                        die("cannot snapshot logical swap tail");
                for (w = 0U; w < D6M_BLOCK_WORDS; ++w)
                        write_word(tmp, block[w]);
        }
        if (fflush(tmp) != 0)
                die("cannot flush temporary swap image");
}

static void restore_swap_image(struct d6m_set *set, FILE *tmp,
    unsigned old_total, unsigned old_tail, unsigned new_tail)
{
        uint64_t block[D6M_BLOCK_WORDS];
        unsigned l, w, mi, sector, copy, new_total;
        long offset;

        copy = old_tail < new_tail ? old_tail : new_tail;
        copy *= set->members;
        new_total = new_tail * set->members;
        offset = (long)((uint64_t)old_total * D6M_BLOCK_WORDS * 8U);
        if (fseek(tmp, offset, SEEK_SET) != 0)
                die("cannot seek temporary swap image");
        for (l = 0U; l < copy; ++l) {
                for (w = 0U; w < D6M_BLOCK_WORDS; ++w)
                        block[w] = read_word(tmp);
                if (tail_map(set, new_tail, l, &mi, &sector) != 0 ||
                    d6m_write_phys(set, mi, sector, block) != 0)
                        die("cannot restore logical swap tail");
        }
        memset(block, 0, sizeof(block));
        for (l = copy; l < new_total; ++l)
                if (tail_map(set, new_tail, l, &mi, &sector) != 0 ||
                    d6m_write_phys(set, mi, sector, block) != 0)
                        die("cannot initialize enlarged swap tail");
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
        struct d6m_super super, dirty_super;
        uint64_t selected_block[D6M_BLOCK_WORDS], dirty_block[D6M_BLOCK_WORDS];
        uint64_t *fm, *sum;
        unsigned selected, dirty_selected, old_total, new_total, i, b, new_map_blocks;
        unsigned old_tail;
        unsigned new_blocks[D6M_MAX_MEMBERS];
        unsigned spare_blocks[D6M_MAX_MEMBERS];
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
                unsigned occupied;

                occupied = oldset.member[i].base + oldset.member[i].blocks +
                    old_tail;
                if (occupied > oldset.member[i].sectors)
                        die("invalid member geometry before swap resize");
                spare_blocks[i] = oldset.member[i].sectors - occupied;
                if (new_tail + spare_blocks[i] >=
                    oldset.member[i].sectors - oldset.member[i].base)
                        die("requested swap tail leaves no D6FS blocks on a member");
                new_blocks[i] = oldset.member[i].sectors -
                    oldset.member[i].base - new_tail - spare_blocks[i];
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
        if (d6m_begin_dirty(&oldset, &super, selected, selected_block,
            &dirty_super, &dirty_selected, dirty_block) != 0)
                die("cannot publish DIRTY state before swap resize");

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
        if (old_tail != 0U) {
                if (fseek(tmp, 0L, SEEK_END) != 0)
                        die("cannot append temporary swap image");
                save_swap_image(&oldset, tmp, old_tail);
        }

        for (i = 0; i < n; ++i)
                update_descriptor(&oldset, i, new_blocks[i], new_tail);
        d6m_close(&oldset);
        if (d6m_open(&newset, dir, n, 1, err, sizeof(err)) != 0)
                die(err);
        restore_logical_image(&newset, tmp, old_total, new_total);
        if (old_tail != 0U || new_tail != 0U)
                restore_swap_image(&newset, tmp, old_total, old_tail, new_tail);
        fclose(tmp);
        unlink(tmppath);

        /* Metadata remains covered by the durable DIRTY generation created
         * before the member descriptors changed.  Update the new logical
         * geometry and reservation, then publish CLEAN only after the free
         * map and summary are durable. */
        if (new_total > old_total)
                for (i = old_total; i < new_total; ++i) {
                        unsigned mbi = i / D6M_BITS_PER_MAP_BLOCK;
                        unsigned bit = i % D6M_BITS_PER_MAP_BLOCK;
                        d6m_set_bit(fm + (size_t)mbi * D6M_BLOCK_WORDS, bit, 0);
                }
        dirty_super.total = new_total;
        dirty_super.swap_start = new_tail == 0U ? 0U : new_total;
        dirty_super.swap_blocks = new_tail * n;
        if (dirty_super.log_blocks != 0U &&
            dirty_super.log_start + dirty_super.log_blocks > new_total)
                die("resize would truncate logstore reservation");
        d6m_rebuild_summary(&dirty_super, fm, sum);
        for (b = 0; b < dirty_super.freemap_blocks; ++b)
                if (d6m_write(&newset, dirty_super.freemap_start + b,
                    fm + (size_t)b * D6M_BLOCK_WORDS) != 0)
                        die("cannot update free map after swap resize");
        for (b = 0; b < dirty_super.summary_blocks; ++b)
                if (d6m_write(&newset, dirty_super.summary_start + b,
                    sum + (size_t)b * D6M_BLOCK_WORDS) != 0)
                        die("cannot update free summary after swap resize");
        if (d6m_publish_clean(&newset, &dirty_super, dirty_selected,
            dirty_block, NULL) != 0)
                die("cannot publish CLEAN state after swap resize");
        d6m_close(&newset);
        free(sum); free(fm);
        if (d6m_run_fsck(argv0, dir, n, 1) != 0 ||
            d6m_run_fsck(argv0, dir, n, 0) != 0)
                die("post-resize d6fsck failed");
        fprintf(stderr,
            "d6swap: resized tail/member %o -> %o blocks; D6FS %o -> %o blocks\n",
            old_tail, new_tail, old_total, new_total);
}

static void migrate_reservation(const char *argv0, const char *dir, unsigned n)
{
        struct d6m_set set;
        struct d6m_super super, dirty_super;
        uint64_t selected_block[D6M_BLOCK_WORDS], dirty_block[D6M_BLOCK_WORDS];
        unsigned selected, dirty_selected, blocks;
        char err[256];

        if (d6m_run_fsck(argv0, dir, n, 0) != 0)
                die("pre-migration d6fsck failed");
        if (d6m_open(&set, dir, n, 1, err, sizeof(err)) != 0)
                die(err);
        if (d6m_select_super(&set, &super, &selected, selected_block,
            err, sizeof(err)) != 0)
                die(err);
        blocks = set.layout.swap_tail_blocks * set.members;
        if (blocks == 0U)
                die("DBOOT has no raw swap tail to migrate");
        if (super.swap_blocks != 0U || super.swap_start != 0U)
                die("D6FS swap reservation already present");
        super.swap_start = super.total;
        super.swap_blocks = blocks;
        if (d6m_begin_dirty(&set, &super, selected, selected_block,
            &dirty_super, &dirty_selected, dirty_block) != 0)
                die("cannot publish DIRTY migration generation");
        if (d6m_publish_clean(&set, &dirty_super, dirty_selected,
            dirty_block, NULL) != 0)
                die("cannot publish CLEAN migration generation");
        d6m_close(&set);
        if (d6m_run_fsck(argv0, dir, n, 0) != 0)
                die("post-migration d6fsck failed");
        fprintf(stderr, "d6swap: migrated DBOOT tail to D6FS reservation %o+%o\n",
            super.total, blocks);
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
        if (migrate) {
                migrate_reservation(argv[0], dir, n);
                return 0;
        }
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
        if (d6m_clone_diskset(dir, output, n, err, sizeof(err)) != 0)
                die(err);
        resize(argv[0], output, n, tail);
        return 0;
}
