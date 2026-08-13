/*
 * mkdsk.c - create small PDP-6 DSK270 DBOOT V1 test bootsets.
 *
 * This is intentionally a test-media builder, not an installer.  It writes
 * DBC compact descriptors or DB0/DB1/DBX descriptors and places a DAIMON
 * stream round-robin across 1..4 member images.
 */

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>

#define WORD_MASK 0777777777777ULL
#define HALF_MASK 0777777U
#define SECTOR_WORDS 0200U
#define SECTORS 02000U
#define SCAN_LIMIT 0200U
#define MAX_MEMBERS 4U
#define DB0_BAD_RUNS 020U
#define DB1_BAD_RUNS 0176U

#define MAGIC_DB0 0444220U
#define MAGIC_DB1 0444221U
#define MAGIC_DBX 0444270U
#define MAGIC_DBC 0444243U
#define DBOOTX_VERSION 0U
#define DBOOT_VERSION 1U
#define DB0_F_HAS_DB1 000004U
#define DBX_F_CHECKSUM 000001U
#define DBX_F_COMPLETE_REQ 000002U
#define DBX_F_COMPACT 000010U
#define MEMBER_F_REQUIRED 000001U
#define MEMBER_F_BOOT_MEMBER 000002U
#define MEMBER_F_REPLICA 000004U
#define MEMBER_F_COMPLETE_REQ 000010U

struct bad_run {
        unsigned start;
        unsigned count;
};

struct member {
        struct bad_run bad[0177];
        unsigned bad_count;
        unsigned db0_sector;
        unsigned db1_sector;
        unsigned dbx_sector;
        unsigned boot_start;
        unsigned boot_count;
};

static unsigned long long mask36(unsigned long long v)
{
        return v & WORD_MASK;
}

static unsigned long long six_header(unsigned magic, unsigned version,
    unsigned count, unsigned flags)
{
        return mask36((((unsigned long long)(magic & HALF_MASK)) << 18) |
            (((unsigned long long)(version & 077U)) << 12) |
            (((unsigned long long)(count & 0177U)) << 5) |
            (unsigned long long)(flags & 037U));
}

static unsigned long long member_word(unsigned mask, unsigned index,
    unsigned count, unsigned flags)
{
        return mask36((((unsigned long long)(mask & 0177777U)) << 20) |
            (((unsigned long long)(index & 017U)) << 16) |
            (((unsigned long long)(count & 017U)) << 12) |
            (unsigned long long)(flags & 07777U));
}

static unsigned long long bad_word(unsigned start, unsigned count)
{
        return mask36((((unsigned long long)(start & HALF_MASK)) << 18) |
            (((unsigned long long)((count - 1U) & 07777U)) << 6) | 1U);
}

static int contains_bad(struct member *m, unsigned sector)
{
        unsigned i;
        for (i = 0; i < m->bad_count; i++) {
                unsigned start = m->bad[i].start;
                unsigned end = start + m->bad[i].count;
                if (sector >= start && sector < end)
                        return 1;
        }
        return 0;
}

static unsigned next_good(struct member *m, unsigned sector)
{
        while (contains_bad(m, sector))
                sector++;
        return sector;
}

static unsigned nth_good(struct member *m, unsigned start, unsigned n)
{
        unsigned sector = start;
        for (;;) {
                if (!contains_bad(m, sector)) {
                        if (n == 0)
                                return sector;
                        n--;
                }
                sector++;
        }
}

static void put64(FILE *f, unsigned long long v)
{
        unsigned i;
        for (i = 0; i < 8; i++)
                fputc((int)((v >> (i * 8)) & 0xff), f);
}

static void write_sector(FILE *f, unsigned sector, unsigned long long *words)
{
        unsigned i;
        if (fseek(f, (long)sector * SECTOR_WORDS * 8L, SEEK_SET) != 0) {
                perror("mkdsk: seek");
                exit(1);
        }
        for (i = 0; i < SECTOR_WORDS; i++)
                put64(f, mask36(words[i]));
}

static void zero_sector(unsigned long long *words)
{
        memset(words, 0, sizeof(unsigned long long) * SECTOR_WORDS);
}

static void add_bad(struct member *m, unsigned start, unsigned count)
{
        if (m->bad_count >= 0177U) {
                fprintf(stderr, "mkdsk: too many bad runs\n");
                exit(1);
        }
        m->bad[m->bad_count].start = start;
        m->bad[m->bad_count].count = count;
        m->bad_count++;
}

static void init_bad_runs(struct member *members, unsigned n, const char *mode)
{
        unsigned u, i;
        if (strcmp(mode, "clean") == 0 || strcmp(mode, "db0") == 0)
                return;
        if (strcmp(mode, "bad") == 0) {
                for (u = 0; u < n; u++) {
                        add_bad(&members[u], 4U + u, 1);
                        add_bad(&members[u], 010U + u * 2U, 2);
                }
                return;
        }
        if (strcmp(mode, "db1") == 0) {
                for (u = 0; u < n; u++) {
                        add_bad(&members[u], 5U + u, 1);
                        for (i = 1; i < 022U; i++)
                                add_bad(&members[u], 0200U + i * 2U + u, 1);
                }
                return;
        }
        fprintf(stderr, "mkdsk: bad mode: %s\n", mode);
        exit(2);
}

static void make_db0(unsigned long long *words, struct member *m)
{
        unsigned i, count = m->bad_count;
        unsigned flags = (count > DB0_BAD_RUNS) ? DB0_F_HAS_DB1 : 0;
        zero_sector(words);
        if (count > DB0_BAD_RUNS)
                count = DB0_BAD_RUNS;
        words[0] = six_header(MAGIC_DB0, DBOOT_VERSION, count, flags);
        if (flags)
                words[021] = m->db1_sector;
        for (i = 0; i < count; i++)
                words[1 + i] = bad_word(m->bad[i].start, m->bad[i].count);
}

static void make_db1(unsigned long long *words, struct member *m)
{
        unsigned i, count = m->bad_count - DB0_BAD_RUNS;
        if (count > DB1_BAD_RUNS) {
                fprintf(stderr, "mkdsk: DB1 overflow beyond one sector\n");
                exit(1);
        }
        zero_sector(words);
        words[0] = six_header(MAGIC_DB1, DBOOT_VERSION, count, 0);
        words[1] = 0;
        for (i = 0; i < count; i++)
                words[2 + i] = bad_word(m->bad[DB0_BAD_RUNS + i].start,
                    m->bad[DB0_BAD_RUNS + i].count);
}

static void make_dbx(unsigned long long *words, unsigned unit, unsigned n,
    int compact)
{
        unsigned mask = (1U << n) - 1U;
        zero_sector(words);
        words[0] = six_header(compact ? MAGIC_DBC : MAGIC_DBX,
            DBOOTX_VERSION, 0, 0);
        words[2] = 0;
        words[5] = member_word(mask, unit, n, 0);
}

static unsigned long long parse_word(const char *s)
{
        unsigned long long v;
        int digit;
        int seen;

        v = 0;
        seen = 0;
        while (*s == ' ' || *s == '\t')
                s++;
        while (*s >= '0' && *s <= '7') {
                digit = *s++ - '0';
                if (v > (WORD_MASK >> 3))
                        return ~0ULL;
                v = (v << 3) | (unsigned long long)digit;
                seen = 1;
        }
        while (*s == ' ' || *s == '\t')
                s++;
        if (!seen || (*s != '\0' && *s != '\n' && *s != '#'))
                return ~0ULL;
        return v;
}

static unsigned long long *read_payload(const char *path, unsigned *countp)
{
        FILE *f = fopen(path, "r");
        char line[256];
        unsigned long long *words = NULL;
        unsigned count = 0, cap = 0;
        if (f == NULL) {
                perror(path);
                exit(1);
        }
        while (fgets(line, sizeof(line), f) != NULL) {
                char *p = line;
                unsigned long long w;
                while (*p == ' ' || *p == '\t')
                        p++;
                if (*p == '\0' || *p == '\n' || *p == '#')
                        continue;
                w = parse_word(p);
                if (w == ~0ULL) {
                        fprintf(stderr, "mkdsk: bad payload word: %s", line);
                        exit(1);
                }
                if (count == cap) {
                        cap = cap ? cap * 2 : 1024;
                        words = realloc(words, cap * sizeof(words[0]));
                        if (words == NULL) {
                                perror("mkdsk: realloc");
                                exit(1);
                        }
                }
                words[count++] = w;
        }
        fclose(f);
        *countp = count;
        return words;
}

static void usage(void)
{
        fprintf(stderr,
            "usage: mkdsk -n members -m clean|db0|db1|bad -p words -o dir\n");
        exit(2);
}

int main(int argc, char **argv)
{
        const char *mode = NULL, *payload_path = NULL, *outdir = NULL;
        unsigned n = 0, payload_count, logical_sectors, u, logical;
        struct member members[MAX_MEMBERS];
        unsigned long long *payload;
        FILE *files[MAX_MEMBERS];
        char path[512];
        int compact;
        unsigned long long sector[SECTOR_WORDS];
        int i;

        for (i = 1; i < argc; i++) {
                if (strcmp(argv[i], "-n") == 0 && i + 1 < argc)
                        n = (unsigned)strtoul(argv[++i], NULL, 0);
                else if (strcmp(argv[i], "-m") == 0 && i + 1 < argc)
                        mode = argv[++i];
                else if (strcmp(argv[i], "-p") == 0 && i + 1 < argc)
                        payload_path = argv[++i];
                else if (strcmp(argv[i], "-o") == 0 && i + 1 < argc)
                        outdir = argv[++i];
                else
                        usage();
        }
        if (n < 1 || n > MAX_MEMBERS || mode == NULL ||
            payload_path == NULL || outdir == NULL)
                usage();

        memset(members, 0, sizeof(members));
        init_bad_runs(members, n, mode);
        compact = strcmp(mode, "clean") == 0;

        if (mkdir(outdir, 0777) != 0 && errno != EEXIST) {
                perror(outdir);
                return 1;
        }

        payload = read_payload(payload_path, &payload_count);
        logical_sectors = (payload_count + SECTOR_WORDS - 1U) / SECTOR_WORDS;
        for (u = 0; u < n; u++) {
                if (compact) {
                        members[u].dbx_sector = 0;
                        members[u].boot_start = 1;
                } else {
                        members[u].db0_sector = next_good(&members[u], 0);
                        members[u].db1_sector = members[u].bad_count > DB0_BAD_RUNS ?
                            next_good(&members[u], members[u].db0_sector + 1) : 0;
                        members[u].dbx_sector = next_good(&members[u],
                            members[u].db0_sector + 1 + (members[u].db1_sector ? 1 : 0));
                        members[u].boot_start = next_good(&members[u],
                            members[u].dbx_sector + 1);
                }
                members[u].boot_count = (logical_sectors + n - 1U - u) / n;
                sprintf(path, "%s/dsk%u.dsk", outdir, u);
                files[u] = fopen(path, "wb+");
                if (files[u] == NULL) {
                        perror(path);
                        return 1;
                }
                if (fseek(files[u], (long)SECTORS * SECTOR_WORDS * 8L - 1L,
                    SEEK_SET) != 0 || fputc(0, files[u]) == EOF) {
                        perror(path);
                        return 1;
                }
        }

        for (u = 0; u < n; u++) {
                make_dbx(sector, u, n, compact);
                write_sector(files[u], members[u].dbx_sector, sector);
                if (!compact) {
                        make_db0(sector, &members[u]);
                        write_sector(files[u], members[u].db0_sector, sector);
                        if (members[u].db1_sector) {
                                make_db1(sector, &members[u]);
                                write_sector(files[u], members[u].db1_sector, sector);
                        }
                }
        }

        for (logical = 0; logical < logical_sectors; logical++) {
                unsigned slot = logical % n;
                unsigned local = logical / n;
                unsigned phys = nth_good(&members[slot],
                    members[slot].boot_start, local);
                unsigned off = logical * SECTOR_WORDS;
                unsigned j;
                zero_sector(sector);
                for (j = 0; j < SECTOR_WORDS && off + j < payload_count; j++)
                        sector[j] = payload[off + j];
                write_sector(files[slot], phys, sector);
        }

        for (u = 0; u < n; u++)
                fclose(files[u]);
        free(payload);
        return 0;
}
