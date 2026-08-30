/* d6log.c - initialize, append and inspect the reserved DAIMOS LOGSTORE. */

#include "d6maint.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define LOG_HEADER_WORDS 8U
#define LOG_PAYLOAD_WORDS 3U
#define LOG_RECORDS_PER_BLOCK (D6M_BLOCK_WORDS / D6M_LOG_RECORD_WORDS)

static void die(const char *msg)
{
        fprintf(stderr, "d6log: %s\n", msg);
        exit(1);
}

static unsigned capacity(const struct d6m_set *set)
{
        if (set->layout.logstore_blocks <= 1U)
                return 0U;
        return (set->layout.logstore_blocks - 1U) * LOG_RECORDS_PER_BLOCK;
}

static int valid_record(const uint64_t r[D6M_LOG_RECORD_WORDS])
{
        unsigned payload;
        if (r[0] != D6M_LOG_RECORD_MAGIC)
                return 0;
        payload = (unsigned)(r[3] & 07U);
        if (payload > LOG_PAYLOAD_WORDS)
                return 0;
        return r[7] == ((D6M_LOG_RECORD_MAGIC ^ r[1]) & D6M_WORD_MASK);
}

static int read_record(const struct d6m_set *set, unsigned slot,
    uint64_t r[D6M_LOG_RECORD_WORDS])
{
        uint64_t block[D6M_BLOCK_WORDS];
        unsigned logical, off;

        if (slot >= capacity(set))
                return -1;
        logical = set->layout.logstore_start + 1U + slot / LOG_RECORDS_PER_BLOCK;
        off = (slot % LOG_RECORDS_PER_BLOCK) * D6M_LOG_RECORD_WORDS;
        if (d6m_read(set, logical, block) != 0)
                return -1;
        memcpy(r, block + off, D6M_LOG_RECORD_WORDS * sizeof(*r));
        return 0;
}

static int write_record(struct d6m_set *set, unsigned slot,
    const uint64_t r[D6M_LOG_RECORD_WORDS])
{
        uint64_t block[D6M_BLOCK_WORDS];
        unsigned logical, off;

        if (slot >= capacity(set))
                return -1;
        logical = set->layout.logstore_start + 1U + slot / LOG_RECORDS_PER_BLOCK;
        off = (slot % LOG_RECORDS_PER_BLOCK) * D6M_LOG_RECORD_WORDS;
        if (d6m_read(set, logical, block) != 0)
                return -1;
        memcpy(block + off, r, D6M_LOG_RECORD_WORDS * sizeof(*r));
        return d6m_write(set, logical, block);
}

static void make_header(const struct d6m_set *set, uint64_t h[D6M_BLOCK_WORDS],
    uint64_t next_seq, unsigned next_slot, uint64_t lost)
{
        memset(h, 0, D6M_BLOCK_WORDS * sizeof(*h));
        h[0] = D6M_LOG_MAGIC;
        h[1] = D6M_LOG_VERSION;
        h[2] = next_seq & D6M_WORD_MASK;
        h[3] = next_slot;
        h[4] = lost & D6M_WORD_MASK;
        h[5] = capacity(set);
        h[6] = D6M_LOG_RECORD_WORDS;
        h[7] = LOG_PAYLOAD_WORDS;
}

static int init_log(struct d6m_set *set)
{
        uint64_t block[D6M_BLOCK_WORDS];
        unsigned b;

        if (set->layout.logstore_blocks < 2U)
                return -1;
        memset(block, 0, sizeof(block));
        for (b = 1U; b < set->layout.logstore_blocks; ++b)
                if (d6m_write(set, set->layout.logstore_start + b, block) != 0)
                        return -1;
        make_header(set, block, 1U, 0U, 0U);
        return d6m_write(set, set->layout.logstore_start, block);
}

static void scan_log(const struct d6m_set *set, uint64_t *next_seqp,
    unsigned *next_slotp, unsigned *validp)
{
        uint64_t r[D6M_LOG_RECORD_WORDS];
        uint64_t best;
        unsigned best_slot, slot, valid;

        best = 0;
        best_slot = 0;
        valid = 0;
        for (slot = 0; slot < capacity(set); ++slot) {
                if (read_record(set, slot, r) != 0)
                        die("cannot scan LOGSTORE record");
                if (!valid_record(r))
                        continue;
                ++valid;
                if (r[1] >= best) {
                        best = r[1];
                        best_slot = slot;
                }
        }
        *next_seqp = best == 0 ? 1U : (best + 1U) & D6M_WORD_MASK;
        *next_slotp = best == 0 ? 0U : (best_slot + 1U) % capacity(set);
        *validp = valid;
}

static uint64_t header_lost(const struct d6m_set *set)
{
        uint64_t h[D6M_BLOCK_WORDS];
        if (d6m_read(set, set->layout.logstore_start, h) != 0)
                die("cannot read LOGSTORE header");
        if (h[0] != D6M_LOG_MAGIC || h[1] != D6M_LOG_VERSION ||
            h[5] != capacity(set) || h[6] != D6M_LOG_RECORD_WORDS)
                return 0;
        return h[4];
}

static void append_log(struct d6m_set *set, unsigned severity, unsigned source,
    uint64_t timestamp, const uint64_t payload[LOG_PAYLOAD_WORDS],
    unsigned payload_words)
{
        uint64_t r[D6M_LOG_RECORD_WORDS], h[D6M_BLOCK_WORDS];
        uint64_t seq, lost;
        unsigned slot, valid, i;

        if (capacity(set) == 0)
                die("LOGSTORE has no record capacity");
        if (severity > 077U || source > 07777U || payload_words > LOG_PAYLOAD_WORDS)
                die("record fields exceed LOGSTORE format");
        scan_log(set, &seq, &slot, &valid);
        lost = header_lost(set);
        if (valid >= capacity(set))
                ++lost;
        memset(r, 0, sizeof(r));
        r[0] = D6M_LOG_RECORD_MAGIC;
        r[1] = seq;
        r[2] = timestamp & D6M_WORD_MASK;
        r[3] = ((uint64_t)severity << 30) | ((uint64_t)source << 18) |
            (uint64_t)payload_words;
        for (i = 0; i < payload_words; ++i)
                r[4U + i] = payload[i] & D6M_WORD_MASK;
        /* Publish the record body with an invalid commit word first.  A
         * power cut at this point leaves a record the scanner ignores. */
        r[7] = 0;
        if (write_record(set, slot, r) != 0)
                die("cannot write LOGSTORE record body");
        r[7] = (D6M_LOG_RECORD_MAGIC ^ seq) & D6M_WORD_MASK;
        if (write_record(set, slot, r) != 0)
                die("cannot commit LOGSTORE record");
        make_header(set, h, (seq + 1U) & D6M_WORD_MASK,
            (slot + 1U) % capacity(set), lost);
        if (d6m_write(set, set->layout.logstore_start, h) != 0)
                die("cannot update LOGSTORE header");
        fprintf(stderr, "d6log: seq=%llo slot=%u%s\n",
            (unsigned long long)seq, slot, valid >= capacity(set) ? " wrapped" : "");
}

struct dump_entry {
        uint64_t r[D6M_LOG_RECORD_WORDS];
};

static int dump_cmp(const void *av, const void *bv)
{
        const struct dump_entry *a = av, *b = bv;
        if (a->r[1] < b->r[1]) return -1;
        if (a->r[1] > b->r[1]) return 1;
        return 0;
}

static void dump_log(const struct d6m_set *set)
{
        struct dump_entry *e;
        uint64_t r[D6M_LOG_RECORD_WORDS], next, lost;
        unsigned slot, valid, n, i, j, next_slot;

        if (capacity(set) == 0)
                die("LOGSTORE has no record capacity");
        e = calloc(capacity(set), sizeof(*e));
        if (e == NULL)
                die("out of memory");
        n = 0;
        for (slot = 0; slot < capacity(set); ++slot) {
                if (read_record(set, slot, r) != 0)
                        die("cannot read LOGSTORE record");
                if (!valid_record(r))
                        continue;
                memcpy(e[n++].r, r, sizeof(r));
        }
        qsort(e, n, sizeof(*e), dump_cmp);
        scan_log(set, &next, &next_slot, &valid);
        lost = header_lost(set);
        printf("LOGSTORE blocks=%o capacity=%u valid=%u next-seq=%llo next-slot=%u lost=%llo\n",
            set->layout.logstore_blocks, capacity(set), valid,
            (unsigned long long)next, next_slot, (unsigned long long)lost);
        for (i = 0; i < n; ++i) {
                unsigned sev = (unsigned)((e[i].r[3] >> 30) & 077U);
                unsigned src = (unsigned)((e[i].r[3] >> 18) & 07777U);
                unsigned words = (unsigned)(e[i].r[3] & 07U);
                printf("seq=%llo time=%llo severity=%o source=%o payload=",
                    (unsigned long long)e[i].r[1],
                    (unsigned long long)e[i].r[2], sev, src);
                for (j = 0; j < words; ++j)
                        printf("%s%012llo", j == 0 ? "" : ",",
                            (unsigned long long)e[i].r[4U + j]);
                putchar('\n');
        }
        free(e);
}

static void usage(void)
{
        fprintf(stderr,
            "usage: d6log -n members -d diskdir --init\n"
            "       d6log -n members -d diskdir --dump\n"
            "       d6log -n members -d diskdir --append -s severity -S source "
            "-t timestamp [-p word] [-p word] [-p word]\n");
        exit(2);
}

int main(int argc, char **argv)
{
        const char *dir = NULL;
        unsigned n = 0, severity = 0, source = 0, npayload = 0;
        uint64_t timestamp = 0, payload[LOG_PAYLOAD_WORDS];
        int init = 0, dump = 0, append = 0, a;
        char err[256];
        struct d6m_set set;

        memset(payload, 0, sizeof(payload));
        for (a = 1; a < argc; ++a) {
                if (strcmp(argv[a], "-n") == 0 && a + 1 < argc)
                        n = (unsigned)strtoul(argv[++a], NULL, 0);
                else if (strcmp(argv[a], "-d") == 0 && a + 1 < argc)
                        dir = argv[++a];
                else if (strcmp(argv[a], "--init") == 0)
                        init = 1;
                else if (strcmp(argv[a], "--dump") == 0)
                        dump = 1;
                else if (strcmp(argv[a], "--append") == 0)
                        append = 1;
                else if (strcmp(argv[a], "-s") == 0 && a + 1 < argc)
                        severity = (unsigned)strtoul(argv[++a], NULL, 0);
                else if (strcmp(argv[a], "-S") == 0 && a + 1 < argc)
                        source = (unsigned)strtoul(argv[++a], NULL, 0);
                else if (strcmp(argv[a], "-t") == 0 && a + 1 < argc)
                        timestamp = strtoull(argv[++a], NULL, 0);
                else if (strcmp(argv[a], "-p") == 0 && a + 1 < argc) {
                        if (npayload >= LOG_PAYLOAD_WORDS)
                                usage();
                        payload[npayload++] = strtoull(argv[++a], NULL, 0) &
                            D6M_WORD_MASK;
                } else
                        usage();
        }
        if (dir == NULL || n == 0 || n > D6M_MAX_MEMBERS ||
            init + dump + append != 1)
                usage();
        if (d6m_open(&set, dir, n, init || append, err, sizeof(err)) != 0)
                die(err);
        if (!set.layout.explicit_ranges || set.layout.logstore_blocks == 0)
                die("diskset has no explicit LOGSTORE reservation");
        if (init) {
                if (init_log(&set) != 0)
                        die("LOGSTORE needs at least two reserved blocks");
                fprintf(stderr, "d6log: initialized %u record slots\n", capacity(&set));
        } else if (dump)
                dump_log(&set);
        else
                append_log(&set, severity, source, timestamp, payload, npayload);
        d6m_close(&set);
        return 0;
}
