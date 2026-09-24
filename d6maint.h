#ifndef PDP10_TOOLS_D6MAINT_H
#define PDP10_TOOLS_D6MAINT_H

#include <stdint.h>
#include <stdio.h>

#define D6M_WORD_MASK               0777777777777ULL
#define D6M_HALF_MASK               0777777U
#define D6M_BLOCK_WORDS             0200U
#define D6M_SCAN_LIMIT              0200U
#define D6M_MAX_MEMBERS             8U
#define D6M_FCB_WORDS               020U
#define D6M_SUPER_WORDS             020U
#define D6M_EXTENTS                 7U
#define D6M_BITS_PER_MAP_BLOCK      (D6M_BLOCK_WORDS * 36U)
#define D6M_MAGIC                   044066263662ULL
#define D6M_VERSION                 2U
#define D6M_LAYOUT_MAGIC            0442654636222ULL
#define D6M_LAYOUT_MAGIC_WORD       006U
#define D6M_LAYOUT_RANGE_WORD       007U
#define D6M_LAYOUT_SUPER_A          010U
#define D6M_LAYOUT_SUPER_B          011U
#define D6M_LAYOUT_SWAP_TAIL        012U
#define D6M_LAYOUT_BOOTSTREAM       013U
#define D6M_LAYOUT_LOGSTORE_START   014U
#define D6M_LAYOUT_LOGSTORE_BLOCKS  015U
#define D6M_LAYOUT_BADMAP_START     016U
#define D6M_LAYOUT_BADMAP_BLOCKS    017U
#define D6M_BADMAP_MAGIC            0442642414422ULL
#define D6M_BADMAP_HEADER_WORDS     4U
#define D6M_BADMAP_MEMBER_SHIFT     16U
#define D6M_BADMAP_MEMBER_MASK      03U
#define D6M_BADMAP_BLOCK_MASK       0177777U
#define D6M_EXTENT_LOW_BITS         12U
#define D6M_EXTENT_LOW_MASK         07777ULL
#define D6M_EXTENT_HIGH_MASK        037ULL
#define D6M_STATE_CLEAN             0U
#define D6M_STATE_DIRTY             1U
#define D6M_RES_START_SHIFT         12U
#define D6M_RES_LEN_LOW_MASK        07777ULL
#define D6M_RES_LEN_HIGH_MASK       07777ULL
#define D6M_RES_SWAP_HI_SHIFT       24U
#define D6M_RES_LOG_HI_SHIFT        12U
#define D6M_RES_RESERVED_MASK       07777ULL
#define D6M_LOGICAL_BLOCK_LIMIT     (1U << 24)
#define D6M_TYPE_FREE               0U
#define D6M_TYPE_REG                1U
#define D6M_TYPE_DIR                2U
#define D6M_TYPE_SYMLINK            3U

struct d6m_member {
        FILE *fp;
        char path[512];
        unsigned descriptor;
        unsigned base;
        unsigned blocks;
        unsigned sectors;
};

struct d6m_layout {
        unsigned super_a;
        unsigned super_b;
        unsigned swap_tail_blocks;
        unsigned bootstream_blocks;
        unsigned logstore_start;
        unsigned logstore_blocks;
        unsigned badmap_start;
        unsigned badmap_blocks;
        int explicit_ranges;
};

struct d6m_super {
        uint64_t sequence;
        unsigned state;
        uint64_t fsid[2];
        unsigned swap_start;
        unsigned swap_blocks;
        unsigned log_start;
        unsigned log_blocks;
        unsigned total;
        unsigned root;
        unsigned fcb_start;
        unsigned fcb_count;
        unsigned freemap_start;
        unsigned freemap_blocks;
        unsigned summary_start;
        unsigned summary_blocks;
};

struct d6m_set {
        struct d6m_member member[D6M_MAX_MEMBERS];
        unsigned members;
        struct d6m_layout layout;
        int writable;
        uint64_t *badmap;
        unsigned badmap_count;
};

int d6m_open(struct d6m_set *set, const char *dir, unsigned members,
    int writable, char *err, size_t errlen);
void d6m_close(struct d6m_set *set);
unsigned d6m_total_blocks(const struct d6m_set *set);
int d6m_map(const struct d6m_set *set, unsigned logical,
    unsigned *memberp, unsigned *localp);
int d6m_inverse_map(const struct d6m_set *set, unsigned member, unsigned local,
    unsigned *logicalp);
int d6m_read(const struct d6m_set *set, unsigned logical,
    uint64_t block[D6M_BLOCK_WORDS]);
int d6m_write(struct d6m_set *set, unsigned logical,
    const uint64_t block[D6M_BLOCK_WORDS]);
int d6m_read_phys(const struct d6m_set *set, unsigned member,
    unsigned sector, uint64_t block[D6M_BLOCK_WORDS]);
int d6m_write_phys(struct d6m_set *set, unsigned member,
    unsigned sector, const uint64_t block[D6M_BLOCK_WORDS]);
int d6m_super_decode(const uint64_t raw[D6M_SUPER_WORDS], unsigned disk_blocks,
    struct d6m_super *s);
int d6m_super_encode(uint64_t raw[D6M_SUPER_WORDS], const struct d6m_super *s);
int d6m_select_super(const struct d6m_set *set, struct d6m_super *s,
    unsigned *selected, uint64_t selected_block[D6M_BLOCK_WORDS],
    char *err, size_t errlen);
unsigned d6m_extent_high(uint64_t word, unsigned e);
int d6m_extent_decode(const uint64_t fcb[D6M_FCB_WORDS], unsigned e,
    unsigned *startp, unsigned *blocksp);
int d6m_extent_set(uint64_t fcb[D6M_FCB_WORDS], unsigned e,
    unsigned start, unsigned blocks);
int d6m_get_bit(const uint64_t *map, unsigned bit);
void d6m_set_bit(uint64_t *map, unsigned bit, int value);
void d6m_rebuild_summary(const struct d6m_super *s, const uint64_t *freemap,
    uint64_t *summary);
int d6m_begin_dirty(struct d6m_set *set, const struct d6m_super *old,
    unsigned selected, const uint64_t old_block[D6M_BLOCK_WORDS],
    struct d6m_super *dirty, unsigned *dirty_selected,
    uint64_t dirty_block[D6M_BLOCK_WORDS]);
int d6m_publish_clean(struct d6m_set *set, const struct d6m_super *old,
    unsigned selected, const uint64_t old_block[D6M_BLOCK_WORDS],
    struct d6m_super *new_super);
int d6m_run_fsck(const char *argv0, const char *dir, unsigned members,
    int repair);
int d6m_clone_diskset(const char *srcdir, const char *dstdir,
    unsigned members, char *err, size_t errlen);

#endif
