/* d6bad.c - offline stable source->spare remap maintenance for D6FS sets. */

#include "d6maint.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct bad_source {
        unsigned member;
        unsigned start;
        unsigned count;
};

static void die(const char *msg)
{
        fprintf(stderr, "d6bad: %s\n", msg);
        exit(1);
}

static int parse_source(const char *s, struct bad_source *r)
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
        c = 1U;
        if (q != NULL) {
                c = strtoul(q, &end, 0);
                if (*end != '\0') {
                        free(copy);
                        return -1;
                }
        }
        free(copy);
        if (a > D6M_BADMAP_MEMBER_MASK || b > D6M_BADMAP_BLOCK_MASK ||
            c == 0U || c > D6M_BADMAP_BLOCK_MASK)
                return -1;
        r->member = (unsigned)a;
        r->start = (unsigned)b;
        r->count = (unsigned)c;
        return 0;
}

static uint64_t locator(unsigned member, unsigned sector)
{
        return ((uint64_t)member << D6M_BADMAP_MEMBER_SHIFT) | sector;
}

static int cmp_word(const void *av, const void *bv)
{
        uint64_t a = *(const uint64_t *)av;
        uint64_t b = *(const uint64_t *)bv;
        uint64_t as = (a >> 18) & D6M_HALF_MASK;
        uint64_t bs = (b >> 18) & D6M_HALF_MASK;
        return as < bs ? -1 : as > bs ? 1 : 0;
}

static int replacement_used(const uint64_t *entries, unsigned count,
    unsigned member, unsigned sector)
{
        unsigned i;
        uint64_t want = locator(member, sector);

        for (i = 0; i < count; ++i)
                if ((entries[i] & D6M_HALF_MASK) == want)
                        return 1;
        return 0;
}

static int source_exists(const uint64_t *entries, unsigned count,
    unsigned member, unsigned sector)
{
        unsigned i;
        uint64_t want = locator(member, sector);

        for (i = 0; i < count; ++i)
                if (((entries[i] >> 18) & D6M_HALF_MASK) == want)
                        return 1;
        return 0;
}

static int source_is_badmap(const struct d6m_set *set, unsigned member,
    unsigned sector)
{
        unsigned i;

        for (i = 0U; i < set->layout.badmap_blocks; ++i) {
                unsigned m, local;
                if (d6m_map(set, set->layout.badmap_start + i, &m, &local) == 0 &&
                    m == member && set->member[m].base + local == sector)
                        return 1;
        }
        return 0;
}

static unsigned choose_spare(const struct d6m_set *set, const uint64_t *entries,
    unsigned count, unsigned member)
{
        const struct d6m_member *m;
        unsigned first, sector;

        m = &set->member[member];
        first = m->base + m->blocks + set->layout.swap_tail_blocks;
        for (sector = first; sector < m->sectors; ++sector)
                if (!replacement_used(entries, count, member, sector) &&
                    !source_exists(entries, count, member, sector))
                        return sector;
        return ~0U;
}

static void list_map(const struct d6m_set *set)
{
        unsigned i;

        printf("badmap entries=%u capacity=%u\n", set->badmap_count,
            set->layout.badmap_blocks * D6M_BLOCK_WORDS -
            D6M_BADMAP_HEADER_WORDS);
        for (i = 0; i < set->badmap_count; ++i) {
                uint64_t source = (set->badmap[i] >> 18) & D6M_HALF_MASK;
                uint64_t spare = set->badmap[i] & D6M_HALF_MASK;
                printf("source=%u:%o spare=%u:%o\n",
                    (unsigned)((source >> D6M_BADMAP_MEMBER_SHIFT) &
                    D6M_BADMAP_MEMBER_MASK),
                    (unsigned)(source & D6M_BADMAP_BLOCK_MASK),
                    (unsigned)((spare >> D6M_BADMAP_MEMBER_SHIFT) &
                    D6M_BADMAP_MEMBER_MASK),
                    (unsigned)(spare & D6M_BADMAP_BLOCK_MASK));
        }
}

static int write_map(struct d6m_set *set, const uint64_t *entries,
    unsigned count)
{
        uint64_t block[D6M_BLOCK_WORDS];
        unsigned m, local;

        if (set->layout.badmap_blocks != 1U ||
            count > D6M_BLOCK_WORDS - D6M_BADMAP_HEADER_WORDS)
                return -1;
        memset(block, 0, sizeof(block));
        block[0] = D6M_BADMAP_MAGIC;
        block[1] = 1U;
        block[2] = count;
        if (count != 0U)
                memcpy(block + D6M_BADMAP_HEADER_WORDS, entries,
                    count * sizeof(*entries));
        if (d6m_map(set, set->layout.badmap_start, &m, &local) != 0)
                return -1;
        return d6m_write_phys(set, m, set->member[m].base + local, block);
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
        unsigned n = 0, i, a;
        int list = 0;
        char err[256];
        struct d6m_set set;
        struct bad_source src;
        uint64_t *entries;

        for (a = 1; a < (unsigned)argc; ++a) {
                if (strcmp(argv[a], "-n") == 0 && a + 1U < (unsigned)argc)
                        n = (unsigned)strtoul(argv[++a], NULL, 0);
                else if (strcmp(argv[a], "-d") == 0 && a + 1U < (unsigned)argc)
                        dir = argv[++a];
                else if (strcmp(argv[a], "--list") == 0)
                        list = 1;
                else if (strcmp(argv[a], "--add") == 0 && a + 1U < (unsigned)argc)
                        add = argv[++a];
                else
                        usage();
        }
        if (dir == NULL || n == 0U || n > D6M_MAX_MEMBERS ||
            ((list != 0) == (add != NULL)))
                usage();
        if (d6m_open(&set, dir, n, add != NULL, err, sizeof(err)) != 0)
                die(err);
        if (set.layout.badmap_blocks != 1U)
                die("v1 requires exactly one atomic BADMAP block");
        if (list) {
                list_map(&set);
                d6m_close(&set);
                return 0;
        }
        if (parse_source(add, &src) != 0 || src.member >= set.members)
                die("invalid source range");
        if (src.start < set.member[src.member].base ||
            src.start >= set.member[src.member].base + set.member[src.member].blocks +
            set.layout.swap_tail_blocks ||
            src.count > set.member[src.member].base + set.member[src.member].blocks +
            set.layout.swap_tail_blocks - src.start)
                die("source must lie in exported data or raw tail");
        if (set.badmap_count + src.count >
            D6M_BLOCK_WORDS - D6M_BADMAP_HEADER_WORDS)
                die("BADMAP is full");
        entries = calloc(set.badmap_count + src.count, sizeof(*entries));
        if (entries == NULL)
                die("out of memory");
        if (set.badmap_count != 0U)
                memcpy(entries, set.badmap,
                    set.badmap_count * sizeof(*entries));
        for (i = 0; i < src.count; ++i) {
                unsigned source = src.start + i;
                unsigned spare;
                uint64_t block[D6M_BLOCK_WORDS];
                if (source_is_badmap(&set, src.member, source))
                        die("BADMAP metadata cannot remap itself");
                if (source_exists(entries, set.badmap_count, src.member, source))
                        continue;
                spare = choose_spare(&set, entries, set.badmap_count, src.member);
                if (spare == ~0U)
                        die("no reserved spare block remains on source member");
                if (d6m_read_phys(&set, src.member, source, block) != 0)
                        die("source cannot be recovered; remap would lose data");
                if (d6m_write_phys(&set, src.member, spare, block) != 0)
                        die("cannot initialize spare block");
                entries[set.badmap_count++] =
                    (locator(src.member, source) << 18) |
                    locator(src.member, spare);
        }
        qsort(entries, set.badmap_count, sizeof(*entries), cmp_word);
        if (write_map(&set, entries, set.badmap_count) != 0)
                die("cannot publish BADMAP");
        fprintf(stderr, "d6bad: BADMAP published with %u remap(s)\n",
            set.badmap_count);
        free(entries);
        d6m_close(&set);
        return 0;
}
