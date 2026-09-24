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
#define DB0_BAD_RUNS 040U
#define DB1_BAD_RUNS 0400U
#define MAX_BAD_RUNS (DB0_BAD_RUNS + DB1_BAD_RUNS)
#define BAD_RUN_MAX_LENGTH 0400U

/* DB0 RH: VERSION3 | DB0_RUN_COUNT6 | DB1_RUN_COUNT9. */
#define MAGIC_DB0 0444220U
#define MAGIC_DBX 0444270U
#define MAGIC_DBC 0444243U
#define DBOOTX_VERSION 0U
#define DBOOT_VERSION 1U
#define DBX_F_CHECKSUM 000001U
#define DBX_F_COMPLETE_REQ 000002U
#define DBX_F_COMPACT 000010U
#define MEMBER_F_REQUIRED 000001U
#define MEMBER_F_BOOT_MEMBER 000002U
#define MEMBER_F_REPLICA 000004U
#define MEMBER_F_COMPLETE_REQ 000010U

/* Optional D6FS V2 root-layout extension in DBOOT words 6..10. */
#define D6FS_LAYOUT_MAGIC 0442654636222ULL /* SIXBIT /D6FSR2/ */
#define D6FS_LAYOUT_MAGIC_WORD 006U
#define D6FS_LAYOUT_RANGE_WORD 007U
#define D6FS_LAYOUT_SUPER_A 010U
#define D6FS_LAYOUT_SUPER_B 011U
#define D6FS_LAYOUT_SWAP_TAIL 012U
#define D6FS_LAYOUT_BOOTSTREAM_BLOCKS 013U
#define D6FS_LAYOUT_LOGSTORE_START 014U
#define D6FS_LAYOUT_LOGSTORE_BLOCKS 015U
#define D6FS_LAYOUT_BADMAP_START 016U
#define D6FS_LAYOUT_BADMAP_BLOCKS 017U

struct bad_run {
        unsigned start;
        unsigned count;
};

struct member {
        struct bad_run bad[MAX_BAD_RUNS];
        unsigned sectors;
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

/* 18-bit run: START_SECTOR10 | RUN_LENGTH_MINUS_ONE8. */
static unsigned bad_half(unsigned start, unsigned count)
{
        return ((start & 01777U) << 8) | ((count - 1U) & 0377U);
}

static unsigned long long db0_header(unsigned db0_count, unsigned db1_count)
{
        return mask36((((unsigned long long)MAGIC_DB0) << 18) |
            (((unsigned long long)(DBOOT_VERSION & 07U)) << 15) |
            (((unsigned long long)(db0_count & 077U)) << 9) |
            (unsigned long long)(db1_count & 0777U));
}

static void put_bad_halves(unsigned long long *words, unsigned word_offset,
    const struct bad_run *bad, unsigned count)
{
        unsigned i;
        for (i = 0; i < count; i++) {
                unsigned half = bad_half(bad[i].start, bad[i].count);
                unsigned wi = word_offset + (i >> 1);
                if ((i & 1U) == 0)
                        words[wi] |= (unsigned long long)half << 18;
                else
                        words[wi] |= (unsigned long long)half;
        }
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
        while (count != 0) {
                unsigned chunk = count > BAD_RUN_MAX_LENGTH ?
                    BAD_RUN_MAX_LENGTH : count;
                if (start >= SECTORS || chunk > SECTORS - start) {
                        fprintf(stderr, "mkdsk: bad run outside disk\n");
                        exit(1);
                }
                if (m->bad_count >= MAX_BAD_RUNS) {
                        fprintf(stderr, "mkdsk: too many bad runs\n");
                        exit(1);
                }
                m->bad[m->bad_count].start = start;
                m->bad[m->bad_count].count = chunk;
                m->bad_count++;
                start += chunk;
                count -= chunk;
        }
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
                        add_bad(&members[u], 1U, 1);
                        add_bad(&members[u], 3U, 1);
                        for (i = 1; i < 0202U; i++)
                                add_bad(&members[u], 0200U + i * 2U + u, 1);
                }
                return;
        }
        fprintf(stderr, "mkdsk: bad mode: %s\n", mode);
        exit(2);
}

static void make_db0(unsigned long long *words, struct member *m)
{
        unsigned db0_count = m->bad_count;
        unsigned db1_count = 0;
        zero_sector(words);
        if (db0_count > DB0_BAD_RUNS) {
                db1_count = db0_count - DB0_BAD_RUNS;
                db0_count = DB0_BAD_RUNS;
        }
        words[0] = db0_header(db0_count, db1_count);
        if (db1_count != 0)
                words[021] = m->db1_sector;
        put_bad_halves(words, 1, m->bad, db0_count);
}

static void make_db1(unsigned long long *words, struct member *m)
{
        unsigned count = m->bad_count - DB0_BAD_RUNS;
        if (count > DB1_BAD_RUNS) {
                fprintf(stderr, "mkdsk: DB1 overflow beyond one sector\n");
                exit(1);
        }
        zero_sector(words);
        put_bad_halves(words, 0, m->bad + DB0_BAD_RUNS, count);
}

static void make_dbx(unsigned long long *words, unsigned mask,
    unsigned index, unsigned count, int compact, const struct member *m,
    int d6fs_layout, unsigned super_a, unsigned super_b, unsigned swap_tail,
    unsigned spare_blocks, unsigned bootstream_blocks, unsigned logstore_start,
    unsigned logstore_blocks, unsigned badmap_start, unsigned badmap_blocks)
{
        zero_sector(words);
        words[0] = six_header(compact ? MAGIC_DBC : MAGIC_DBX,
            DBOOTX_VERSION, 0, 0);
        words[2] = 0;
        words[5] = member_word(mask, index, count, 0);
        if (d6fs_layout) {
                unsigned usable;

                if (m->boot_start >= m->sectors ||
                    swap_tail >= m->sectors - m->boot_start ||
                    spare_blocks > m->sectors - m->boot_start - swap_tail) {
                        fprintf(stderr, "mkdsk: D6FS layout leaves no usable sectors\n");
                        exit(1);
                }
                usable = m->sectors - m->boot_start - swap_tail - spare_blocks;
                words[D6FS_LAYOUT_MAGIC_WORD] = D6FS_LAYOUT_MAGIC;
                words[D6FS_LAYOUT_RANGE_WORD] =
                    ((unsigned long long)m->boot_start << 18) | usable;
                words[D6FS_LAYOUT_SUPER_A] = super_a;
                words[D6FS_LAYOUT_SUPER_B] = super_b;
                words[D6FS_LAYOUT_SWAP_TAIL] = swap_tail;
                words[D6FS_LAYOUT_BOOTSTREAM_BLOCKS] = bootstream_blocks;
                words[D6FS_LAYOUT_LOGSTORE_START] = logstore_start;
                words[D6FS_LAYOUT_LOGSTORE_BLOCKS] = logstore_blocks;
                words[D6FS_LAYOUT_BADMAP_START] = badmap_start;
                words[D6FS_LAYOUT_BADMAP_BLOCKS] = badmap_blocks;
        }
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
            "usage: mkdsk -n members [-M member-mask] "
            "-m clean|db0|db1|bad -p words -o dir "
            "[--d6fs-layout --logstore-blocks n --badmap-blocks n --spare-blocks n --swap-tail-blocks n] "
            "[--member-sectors s0[,s1...]]\n");
        exit(2);
}

static unsigned popcount4(unsigned mask)
{
        unsigned count = 0;
        mask &= 017U;
        while (mask != 0) {
                count += mask & 1U;
                mask >>= 1;
        }
        return count;
}

int main(int argc, char **argv)
{
        const char *mode = NULL, *payload_path = NULL, *outdir = NULL;
        unsigned n = 0, member_mask = 0, payload_count, logical_sectors;
        unsigned logstore_blocks = 0, badmap_blocks = 1, spare_blocks = 0, swap_tail_blocks = 0;
        const char *member_sectors_arg = NULL;
        unsigned super_a = 0, super_b = 0;
        int d6fs_layout = 0;
        unsigned logical_index[MAX_MEMBERS], u, logical;
        struct member members[MAX_MEMBERS];
        unsigned long long *payload;
        FILE *files[MAX_MEMBERS];
        char path[512];
        int compact;
        unsigned long long sector[SECTOR_WORDS];
        int i, pathlen;

        for (i = 1; i < argc; i++) {
                if (strcmp(argv[i], "-n") == 0 && i + 1 < argc)
                        n = (unsigned)strtoul(argv[++i], NULL, 0);
                else if (strcmp(argv[i], "-M") == 0 && i + 1 < argc)
                        member_mask = (unsigned)strtoul(argv[++i], NULL, 0);
                else if (strcmp(argv[i], "-m") == 0 && i + 1 < argc)
                        mode = argv[++i];
                else if (strcmp(argv[i], "-p") == 0 && i + 1 < argc)
                        payload_path = argv[++i];
                else if (strcmp(argv[i], "-o") == 0 && i + 1 < argc)
                        outdir = argv[++i];
                else if (strcmp(argv[i], "--d6fs-layout") == 0)
                        d6fs_layout = 1;
                else if (strcmp(argv[i], "--logstore-blocks") == 0 && i + 1 < argc)
                        logstore_blocks = (unsigned)strtoul(argv[++i], NULL, 0);
                else if (strcmp(argv[i], "--badmap-blocks") == 0 && i + 1 < argc)
                        badmap_blocks = (unsigned)strtoul(argv[++i], NULL, 0);
                else if (strcmp(argv[i], "--spare-blocks") == 0 && i + 1 < argc)
                        spare_blocks = (unsigned)strtoul(argv[++i], NULL, 0);
                else if (strcmp(argv[i], "--swap-tail-blocks") == 0 && i + 1 < argc)
                        swap_tail_blocks = (unsigned)strtoul(argv[++i], NULL, 0);
                else if (strcmp(argv[i], "--member-sectors") == 0 && i + 1 < argc)
                        member_sectors_arg = argv[++i];
                else
                        usage();
        }
        if (n < 1 || n > MAX_MEMBERS || mode == NULL ||
            payload_path == NULL || outdir == NULL)
                usage();
        if (member_mask == 0)
                member_mask = (1U << n) - 1U;
        if ((member_mask & ~017U) != 0 || popcount4(member_mask) != n)
                usage();
        {
                unsigned index, slot = 0;
                for (index = 0; index < MAX_MEMBERS; index++) {
                        if ((member_mask & (1U << index)) != 0)
                                logical_index[slot++] = index;
                }
        }

        memset(members, 0, sizeof(members));
        for (u = 0; u < n; ++u)
                members[u].sectors = SECTORS;
        if (member_sectors_arg != NULL) {
                const char *q = member_sectors_arg;

                for (u = 0; u < n; ++u) {
                        char *end;
                        unsigned long v;

                        if (*q == '\0')
                                usage();
                        v = strtoul(q, &end, 0);
                        if (end == q || v == 0UL || v > SECTORS ||
                            (u + 1U < n && *end != ',') ||
                            (u + 1U == n && *end != '\0'))
                                usage();
                        members[u].sectors = (unsigned)v;
                        q = end + (u + 1U < n ? 1 : 0);
                }
        }
        init_bad_runs(members, n, mode);
        compact = strcmp(mode, "clean") == 0;

        if (mkdir(outdir, 0777) != 0 && errno != EEXIST) {
                perror(outdir);
                return 1;
        }

        payload = read_payload(payload_path, &payload_count);
        logical_sectors = (payload_count + SECTOR_WORDS - 1U) / SECTOR_WORDS;
        if (d6fs_layout) {
                if (logical_sectors > HALF_MASK || logstore_blocks > HALF_MASK ||
                    badmap_blocks > HALF_MASK ||
                    logical_sectors > HALF_MASK - logstore_blocks ||
                    logical_sectors + logstore_blocks > HALF_MASK - badmap_blocks - 2U) {
                        fprintf(stderr, "mkdsk: D6FS reserved layout too large\n");
                        return 1;
                }
                super_a = logical_sectors + logstore_blocks + badmap_blocks;
                super_b = super_a + 1U;
        }
        for (u = 0; u < n; u++) {
                if (compact) {
                        members[u].dbx_sector = 0;
                        members[u].boot_start = 1;
                } else {
                        members[u].db0_sector = next_good(&members[u], 0);
                        members[u].db1_sector = members[u].bad_count > DB0_BAD_RUNS ?
                            next_good(&members[u], members[u].db0_sector + 1) : 0;
                        members[u].dbx_sector = next_good(&members[u],
                            members[u].db1_sector ? members[u].db1_sector + 1 :
                            members[u].db0_sector + 1);
                        members[u].boot_start = next_good(&members[u],
                            members[u].dbx_sector + 1);
                }
                members[u].boot_count = (logical_sectors + n - 1U - u) / n;
                if (members[u].boot_start >= members[u].sectors ||
                    members[u].boot_count > members[u].sectors -
                    members[u].boot_start) {
                        fprintf(stderr, "mkdsk: member %u is too small for boot stream\n", u);
                        return 1;
                }
                pathlen = snprintf(path, sizeof(path), "%s/dsk%u.dsk",
                    outdir, u);
                if (pathlen < 0 || (size_t)pathlen >= sizeof(path)) {
                        fprintf(stderr, "mkdsk: output path too long\n");
                        return 1;
                }
                files[u] = fopen(path, "wb+");
                if (files[u] == NULL) {
                        perror(path);
                        return 1;
                }
                if (fseek(files[u], (long)members[u].sectors *
                    SECTOR_WORDS * 8L - 1L,
                    SEEK_SET) != 0 || fputc(0, files[u]) == EOF) {
                        perror(path);
                        return 1;
                }
        }

        for (u = 0; u < n; u++) {
                make_dbx(sector, member_mask, logical_index[u], n, compact,
                    &members[u], d6fs_layout, super_a, super_b, swap_tail_blocks,
                    spare_blocks, logical_sectors, logical_sectors, logstore_blocks,
                    logical_sectors + logstore_blocks, badmap_blocks);
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
