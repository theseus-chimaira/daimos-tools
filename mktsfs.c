/* mktsfs.c - create a TSFS DECtape set. */

#include "tsfs-format.h"
#include "d6lz-codec.h"

#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MANIFEST_LINE_MAX 1024
#define PATH_MAX_LOCAL 102U

struct tsfs_id {
        uint64_t hi;
        uint64_t lo;
};

struct manifest_node {
        char path[PATH_MAX_LOCAL + 1U];
        char name[TSFS_FILE_NAME_CHARS + 1U];
        char source[MANIFEST_LINE_MAX];
        int is_dir;
        int parent_node;
        unsigned int record_index;
        unsigned int child_start;
        unsigned int child_count;
        unsigned int size_words;
        unsigned int first_extent;
        unsigned int extent_count;
        unsigned int mode;
        unsigned int uid;
        unsigned int gid;
};

static void
usage(void)
{
        fprintf(stderr,
            "usage: mktsfs -n members -i hi:lo [-g generation] [-m manifest] -o prefix\n");
        exit(2);
}

static int
parse_octal_word(const char *text, uint64_t *value)
{
        char *end;
        unsigned long v;

        errno = 0;
        v = strtoul(text, &end, 8);
        if (errno != 0 || end == text || *end != '\0' ||
            (v & ~TSFS_WORD_MASK) != 0)
                return -1;
        *value = (uint64_t)v;
        return 0;
}

static int
parse_uint_limited(const char *text, int base, unsigned int limit,
    unsigned int *value)
{
        char *end;
        unsigned long v;

        if (text == NULL || *text == '\0')
                return -1;
        errno = 0;
        v = strtoul(text, &end, base);
        if (errno != 0 || end == text || *end != '\0' || v > limit)
                return -1;
        *value = (unsigned int)v;
        return 0;
}

static int
parse_id(const char *text, struct tsfs_id *id)
{
        char buf[128];
        char *colon;

        if (text == NULL || id == NULL || strlen(text) >= sizeof(buf))
                return -1;
        strcpy(buf, text);
        colon = strchr(buf, ':');
        if (colon == NULL)
                return -1;
        *colon++ = '\0';
        if (parse_octal_word(buf, &id->hi) != 0 ||
            parse_octal_word(colon, &id->lo) != 0)
                return -1;
        return 0;
}

static uint64_t
get64le(const unsigned char b[8])
{
        uint64_t v;
        unsigned int i;

        v = 0U;
        for (i = 0U; i < 8U; ++i)
                v |= (uint64_t)b[i] << (8U * i);
        return v;
}

static int
source_word_count(const char *path, unsigned int *wordsp)
{
        FILE *fp;
        long size;

        *wordsp = 0U;
        if (path == NULL || path[0] == '\0')
                return 0;
        fp = fopen(path, "rb");
        if (fp == NULL)
                return -1;
        if (fseek(fp, 0L, SEEK_END) != 0) {
                fclose(fp);
                return -1;
        }
        size = ftell(fp);
        if (size < 0) {
                fclose(fp);
                return -1;
        }
        if (fclose(fp) != 0)
                return -1;
        if ((size & 7L) != 0L ||
            (unsigned long)size / 8UL > (unsigned long)TSFS_HALF_MASK)
                return -1;
        *wordsp = (unsigned int)((unsigned long)size / 8UL);
        return 0;
}

static int
read_source_words(const char *path, uint64_t **wordsp, unsigned int words)
{
        FILE *fp;
        uint64_t *data;
        unsigned int i;
        unsigned char b[8];

        *wordsp = NULL;
        if (words == 0U)
                return 0;
        fp = fopen(path, "rb");
        if (fp == NULL)
                return -1;
        data = (uint64_t *)malloc((size_t)words * sizeof(*data));
        if (data == NULL) {
                fclose(fp);
                return -1;
        }
        for (i = 0U; i < words; ++i) {
                if (fread(b, 1U, 8U, fp) != 8U) {
                        free(data);
                        fclose(fp);
                        return -1;
                }
                data[i] = get64le(b);
                if ((data[i] & ~TSFS_WORD_MASK) != 0U) {
                        free(data);
                        fclose(fp);
                        return -1;
                }
        }
        if (fgetc(fp) != EOF || ferror(fp)) {
                free(data);
                fclose(fp);
                return -1;
        }
        if (fclose(fp) != 0) {
                free(data);
                return -1;
        }
        *wordsp = data;
        return 0;
}

static int
sixbit_char_ok(unsigned char ch)
{
        return ch >= 040U && ch <= 0137U;
}

static int
path_valid(const char *path)
{
        const char *p;
        unsigned int n;

        if (path == NULL || path[0] != '/' || path[1] == '\0' ||
            strlen(path) > PATH_MAX_LOCAL)
                return 0;
        p = path + 1;
        n = 0U;
        while (*p != '\0') {
                unsigned char ch;

                ch = (unsigned char)*p++;
                if (ch == '/') {
                        if (n == 0U || n > TSFS_FILE_NAME_CHARS)
                                return 0;
                        n = 0U;
                        continue;
                }
                if (!sixbit_char_ok(ch))
                        return 0;
                ++n;
        }
        return n != 0U && n <= TSFS_FILE_NAME_CHARS;
}

static void
path_parent(const char *path, char *parent, char *name)
{
        const char *slash;
        size_t plen;

        slash = strrchr(path, '/');
        strcpy(name, slash + 1);
        if (slash == path) {
                strcpy(parent, "/");
                return;
        }
        plen = (size_t)(slash - path);
        memcpy(parent, path, plen);
        parent[plen] = '\0';
}

static int
find_node(const struct manifest_node *nodes, unsigned int count,
    const char *path)
{
        unsigned int i;

        for (i = 0U; i < count; ++i)
                if (strcmp(nodes[i].path, path) == 0)
                        return (int)i;
        return -1;
}

static int
child_name_compare(const void *ap, const void *bp)
{
        const struct manifest_node *const *a;
        const struct manifest_node *const *b;

        a = (const struct manifest_node *const *)ap;
        b = (const struct manifest_node *const *)bp;
        return strcmp((*a)->name, (*b)->name);
}

static int
read_manifest(const char *path, struct manifest_node **nodesp,
    unsigned int *countp)
{
        FILE *fp;
        struct manifest_node *nodes;
        unsigned int count;
        unsigned int cap;
        char line[MANIFEST_LINE_MAX];
        unsigned int lineno;

        *nodesp = NULL;
        *countp = 0U;
        if (path == NULL)
                return 0;
        fp = fopen(path, "r");
        if (fp == NULL)
                return -1;
        nodes = NULL;
        count = 0U;
        cap = 0U;
        lineno = 0U;
        while (fgets(line, sizeof(line), fp) != NULL) {
                char *p;
                char *end;
                int is_dir;
                size_t len;
                char parent[PATH_MAX_LOCAL + 1U];
                char name[TSFS_FILE_NAME_CHARS + 1U];
                int parent_node;
                struct manifest_node *tmp;

                ++lineno;
                len = strlen(line);
                while (len != 0U && (line[len - 1U] == '\n' ||
                    line[len - 1U] == '\r'))
                        line[--len] = '\0';
                p = line;
                while (*p == ' ' || *p == '\t')
                        ++p;
                if (*p == '\0' || *p == '#')
                        continue;
                if ((p[0] != 'D' && p[0] != 'F') ||
                    (p[1] != ' ' && p[1] != '\t')) {
                        fprintf(stderr, "mktsfs: manifest line %u: expected D or F\n",
                            lineno);
                        free(nodes);
                        fclose(fp);
                        return -1;
                }
                is_dir = p[0] == 'D';
                p += 2;
                while (*p == ' ' || *p == '\t')
                        ++p;
                end = p;
                while (*end != '\0' && *end != ' ' && *end != '\t')
                        ++end;
                if (*end != '\0') {
                        *end++ = '\0';
                        while (*end == ' ' || *end == '\t')
                                ++end;
                }
                if (!path_valid(p) || find_node(nodes, count, p) >= 0) {
                        fprintf(stderr, "mktsfs: manifest line %u: invalid or duplicate path\n",
                            lineno);
                        free(nodes);
                        fclose(fp);
                        return -1;
                }
                path_parent(p, parent, name);
                if (strcmp(parent, "/") == 0)
                        parent_node = -1;
                else {
                        parent_node = find_node(nodes, count, parent);
                        if (parent_node < 0 || !nodes[parent_node].is_dir) {
                                fprintf(stderr,
                                    "mktsfs: manifest line %u: parent directory must appear first\n",
                                    lineno);
                                free(nodes);
                                fclose(fp);
                                return -1;
                        }
                }
                if (count == cap) {
                        unsigned int newcap;

                        newcap = cap == 0U ? 16U : cap * 2U;
                        tmp = (struct manifest_node *)realloc(nodes,
                            (size_t)newcap * sizeof(*nodes));
                        if (tmp == NULL) {
                                free(nodes);
                                fclose(fp);
                                return -1;
                        }
                        nodes = tmp;
                        cap = newcap;
                }
                memset(&nodes[count], 0, sizeof(nodes[count]));
                strcpy(nodes[count].path, p);
                strcpy(nodes[count].name, name);
                nodes[count].mode = is_dir ? 0555U : 0444U;
                nodes[count].uid = 0U;
                nodes[count].gid = 0U;
                {
                        char *field[4];
                        unsigned int nf;
                        char *q;

                        nf = 0U;
                        q = end;
                        while (*q != '\0') {
                                while (*q == ' ' || *q == '\t')
                                        ++q;
                                if (*q == '\0')
                                        break;
                                if (nf == 4U)
                                        break;
                                field[nf++] = q;
                                while (*q != '\0' && *q != ' ' && *q != '\t')
                                        ++q;
                                if (*q != '\0')
                                        *q++ = '\0';
                        }
                        while (*q == ' ' || *q == '\t')
                                ++q;
                        if (*q != '\0' ||
                            (is_dir && nf != 0U && nf != 3U) ||
                            (!is_dir && nf != 1U && nf != 4U)) {
                                fprintf(stderr,
                                    "mktsfs: manifest line %u: expected D path [mode uid gid] or F path source [mode uid gid]\n",
                                    lineno);
                                free(nodes);
                                fclose(fp);
                                return -1;
                        }
                        if (!is_dir) {
                                if (strlen(field[0]) >= sizeof(nodes[count].source)) {
                                        fprintf(stderr,
                                            "mktsfs: manifest line %u: source path too long\n",
                                            lineno);
                                        free(nodes);
                                        fclose(fp);
                                        return -1;
                                }
                                strcpy(nodes[count].source, field[0]);
                        }
                        if ((is_dir && nf == 3U) || (!is_dir && nf == 4U)) {
                                unsigned int ai;
                                ai = is_dir ? 0U : 1U;
                                if (parse_uint_limited(field[ai], 8,
                                    TSFS_FILE_MODE_MASK, &nodes[count].mode) != 0 ||
                                    parse_uint_limited(field[ai + 1U], 10,
                                    TSFS_FILE_ID_MASK, &nodes[count].uid) != 0 ||
                                    parse_uint_limited(field[ai + 2U], 10,
                                    TSFS_FILE_ID_MASK, &nodes[count].gid) != 0) {
                                        fprintf(stderr,
                                            "mktsfs: manifest line %u: invalid mode/uid/gid\n",
                                            lineno);
                                        free(nodes);
                                        fclose(fp);
                                        return -1;
                                }
                        }
                }
                nodes[count].is_dir = is_dir;
                nodes[count].parent_node = parent_node;
                ++count;
        }
        if (ferror(fp)) {
                free(nodes);
                fclose(fp);
                return -1;
        }
        fclose(fp);
        *nodesp = nodes;
        *countp = count;
        return 0;
}

static int
assign_records(struct manifest_node *nodes, unsigned int count,
    unsigned int **record_to_nodep)
{
        unsigned int *record_to_node;
        unsigned int next;
        unsigned int parent_record;

        if (count == 0U) {
                *record_to_nodep = NULL;
                return 0;
        }
        record_to_node = (unsigned int *)malloc((size_t)count *
            sizeof(*record_to_node));
        if (record_to_node == NULL)
                return -1;
        next = 1U;
        parent_record = 0U;
        while (parent_record < next) {
                int parent_node;
                struct manifest_node **children;
                unsigned int nchild;
                unsigned int i;

                parent_node = parent_record == 0U ? -1 :
                    (int)record_to_node[parent_record - 1U];
                children = (struct manifest_node **)malloc((size_t)count *
                    sizeof(*children));
                if (children == NULL) {
                        free(record_to_node);
                        return -1;
                }
                nchild = 0U;
                for (i = 0U; i < count; ++i)
                        if (nodes[i].parent_node == parent_node)
                                children[nchild++] = &nodes[i];
                qsort(children, nchild, sizeof(*children), child_name_compare);
                if (parent_record != 0U) {
                        struct manifest_node *parent;

                        parent = &nodes[record_to_node[parent_record - 1U]];
                        parent->child_start = next;
                        parent->child_count = nchild;
                }
                for (i = 0U; i < nchild; ++i) {
                        unsigned int node_index;

                        node_index = (unsigned int)(children[i] - nodes);
                        children[i]->record_index = next;
                        record_to_node[next - 1U] = node_index;
                        ++next;
                }
                if (parent_record == 0U && nchild != 0U) {
                        /* Root's child range is emitted directly later. */
                }
                free(children);
                ++parent_record;
        }
        if (next != count + 1U) {
                free(record_to_node);
                return -1;
        }
        *record_to_nodep = record_to_node;
        return 0;
}

static uint64_t
pack_sixbit6(const char *text, unsigned int start, unsigned int nchars)
{
        char six[6];
        unsigned int i;

        for (i = 0U; i < 6U; ++i) {
                unsigned int pos;

                pos = start + i;
                six[i] = pos < nchars ? text[pos] : ' ';
        }
        return tsfs_sixbit_word(six);
}

static void
build_file_record(uint64_t rec[TSFS_FILE_WORDS],
    const struct manifest_node *node, unsigned int parent_record)
{
        unsigned int i;
        unsigned int nchars;
        uint64_t flags;

        memset(rec, 0, TSFS_FILE_WORDS * sizeof(rec[0]));
        flags = node->is_dir ? TSFS_FILE_FLAG_DIR : TSFS_FILE_FLAG_REG;
        flags |= (uint64_t)(node->mode & TSFS_FILE_MODE_MASK) <<
            TSFS_FILE_MODE_SHIFT;
        rec[TSFS_FILE_PARENT_FLAGS] = ((uint64_t)parent_record << 18) | flags;
        nchars = (unsigned int)strlen(node->name);
        for (i = 0U; i < TSFS_FILE_NAME_WORDS; ++i)
                rec[TSFS_FILE_NAME0 + i] = pack_sixbit6(node->name,
                    i * 6U, nchars);
        if (node->is_dir) {
                rec[TSFS_FILE_SIZE_WORDS] =
                    ((uint64_t)node->uid << TSFS_FILE_OWNER_SHIFT) | node->gid;
                rec[TSFS_FILE_EXTENT_RANGE] = 0;
                rec[TSFS_FILE_AUX] = node->child_count == 0U ? 0U :
                    ((uint64_t)node->child_start << 18) | node->child_count;
        } else {
                rec[TSFS_FILE_SIZE_WORDS] = node->size_words;
                rec[TSFS_FILE_EXTENT_RANGE] = node->extent_count == 0U ? 0U :
                    ((uint64_t)node->first_extent << 18) | node->extent_count;
                rec[TSFS_FILE_AUX] =
                    ((uint64_t)node->uid << TSFS_FILE_OWNER_SHIFT) | node->gid;
        }
}

static void
build_root_record(uint64_t rec[TSFS_FILE_WORDS],
    unsigned int child_count)
{
        memset(rec, 0, TSFS_FILE_WORDS * sizeof(rec[0]));
        rec[TSFS_FILE_PARENT_FLAGS] =
            ((uint64_t)0555U << TSFS_FILE_MODE_SHIFT) | TSFS_FILE_FLAG_DIR;
        rec[TSFS_FILE_SIZE_WORDS] = 0U;   /* root owner 0:0 */
        rec[TSFS_FILE_AUX] = ((uint64_t)(child_count == 0U ? 0U : 1U) << 18) |
            child_count;
}

static void
build_tdir(uint64_t block[TSFS_BLOCK_WORDS], unsigned int file_count,
    unsigned int file_member, unsigned int file_block,
    unsigned int file_blocks, uint64_t file_checksum,
    unsigned int extent_count, unsigned int extent_member,
    unsigned int extent_block, unsigned int extent_blocks,
    uint64_t extent_checksum)
{
        uint64_t *e;

        memset(block, 0, TSFS_BLOCK_WORDS * sizeof(block[0]));
        block[TSFS_TDIR_MAGIC_WORD] = tsfs_sixbit_word("TSDIR ");
        block[TSFS_TDIR_VERSION_WORD] =
            ((uint64_t)TSFS_FORMAT_MAJOR << 18) | TSFS_FORMAT_MINOR;
        block[TSFS_TDIR_FLAGS_WORD] = 0;
        block[TSFS_TDIR_FILE_COUNT_WORD] = file_count;
        block[TSFS_TDIR_EXTENT_COUNT_WORD] = extent_count;
        block[TSFS_TDIR_PATH_WORDS_WORD] = 0;
        e = block + TSFS_TDIR_USED_WORDS;
        if (file_count != 0U) {
                e[TSFS_TDIRE_ID_FLAGS] = (uint64_t)TSFS_TABLE_FILE << 18;
                e[TSFS_TDIRE_MEMBER_BLOCK] =
                    ((uint64_t)file_member << 18) | file_block;
                e[TSFS_TDIRE_BLOCKS_RECWORDS] =
                    ((uint64_t)file_blocks << 18) | TSFS_FILE_WORDS;
                e[TSFS_TDIRE_RECORD_COUNT] = file_count;
                e[TSFS_TDIRE_CHECKSUM] = file_checksum;
                e += TSFS_TDIRE_WORDS;
        }
        if (extent_count != 0U) {
                e[TSFS_TDIRE_ID_FLAGS] = (uint64_t)TSFS_TABLE_EXTENT << 18;
                e[TSFS_TDIRE_MEMBER_BLOCK] =
                    ((uint64_t)extent_member << 18) | extent_block;
                e[TSFS_TDIRE_BLOCKS_RECWORDS] =
                    ((uint64_t)extent_blocks << 18) | TSFS_EXTENT_WORDS;
                e[TSFS_TDIRE_RECORD_COUNT] = extent_count;
                e[TSFS_TDIRE_CHECKSUM] = extent_checksum;
        }
        block[TSFS_TDIR_CHECKSUM_WORD] = tsfs_checksum36(block,
            TSFS_BLOCK_WORDS, TSFS_TDIR_CHECKSUM_WORD);
}

static void
build_descriptor(uint64_t block[TSFS_BLOCK_WORDS], const struct tsfs_id *id,
    uint64_t generation, unsigned int members, unsigned int member,
    uint64_t tdir_checksum)
{
        memset(block, 0, TSFS_BLOCK_WORDS * sizeof(block[0]));
        block[TSFS_DESC_MAGIC_WORD] = tsfs_sixbit_word("TSFS  ");
        block[TSFS_DESC_VERSION_WORD] =
            ((uint64_t)TSFS_FORMAT_MAJOR << 18) | TSFS_FORMAT_MINOR;
        block[TSFS_DESC_FLAGS_WORD] = 0;
        block[TSFS_DESC_ID_HI_WORD] = id->hi;
        block[TSFS_DESC_ID_LO_WORD] = id->lo;
        block[TSFS_DESC_GENERATION_WORD] = generation;
        block[TSFS_DESC_MEMBER_WORD] = ((uint64_t)members << 18) | member;
        block[TSFS_DESC_BLOCK_COUNT_WORD] = TSFS_BLOCK_COUNT;
        block[TSFS_DESC_TDIR_WORD] = TSFS_TDIR_DEFAULT_BLOCK;
        block[TSFS_DESC_TDIR_BLOCKS_WORD] = 1;
        block[TSFS_DESC_TDIR_CHECKSUM_WORD] = tdir_checksum;
        block[TSFS_DESC_CHECKSUM_WORD] = tsfs_checksum36(block,
            TSFS_BLOCK_WORDS, TSFS_DESC_CHECKSUM_WORD);
}

int
main(int argc, char **argv)
{
        const char *prefix;
        const char *id_text;
        const char *manifest;
        struct tsfs_id id;
        struct manifest_node *nodes;
        unsigned int node_count;
        unsigned int *record_to_node;
        uint64_t generation;
        uint64_t **images;
        uint64_t descriptor[TSFS_BLOCK_WORDS];
        uint64_t tdir[TSFS_BLOCK_WORDS];
        uint64_t *file_words;
        uint64_t *extent_words;
        uint64_t file_checksum;
        uint64_t extent_checksum;
        unsigned int file_count;
        unsigned int file_blocks;
        unsigned int file_block;
        unsigned int extent_count;
        unsigned int extent_blocks;
        unsigned int extent_block;
        unsigned int members;
        unsigned int member;
        unsigned int i;
        unsigned int next_block[TSFS_MAX_MEMBERS];
        unsigned int data_member;
        char path[1024];
        size_t image_words;

        prefix = NULL;
        id_text = NULL;
        manifest = NULL;
        generation = 1;
        members = 0;
        images = NULL;
        file_words = NULL;
        extent_words = NULL;
        for (i = 1U; i < (unsigned int)argc; ++i) {
                if (strcmp(argv[i], "-n") == 0 && i + 1U < (unsigned int)argc) {
                        char *end;
                        unsigned long v;

                        errno = 0;
                        v = strtoul(argv[++i], &end, 0);
                        if (errno != 0 || *end != '\0' || v == 0 ||
                            v > TSFS_MAX_MEMBERS)
                                usage();
                        members = (unsigned int)v;
                } else if (strcmp(argv[i], "-i") == 0 &&
                    i + 1U < (unsigned int)argc) {
                        id_text = argv[++i];
                } else if (strcmp(argv[i], "-g") == 0 &&
                    i + 1U < (unsigned int)argc) {
                        if (parse_octal_word(argv[++i], &generation) != 0)
                                usage();
                } else if (strcmp(argv[i], "-m") == 0 &&
                    i + 1U < (unsigned int)argc) {
                        manifest = argv[++i];
                } else if (strcmp(argv[i], "-o") == 0 &&
                    i + 1U < (unsigned int)argc) {
                        prefix = argv[++i];
                } else {
                        usage();
                }
        }
        if (members == 0U || id_text == NULL || prefix == NULL ||
            parse_id(id_text, &id) != 0)
                usage();
        if (read_manifest(manifest, &nodes, &node_count) != 0) {
                perror(manifest != NULL ? manifest : "mktsfs");
                return 1;
        }
        if (assign_records(nodes, node_count, &record_to_node) != 0) {
                fprintf(stderr, "mktsfs: cannot order manifest\n");
                free(nodes);
                return 1;
        }

        extent_count = 0U;
        for (i = 0U; i < node_count; ++i) {
                struct manifest_node *node;

                node = &nodes[i];
                if (node->is_dir)
                        continue;
                if (source_word_count(node->source, &node->size_words) != 0) {
                        fprintf(stderr, "mktsfs: invalid source: %s\n",
                            node->source[0] != '\0' ? node->source : "(empty)");
                        free(record_to_node);
                        free(nodes);
                        return 1;
                }
                node->extent_count = (node->size_words + TSFS_RESTART_WORDS - 1U) /
                    TSFS_RESTART_WORDS;
        }
        for (i = 1U; i <= node_count; ++i) {
                struct manifest_node *node;

                node = &nodes[record_to_node[i - 1U]];
                if (node->is_dir)
                        continue;
                node->first_extent = extent_count;
                if (node->extent_count > (unsigned int)TSFS_HALF_MASK - extent_count) {
                        fprintf(stderr, "mktsfs: too many extents\n");
                        free(record_to_node);
                        free(nodes);
                        return 1;
                }
                extent_count += node->extent_count;
        }

        file_count = node_count == 0U ? 0U : node_count + 1U;
        file_blocks = file_count == 0U ? 0U :
            (file_count * TSFS_FILE_WORDS + TSFS_BLOCK_WORDS - 1U) /
            TSFS_BLOCK_WORDS;
        extent_blocks = extent_count == 0U ? 0U :
            (extent_count * TSFS_EXTENT_WORDS + TSFS_BLOCK_WORDS - 1U) /
            TSFS_BLOCK_WORDS;
        file_block = TSFS_TDIR_DEFAULT_BLOCK + 1U;
        extent_block = file_block + file_blocks;
        if (extent_block + extent_blocks > TSFS_BLOCK_COUNT) {
                fprintf(stderr, "mktsfs: metadata does not fit member 0\n");
                free(record_to_node);
                free(nodes);
                return 1;
        }

        file_words = NULL;
        extent_words = NULL;
        if (file_count != 0U) {
                size_t words;
                uint64_t rec[TSFS_FILE_WORDS];
                unsigned int root_children;

                words = (size_t)file_blocks * TSFS_BLOCK_WORDS;
                file_words = (uint64_t *)calloc(words, sizeof(*file_words));
                if (file_words == NULL)
                        goto nomem;
                root_children = 0U;
                for (i = 0U; i < node_count; ++i)
                        if (nodes[i].parent_node < 0)
                                ++root_children;
                build_root_record(rec, root_children);
                memcpy(file_words, rec, sizeof(rec));
                for (i = 1U; i < file_count; ++i) {
                        struct manifest_node *node;
                        unsigned int parent_record;

                        node = &nodes[record_to_node[i - 1U]];
                        parent_record = 0U;
                        if (node->parent_node >= 0)
                                parent_record = nodes[node->parent_node].record_index;
                        build_file_record(rec, node, parent_record);
                        memcpy(file_words + (size_t)i * TSFS_FILE_WORDS,
                            rec, sizeof(rec));
                }
        }
        if (extent_count != 0U) {
                extent_words = (uint64_t *)calloc(
                    (size_t)extent_blocks * TSFS_BLOCK_WORDS,
                    sizeof(*extent_words));
                if (extent_words == NULL)
                        goto nomem;
        }

        image_words = (size_t)TSFS_BLOCK_COUNT * TSFS_BLOCK_WORDS;
        images = (uint64_t **)calloc(members, sizeof(*images));
        if (images == NULL)
                goto nomem;
        for (member = 0U; member < members; ++member) {
                images[member] = (uint64_t *)calloc(image_words,
                    sizeof(*images[member]));
                if (images[member] == NULL) {
                        while (member != 0U)
                                free(images[--member]);
                        free(images);
                        images = NULL;
                        goto nomem;
                }
        }

        next_block[0] = extent_block + extent_blocks;
        for (member = 1U; member < members; ++member)
                next_block[member] = TSFS_TDIR_DEFAULT_BLOCK;
        data_member = 0U;

        /*
         * File-table records are breadth-first and name-sorted for cheap
         * lookup, but physical tape order follows the manifest.  Manifest
         * order is the only practical locality hint available to a read-only
         * sequential medium and avoids turning boot into long seek sweeps.
         */
        for (i = 0U; i < node_count; ++i) {
                struct manifest_node *node;
                uint64_t *source;
                unsigned int e;

                node = &nodes[i];
                if (node->is_dir || node->size_words == 0U)
                        continue;
                if (read_source_words(node->source, &source,
                    node->size_words) != 0) {
                        fprintf(stderr, "mktsfs: cannot read source: %s\n",
                            node->source);
                        goto fail;
                }
                for (e = 0U; e < node->extent_count; ++e) {
                        unsigned int logical_start;
                        unsigned int logical_words;
                        unsigned int raw_blocks;
                        unsigned int blocks;
                        unsigned int flags;
                        uint64_t *compressed;
                        size_t compressed_words;
                        uint64_t *dst;
                        uint64_t *rec;

                        logical_start = e * TSFS_RESTART_WORDS;
                        logical_words = node->size_words - logical_start;
                        if (logical_words > TSFS_RESTART_WORDS)
                                logical_words = TSFS_RESTART_WORDS;
                        raw_blocks = (logical_words + TSFS_BLOCK_WORDS - 1U) /
                            TSFS_BLOCK_WORDS;
                        compressed = NULL;
                        compressed_words = 0U;
                        if (d6lz_codec_compress(source + logical_start,
                            logical_words, &compressed, &compressed_words) != 0) {
                                free(source);
                                goto fail;
                        }
                        flags = TSFS_EXTENT_FLAG_STORED;
                        blocks = raw_blocks;
                        if (raw_blocks > 1U && compressed_words + 2U <=
                            TSFS_BLOCK_WORDS) {
                                flags = TSFS_EXTENT_FLAG_D6LZ;
                                blocks = 1U;
                        }
                        while (data_member < members &&
                            next_block[data_member] + blocks > TSFS_BLOCK_COUNT)
                                ++data_member;
                        if (data_member >= members) {
                                fprintf(stderr, "mktsfs: file data does not fit set\n");
                                free(compressed);
                                free(source);
                                goto fail;
                        }
                        dst = images[data_member] +
                            (size_t)next_block[data_member] * TSFS_BLOCK_WORDS;
                        if (flags == TSFS_EXTENT_FLAG_D6LZ) {
                                dst[0] = logical_words;
                                dst[1] = compressed_words;
                                memcpy(dst + 2U, compressed,
                                    compressed_words * sizeof(*compressed));
                        } else {
                                memcpy(dst, source + logical_start,
                                    (size_t)logical_words * sizeof(*source));
                        }
                        rec = extent_words +
                            (size_t)(node->first_extent + e) * TSFS_EXTENT_WORDS;
                        rec[TSFS_EXTENT_FILE_WORD_START] = logical_start;
                        rec[TSFS_EXTENT_LOCATION] =
                            ((uint64_t)data_member << 18) |
                            next_block[data_member];
                        rec[TSFS_EXTENT_SHAPE] =
                            ((uint64_t)flags << 18) | blocks;
                        rec[TSFS_EXTENT_CHECKSUM] = tsfs_checksum36(dst,
                            (size_t)blocks * TSFS_BLOCK_WORDS, (size_t)-1);
                        next_block[data_member] += blocks;
                        free(compressed);
                }
                free(source);
        }

        file_checksum = file_count == 0U ? 0U : tsfs_checksum36(file_words,
            (size_t)file_blocks * TSFS_BLOCK_WORDS, (size_t)-1);
        extent_checksum = extent_count == 0U ? 0U :
            tsfs_checksum36(extent_words,
            (size_t)extent_blocks * TSFS_BLOCK_WORDS, (size_t)-1);
        build_tdir(tdir, file_count, 0U, file_block, file_blocks,
            file_checksum, extent_count, 0U, extent_block, extent_blocks,
            extent_checksum);

        for (member = 0U; member < members; ++member) {
                build_descriptor(descriptor, &id, generation, members,
                    member, tdir[TSFS_TDIR_CHECKSUM_WORD]);
                memcpy(images[member] +
                    TSFS_DESC_PRIMARY_BLOCK * TSFS_BLOCK_WORDS,
                    descriptor, sizeof(descriptor));
                memcpy(images[member] +
                    TSFS_DESC_BACKUP_BLOCK * TSFS_BLOCK_WORDS,
                    descriptor, sizeof(descriptor));
        }
        memcpy(images[0] + TSFS_TDIR_DEFAULT_BLOCK * TSFS_BLOCK_WORDS,
            tdir, sizeof(tdir));
        if (file_count != 0U)
                memcpy(images[0] + (size_t)file_block * TSFS_BLOCK_WORDS,
                    file_words,
                    (size_t)file_blocks * TSFS_BLOCK_WORDS * sizeof(*file_words));
        if (extent_count != 0U)
                memcpy(images[0] + (size_t)extent_block * TSFS_BLOCK_WORDS,
                    extent_words,
                    (size_t)extent_blocks * TSFS_BLOCK_WORDS *
                    sizeof(*extent_words));

        for (member = 0U; member < members; ++member) {
                if (strlen(prefix) + 32U >= sizeof(path)) {
                        fprintf(stderr, "mktsfs: output path too long\n");
                        goto fail;
                }
                sprintf(path, "%s%u.dta", prefix, member);
                if (tsfs_write_image(path, images[member], image_words) != 0) {
                        perror(path);
                        goto fail;
                }
        }
        for (i = 0U; i < members; ++i)
                free(images[i]);
        free(images);
        free(extent_words);
        free(file_words);
        free(record_to_node);
        free(nodes);
        return 0;

nomem:
        fprintf(stderr, "mktsfs: out of memory\n");
fail:
        if (images != NULL) {
                for (i = 0U; i < members; ++i)
                        free(images[i]);
                free(images);
        }
        free(extent_words);
        free(file_words);
        free(record_to_node);
        free(nodes);
        return 1;
}
