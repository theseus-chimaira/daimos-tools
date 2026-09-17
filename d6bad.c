/* d6bad.c - offline physical bad-sector maintenance for D6FS V2 disksets. */

#include "d6maint.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct bad_run {
        unsigned member;
        unsigned start;
        unsigned count;
};

static void die(const char *msg)
{
        fprintf(stderr, "d6bad: %s\n", msg);
        exit(1);
}

static int parse_run(const char *s, struct bad_run *r)
{
        char *copy, *p, *q, *end;
        unsigned long a, b, c;

        copy = malloc(strlen(s) + 1U);
        if (copy == NULL)
                return -1;
        memcpy(copy, s, strlen(s) + 1U);
        p = strchr(copy, ':');
        if (p == NULL) {
                free(copy);
                return -1;
        }
        *p++ = '\0';
        q = strchr(p, ':');
        if (q != NULL)
                *q++ = '\0';
        a = strtoul(copy, &end, 0);
        if (*end != '\0') {
                free(copy);
                return -1;
        }
        b = strtoul(p, &end, 0);
        if (*end != '\0') {
                free(copy);
                return -1;
        }
        c = 1;
        if (q != NULL) {
                c = strtoul(q, &end, 0);
                if (*end != '\0') {
                        free(copy);
                        return -1;
                }
        }
        free(copy);
        if (a > D6M_HALF_MASK || b > D6M_HALF_MASK || c == 0 ||
            c > D6M_HALF_MASK)
                return -1;
        r->member = (unsigned)a;
        r->start = (unsigned)b;
        r->count = (unsigned)c;
        return 0;
}

static uint64_t *load_badmap(struct d6m_set *set, unsigned *countp,
    unsigned *capacityp)
{
        uint64_t *words;
        unsigned b, count, cap;

        if (set->layout.badmap_blocks == 0)
                die("diskset has no reserved bad-block table");
        words = calloc((size_t)set->layout.badmap_blocks * D6M_BLOCK_WORDS,
            sizeof(*words));
        if (words == NULL)
                die("out of memory");
        for (b = 0; b < set->layout.badmap_blocks; ++b)
                if (d6m_read(set, set->layout.badmap_start + b,
                    words + (size_t)b * D6M_BLOCK_WORDS) != 0)
                        die("cannot read bad-block table");
        if (words[0] != D6M_BADMAP_MAGIC)
                die("invalid bad-block table magic");
        cap = (set->layout.badmap_blocks * D6M_BLOCK_WORDS - 4U) / 2U;
        count = (unsigned)words[2];
        if (count > cap)
                die("bad-block table count exceeds reserved capacity");
        *countp = count;
        *capacityp = cap;
        return words;
}

static void decode_run(const uint64_t *words, unsigned i, struct bad_run *r)
{
        uint64_t a = words[4U + i * 2U];
        r->member = (unsigned)((a >> 18) & D6M_HALF_MASK);
        r->start = (unsigned)(a & D6M_HALF_MASK);
        r->count = (unsigned)words[5U + i * 2U];
}

static void encode_run(uint64_t *words, unsigned i, const struct bad_run *r)
{
        words[4U + i * 2U] = ((uint64_t)r->member << 18) | r->start;
        words[5U + i * 2U] = r->count;
}

static int cmp_run(const void *av, const void *bv)
{
        const struct bad_run *a = av, *b = bv;
        if (a->member != b->member)
                return a->member < b->member ? -1 : 1;
        if (a->start != b->start)
                return a->start < b->start ? -1 : 1;
        return 0;
}

static unsigned merge_runs(struct bad_run *runs, unsigned n)
{
        unsigned in, out;

        if (n == 0)
                return 0;
        qsort(runs, n, sizeof(*runs), cmp_run);
        out = 0;
        for (in = 1; in < n; ++in) {
                unsigned end = runs[out].start + runs[out].count;
                if (runs[out].member == runs[in].member && runs[in].start <= end) {
                        unsigned nend = runs[in].start + runs[in].count;
                        if (nend > end)
                                runs[out].count = nend - runs[out].start;
                } else
                        runs[++out] = runs[in];
        }
        return out + 1U;
}

static int write_badmap(struct d6m_set *set, uint64_t *words,
    const struct bad_run *runs, unsigned count)
{
        unsigned b, i;

        memset(words + 4, 0,
            ((size_t)set->layout.badmap_blocks * D6M_BLOCK_WORDS - 4U) *
            sizeof(*words));
        words[0] = D6M_BADMAP_MAGIC;
        words[1] = (words[1] + 1U) & D6M_WORD_MASK;
        words[2] = count;
        words[3] = 0;
        for (i = 0; i < count; ++i)
                encode_run(words, i, &runs[i]);
        for (b = 0; b < set->layout.badmap_blocks; ++b)
                if (d6m_write(set, set->layout.badmap_start + b,
                    words + (size_t)b * D6M_BLOCK_WORDS) != 0)
                        return -1;
        return 0;
}

static void list_runs(const struct d6m_set *set, const uint64_t *words,
    unsigned count)
{
        unsigned i, p;
        struct bad_run r;

        printf("badmap seq=%llo entries=%u capacity=%u\n",
            (unsigned long long)words[1], count,
            (set->layout.badmap_blocks * D6M_BLOCK_WORDS - 4U) / 2U);
        for (i = 0; i < count; ++i) {
                decode_run(words, i, &r);
                printf("member=%u physical=%o count=%o logical=", r.member,
                    r.start, r.count);
                for (p = 0; p < r.count; ++p) {
                        unsigned logical;
                        if (r.member < set->members &&
                            r.start + p >= set->member[r.member].base &&
                            r.start + p < set->member[r.member].base +
                            set->member[r.member].blocks &&
                            d6m_inverse_map(set, r.member,
                            r.start + p - set->member[r.member].base,
                            &logical) == 0)
                                printf("%s%o", p == 0 ? "" : ",", logical);
                        else
                                printf("%s-", p == 0 ? "" : ",");
                }
                putchar('\n');
        }
}

static void load_maps(struct d6m_set *set, const struct d6m_super *s,
    uint64_t **freemapp, uint64_t **summaryp)
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
        for (b = 0; b < s->summary_blocks; ++b)
                if (d6m_read(set, s->summary_start + b,
                    sum + (size_t)b * D6M_BLOCK_WORDS) != 0)
                        die("cannot read free summary");
        *freemapp = fm;
        *summaryp = sum;
}

static int write_maps(struct d6m_set *set, const struct d6m_super *s,
    const uint64_t *fm, const uint64_t *sum)
{
        unsigned b;
        for (b = 0; b < s->freemap_blocks; ++b)
                if (d6m_write(set, s->freemap_start + b,
                    fm + (size_t)b * D6M_BLOCK_WORDS) != 0)
                        return -1;
        for (b = 0; b < s->summary_blocks; ++b)
                if (d6m_write(set, s->summary_start + b,
                    sum + (size_t)b * D6M_BLOCK_WORDS) != 0)
                        return -1;
        return 0;
}

static void usage(void)
{
        fprintf(stderr,
            "usage: d6bad -n members -d diskdir --list\n"
            "       d6bad -n members -d diskdir --add member:sector[:count]\n");
        exit(2);
}

int main(int argc, char **argv)
{
        const char *dir = NULL, *add = NULL;
        unsigned n = 0, selected, count, old_count, capacity, i, add_logical_count;
        int list = 0, a;
        char err[256];
        struct d6m_set set;
        struct d6m_super super;
        uint64_t super_block[D6M_BLOCK_WORDS], dirty_block[D6M_BLOCK_WORDS];
        uint64_t *words, *fm, *sum;
        struct d6m_super dirty_super;
        unsigned dirty_selected;
        struct bad_run nr, *runs;

        for (a = 1; a < argc; ++a) {
                if (strcmp(argv[a], "-n") == 0 && a + 1 < argc)
                        n = (unsigned)strtoul(argv[++a], NULL, 0);
                else if (strcmp(argv[a], "-d") == 0 && a + 1 < argc)
                        dir = argv[++a];
                else if (strcmp(argv[a], "--list") == 0)
                        list = 1;
                else if (strcmp(argv[a], "--add") == 0 && a + 1 < argc)
                        add = argv[++a];
                else
                        usage();
        }
        if (dir == NULL || n == 0 || n > D6M_MAX_MEMBERS ||
            ((list != 0) == (add != NULL)))
                usage();

        if (d6m_open(&set, dir, n, add != NULL, err, sizeof(err)) != 0)
                die(err);
        words = load_badmap(&set, &count, &capacity);
        if (list) {
                list_runs(&set, words, count);
                free(words);
                d6m_close(&set);
                return 0;
        }
        if (parse_run(add, &nr) != 0 || nr.member >= set.members ||
            nr.start < set.member[nr.member].base ||
            nr.start >= set.member[nr.member].base + set.member[nr.member].blocks ||
            nr.count > set.member[nr.member].base + set.member[nr.member].blocks -
            nr.start)
                die("bad run must lie wholly inside one member's D6FS physical range");
        if (d6m_run_fsck(argv[0], dir, n, 0) != 0)
                die("pre-maintenance d6fsck failed");
        if (d6m_select_super(&set, &super, &selected, super_block,
            err, sizeof(err)) != 0)
                die(err);
        load_maps(&set, &super, &fm, &sum);

        /* Existing entries make --add idempotent. */
        old_count = count;
        runs = calloc(capacity + 1U, sizeof(*runs));
        if (runs == NULL)
                die("out of memory");
        for (i = 0; i < count; ++i)
                decode_run(words, i, &runs[i]);
        runs[count++] = nr;
        count = merge_runs(runs, count);
        if (count > capacity)
                die("bad-block table is full");

        /* Refuse to condemn blocks that are currently allocated unless they
         * were already in the bad table.  This avoids silently discarding file
         * data; data recovery/relocation must happen first. */
        add_logical_count = 0;
        for (i = 0; i < nr.count; ++i) {
                unsigned logical, mbi, bit, j;
                int already = 0;
                struct bad_run er;
                if (d6m_inverse_map(&set, nr.member,
                    nr.start + i - set.member[nr.member].base, &logical) != 0)
                        die("cannot invert unequal-stripe mapping");
                for (j = 0; j < old_count; ++j) {
                        decode_run(words, j, &er);
                        if (er.member == nr.member && nr.start + i >= er.start &&
                            nr.start + i < er.start + er.count) {
                                already = 1;
                                break;
                        }
                }
                mbi = logical / D6M_BITS_PER_MAP_BLOCK;
                bit = logical % D6M_BITS_PER_MAP_BLOCK;
                if (!already && d6m_get_bit(fm +
                    (size_t)mbi * D6M_BLOCK_WORDS, bit))
                        die("new bad sector maps to an allocated block; recover or relocate it first");
                ++add_logical_count;
        }

        /* Publish physical failure knowledge first.  If power is lost before
         * the free map update, d6fsck -r deterministically marks these blocks
         * allocated from the bad table. */
        if (d6m_begin_dirty(&set, &super, selected, super_block, &dirty_super,
            &dirty_selected, dirty_block) != 0)
                die("cannot publish DIRTY state before bad-block maintenance");
        if (write_badmap(&set, words, runs, count) != 0)
                die("cannot write bad-block table");
        for (i = 0; i < nr.count; ++i) {
                unsigned logical, mbi, bit;
                if (d6m_inverse_map(&set, nr.member,
                    nr.start + i - set.member[nr.member].base, &logical) != 0)
                        die("cannot invert unequal-stripe mapping");
                mbi = logical / D6M_BITS_PER_MAP_BLOCK;
                bit = logical % D6M_BITS_PER_MAP_BLOCK;
                d6m_set_bit(fm + (size_t)mbi * D6M_BLOCK_WORDS, bit, 1);
        }
        d6m_rebuild_summary(&super, fm, sum);
        if (write_maps(&set, &super, fm, sum) != 0)
                die("cannot write bad-block allocation exclusions");
        if (d6m_publish_clean(&set, &dirty_super, dirty_selected, dirty_block,
            NULL) != 0)
                die("cannot publish CLEAN superblock after bad-block update");
        d6m_close(&set);
        free(runs); free(sum); free(fm); free(words);
        if (d6m_run_fsck(argv[0], dir, n, 0) != 0)
                die("post-maintenance d6fsck failed");
        fprintf(stderr, "d6bad: recorded %u physical sector(s); filesystem clean\n",
            add_logical_count);
        return 0;
}
