#ifndef DAIMOS_TOOLS_DOBJ_NATIVE_H
#define DAIMOS_TOOLS_DOBJ_NATIVE_H

#include "u.h"

#define ND_SEC_ABS  0U
#define ND_SEC_TEXT 1U
#define ND_SEC_DATA 2U
#define ND_SEC_BSS  3U

#define ND_SYM_DEF   1U
#define ND_SYM_UNDEF 2U

#define ND_RELOC_LOCAL_RH18  1U
#define ND_RELOC_SYMBOL_RH18 2U
#define ND_RELOC_LOCAL_LH18  3U
#define ND_RELOC_SYMBOL_LH18 4U

#define ND_NAME_MAX 63U
#define ND_IO_WORDS 64U

struct nd_obj {
        kword_t base;
        kword_t text_words;
        kword_t data_words;
        kword_t bss_words;
        kword_t symbol_count;
        kword_t reloc_count;
        kword_t entry_symbol;
        kword_t text_off;
        kword_t data_off;
        kword_t symbol_off;
        kword_t reloc_off;
        kword_t object_words;
};

struct nd_symbol {
        unsigned int kind;
        unsigned int sec;
        kword_t value;
        char name[ND_NAME_MAX + 1U];
};

struct nd_reloc {
        unsigned int loc_sec;
        unsigned int type;
        unsigned int target_sec;
        kword_t offset;
        kword_t symbol;
        kword_t addend;
};

kword_t nd_lh(kword_t w);
kword_t nd_rh(kword_t w);
kword_t nd_halves(kword_t lh, kword_t rh);
kword_t nd_magic(const char *s);
unsigned int nd_name_words(unsigned int len);
unsigned int nd_strlen(const char *s);
int nd_streq(const char *a, const char *b);
void nd_strcpy(char *dst, const char *src);

int nd_seek(int fd, kword_t word);
int nd_read_exact(int fd, kword_t *dst, unsigned int words);
int nd_write_exact(int fd, const kword_t *src, unsigned int words);
int nd_read_at(int fd, kword_t word, kword_t *dst, unsigned int words);
int nd_write_at(int fd, kword_t word, const kword_t *src,
    unsigned int words);

int nd_read_name(int fd, unsigned int len, char *name);
int nd_write_name(int fd, const char *name);
int nd_obj_parse(int fd, kword_t base, struct nd_obj *obj);
int nd_obj_symbol(int fd, const struct nd_obj *obj, unsigned int index,
    struct nd_symbol *sym);
int nd_obj_each_symbol(int fd, const struct nd_obj *obj,
    int (*fn)(unsigned int, const struct nd_symbol *, void *), void *arg);
int nd_obj_reloc(int fd, const struct nd_obj *obj, unsigned int index,
    struct nd_reloc *rel);
int nd_addend18(kword_t w, int *value);

int nd_arc_header(int fd, kword_t *members, kword_t *index_count);
int nd_arc_find(int fd, const char *name, kword_t *member_off);

int nd_copy_words(int infd, kword_t inoff, int outfd, kword_t words);
int nd_put_name(int fd, const char *name);
int nd_put_uint(int fd, kword_t value);
int nd_put_octal(int fd, kword_t value, unsigned int digits);

#endif
