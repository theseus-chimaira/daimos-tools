/* tsfscheck.c - validate a TSFS DECtape set. */

#include "tsfs-format.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct member_info {
        uint64_t id_hi;
        uint64_t id_lo;
        uint64_t generation;
        uint64_t tdir_checksum;
        unsigned int count;
        unsigned int index;
        unsigned int tdir_member;
        unsigned int tdir_block;
        unsigned int tdir_blocks;
};

struct table_info {
        unsigned int id;
        unsigned int member;
        unsigned int block;
        unsigned int blocks;
        unsigned int record_words;
        unsigned int record_count;
        uint64_t checksum;
};

static int
validate_descriptor(const uint64_t block[TSFS_BLOCK_WORDS],
    struct member_info *info)
{
        uint64_t version;
        uint64_t member;
        uint64_t tdir;
        uint64_t checksum;

        if (block[TSFS_DESC_MAGIC_WORD] != tsfs_sixbit_word("TSFS  "))
                return -1;
        version = block[TSFS_DESC_VERSION_WORD];
        if (((version >> 18) & TSFS_HALF_MASK) != TSFS_FORMAT_MAJOR ||
            (version & TSFS_HALF_MASK) != TSFS_FORMAT_MINOR ||
            block[TSFS_DESC_FLAGS_WORD] != TSFS_FLAGS_V1_SUPPORTED)
                return -1;
        checksum = tsfs_checksum36(block, TSFS_BLOCK_WORDS,
            TSFS_DESC_CHECKSUM_WORD);
        if (block[TSFS_DESC_CHECKSUM_WORD] != checksum)
                return -1;
        member = block[TSFS_DESC_MEMBER_WORD];
        info->count = (unsigned int)((member >> 18) & TSFS_HALF_MASK);
        info->index = (unsigned int)(member & TSFS_HALF_MASK);
        if (info->count == 0U || info->count > TSFS_MAX_MEMBERS ||
            info->index >= info->count ||
            block[TSFS_DESC_BLOCK_COUNT_WORD] != TSFS_BLOCK_COUNT)
                return -1;
        tdir = block[TSFS_DESC_TDIR_WORD];
        info->tdir_member = (unsigned int)((tdir >> 18) & TSFS_HALF_MASK);
        info->tdir_block = (unsigned int)(tdir & TSFS_HALF_MASK);
        info->tdir_blocks = (unsigned int)block[TSFS_DESC_TDIR_BLOCKS_WORD];
        if (info->tdir_member >= info->count || info->tdir_blocks != 1U ||
            info->tdir_block < 3U || info->tdir_block >= TSFS_BLOCK_COUNT)
                return -1;
        info->id_hi = block[TSFS_DESC_ID_HI_WORD];
        info->id_lo = block[TSFS_DESC_ID_LO_WORD];
        info->generation = block[TSFS_DESC_GENERATION_WORD];
        info->tdir_checksum = block[TSFS_DESC_TDIR_CHECKSUM_WORD];
        return 0;
}

static int
same_set(const struct member_info *a, const struct member_info *b)
{
        return a->id_hi == b->id_hi && a->id_lo == b->id_lo &&
            a->generation == b->generation && a->count == b->count &&
            a->tdir_member == b->tdir_member &&
            a->tdir_block == b->tdir_block &&
            a->tdir_blocks == b->tdir_blocks &&
            a->tdir_checksum == b->tdir_checksum;
}

static int
read_table(const char *path, const struct table_info *t, uint64_t **wordsp)
{
        uint64_t *words;
        unsigned int i;

        words = (uint64_t *)calloc((size_t)t->blocks * TSFS_BLOCK_WORDS,
            sizeof(*words));
        if (words == NULL)
                return -1;
        for (i = 0U; i < t->blocks; ++i)
                if (tsfs_read_image_block(path, t->block + i,
                    words + (size_t)i * TSFS_BLOCK_WORDS) != 0) {
                        free(words);
                        return -1;
                }
        if (tsfs_checksum36(words,
            (size_t)t->blocks * TSFS_BLOCK_WORDS, (size_t)-1) != t->checksum) {
                free(words);
                return -1;
        }
        *wordsp = words;
        return 0;
}

static int
name_record_valid(const uint64_t *rec, int root)
{
        unsigned int wi;
        unsigned int ci;
        int saw_space;

        if (root) {
                for (wi = 0U; wi < TSFS_FILE_NAME_WORDS; ++wi)
                        if (rec[TSFS_FILE_NAME0 + wi] != 0)
                                return 0;
                return 1;
        }
        saw_space = 0;
        for (wi = 0U; wi < TSFS_FILE_NAME_WORDS; ++wi) {
                uint64_t w;

                w = rec[TSFS_FILE_NAME0 + wi];
                for (ci = 0U; ci < 6U; ++ci) {
                        unsigned int ch;

                        ch = (unsigned int)((w >> (30U - ci * 6U)) & 077U);
                        if (ch == 0U)
                                saw_space = 1;
                        else if (saw_space)
                                return 0;
                }
        }
        return !saw_space || rec[TSFS_FILE_NAME0] != 0;
}

static int
validate_file_table(const struct table_info *t, const uint64_t *words)
{
        unsigned int i;

        if (t->record_words != TSFS_FILE_WORDS || t->record_count == 0U ||
            (size_t)t->record_count * TSFS_FILE_WORDS >
            (size_t)t->blocks * TSFS_BLOCK_WORDS)
                return -1;
        for (i = 0U; i < t->record_count; ++i) {
                const uint64_t *r;
                unsigned int parent;
                unsigned int flags;
                unsigned int first;
                unsigned int count;

                r = words + (size_t)i * TSFS_FILE_WORDS;
                parent = (unsigned int)((r[TSFS_FILE_PARENT_FLAGS] >> 18) &
                    TSFS_HALF_MASK);
                flags = (unsigned int)(r[TSFS_FILE_PARENT_FLAGS] &
                    TSFS_HALF_MASK);
                if ((flags & ~TSFS_FILE_FLAG_MASK) != 0U ||
                    (flags != TSFS_FILE_FLAG_DIR && flags != TSFS_FILE_FLAG_REG) ||
                    !name_record_valid(r, i == TSFS_FILE_ROOT_INDEX))
                        return -1;
                if (i == TSFS_FILE_ROOT_INDEX) {
                        if (parent != 0U || flags != TSFS_FILE_FLAG_DIR)
                                return -1;
                } else if (parent >= i) {
                        return -1;
                }
                if (flags == TSFS_FILE_FLAG_DIR) {
                        if (r[TSFS_FILE_SIZE_WORDS] != 0 ||
                            r[TSFS_FILE_EXTENT_RANGE] != 0)
                                return -1;
                        first = (unsigned int)((r[TSFS_FILE_AUX] >> 18) &
                            TSFS_HALF_MASK);
                        count = (unsigned int)(r[TSFS_FILE_AUX] & TSFS_HALF_MASK);
                        if ((count == 0U && first != 0U) ||
                            (count != 0U && (first <= i ||
                            first + count > t->record_count)))
                                return -1;
                } else {
                        if (r[TSFS_FILE_SIZE_WORDS] != 0 ||
                            r[TSFS_FILE_EXTENT_RANGE] != 0 ||
                            r[TSFS_FILE_AUX] != 0)
                                return -1;
                }
        }
        return 0;
}

int
main(int argc, char **argv)
{
        struct member_info info[TSFS_MAX_MEMBERS];
        const char *member_path[TSFS_MAX_MEMBERS];
        struct table_info file_table;
        unsigned int seen;
        uint64_t block[TSFS_BLOCK_WORDS];
        uint64_t backup[TSFS_BLOCK_WORDS];
        uint64_t tdir[TSFS_BLOCK_WORDS];
        uint64_t *file_words;
        int i;

        if (argc < 2 || argc > (int)TSFS_MAX_MEMBERS + 1) {
                fprintf(stderr, "usage: tsfscheck member0.dta [member1.dta ...]\n");
                return 2;
        }
        seen = 0U;
        for (i = 0; i < (int)TSFS_MAX_MEMBERS; ++i)
                member_path[i] = NULL;
        for (i = 1; i < argc; ++i) {
                struct member_info primary_info;
                struct member_info backup_info;
                int primary_valid;
                int backup_valid;

                if (tsfs_read_image_block(argv[i], TSFS_DESC_PRIMARY_BLOCK,
                    block) != 0 || tsfs_read_image_block(argv[i],
                    TSFS_DESC_BACKUP_BLOCK, backup) != 0) {
                        fprintf(stderr, "tsfscheck: cannot read descriptors: %s\n",
                            argv[i]);
                        return 1;
                }
                primary_valid = validate_descriptor(block, &primary_info) == 0;
                backup_valid = validate_descriptor(backup, &backup_info) == 0;
                if (!primary_valid && !backup_valid) {
                        fprintf(stderr, "tsfscheck: invalid descriptor: %s\n",
                            argv[i]);
                        return 1;
                }
                if (primary_valid && backup_valid &&
                    memcmp(block, backup, sizeof(block)) != 0) {
                        fprintf(stderr, "tsfscheck: conflicting descriptors: %s\n",
                            argv[i]);
                        return 1;
                }
                info[i - 1] = primary_valid ? primary_info : backup_info;
                if (i != 1 && !same_set(&info[0], &info[i - 1])) {
                        fprintf(stderr, "tsfscheck: members belong to different sets\n");
                        return 1;
                }
                if ((seen & (1U << info[i - 1].index)) != 0U) {
                        fprintf(stderr, "tsfscheck: duplicate member index\n");
                        return 1;
                }
                seen |= 1U << info[i - 1].index;
                member_path[info[i - 1].index] = argv[i];
        }
        if ((unsigned int)(argc - 1) != info[0].count ||
            seen != (1U << info[0].count) - 1U) {
                fprintf(stderr, "tsfscheck: incomplete set\n");
                return 1;
        }
        if (member_path[info[0].tdir_member] == NULL ||
            tsfs_read_image_block(member_path[info[0].tdir_member],
            info[0].tdir_block, tdir) != 0 ||
            tdir[TSFS_TDIR_MAGIC_WORD] != tsfs_sixbit_word("TSDIR ") ||
            ((tdir[TSFS_TDIR_VERSION_WORD] >> 18) & TSFS_HALF_MASK) !=
            TSFS_FORMAT_MAJOR ||
            (tdir[TSFS_TDIR_VERSION_WORD] & TSFS_HALF_MASK) !=
            TSFS_FORMAT_MINOR ||
            tdir[TSFS_TDIR_FLAGS_WORD] != TSFS_FLAGS_V1_SUPPORTED ||
            tdir[TSFS_TDIR_CHECKSUM_WORD] != info[0].tdir_checksum ||
            tsfs_checksum36(tdir, TSFS_BLOCK_WORDS,
            TSFS_TDIR_CHECKSUM_WORD) != info[0].tdir_checksum ||
            tdir[TSFS_TDIR_PATH_WORDS_WORD] != 0) {
                fprintf(stderr, "tsfscheck: invalid table directory\n");
                return 1;
        }

        memset(&file_table, 0, sizeof(file_table));
        if (tdir[TSFS_TDIR_FILE_COUNT_WORD] == 0) {
                if (tdir[TSFS_TDIR_EXTENT_COUNT_WORD] != 0) {
                        fprintf(stderr, "tsfscheck: extents without files\n");
                        return 1;
                }
        } else {
                const uint64_t *e;
                uint64_t shape;

                e = tdir + TSFS_TDIR_USED_WORDS;
                if (((e[TSFS_TDIRE_ID_FLAGS] >> 18) & TSFS_HALF_MASK) !=
                    TSFS_TABLE_FILE ||
                    (e[TSFS_TDIRE_ID_FLAGS] & TSFS_HALF_MASK) != 0) {
                        fprintf(stderr, "tsfscheck: missing file table\n");
                        return 1;
                }
                file_table.id = TSFS_TABLE_FILE;
                file_table.member = (unsigned int)((e[TSFS_TDIRE_MEMBER_BLOCK] >>
                    18) & TSFS_HALF_MASK);
                file_table.block = (unsigned int)(e[TSFS_TDIRE_MEMBER_BLOCK] &
                    TSFS_HALF_MASK);
                shape = e[TSFS_TDIRE_BLOCKS_RECWORDS];
                file_table.blocks = (unsigned int)((shape >> 18) & TSFS_HALF_MASK);
                file_table.record_words = (unsigned int)(shape & TSFS_HALF_MASK);
                file_table.record_count = (unsigned int)e[TSFS_TDIRE_RECORD_COUNT];
                file_table.checksum = e[TSFS_TDIRE_CHECKSUM];
                if (file_table.member >= info[0].count || file_table.blocks == 0U ||
                    file_table.block < 3U ||
                    file_table.block + file_table.blocks > TSFS_BLOCK_COUNT ||
                    file_table.record_count !=
                    (unsigned int)tdir[TSFS_TDIR_FILE_COUNT_WORD] ||
                    member_path[file_table.member] == NULL ||
                    read_table(member_path[file_table.member], &file_table,
                    &file_words) != 0 ||
                    validate_file_table(&file_table, file_words) != 0) {
                        fprintf(stderr, "tsfscheck: invalid file table\n");
                        return 1;
                }
                free(file_words);
        }
        if (tdir[TSFS_TDIR_EXTENT_COUNT_WORD] != 0) {
                fprintf(stderr, "tsfscheck: extent-bearing images not supported by this V1 builder yet\n");
                return 1;
        }
        printf("TSFS OK members=%u generation=%lo files=%lo\n",
            info[0].count, (unsigned long)info[0].generation,
            (unsigned long)tdir[TSFS_TDIR_FILE_COUNT_WORD]);
        return 0;
}
