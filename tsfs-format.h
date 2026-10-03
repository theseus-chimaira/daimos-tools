#ifndef PDP10_TSFS_FORMAT_H
#define PDP10_TSFS_FORMAT_H

#include <stddef.h>
#include <stdint.h>

#define TSFS_WORD_MASK UINT64_C(0777777777777)
#define TSFS_HALF_MASK UINT64_C(0777777)
#define TSFS_BLOCK_WORDS 0200U
#define TSFS_BLOCK_COUNT 01102U
#define TSFS_MAX_MEMBERS 8U

#define TSFS_FORMAT_MAJOR 1U
#define TSFS_FORMAT_MINOR 1U

#define TSFS_DESC_PRIMARY_BLOCK 1U
#define TSFS_DESC_BACKUP_BLOCK  2U
#define TSFS_TDIR_DEFAULT_BLOCK  3U

#define TSFS_DESC_MAGIC_WORD         0U
#define TSFS_DESC_VERSION_WORD       1U
#define TSFS_DESC_FLAGS_WORD         2U
#define TSFS_DESC_ID_HI_WORD         3U
#define TSFS_DESC_ID_LO_WORD         4U
#define TSFS_DESC_GENERATION_WORD    5U
#define TSFS_DESC_MEMBER_WORD        6U
#define TSFS_DESC_BLOCK_COUNT_WORD   7U
#define TSFS_DESC_TDIR_WORD          8U
#define TSFS_DESC_TDIR_BLOCKS_WORD   9U
#define TSFS_DESC_TDIR_CHECKSUM_WORD 10U
#define TSFS_DESC_CHECKSUM_WORD      11U
#define TSFS_DESC_USED_WORDS         12U

#define TSFS_TDIR_MAGIC_WORD         0U
#define TSFS_TDIR_VERSION_WORD       1U
#define TSFS_TDIR_FLAGS_WORD         2U
#define TSFS_TDIR_FILE_COUNT_WORD    3U
#define TSFS_TDIR_EXTENT_COUNT_WORD  4U
/* Kept zero in V1.  Non-empty V1 uses component names in file records. */
#define TSFS_TDIR_PATH_WORDS_WORD    5U
#define TSFS_TDIR_CHECKSUM_WORD      6U
#define TSFS_TDIR_USED_WORDS         7U

/* Fixed five-word table-directory entry, beginning at TDIR word 7. */
#define TSFS_TDIRE_WORDS             5U
#define TSFS_TDIRE_ID_FLAGS          0U
#define TSFS_TDIRE_MEMBER_BLOCK      1U
#define TSFS_TDIRE_BLOCKS_RECWORDS   2U
#define TSFS_TDIRE_RECORD_COUNT      3U
#define TSFS_TDIRE_CHECKSUM          4U
#define TSFS_TDIRE_MAX_ENTRIES \
    ((TSFS_BLOCK_WORDS - TSFS_TDIR_USED_WORDS) / TSFS_TDIRE_WORDS)

#define TSFS_TABLE_NONE              0U
#define TSFS_TABLE_FILE              1U
#define TSFS_TABLE_EXTENT            2U

/*
 * File table.  Records are breadth-first.  Each directory's immediate
 * children occupy one contiguous range and are sorted by component name.
 * Record zero is the root record whenever FILE_COUNT is nonzero.
 */
#define TSFS_FILE_WORDS              8U
#define TSFS_FILE_PARENT_FLAGS       0U
#define TSFS_FILE_NAME0              1U
#define TSFS_FILE_NAME_WORDS         4U
#define TSFS_FILE_SIZE_WORDS         5U
#define TSFS_FILE_EXTENT_RANGE       6U
#define TSFS_FILE_AUX                7U

#define TSFS_FILE_FLAG_DIR           1U
#define TSFS_FILE_FLAG_REG           2U
#define TSFS_FILE_FLAG_MASK          3U
#define TSFS_FILE_MODE_SHIFT         6U
#define TSFS_FILE_MODE_MASK          07777U
#define TSFS_FILE_OWNER_SHIFT        9U
#define TSFS_FILE_ID_MASK            0777U
#define TSFS_FILE_FLAG_RESERVED_MASK 074U
#define TSFS_FILE_NAME_CHARS         (TSFS_FILE_NAME_WORDS * 6U)

/* MODE occupies bits 6..17 of PARENT_FLAGS RH; bits 2..5 stay zero.
 * Owner is compact UID9,,GID9.  Directories store owner in SIZE_WORDS, whose
 * size is otherwise always zero; regular files store owner in AUX, otherwise
 * unused for regular records.  Directory AUX remains FIRST_CHILD,,CHILD_COUNT. */
#define TSFS_FILE_ROOT_INDEX         0U

/*
 * Extent table.  FILE_WORD_START is a word offset in the file.  LOCATION is
 * MEMBER_INDEX,,START_BLOCK.  SHAPE is FLAGS,,BLOCK_COUNT.  CHECKSUM covers
 * every 36-bit word in the extent blocks, including zero padding in the last
 * block. Regular-file data uses fixed 0400-word restart extents. Each
 * extent is explicitly encoded as STORED or D6LZ.
 */
#define TSFS_EXTENT_WORDS            4U
#define TSFS_EXTENT_FILE_WORD_START  0U
#define TSFS_EXTENT_LOCATION         1U
#define TSFS_EXTENT_SHAPE            2U
#define TSFS_EXTENT_CHECKSUM         3U
#define TSFS_EXTENT_FLAG_STORED     0U
#define TSFS_EXTENT_FLAG_D6LZ       1U
#define TSFS_EXTENT_FLAG_MASK       1U
#define TSFS_RESTART_WORDS          0400U

#define TSFS_FLAGS_V1_SUPPORTED UINT64_C(0)

uint64_t tsfs_sixbit_word(const char text[6]);
uint64_t tsfs_checksum36(const uint64_t *words, size_t count,
    size_t zero_word);
int tsfs_read_image_block(const char *path, unsigned int block,
    uint64_t words[TSFS_BLOCK_WORDS]);
int tsfs_write_image(const char *path,
    const uint64_t *words, size_t word_count);

#endif
