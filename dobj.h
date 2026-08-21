#ifndef PDP10_DOBJ_H
#define PDP10_DOBJ_H

#include <stdio.h>

#define DOBJ_SEC_ABS  0
#define DOBJ_SEC_TEXT 1
#define DOBJ_SEC_DATA 2
#define DOBJ_SEC_BSS  3

#define DOBJ_SYM_DEF   1
#define DOBJ_SYM_UNDEF 2

#define DOBJ_RELOC_LOCAL_RH18  1
#define DOBJ_RELOC_SYMBOL_RH18 2
#define DOBJ_RELOC_LOCAL_LH18  3
#define DOBJ_RELOC_SYMBOL_LH18 4

#define DOBJ_NAME_MAX 63

struct dobj_word {
    unsigned long lh;
    unsigned long rh;
};

struct dobj_symbol {
    char name[DOBJ_NAME_MAX + 1];
    int kind;
    int sec;
    struct dobj_word value;
};

struct dobj_reloc {
    int loc_sec;
    int type;
    int target_sec;
    unsigned long offset;
    unsigned long symbol;
    struct dobj_word addend;
};

struct dobj_object {
    unsigned long text_words;
    unsigned long data_words;
    unsigned long bss_words;
    unsigned long symbol_count;
    unsigned long reloc_count;
    unsigned long entry_symbol;
    struct dobj_word *text;
    struct dobj_word *data;
    struct dobj_symbol *symbols;
    struct dobj_reloc *relocs;
};

struct dobj_archive_index {
    char name[DOBJ_NAME_MAX + 1];
    unsigned long member_offset;
};

struct dobj_archive {
    FILE *file;
    unsigned long index_count;
    struct dobj_archive_index *index;
};

void dobj_word_zero(struct dobj_word *w);
struct dobj_word dobj_word_halves(unsigned long lh, unsigned long rh);
int dobj_word_is_zero(struct dobj_word w);
int dobj_word_addend18(struct dobj_word w, long *value);

int dobj_read_word(FILE *f, struct dobj_word *w);
int dobj_write_word(FILE *f, struct dobj_word w);
int dobj_read(FILE *f, struct dobj_object *obj);
int dobj_write(FILE *f, const struct dobj_object *obj);
void dobj_free(struct dobj_object *obj);
int dobj_is_object(FILE *f);

int dobj_archive_open(FILE *f, struct dobj_archive *ar);
void dobj_archive_close(struct dobj_archive *ar);
int dobj_archive_find(const struct dobj_archive *ar, const char *name,
                      unsigned long *member_offset);
int dobj_archive_read_member(const struct dobj_archive *ar,
                             unsigned long member_offset,
                             struct dobj_object *obj);
int dobj_is_archive(FILE *f);
int dobj_archive_write(FILE *out, struct dobj_object *objects,
                       unsigned long count);

#endif
