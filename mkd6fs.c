#include "d6maint.h"
/* mkd6fs.c - format a D6FS V2 filesystem in a DBOOT diskset. */

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define WORD_MASK               0777777777777ULL
#define HALF_MASK               0777777U
#define BLOCK_WORDS             0200U
#define SCAN_LIMIT              0200U
#define MAX_MEMBERS             8U
#define MAX_NODES               128U
#define MAX_NAME_CHARS          24U
#define MAX_PATH_CHARS          255U
#define FCB_WORDS               020U
#define SUPER_WORDS             020U
#define DIRENT_WORDS            6U
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
#define D6FS_STATE_CLEAN        0U
#define HASH_MASK               077777777ULL
#define EXTENT_LOW_BITS         12U
#define EXTENT_LOW_MASK         07777ULL
#define EXTENT_HIGH_MASK        037ULL
#define DIRENT_HASH_SHIFT       12U
#define DIRENT_TYPE_SHIFT       9U

struct member {
        FILE *fp;
        char path[512];
        unsigned descriptor;
        unsigned base;
        unsigned blocks;
        unsigned swap_tail;
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

struct node {
        char name[MAX_NAME_CHARS + 1U];
        unsigned parent;
        unsigned type;
        unsigned mode;
        uint64_t *data;
        unsigned data_words;
        unsigned tail;
        unsigned extent_start;
        unsigned extent_blocks;
};

static struct member members[MAX_MEMBERS];
static unsigned member_count;
static struct node nodes[MAX_NODES];
static unsigned node_count;

static void die(const char *msg)
{
        fprintf(stderr, "mkd6fs: %s\n", msg);
        exit(1);
}

static void die_path(const char *path)
{
        fprintf(stderr, "mkd6fs: %s: %s\n", path, strerror(errno));
        exit(1);
}

static uint64_t mask36(uint64_t v)
{
        return v & WORD_MASK;
}

static uint64_t get64le(const unsigned char p[8])
{
        uint64_t v;
        unsigned i;

        v = 0;
        for (i = 0U; i < 8U; ++i)
                v |= (uint64_t)p[i] << (i * 8U);
        return v;
}

static void put64le(FILE *fp, uint64_t v)
{
        unsigned i;

        for (i = 0U; i < 8U; ++i)
                if (fputc((int)((v >> (i * 8U)) & 0xffU), fp) == EOF)
                        die("write failed");
}

static int read_phys(struct member *m, unsigned sector, uint64_t block[BLOCK_WORDS])
{
        unsigned char raw[8];
        unsigned i;

        if (fseek(m->fp, (long)sector * BLOCK_WORDS * 8L, SEEK_SET) != 0)
                return -1;
        for (i = 0U; i < BLOCK_WORDS; ++i) {
                if (fread(raw, 1U, 8U, m->fp) != 8U)
                        return -1;
                block[i] = get64le(raw) & WORD_MASK;
        }
        return 0;
}

static int write_phys(struct member *m, unsigned sector,
    const uint64_t block[BLOCK_WORDS])
{
        unsigned i;

        if (fseek(m->fp, (long)sector * BLOCK_WORDS * 8L, SEEK_SET) != 0)
                return -1;
        for (i = 0U; i < BLOCK_WORDS; ++i)
                put64le(m->fp, mask36(block[i]));
        return ferror(m->fp) ? -1 : 0;
}

static unsigned total_blocks(void)
{
        unsigned i, total;

        total = 0U;
        for (i = 0U; i < member_count; ++i)
                total += members[i].blocks;
        return total;
}

static int map_logical(unsigned logical, unsigned *memberp, unsigned *blockp)
{
        unsigned floor, next, width, zone, rel, slot, i;

        if (logical >= total_blocks())
                return -1;
        floor = 0U;
        for (;;) {
                next = 0U;
                width = 0U;
                for (i = 0U; i < member_count; ++i) {
                        if (members[i].blocks > floor) {
                                ++width;
                                if (next == 0U || members[i].blocks < next)
                                        next = members[i].blocks;
                        }
                }
                if (next == 0U || width == 0U)
                        return -1;
                zone = (next - floor) * width;
                if (logical < zone)
                        break;
                logical -= zone;
                floor = next;
        }
        slot = logical % width;
        rel = logical / width;
        for (i = 0U; i < member_count; ++i) {
                if (members[i].blocks <= floor)
                        continue;
                if (slot == 0U) {
                        *memberp = i;
                        *blockp = floor + rel;
                        return 0;
                }
                --slot;
        }
        return -1;
}

static int write_logical(unsigned logical, const uint64_t block[BLOCK_WORDS])
{
        unsigned mi, local;

        if (map_logical(logical, &mi, &local) != 0)
                return -1;
        return write_phys(&members[mi], members[mi].base + local, block);
}

static int sixbit_name(const char *s, uint64_t out[4])
{
        unsigned i, n, wi, shift, c;

        n = (unsigned)strlen(s);
        if (n == 0U || n > MAX_NAME_CHARS)
                return -1;
        for (i = 0U; i < 4U; ++i)
                out[i] = 0U;
        for (i = 0U; i < n; ++i) {
                c = (unsigned char)s[i];
                if (c < 040U || c > 0137U)
                        return -1;
                wi = i / 6U;
                shift = 30U - (i % 6U) * 6U;
                out[wi] |= (uint64_t)(c - 040U) << shift;
        }
        return (int)n;
}

static uint64_t name_hash(const uint64_t words[4], unsigned chars)
{
        uint64_t h, word;
        unsigned i, c;

        h = 0U;
        word = 0U;
        for (i = 0U; i < chars; ++i) {
                if ((i % 6U) == 0U)
                        word = words[i / 6U];
                c = (unsigned)(((word >> 30) & 077U) + 040U);
                word <<= 6;
                h = ((h << 5) ^ (h >> 19) ^ (c & 0377U)) & HASH_MASK;
        }
        return h;
}

static int find_child(unsigned parent, const char *name)
{
        unsigned i;

        for (i = 1U; i < node_count; ++i)
                if (nodes[i].parent == parent && strcmp(nodes[i].name, name) == 0)
                        return (int)i;
        return -1;
}

static unsigned add_node(unsigned parent, const char *name, unsigned type,
    unsigned mode)
{
        struct node *n;

        if (node_count >= MAX_NODES)
                die("too many filesystem nodes");
        if (strlen(name) > MAX_NAME_CHARS)
                die("name exceeds 24 SIXBIT characters");
        n = &nodes[node_count];
        memset(n, 0, sizeof(*n));
        strcpy(n->name, name);
        n->parent = parent;
        n->type = type;
        n->mode = mode;
        return node_count++;
}

static unsigned ensure_path_dirs(char *path, char **leafp)
{
        char *p, *slash;
        unsigned parent;
        int found;

        if (path[0] != '/')
                die("filesystem path must be absolute");
        parent = 0U;
        p = path + 1;
        for (;;) {
                slash = strchr(p, '/');
                if (slash == NULL) {
                        if (*p == '\0')
                                die("empty final path component");
                        *leafp = p;
                        return parent;
                }
                *slash = '\0';
                if (*p == '\0')
                        die("empty path component");
                found = find_child(parent, p);
                if (found < 0)
                        parent = add_node(parent, p, D6FS_TYPE_DIR, 0755U);
                else {
                        if (nodes[found].type != D6FS_TYPE_DIR)
                                die("path component is not a directory");
                        parent = (unsigned)found;
                }
                p = slash + 1;
        }
}

static uint64_t *load_dxr(const char *path, unsigned *wordsp)
{
        FILE *fp;
        long size;
        unsigned char raw[8];
        uint64_t *words;
        unsigned n, i, image, reloc;
        uint64_t dxr;

        fp = fopen(path, "rb");
        if (fp == NULL)
                die_path(path);
        if (fseek(fp, 0L, SEEK_END) != 0 || (size = ftell(fp)) < 0)
                die_path(path);
        rewind(fp);
        if (size < 16L || (size % 8L) != 0L)
                die("DXR file has invalid container length");
        n = (unsigned)(size / 8L);
        words = calloc(n, sizeof(*words));
        if (words == NULL)
                die("out of memory");
        for (i = 0U; i < n; ++i) {
                uint64_t v;

                if (fread(raw, 1U, 8U, fp) != 8U)
                        die_path(path);
                v = get64le(raw);
                if ((v & ~WORD_MASK) != 0U)
                        die("DXR word exceeds 36 bits");
                words[i] = v;
        }
        fclose(fp);
        dxr = ((uint64_t)('D' - 040) << 12) |
            ((uint64_t)('X' - 040) << 6) | (uint64_t)('R' - 040);
        if (((words[0] >> 18) & HALF_MASK) != dxr)
                die("input is not a DXR executable");
        image = (unsigned)((words[1] >> 18) & HALF_MASK);
        reloc = (image + 35U) / 36U;
        if (n != 2U + image + reloc)
                die("DXR executable has inconsistent length");
        *wordsp = n;
        return words;
}

static uint64_t *load_words(const char *path, unsigned *wordsp)
{
        FILE *fp;
        char line[256];
        uint64_t *words;
        unsigned n, cap;

        fp = fopen(path, "r");
        if (fp == NULL)
                die_path(path);
        words = NULL;
        n = 0U;
        cap = 0U;
        while (fgets(line, sizeof(line), fp) != NULL) {
                char *p, *end;
                unsigned long long v;

                p = line;
                while (*p == ' ' || *p == '\t')
                        ++p;
                if (*p == '\0' || *p == '\n' || *p == '#')
                        continue;
                errno = 0;
                v = strtoull(p, &end, 8);
                while (*end == ' ' || *end == '\t')
                        ++end;
                if (errno != 0 || v > WORD_MASK ||
                    (*end != '\0' && *end != '\n' && *end != '#'))
                        die("invalid octal word file");
                if (n == cap) {
                        uint64_t *nw;
                        cap = cap ? cap * 2U : 256U;
                        nw = realloc(words, cap * sizeof(*words));
                        if (nw == NULL)
                                die("out of memory");
                        words = nw;
                }
                words[n++] = v;
        }
        fclose(fp);
        *wordsp = n;
        return words;
}

static void parse_file_spec(const char *spec)
{
        char *copy, *path, *host, *mode_s, *enc, *leaf;
        char *p;
        unsigned parent, mode, idx;
        int found;

        copy = malloc(strlen(spec) + 1U);
        if (copy == NULL)
                die("out of memory");
        strcpy(copy, spec);
        path = copy;
        p = strchr(path, ':');
        if (p == NULL) die("file spec requires PATH:HOST:MODE:ENCODING");
        *p++ = '\0'; host = p;
        p = strchr(host, ':');
        if (p == NULL) die("file spec requires PATH:HOST:MODE:ENCODING");
        *p++ = '\0'; mode_s = p;
        p = strchr(mode_s, ':');
        if (p == NULL) die("file spec requires PATH:HOST:MODE:ENCODING");
        *p++ = '\0'; enc = p;
        mode = (unsigned)strtoul(mode_s, NULL, 8);
        if (mode > 07777U)
                die("invalid file mode");
        parent = ensure_path_dirs(path, &leaf);
        found = find_child(parent, leaf);
        if (found >= 0)
                die("duplicate filesystem path");
        idx = add_node(parent, leaf, D6FS_TYPE_REG, mode);
        if (strcmp(enc, "dxr") == 0)
                nodes[idx].data = load_dxr(host, &nodes[idx].data_words);
        else if (strcmp(enc, "words") == 0)
                nodes[idx].data = load_words(host, &nodes[idx].data_words);
        else
                die("unsupported encoding (use dxr or words)");
        nodes[idx].tail = nodes[idx].data_words == 0U ? 0U : 4U;
        free(copy);
}

static void parse_symlink_spec(const char *spec)
{
        char *copy, *path, *target, *mode_s, *leaf, *p;
        unsigned parent, mode, idx, chars, i, wi, shift, c;

        copy = malloc(strlen(spec) + 1U);
        if (copy == NULL)
                die("out of memory");
        strcpy(copy, spec);
        path = copy;
        p = strchr(path, ':');
        if (p == NULL)
                die("symlink spec requires PATH:TARGET:MODE");
        *p++ = '\0';
        target = p;
        p = strrchr(target, ':');
        if (p == NULL)
                die("symlink spec requires PATH:TARGET:MODE");
        *p++ = '\0';
        mode_s = p;
        mode = (unsigned)strtoul(mode_s, NULL, 8);
        chars = (unsigned)strlen(target);
        if (mode > 07777U || chars == 0U || chars > MAX_PATH_CHARS)
                die("invalid symlink mode or target length");
        parent = ensure_path_dirs(path, &leaf);
        if (find_child(parent, leaf) >= 0)
                die("duplicate filesystem path");
        idx = add_node(parent, leaf, D6FS_TYPE_SYMLINK, mode);
        nodes[idx].data_words = (chars + 5U) / 6U;
        nodes[idx].tail = chars - (nodes[idx].data_words - 1U) * 6U;
        nodes[idx].data = calloc(nodes[idx].data_words, sizeof(uint64_t));
        if (nodes[idx].data == NULL)
                die("out of memory");
        for (i = 0U; i < chars; ++i) {
                c = (unsigned char)target[i];
                if (c < 040U || c > 0137U)
                        die("symlink target is not SIXBIT representable");
                wi = i / 6U;
                shift = 30U - (i % 6U) * 6U;
                nodes[idx].data[wi] |= (uint64_t)(c - 040U) << shift;
        }
        free(copy);
}

static void scan_member(struct member *m)
{
        uint64_t block[BLOCK_WORDS];
        unsigned s;

        for (s = 0U; s < SCAN_LIMIT; ++s) {
                if (read_phys(m, s, block) != 0)
                        die("cannot scan DBOOT descriptor");
                if (block[D6FS_LAYOUT_MAGIC_WORD] != D6FS_LAYOUT_MAGIC)
                        continue;
                m->descriptor = s;
                m->base = (unsigned)((block[D6FS_LAYOUT_RANGE_WORD] >> 18) & HALF_MASK);
                m->blocks = (unsigned)(block[D6FS_LAYOUT_RANGE_WORD] & HALF_MASK);
                m->swap_tail = (unsigned)block[D6FS_LAYOUT_SWAP_TAIL];
                return;
        }
        die("D6FS root-layout descriptor not found");
}

static void open_members(const char *dir, unsigned n, unsigned *super_ap,
    unsigned *super_bp)
{
        uint64_t block[BLOCK_WORDS];
        unsigned i, sa, sb;

        member_count = n;
        sa = sb = 0U;
        for (i = 0U; i < n; ++i) {
                int len;
                len = snprintf(members[i].path, sizeof(members[i].path),
                    "%s/dsk%u.dsk", dir, i);
                if (len < 0 || (size_t)len >= sizeof(members[i].path))
                        die("disk path too long");
                members[i].fp = fopen(members[i].path, "rb+");
                if (members[i].fp == NULL)
                        die_path(members[i].path);
                scan_member(&members[i]);
                if (read_phys(&members[i], members[i].descriptor, block) != 0)
                        die("cannot reread DBOOT descriptor");
                if (i == 0U) {
                        sa = (unsigned)block[D6FS_LAYOUT_SUPER_A];
                        sb = (unsigned)block[D6FS_LAYOUT_SUPER_B];
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
                        disk_layout.swap_tail_blocks =
                            (unsigned)block[D6FS_LAYOUT_SWAP_TAIL];
                } else if (sa != (unsigned)block[D6FS_LAYOUT_SUPER_A] ||
                    sb != (unsigned)block[D6FS_LAYOUT_SUPER_B] ||
                    disk_layout.bootstream_blocks !=
                    (unsigned)block[D6FS_LAYOUT_BOOTSTREAM_BLOCKS] ||
                    disk_layout.logstore_start !=
                    (unsigned)block[D6FS_LAYOUT_LOGSTORE_START] ||
                    disk_layout.logstore_blocks !=
                    (unsigned)block[D6FS_LAYOUT_LOGSTORE_BLOCKS] ||
                    disk_layout.badmap_start !=
                    (unsigned)block[D6FS_LAYOUT_BADMAP_START] ||
                    disk_layout.badmap_blocks !=
                    (unsigned)block[D6FS_LAYOUT_BADMAP_BLOCKS] ||
                    disk_layout.swap_tail_blocks !=
                    (unsigned)block[D6FS_LAYOUT_SWAP_TAIL])
                        die("members disagree on D6FS reserved layout");
        }
        if (sa == sb || sa >= total_blocks() || sb >= total_blocks())
                die("invalid D6FS superblock locations");
        if (disk_layout.bootstream_blocks != 0U ||
            disk_layout.logstore_start != 0U || disk_layout.logstore_blocks != 0U ||
            disk_layout.badmap_start != 0U || disk_layout.badmap_blocks != 0U) {
                if (disk_layout.logstore_start != disk_layout.bootstream_blocks ||
                    disk_layout.badmap_start != disk_layout.logstore_start +
                    disk_layout.logstore_blocks ||
                    sa != disk_layout.badmap_start + disk_layout.badmap_blocks ||
                    sb != sa + 1U)
                        die("invalid D6FS reserved-range layout");
        } else {
                disk_layout.bootstream_blocks = sa;
                disk_layout.logstore_start = sa;
                disk_layout.logstore_blocks = 0U;
                disk_layout.badmap_start = sa;
                disk_layout.badmap_blocks = 0U;
                fprintf(stderr, "mkd6fs: warning: legacy D6FSR2 layout lacks explicit reserved ranges\n");
        }
        *super_ap = sa;
        *super_bp = sb;
}

static uint64_t extent_word(unsigned start, unsigned blocks, unsigned *highp)
{
        uint64_t count;

        if (blocks == 0U)
                die("zero-length extent");
        count = (uint64_t)blocks - 1U;
        *highp = (unsigned)((count >> EXTENT_LOW_BITS) & EXTENT_HIGH_MASK);
        return ((uint64_t)start << EXTENT_LOW_BITS) | (count & EXTENT_LOW_MASK);
}

static void encode_fcb(uint64_t out[FCB_WORDS], const struct node *n)
{
        unsigned high, i;
        uint64_t lenhigh;

        memset(out, 0, FCB_WORDS * sizeof(*out));
        out[0] = ((uint64_t)n->type << 33) | ((uint64_t)n->mode << 12) |
            ((uint64_t)n->tail << 8) |
            ((uint64_t)(n->extent_blocks == 0U ? 0U : 1U) << 4);
        out[2] = n->data_words;
        out[4] = (uint64_t)n->parent << 18;
        if (n->extent_blocks != 0U) {
                out[6] = extent_word(n->extent_start, n->extent_blocks, &high);
                lenhigh = (uint64_t)high;
                out[5] = lenhigh;
        }
        for (i = 0U; i < FCB_WORDS; ++i)
                out[i] &= WORD_MASK;
}

static void encode_dirent(uint64_t out[DIRENT_WORDS], const struct node *child,
    unsigned child_index)
{
        uint64_t name[4];
        int chars;
        unsigned i;

        chars = sixbit_name(child->name, name);
        if (chars < 0)
                die("invalid SIXBIT filename");
        for (i = 0U; i < 4U; ++i)
                out[i] = name[i];
        out[4] = (name_hash(name, (unsigned)chars) << DIRENT_HASH_SHIFT) |
            ((uint64_t)child->type << DIRENT_TYPE_SHIFT);
        out[5] = (uint64_t)child_index << 18;
}

static void build_directories(void)
{
        unsigned i, j, children, w;

        for (i = 0U; i < node_count; ++i) {
                if (nodes[i].type != D6FS_TYPE_DIR)
                        continue;
                children = 0U;
                for (j = 1U; j < node_count; ++j)
                        if (nodes[j].parent == i)
                                ++children;
                nodes[i].data_words = children * DIRENT_WORDS;
                nodes[i].tail = nodes[i].data_words == 0U ? 0U : 4U;
                if (nodes[i].data_words == 0U)
                        continue;
                nodes[i].data = calloc(nodes[i].data_words, sizeof(uint64_t));
                if (nodes[i].data == NULL)
                        die("out of memory");
                w = 0U;
                for (j = 1U; j < node_count; ++j) {
                        if (nodes[j].parent != i)
                                continue;
                        encode_dirent(nodes[i].data + w, &nodes[j], j);
                        w += DIRENT_WORDS;
                }
        }
}

static void set_allocated(uint64_t *freemap, unsigned logical)
{
        unsigned mbi, in, wi, bi;
        uint64_t *block;

        mbi = logical / BITS_PER_MAP_BLOCK;
        in = logical % BITS_PER_MAP_BLOCK;
        wi = in / 36U;
        bi = in % 36U;
        block = freemap + (size_t)mbi * BLOCK_WORDS;
        block[wi] |= (uint64_t)1U << (35U - bi);
}

static int map_block_has_free(const uint64_t *freemap, unsigned mbi,
    unsigned total)
{
        unsigned first, limit, logical, in, wi, bi;
        const uint64_t *block;

        first = mbi * BITS_PER_MAP_BLOCK;
        limit = first + BITS_PER_MAP_BLOCK;
        if (limit > total)
                limit = total;
        block = freemap + (size_t)mbi * BLOCK_WORDS;
        for (logical = first; logical < limit; ++logical) {
                in = logical - first;
                wi = in / 36U;
                bi = in % 36U;
                if ((block[wi] & ((uint64_t)1U << (35U - bi))) == 0U)
                        return 1;
        }
        return 0;
}

static uint64_t deterministic_id(uint64_t seed, unsigned total)
{
        uint64_t h;
        unsigned i;

        h = seed & WORD_MASK;
        for (i = 0U; i < node_count; ++i) {
                const unsigned char *p = (const unsigned char *)nodes[i].name;
                while (*p != 0U) {
                        h = ((h << 5) ^ (h >> 31) ^ *p++) & WORD_MASK;
                }
        }
        h ^= total;
        return h & WORD_MASK;
}

static void initialize_badmap(void)
{
        uint64_t block[BLOCK_WORDS];
        unsigned b;

        if (disk_layout.badmap_blocks == 0U)
                return;
        memset(block, 0, sizeof(block));
        block[0] = D6FS_BADMAP_MAGIC;
        block[1] = 1U;
        block[2] = 0U;
        if (write_logical(disk_layout.badmap_start, block) != 0)
                die("cannot initialize D6FS bad-block table");
        memset(block, 0, sizeof(block));
        for (b = 1U; b < disk_layout.badmap_blocks; ++b)
                if (write_logical(disk_layout.badmap_start + b, block) != 0)
                        die("cannot clear D6FS bad-block table");
}

static void format_fs(unsigned super_a, unsigned super_b)
{
        unsigned total, fcb_count, fcb_blocks, freemap_blocks, summary_blocks;
        unsigned fcb_start, freemap_start, summary_start, data_cursor;
        unsigned i, j, b, need, word_index, inblock, mbi;
        uint64_t block[BLOCK_WORDS];
        uint64_t fcb[FCB_WORDS];
        uint64_t *freemap, *summary;
        uint64_t fsid0, fsid1, dsid0, dsid1;

        total = total_blocks();
        fcb_count = 64U;
        if (node_count > fcb_count)
                die("filesystem needs more than 64 FCBs");
        fcb_blocks = (fcb_count * FCB_WORDS + BLOCK_WORDS - 1U) / BLOCK_WORDS;
        freemap_blocks = (total + BITS_PER_MAP_BLOCK - 1U) / BITS_PER_MAP_BLOCK;
        summary_blocks = (freemap_blocks + BITS_PER_MAP_BLOCK - 1U) /
            BITS_PER_MAP_BLOCK;
        fcb_start = (super_a > super_b ? super_a : super_b) + 1U;
        freemap_start = fcb_start + fcb_blocks;
        summary_start = freemap_start + freemap_blocks;
        data_cursor = summary_start + summary_blocks;
        if (data_cursor >= total)
                die("diskset too small for D6FS metadata");

        initialize_badmap();
        build_directories();
        for (i = 0U; i < node_count; ++i) {
                if (nodes[i].data_words == 0U)
                        continue;
                need = (nodes[i].data_words + BLOCK_WORDS - 1U) / BLOCK_WORDS;
                if (need > total - data_cursor)
                        die("diskset is full while placing files");
                nodes[i].extent_start = data_cursor;
                nodes[i].extent_blocks = need;
                data_cursor += need;
        }

        freemap = calloc((size_t)freemap_blocks * BLOCK_WORDS,
            sizeof(uint64_t));
        summary = calloc((size_t)summary_blocks * BLOCK_WORDS,
            sizeof(uint64_t));
        if (freemap == NULL || summary == NULL)
                die("out of memory");
        for (i = 0U; i < data_cursor; ++i)
                set_allocated(freemap, i);
        for (mbi = 0U; mbi < freemap_blocks; ++mbi)
                if (map_block_has_free(freemap, mbi, total)) {
                        unsigned sbi = mbi / BITS_PER_MAP_BLOCK;
                        unsigned in = mbi % BITS_PER_MAP_BLOCK;
                        summary[(size_t)sbi * BLOCK_WORDS + in / 36U] |=
                            (uint64_t)1U << (35U - (in % 36U));
                }

        for (i = 0U; i < node_count; ++i) {
                for (b = 0U; b < nodes[i].extent_blocks; ++b) {
                        unsigned off = b * BLOCK_WORDS;
                        memset(block, 0, sizeof(block));
                        for (j = 0U; j < BLOCK_WORDS && off + j < nodes[i].data_words;
                            ++j)
                                block[j] = nodes[i].data[off + j];
                        if (write_logical(nodes[i].extent_start + b, block) != 0)
                                die("cannot write file data");
                }
        }

        for (b = 0U; b < fcb_blocks; ++b) {
                memset(block, 0, sizeof(block));
                for (i = 0U; i < fcb_count; ++i) {
                        word_index = i * FCB_WORDS;
                        if (word_index / BLOCK_WORDS != b)
                                continue;
                        inblock = word_index % BLOCK_WORDS;
                        if (i < node_count)
                                encode_fcb(fcb, &nodes[i]);
                        else
                                memset(fcb, 0, sizeof(fcb));
                        for (j = 0U; j < FCB_WORDS; ++j)
                                block[inblock + j] = fcb[j];
                }
                if (write_logical(fcb_start + b, block) != 0)
                        die("cannot write FCB table");
        }
        for (b = 0U; b < freemap_blocks; ++b)
                if (write_logical(freemap_start + b,
                    freemap + (size_t)b * BLOCK_WORDS) != 0)
                        die("cannot write free map");
        for (b = 0U; b < summary_blocks; ++b)
                if (write_logical(summary_start + b,
                    summary + (size_t)b * BLOCK_WORDS) != 0)
                        die("cannot write free summary");

        fsid0 = deterministic_id(012345670123ULL, total);
        fsid1 = deterministic_id(076543210765ULL, total);
        dsid0 = deterministic_id(011111122222ULL, total);
        dsid1 = deterministic_id(033333344444ULL, total);
        memset(block, 0, sizeof(block));
        block[0] = (D6FS_MAGIC & ~077ULL) | D6FS_VERSION;
        block[1] = 1U;
        block[2] = D6FS_STATE_CLEAN;
        block[3] = fsid0; block[4] = fsid1;
        block[5] = dsid0; block[6] = dsid1;
        block[7] = total;
        block[010] = 0U;
        block[011] = fcb_start;
        block[012] = fcb_count;
        block[013] = freemap_start;
        block[014] = freemap_blocks;
        block[015] = summary_start;
        block[016] = summary_blocks;
        if (write_logical(super_a, block) != 0 ||
            write_logical(super_b, block) != 0)
                die("cannot write superblocks");

        free(freemap);
        free(summary);
        fprintf(stderr,
            "mkd6fs: %u blocks, %u nodes, boot=%o log=%o+%o badmap=%o+%o "
            "super=%o/%o data=%o swap-tail/member=%o\n",
            total, node_count, disk_layout.bootstream_blocks,
            disk_layout.logstore_start, disk_layout.logstore_blocks,
            disk_layout.badmap_start, disk_layout.badmap_blocks, super_a, super_b,
            summary_start + summary_blocks, disk_layout.swap_tail_blocks);
}

static void usage(void)
{
        fprintf(stderr,
            "usage: mkd6fs -n members -d diskdir "
            "[-f PATH:HOST:MODE:dxr|words] [-l PATH:TARGET:MODE] [...]\n");
        exit(2);
}

int main(int argc, char **argv)
{
        const char *dir;
        const char *specs[MAX_NODES];
        const char *links[MAX_NODES];
        unsigned nspec, nlink, n, super_a, super_b, i;
        int a;

        dir = NULL;
        nspec = 0U;
        nlink = 0U;
        n = 0U;
        memset(nodes, 0, sizeof(nodes));
        node_count = 1U;
        nodes[0].type = D6FS_TYPE_DIR;
        nodes[0].mode = 0755U;
        nodes[0].parent = 0U;
        for (a = 1; a < argc; ++a) {
                if (strcmp(argv[a], "-n") == 0 && a + 1 < argc)
                        n = (unsigned)strtoul(argv[++a], NULL, 0);
                else if (strcmp(argv[a], "-d") == 0 && a + 1 < argc)
                        dir = argv[++a];
                else if (strcmp(argv[a], "-f") == 0 && a + 1 < argc) {
                        if (nspec >= MAX_NODES)
                                die("too many file specs");
                        specs[nspec++] = argv[++a];
                } else if (strcmp(argv[a], "-l") == 0 && a + 1 < argc) {
                        if (nlink >= MAX_NODES)
                                die("too many symlink specs");
                        links[nlink++] = argv[++a];
                } else
                        usage();
        }
        if (dir == NULL || n == 0U || n > MAX_MEMBERS ||
            (nspec == 0U && nlink == 0U))
                usage();
        for (i = 0U; i < nspec; ++i)
                parse_file_spec(specs[i]);
        for (i = 0U; i < nlink; ++i)
                parse_symlink_spec(links[i]);
        open_members(dir, n, &super_a, &super_b);
        format_fs(super_a, super_b);
        for (i = 0U; i < member_count; ++i)
                if (fclose(members[i].fp) != 0)
                        die_path(members[i].path);
        for (i = 0U; i < node_count; ++i)
                free(nodes[i].data);
        if (d6m_run_fsck(argv[0], dir, n, 0) != 0)
                die("post-format d6fsck failed");
        return 0;
}
