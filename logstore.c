/* logstore.c - initialize, append and inspect the DAIMOS V1 LOGSTORE. */

#include "d6maint.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define LOGSTORE_RECORD_MAGIC   0546362454321ULL /* SIXBIT /LSREC1/ */
#define LOGSTORE_STATE_MAGIC    0546363644121ULL /* SIXBIT /LSSTA1/ */
#define LOGSTORE_PAYLOAD_WORDS  (D6M_BLOCK_WORDS - 5U)
#define LOGSTORE_STATE_NONE     2U

struct log_state {
        uint64_t next_sequence;
        uint64_t next_drain_sequence;
        uint64_t lost_records;
        uint64_t state_generation;
        unsigned next_slot;
        unsigned capacity;
        unsigned state_copy;
};

struct state_candidate {
        uint64_t generation;
        uint64_t next_drain_sequence;
        uint64_t lost_records;
        unsigned copy;
        int valid;
};

struct dump_entry {
        uint64_t block[D6M_BLOCK_WORDS];
};

static void die(const char *msg)
{
        fprintf(stderr, "logstore: %s\n", msg);
        exit(1);
}

static unsigned capacity(const struct d6m_set *set)
{
        if (set->layout.logstore_blocks < 3U)
                return 0U;
        return set->layout.logstore_blocks - 2U;
}

static unsigned logical_block(const struct d6m_set *set, unsigned block)
{
        return set->layout.logstore_start + block;
}

static int read_block(const struct d6m_set *set, unsigned block,
    uint64_t data[D6M_BLOCK_WORDS])
{
        if (block >= set->layout.logstore_blocks)
                return -1;
        return d6m_read(set, logical_block(set, block), data);
}

static int write_block(struct d6m_set *set, unsigned block,
    const uint64_t data[D6M_BLOCK_WORDS])
{
        if (block >= set->layout.logstore_blocks)
                return -1;
        return d6m_write(set, logical_block(set, block), data);
}

static uint64_t record_commit(uint64_t sequence, unsigned payload_words)
{
        uint64_t commit;

        commit = (LOGSTORE_RECORD_MAGIC + sequence +
            ((uint64_t)payload_words << 18)) & D6M_WORD_MASK;
        return (commit ^ D6M_WORD_MASK) | 1U;
}

static uint64_t state_commit(uint64_t generation, uint64_t next_drain_sequence,
    uint64_t lost_records, unsigned cap)
{
        uint64_t commit;

        commit = (LOGSTORE_STATE_MAGIC + generation + next_drain_sequence +
            lost_records + (uint64_t)cap) & D6M_WORD_MASK;
        return (commit ^ D6M_WORD_MASK) | 1U;
}

static int record_valid(const uint64_t block[D6M_BLOCK_WORDS])
{
        uint64_t sequence;
        unsigned payload_words;

        if (block[0] != LOGSTORE_RECORD_MAGIC || block[1] == 0)
                return 0;
        sequence = block[1];
        payload_words = (unsigned)(block[3] & D6M_HALF_MASK);
        return payload_words <= LOGSTORE_PAYLOAD_WORDS &&
            block[D6M_BLOCK_WORDS - 1U] ==
            record_commit(sequence, payload_words);
}

static void read_state_candidate(const struct d6m_set *set, unsigned copy,
    unsigned cap, struct state_candidate *state)
{
        uint64_t block[D6M_BLOCK_WORDS];

        state->valid = 0;
        state->copy = copy;
        if (read_block(set, copy, block) != 0)
                return;
        if (block[0] != LOGSTORE_STATE_MAGIC || block[1] == 0 ||
            block[2] == 0 || block[4] != cap ||
            block[D6M_BLOCK_WORDS - 1U] !=
            state_commit(block[1], block[2], block[3], cap))
                return;
        state->generation = block[1];
        state->next_drain_sequence = block[2];
        state->lost_records = block[3];
        state->valid = 1;
}

static int write_state(struct d6m_set *set, struct log_state *log,
    uint64_t next_drain_sequence, uint64_t lost_records)
{
        uint64_t block[D6M_BLOCK_WORDS];
        uint64_t generation;
        unsigned copy;

        if (log->state_generation == D6M_WORD_MASK || next_drain_sequence == 0)
                return -1;
        generation = log->state_generation + 1U;
        copy = log->state_copy <= 1U ? log->state_copy ^ 1U : 0U;
        memset(block, 0, sizeof(block));
        block[0] = LOGSTORE_STATE_MAGIC;
        block[1] = generation;
        block[2] = next_drain_sequence;
        block[3] = lost_records;
        block[4] = log->capacity;
        block[5] = 0;
        block[D6M_BLOCK_WORDS - 1U] = state_commit(generation,
            next_drain_sequence, lost_records, log->capacity);
        if (write_block(set, copy, block) != 0)
                return -1;
        log->state_generation = generation;
        log->state_copy = copy;
        log->next_drain_sequence = next_drain_sequence;
        log->lost_records = lost_records;
        return 0;
}

static int recover_log(const struct d6m_set *set, struct log_state *log)
{
        struct state_candidate a, b, *state;
        uint64_t block[D6M_BLOCK_WORDS];
        uint64_t best, oldest;
        unsigned best_slot, cap, slot;

        cap = capacity(set);
        if (cap == 0)
                return -1;
        read_state_candidate(set, 0U, cap, &a);
        read_state_candidate(set, 1U, cap, &b);
        state = NULL;
        if (a.valid)
                state = &a;
        if (b.valid && (state == NULL || b.generation > state->generation))
                state = &b;

        best = 0;
        best_slot = 0U;
        for (slot = 0; slot < cap; ++slot) {
                if (read_block(set, slot + 2U, block) != 0)
                        return -1;
                if (record_valid(block) && block[1] > best) {
                        best = block[1];
                        best_slot = slot;
                }
        }
        if (best == D6M_WORD_MASK)
                return -1;

        log->capacity = cap;
        log->next_sequence = best == 0 ? 1U : best + 1U;
        log->next_slot = best == 0 ? 0U : best_slot + 1U;
        if (log->next_slot >= cap)
                log->next_slot = 0U;
        oldest = best >= cap ? best - cap + 1U : 1U;
        if (state != NULL) {
                log->state_generation = state->generation;
                log->state_copy = state->copy;
                log->lost_records = state->lost_records;
                log->next_drain_sequence = state->next_drain_sequence;
                if (log->next_drain_sequence < oldest)
                        log->next_drain_sequence = oldest;
                if (log->next_drain_sequence > log->next_sequence)
                        log->next_drain_sequence = oldest;
        } else {
                log->state_generation = 0;
                log->state_copy = LOGSTORE_STATE_NONE;
                log->lost_records = 0;
                log->next_drain_sequence = oldest;
        }
        return 0;
}

static int append_log(struct d6m_set *set, struct log_state *log,
    unsigned severity, unsigned source, uint64_t timestamp,
    const uint64_t *payload, unsigned payload_words)
{
        uint64_t block[D6M_BLOCK_WORDS];
        uint64_t sequence, overwritten, delta, lost;
        unsigned i;

        if (log->capacity == 0 || log->next_sequence == 0 ||
            log->next_sequence == D6M_WORD_MASK || severity > 077U ||
            source > 07777U || payload_words > LOGSTORE_PAYLOAD_WORDS ||
            (payload_words != 0 && payload == NULL))
                return -1;
        sequence = log->next_sequence;
        if (sequence > log->capacity) {
                overwritten = sequence - log->capacity;
                if (log->next_drain_sequence <= overwritten) {
                        delta = overwritten - log->next_drain_sequence + 1U;
                        if (delta > D6M_WORD_MASK - log->lost_records)
                                return -1;
                        lost = log->lost_records + delta;
                        if (write_state(set, log, overwritten + 1U, lost) != 0)
                                return -1;
                }
        }

        memset(block, 0, sizeof(block));
        block[0] = LOGSTORE_RECORD_MAGIC;
        block[1] = sequence;
        block[2] = timestamp & D6M_WORD_MASK;
        block[3] = ((uint64_t)severity << 30) |
            ((uint64_t)source << 18) | payload_words;
        for (i = 0; i < payload_words; ++i)
                block[4U + i] = payload[i] & D6M_WORD_MASK;
        block[D6M_BLOCK_WORDS - 1U] = record_commit(sequence, payload_words);
        if (write_block(set, log->next_slot + 2U, block) != 0)
                return -1;
        log->next_sequence = sequence + 1U;
        log->next_slot = (log->next_slot + 1U) % log->capacity;
        return 0;
}

static int init_log(struct d6m_set *set)
{
        uint64_t block[D6M_BLOCK_WORDS];
        unsigned i;

        if (capacity(set) == 0)
                return -1;
        memset(block, 0, sizeof(block));
        for (i = 0; i < set->layout.logstore_blocks; ++i)
                if (write_block(set, i, block) != 0)
                        return -1;
        return 0;
}

static int dump_cmp(const void *av, const void *bv)
{
        const struct dump_entry *a = av;
        const struct dump_entry *b = bv;

        if (a->block[1] < b->block[1])
                return -1;
        if (a->block[1] > b->block[1])
                return 1;
        return 0;
}

static void dump_log(const struct d6m_set *set)
{
        struct log_state log;
        struct dump_entry *entries;
        uint64_t block[D6M_BLOCK_WORDS];
        uint64_t oldest, newest;
        unsigned slot, valid, n, i, j, words;

        if (recover_log(set, &log) != 0)
                die("cannot recover LOGSTORE");
        entries = calloc(log.capacity, sizeof(*entries));
        if (entries == NULL)
                die("out of memory");
        n = 0;
        for (slot = 0; slot < log.capacity; ++slot) {
                if (read_block(set, slot + 2U, block) != 0)
                        die("cannot read LOGSTORE record");
                if (!record_valid(block))
                        continue;
                memcpy(entries[n++].block, block, sizeof(block));
        }
        qsort(entries, n, sizeof(*entries), dump_cmp);
        valid = n;
        newest = log.next_sequence - 1U;
        oldest = newest >= log.capacity ? newest - log.capacity + 1U :
            (newest != 0 ? 1U : 0U);
        printf("LOGSTORE blocks=%o capacity=%u valid=%u oldest=%llo newest=%llo "
            "next-seq=%llo next-slot=%u next-drain=%llo lost=%llo state-gen=%llo\n",
            set->layout.logstore_blocks, log.capacity, valid,
            (unsigned long long)oldest, (unsigned long long)newest,
            (unsigned long long)log.next_sequence, log.next_slot,
            (unsigned long long)log.next_drain_sequence,
            (unsigned long long)log.lost_records,
            (unsigned long long)log.state_generation);
        for (i = 0; i < n; ++i) {
                unsigned sev = (unsigned)((entries[i].block[3] >> 30) & 077U);
                unsigned src = (unsigned)((entries[i].block[3] >> 18) & 07777U);
                words = (unsigned)(entries[i].block[3] & D6M_HALF_MASK);
                printf("seq=%llo time=%llo severity=%o source=%o payload=",
                    (unsigned long long)entries[i].block[1],
                    (unsigned long long)entries[i].block[2], sev, src);
                for (j = 0; j < words; ++j)
                        printf("%s%012llo", j == 0 ? "" : ",",
                            (unsigned long long)entries[i].block[4U + j]);
                putchar('\n');
        }
        free(entries);
}

static void usage(void)
{
        fprintf(stderr,
            "usage: logstore -n members -d diskdir --init\n"
            "       logstore -n members -d diskdir --dump\n"
            "       logstore -n members -d diskdir --append -s severity "
            "-S source -t timestamp [-p word ...]\n");
        exit(2);
}

int main(int argc, char **argv)
{
        const char *dir = NULL;
        unsigned n = 0, severity = 0, source = 0, npayload = 0;
        uint64_t timestamp = 0;
        uint64_t payload[LOGSTORE_PAYLOAD_WORDS];
        int init = 0, dump = 0, append = 0, a;
        char err[256];
        struct d6m_set set;
        struct log_state log;

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
                        if (npayload >= LOGSTORE_PAYLOAD_WORDS)
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
                        die("LOGSTORE needs two state blocks plus one record block");
                fprintf(stderr, "logstore: initialized %u record slots\n",
                    capacity(&set));
        } else if (dump) {
                dump_log(&set);
        } else {
                if (recover_log(&set, &log) != 0)
                        die("cannot recover LOGSTORE");
                if (append_log(&set, &log, severity, source, timestamp,
                    payload, npayload) != 0)
                        die("cannot append LOGSTORE record");
                fprintf(stderr, "logstore: seq=%llo slot=%u\n",
                    (unsigned long long)(log.next_sequence - 1U),
                    log.next_slot == 0 ? log.capacity - 1U : log.next_slot - 1U);
        }
        d6m_close(&set);
        return 0;
}
