/*
 * p10super - conservative host-side PDP-10 scheduling similarity analyzer.
 *
 * This first experimental pass deliberately handles only instructions whose
 * AC dataflow is simple and whose effective address cannot hide a dependency.
 * It does not rewrite objects.  It canonicalizes independent straight-line
 * instruction regions and reports regions from different objects which can be
 * made byte-identical merely by reordering instructions.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "dobj.h"

#define MAX_REGION 8UL
#define F_FIXED 1U
#define MEM_UNKNOWN (-1)
#define MEM_AC      (-2)

struct block {
        unsigned long start;
        unsigned long end;
        unsigned long succ[2];
        unsigned int nsucc;
        unsigned int incoming_branch;
        unsigned int incoming_fallthrough;
};

#define NO_SUCC (~0UL)

static const struct dobj_reloc *rh_reloc(const struct dobj_object *,
    unsigned long);

struct input {
        const char *name;
        struct dobj_object obj;
};

struct insn {
        struct dobj_word word;
        const struct dobj_object *obj;
        unsigned long off;
        unsigned int ac;
        unsigned long reads;
        unsigned long writes;
        int mem_read;
        int mem_write;
        int mem_kind;
        unsigned long mem_id;
        const char *mem_symbol;
        struct dobj_word mem_addend;
        unsigned int flags;
};

struct region {
        int input;
        unsigned long off;
        unsigned long n;
        struct insn insn[MAX_REGION];
        int tail_candidate;
        unsigned long renameable;
};

enum candidate_kind {
        CAND_SCHEDULE,
        CAND_ZERO,
        CAND_RENAME
};

struct normalization_action {
        int input;
        unsigned long off;
        unsigned long n;
        unsigned int order[MAX_REGION];
        int zero_form;
        int rename[16];
};

struct candidate {
        enum candidate_kind kind;
        unsigned long a_region;
        unsigned long b_region;
        struct normalization_action action[2];
        unsigned long fold_save;
        unsigned long span;
};

struct plan_score {
        unsigned long fold_save;
        unsigned long span;
        unsigned long count;
};

struct plan_search {
        const struct candidate *candidate;
        unsigned long count;
        unsigned char *current;
        unsigned char *best;
        unsigned long *suffix_fold;
        unsigned long *suffix_span;
        struct plan_score best_score;
};

struct guard_shape {
        const struct block *guard;
        const struct block *taken;
        const struct block *fall;
        unsigned int family;
        unsigned int condition;
        unsigned long cond_off;
};

static int
unconditional_end_word(struct dobj_word w)
{
        unsigned long op = (w.lh >> 9) & 0777UL;
        return op == 0254UL || op == 0263UL; /* JRST, POPJ */
}

static int
skip_family(unsigned int op)
{
        if (op >= 0300 && op <= 0317)     /* CAI, CAM */
                return (int)(op & ~07U);
        if ((op >= 0330 && op <= 0337) || /* SKIP */
            (op >= 0350 && op <= 0357) || /* AOS */
            (op >= 0370 && op <= 0377))   /* SOS */
                return (int)(op & ~07U);
        if (op >= 0600 && op <= 0677)     /* TR/TL test families */
                return (int)(op & ~07U);
        return -1;
}

static int
is_conditional_skip_opcode(unsigned int op)
{
        unsigned int condition = op & 07U;

        return skip_family(op) >= 0 && condition != 0U && condition != 04U;
}

static int
is_always_skip_opcode(unsigned int op)
{
        return skip_family(op) >= 0 && (op & 07U) == 04U;
}

static int
is_control_word(struct dobj_word w)
{
        unsigned int op = (unsigned int)((w.lh >> 9) & 0777UL);
        if (op == 0254 || op == 0263)
                return 1;
        if ((op >= 0320 && op <= 0327) || /* JUMP */
            (op >= 0340 && op <= 0347) || /* AOJ */
            (op >= 0360 && op <= 0367))   /* SOJ */
                return 1;
        return is_conditional_skip_opcode(op) || is_always_skip_opcode(op);
}

static int
is_invertible_jump(struct dobj_word w)
{
        unsigned int op = (unsigned int)((w.lh >> 9) & 0777UL);
        return (op >= 0321 && op <= 0327 && op != 0324) ||
            (op >= 0341 && op <= 0347 && op != 0344) ||
            (op >= 0361 && op <= 0367 && op != 0364);
}

static unsigned long
build_blocks(const struct dobj_object *o, struct block **out)
{
        unsigned char *leader;
        struct block *b;
        unsigned long i, n = 0, cap = 0;

        if (o->text_words == 0) {
                *out = NULL;
                return 0;
        }
        leader = calloc((size_t)o->text_words, 1);
        if (leader == NULL)
                return 0;
        leader[0] = 1;
        for (i = 0; i < o->symbol_count; i++)
                if (o->symbols[i].kind == DOBJ_SYM_DEF &&
                    o->symbols[i].sec == DOBJ_SEC_TEXT &&
                    o->symbols[i].value.rh < o->text_words)
                        leader[o->symbols[i].value.rh] = 1;
        for (i = 0; i < o->reloc_count; i++) {
                const struct dobj_reloc *r = &o->relocs[i];
                if ((r->type == DOBJ_RELOC_LOCAL_RH18 ||
                    r->type == DOBJ_RELOC_LOCAL_LH18) &&
                    r->target_sec == DOBJ_SEC_TEXT &&
                    r->addend.rh < o->text_words)
                        leader[r->addend.rh] = 1;
        }
        for (i = 0; i + 1 < o->text_words; i++) {
                if (is_control_word(o->text[i]))
                        leader[i + 1] = 1;
                if ((is_conditional_skip_opcode((unsigned int)
                    ((o->text[i].lh >> 9) & 0777UL)) ||
                    is_always_skip_opcode((unsigned int)
                    ((o->text[i].lh >> 9) & 0777UL))) &&
                    i + 2 < o->text_words)
                        leader[i + 2] = 1;
        }

        for (i = 0; i < o->text_words; ) {
                unsigned long end = i + 1;
                struct block *q;
                while (end < o->text_words && !leader[end])
                        end++;
                if (n == cap) {
                        unsigned long nc = cap ? cap * 2 : 64;
                        q = realloc(*out, (size_t)nc * sizeof(**out));
                        if (q == NULL) {
                                free(leader);
                                free(*out);
                                *out = NULL;
                                return 0;
                        }
                        *out = q;
                        cap = nc;
                }
                b = *out;
                b[n].start = i;
                b[n].end = end;
                b[n].succ[0] = b[n].succ[1] = NO_SUCC;
                b[n].nsucc = 0;
                b[n].incoming_branch = 0;
                b[n].incoming_fallthrough = 0;
                n++;
                i = end;
        }
        b = *out;
        for (i = 0; i < n; i++) {
                unsigned long last = b[i].end - 1;
                unsigned int op = (unsigned int)((o->text[last].lh >> 9) & 0777UL);
                const struct dobj_reloc *rr = rh_reloc(o, last);
                unsigned long target = NO_SUCC;

                if (rr != NULL && rr->type == DOBJ_RELOC_LOCAL_RH18 &&
                    rr->target_sec == DOBJ_SEC_TEXT &&
                    rr->addend.rh < o->text_words)
                        target = rr->addend.rh;
                if (op == 0254) { /* JRST */
                        if (target != NO_SUCC)
                                b[i].succ[b[i].nsucc++] = target;
                } else if ((op >= 0320 && op <= 0327) ||
                    (op >= 0340 && op <= 0347) ||
                    (op >= 0360 && op <= 0367)) {
                        if (target != NO_SUCC)
                                b[i].succ[b[i].nsucc++] = target;
                        if (b[i].end < o->text_words && b[i].nsucc < 2)
                                b[i].succ[b[i].nsucc++] = b[i].end;
                } else if (is_conditional_skip_opcode(op)) {
                        if (b[i].end < o->text_words)
                                b[i].succ[b[i].nsucc++] = b[i].end;
                        if (b[i].end + 1 < o->text_words && b[i].nsucc < 2)
                                b[i].succ[b[i].nsucc++] = b[i].end + 1;
                } else if (is_always_skip_opcode(op)) {
                        if (b[i].end + 1 < o->text_words)
                                b[i].succ[b[i].nsucc++] = b[i].end + 1;
                } else if (op != 0263 && b[i].end < o->text_words) {
                        b[i].succ[b[i].nsucc++] = b[i].end;
                }
        }
        /*
         * Classify incoming edges.  An existing explicit branch can be
         * retargeted to a shared tail at zero execution cost.  A fall-through
         * edge cannot: replacing it by JRST would add an instruction.
         */
        for (i = 0; i < n; i++) {
                unsigned long si;
                for (si = 0; si < b[i].nsucc; si++) {
                        unsigned long j;
                        for (j = 0; j < n; j++)
                                if (b[j].start == b[i].succ[si]) {
                                        unsigned long last = b[i].end - 1;
                                        unsigned int op = (unsigned int)
                                            ((o->text[last].lh >> 9) & 0777UL);
                                        if ((op == 0254 ||
                                            (op >= 0320 && op <= 0327) ||
                                            (op >= 0340 && op <= 0347) ||
                                            (op >= 0360 && op <= 0367)) &&
                                            b[i].succ[si] != b[i].end)
                                                b[j].incoming_branch++;
                                        else
                                                b[j].incoming_fallthrough++;
                                        break;
                                }
                }
        }
        free(leader);
        return n;
}

static void
usage(void)
{
        fprintf(stderr, "usage: p10super object.dobj ...\n");
}

static const struct dobj_reloc *
half_reloc(const struct dobj_object *o, unsigned long off, int left)
{
        unsigned long i;
        for (i = 0; i < o->reloc_count; i++)
                if (o->relocs[i].loc_sec == DOBJ_SEC_TEXT &&
                    o->relocs[i].offset == off &&
                    (left ?
                    (o->relocs[i].type == DOBJ_RELOC_LOCAL_LH18 ||
                    o->relocs[i].type == DOBJ_RELOC_SYMBOL_LH18) :
                    (o->relocs[i].type == DOBJ_RELOC_LOCAL_RH18 ||
                    o->relocs[i].type == DOBJ_RELOC_SYMBOL_RH18)))
                        return &o->relocs[i];
        return NULL;
}

static const struct dobj_reloc *
rh_reloc(const struct dobj_object *o, unsigned long off)
{
        return half_reloc(o, off, 0);
}

static int
has_text_entry(const struct dobj_object *o, unsigned long off)
{
        unsigned long i;
        for (i = 0; i < o->symbol_count; i++)
                if (o->symbols[i].kind == DOBJ_SYM_DEF &&
                    o->symbols[i].sec == DOBJ_SEC_TEXT &&
                    o->symbols[i].value.rh == off)
                        return 1;
        for (i = 0; i < o->reloc_count; i++) {
                const struct dobj_reloc *r = &o->relocs[i];
                if ((r->type == DOBJ_RELOC_LOCAL_RH18 ||
                    r->type == DOBJ_RELOC_LOCAL_LH18) &&
                    r->target_sec == DOBJ_SEC_TEXT &&
                    r->addend.rh == off)
                        return 1;
        }
        return 0;
}

static int
has_named_text_entry(const struct dobj_object *o, unsigned long off)
{
        unsigned long i;
        for (i = 0; i < o->symbol_count; i++)
                if (o->symbols[i].kind == DOBJ_SYM_DEF &&
                    o->symbols[i].sec == DOBJ_SEC_TEXT &&
                    o->symbols[i].value.rh == off)
                        return 1;
        return 0;
}

static int
cai0_jrst_replacement(const struct dobj_object *o, unsigned long off,
    unsigned int *replacement)
{
        unsigned long lh, nlh;
        unsigned int op, nextop;
        if (off + 1 >= o->text_words || rh_reloc(o, off) != NULL ||
            has_text_entry(o, off + 1))
                return 0;
        lh = o->text[off].lh;
        nlh = o->text[off + 1].lh;
        op = (unsigned int)((lh >> 9) & 0777UL);
        nextop = (unsigned int)((nlh >> 9) & 0777UL);
        if (op < 0301 || op > 0307 || o->text[off].rh != 0 ||
            (lh & 037UL) != 0UL || nextop != 0254 ||
            ((nlh >> 5) & 017UL) != 0UL)
                return 0;
        /*
         * CAIx AC,0 skips the JRST when condition x is true.  A single
         * JUMP with the complementary condition therefore has identical
         * control flow.  PDP-10 condition complements differ by bit 04.
         */
        *replacement = 0320U + (((op - 0300U) ^ 04U) & 07U);
        return 1;
}

static const char *
text_symbol_before(const struct dobj_object *o, unsigned long off,
    unsigned long *basep)
{
        const char *best = NULL;
        unsigned long base = 0UL;
        unsigned long i;

        for (i = 0UL; i < o->symbol_count; i++) {
                const struct dobj_symbol *s = &o->symbols[i];

                if (s->kind != DOBJ_SYM_DEF || s->sec != DOBJ_SEC_TEXT ||
                    s->value.rh > off)
                        continue;
                if (best == NULL || s->value.rh > base) {
                        best = s->name;
                        base = s->value.rh;
                }
        }
        if (basep != NULL)
                *basep = base;
        return best;
}

static void
print_source_location(FILE *f, const struct input *in, unsigned long off)
{
        unsigned long base;
        const char *s = text_symbol_before(&in->obj, off, &base);

        fprintf(f, "%s:", in->name);
        if (s == NULL)
                fprintf(f, "TEXT+%06lo", off);
        else if (base == off)
                fprintf(f, "%s", s);
        else
                fprintf(f, "%s+%lo", s, off - base);
}

static const char *
condition_mnemonic(unsigned int family, unsigned int op)
{
        static const char *const suffix[8] = {
                "", "L", "E", "LE", "A", "GE", "N", "G"
        };
        static char name[16];
        const char *base;

        switch (family) {
        case 0300U: base = "CAI"; break;
        case 0320U: base = "JUMP"; break;
        case 0340U: base = "AOJ"; break;
        case 0360U: base = "SOJ"; break;
        default: return "OP";
        }
        snprintf(name, sizeof(name), "%s%s", base, suffix[(op - family) & 7U]);
        return name;
}

static void
print_ea(FILE *f, const struct dobj_object *o, unsigned long off,
    struct dobj_word w)
{
        const struct dobj_reloc *r = rh_reloc(o, off);
        unsigned int ind = (unsigned int)((w.lh >> 4) & 1UL);
        unsigned int xr = (unsigned int)(w.lh & 017UL);

        if (ind)
                fputc('@', f);
        if (r != NULL && (r->type == DOBJ_RELOC_SYMBOL_RH18) &&
            r->symbol != 0UL && r->symbol <= o->symbol_count) {
                long add = 0L;
                fprintf(f, "%s", o->symbols[r->symbol - 1UL].name);
                if (dobj_word_addend18(r->addend, &add) == 0 && add != 0L)
                        fprintf(f, "%+ld", add);
        } else if (r != NULL && r->type == DOBJ_RELOC_LOCAL_RH18 &&
            r->target_sec == DOBJ_SEC_TEXT) {
                unsigned long base;
                const char *s = text_symbol_before(o, r->addend.rh, &base);
                if (s != NULL && base == r->addend.rh)
                        fprintf(f, "%s", s);
                else
                        fprintf(f, "%06lo", r->addend.rh);
        } else {
                fprintf(f, "%06lo", w.rh);
        }
        if (xr != 0U)
                fprintf(f, "(%o)", xr);
}

static void
print_subset_insn(FILE *f, const struct dobj_object *o, unsigned long off,
    struct dobj_word w)
{
        unsigned int op = (unsigned int)((w.lh >> 9) & 0777UL);
        unsigned int ac = (unsigned int)((w.lh >> 5) & 017UL);

        if (op >= 0300U && op <= 0307U)
                fprintf(f, "%s %o,", condition_mnemonic(0300U, op), ac);
        else if (op >= 0320U && op <= 0327U)
                fprintf(f, "%s %o,", condition_mnemonic(0320U, op), ac);
        else if (op >= 0340U && op <= 0347U)
                fprintf(f, "%s %o,", condition_mnemonic(0340U, op), ac);
        else if (op >= 0360U && op <= 0367U)
                fprintf(f, "%s %o,", condition_mnemonic(0360U, op), ac);
        else if (op == 0271U)
                fprintf(f, "ADDI %o,", ac);
        else if (op == 0275U)
                fprintf(f, "SUBI %o,", ac);
        else if (op == 0254U)
                fprintf(f, "JRST ");
        else {
                fprintf(f, "%06lo,,%06lo", w.lh, w.rh);
                return;
        }
        print_ea(f, o, off, w);
}

static struct dobj_word
peephole_replacement_word(const struct dobj_object *o, unsigned long off,
    unsigned int replacement)
{
        struct dobj_word w = o->text[off + 1UL];
        unsigned int first_ac = (unsigned int)((o->text[off].lh >> 5) & 017UL);

        w.lh &= 037UL;
        w.lh |= ((unsigned long)replacement << 9) |
            ((unsigned long)first_ac << 5);
        return w;
}

static void
print_peephole_rewrite(const struct input *in, unsigned long off,
    unsigned int replacement, const char *reason)
{
        struct dobj_word nw = peephole_replacement_word(&in->obj, off,
            replacement);

        printf("REWRITE ");
        print_source_location(stdout, in, off);
        printf(" : SAVE 1; COST=ZERO; PARTNER=-; LEGAL=%s\n  OLD ", reason);
        print_subset_insn(stdout, &in->obj, off, in->obj.text[off]);
        printf(" ; ");
        print_subset_insn(stdout, &in->obj, off + 1UL,
            in->obj.text[off + 1UL]);
        printf("\n  NEW ");
        print_subset_insn(stdout, &in->obj, off + 1UL, nw);
        putchar('\n');
}

static int
incdec_jump_replacement(const struct dobj_object *o, unsigned long off,
    unsigned int *replacement)
{
        unsigned long lh, nlh;
        unsigned int op, nop, ac, nac;

        if (off + 1 >= o->text_words || rh_reloc(o, off) != NULL ||
            has_text_entry(o, off + 1))
                return 0;
        lh = o->text[off].lh;
        nlh = o->text[off + 1].lh;
        op = (unsigned int)((lh >> 9) & 0777UL);
        nop = (unsigned int)((nlh >> 9) & 0777UL);
        ac = (unsigned int)((lh >> 5) & 017UL);
        nac = (unsigned int)((nlh >> 5) & 017UL);
        if ((op != 0271 && op != 0275) || o->text[off].rh != 1 ||
            (lh & 037UL) != 0 || nop < 0321 || nop > 0327 || ac != nac)
                return 0;
        *replacement = (op == 0271 ? 0340U : 0360U) + (nop - 0320U);
        return 1;
}

static int
incdec_jrst_replacement(const struct dobj_object *o, unsigned long off,
    unsigned int *replacement)
{
        unsigned long lh, nlh;
        unsigned int op, nop, nac;

        if (off + 1 >= o->text_words || rh_reloc(o, off) != NULL ||
            has_text_entry(o, off + 1))
                return 0;
        lh = o->text[off].lh;
        nlh = o->text[off + 1].lh;
        op = (unsigned int)((lh >> 9) & 0777UL);
        nop = (unsigned int)((nlh >> 9) & 0777UL);
        nac = (unsigned int)((nlh >> 5) & 017UL);
        if ((op != 0271 && op != 0275) || o->text[off].rh != 1 ||
            (lh & 037UL) != 0 || nop != 0254 || nac != 0)
                return 0;
        *replacement = op == 0271 ? 0344U : 0364U; /* AOJA / SOJA */
        return 1;
}

static int
reloc_equal(const struct dobj_object *ao, unsigned long aoff,
    const struct dobj_object *bo, unsigned long boff, int left)
{
        const struct dobj_reloc *a = half_reloc(ao, aoff, left);
        const struct dobj_reloc *b = half_reloc(bo, boff, left);
        int asym, bsym;

        if (a == NULL || b == NULL)
                return a == b;
        asym = a->type == DOBJ_RELOC_SYMBOL_LH18 ||
            a->type == DOBJ_RELOC_SYMBOL_RH18;
        bsym = b->type == DOBJ_RELOC_SYMBOL_LH18 ||
            b->type == DOBJ_RELOC_SYMBOL_RH18;
        if (asym != bsym ||
            a->addend.lh != b->addend.lh || a->addend.rh != b->addend.rh)
                return 0;
        if (asym) {
                if (a->symbol == 0UL || a->symbol > ao->symbol_count ||
                    b->symbol == 0UL || b->symbol > bo->symbol_count)
                        return 0;
                return strcmp(ao->symbols[a->symbol - 1UL].name,
                    bo->symbols[b->symbol - 1UL].name) == 0;
        }
        return a->target_sec == b->target_sec;
}

static int
insn_equal(const struct insn *a, const struct insn *b)
{
        const struct dobj_reloc *al = half_reloc(a->obj, a->off, 1);
        const struct dobj_reloc *bl = half_reloc(b->obj, b->off, 1);
        const struct dobj_reloc *ar = half_reloc(a->obj, a->off, 0);
        const struct dobj_reloc *br = half_reloc(b->obj, b->off, 0);

        if (al == NULL && bl == NULL && a->word.lh != b->word.lh)
                return 0;
        if (ar == NULL && br == NULL && a->word.rh != b->word.rh)
                return 0;
        return reloc_equal(a->obj, a->off, b->obj, b->off, 1) &&
            reloc_equal(a->obj, a->off, b->obj, b->off, 0);
}

static int
zero_form_equal(const struct insn *a, const struct insn *b)
{
        unsigned int aop, bop, aac, bac;

        if (insn_equal(a, b))
                return 1;
        aop = (unsigned int)((a->word.lh >> 9) & 0777UL);
        bop = (unsigned int)((b->word.lh >> 9) & 0777UL);
        if (!((aop == 0201 && bop == 0400) ||
            (aop == 0400 && bop == 0201)))
                return 0;
        if (a->word.rh != 0 || b->word.rh != 0 ||
            (a->word.lh & 037UL) != 0 || (b->word.lh & 037UL) != 0 ||
            half_reloc(a->obj, a->off, 0) != NULL ||
            half_reloc(b->obj, b->off, 0) != NULL)
                return 0;
        aac = (unsigned int)((a->word.lh >> 5) & 017UL);
        bac = (unsigned int)((b->word.lh >> 5) & 017UL);
        if (aac != bac)
                return 0;
        if (aop == 0201 && has_text_entry(a->obj, a->off))
                return 0;
        if (bop == 0201 && has_text_entry(b->obj, b->off))
                return 0;
        return 1;
}

static int
word_at_equal(const struct dobj_object *ao, unsigned long aoff,
    const struct dobj_object *bo, unsigned long boff)
{
        struct insn a, b;
        memset(&a, 0, sizeof(a));
        memset(&b, 0, sizeof(b));
        a.word = ao->text[aoff]; a.obj = ao; a.off = aoff;
        b.word = bo->text[boff]; b.obj = bo; b.off = boff;
        return insn_equal(&a, &b);
}

static unsigned long
common_block_suffix(const struct dobj_object *ao, const struct block *a,
    const struct dobj_object *bo, const struct block *b)
{
        unsigned long n = 0;
        unsigned long an = a->end - a->start;
        unsigned long bn = b->end - b->start;
        while (n < an && n < bn &&
            word_at_equal(ao, a->end - 1 - n, bo, b->end - 1 - n))
                n++;
        return n;
}

static const struct block *
block_at(const struct block *b, unsigned long n, unsigned long start)
{
        unsigned long i;
        for (i = 0; i < n; i++)
                if (b[i].start == start)
                        return &b[i];
        return NULL;
}

static int same_block_occurrence(const struct dobj_object *,
    const struct block *, const struct dobj_object *, const struct block *);

static int
compare_operand_equal(const struct dobj_object *ao, unsigned long aoff,
    const struct dobj_object *bo, unsigned long boff)
{
        struct insn a, b;

        memset(&a, 0, sizeof(a));
        memset(&b, 0, sizeof(b));
        a.word = ao->text[aoff]; a.obj = ao; a.off = aoff;
        b.word = bo->text[boff]; b.obj = bo; b.off = boff;
        /*
         * Strip only the three condition bits.  The CAI/CAM family, AC,
         * indirect/index mode and operand/relocation must still match.
         */
        a.word.lh &= ~(07UL << 9);
        b.word.lh &= ~(07UL << 9);
        return insn_equal(&a, &b);
}

static int
guard_shape_at(const struct dobj_object *o, const struct block *g,
    const struct block *blocks, unsigned long nb, struct guard_shape *out)
{
        unsigned long last;
        unsigned int op, family;
        const struct block *j, *fall, *taken;

        if (g->end <= g->start || g->nsucc != 2)
                return 0;
        last = g->end - 1;
        op = (unsigned int)((o->text[last].lh >> 9) & 0777UL);
        family = (unsigned int)skip_family(op);
        if (!is_conditional_skip_opcode(op))
                return 0;
        /*
         * The non-skipped successor must be a private one-word JRST.
         * Otherwise collapsing/reorienting the guard could affect another
         * entry point.
         */
        j = block_at(blocks, nb, g->succ[0]);
        fall = block_at(blocks, nb, g->succ[1]);
        if (j == NULL || fall == NULL || j->end != j->start + 1 ||
            ((o->text[j->start].lh >> 9) & 0777UL) != 0254UL ||
            j->nsucc != 1 || has_text_entry(o, j->start))
                return 0;
        taken = block_at(blocks, nb, j->succ[0]);
        if (taken == NULL)
                return 0;
        out->guard = g;
        out->taken = taken;
        out->fall = fall;
        out->family = family;
        /*
         * CAI/CAM condition C skips the JRST when C is true, so the JRST
         * target is reached on !C.  Complement conditions differ by bit 04.
         */
        out->condition = ((op - family) ^ 04U) & 07U;
        out->cond_off = last;
        return 1;
}

static unsigned long
guard_suffix_score(const struct dobj_object *ao, const struct guard_shape *a,
    const struct dobj_object *bo, const struct guard_shape *b,
    unsigned long *taken_suffix, unsigned long *fall_suffix)
{
        unsigned long i, an;
        const struct block *bt, *bf;

        *taken_suffix = *fall_suffix = 0;
        if (a->family != b->family ||
            !compare_operand_equal(ao, a->cond_off, bo, b->cond_off))
                return 0;
        an = a->guard->end - a->guard->start;
        if (an != b->guard->end - b->guard->start)
                return 0;
        for (i = 0; i + 1 < an; i++)
                if (!word_at_equal(ao, a->guard->start + i,
                    bo, b->guard->start + i))
                        return 0;
        bt = b->taken;
        bf = b->fall;
        if (a->condition == (b->condition ^ 04U)) {
                bt = b->fall;
                bf = b->taken;
        } else if (a->condition != b->condition) {
                return 0;
        }
        if (!same_block_occurrence(ao, a->taken, bo, bt))
                *taken_suffix = common_block_suffix(ao, a->taken, bo, bt);
        if (!same_block_occurrence(ao, a->fall, bo, bf))
                *fall_suffix = common_block_suffix(ao, a->fall, bo, bf);
        return *taken_suffix + *fall_suffix;
}

static int
same_block_occurrence(const struct dobj_object *ao, const struct block *a,
    const struct dobj_object *bo, const struct block *b)
{
        return ao == bo && a->start == b->start && a->end == b->end;
}

static int
local_control_target(const struct dobj_object *o, unsigned long off)
{
        const struct dobj_reloc *r = rh_reloc(o, off);
        unsigned int op;
        if (r == NULL || r->type != DOBJ_RELOC_LOCAL_RH18 ||
            r->target_sec != DOBJ_SEC_TEXT)
                return 0;
        op = (unsigned int)((o->text[off].lh >> 9) & 0777UL);
        return op == 0254 ||
            (op >= 0320 && op <= 0327) ||
            (op >= 0340 && op <= 0347) ||
            (op >= 0360 && op <= 0367);
}

/*
 * Compare block instruction shape while abstracting a local branch target.
 * This is for CFG-layout discovery only: final equality still goes through
 * reassembly and p10fold.
 */
static int
block_shape_equal(const struct dobj_object *ao, const struct block *a,
    const struct dobj_object *bo, const struct block *b)
{
        unsigned long i, n = a->end - a->start;
        if (n != b->end - b->start)
                return 0;
        for (i = 0; i < n; i++) {
                unsigned long ap = a->start + i, bp = b->start + i;
                if (local_control_target(ao, ap) &&
                    local_control_target(bo, bp)) {
                        if (ao->text[ap].lh != bo->text[bp].lh)
                                return 0;
                } else if (!word_at_equal(ao, ap, bo, bp)) {
                        return 0;
                }
        }
        return 1;
}

/*
 * Return 1 for the same successor orientation, 2 when the blocks differ
 * only by an invertible terminal JUMP/AOJ/SOJ condition and therefore have
 * equivalent semantics with their two successors exchanged.
 */
static int
block_shape_relation(const struct dobj_object *ao, const struct block *a,
    const struct dobj_object *bo, const struct block *b)
{
        unsigned long i, n;
        unsigned long opmask = 0777UL << 9;
        unsigned int aop, bop;

        if (block_shape_equal(ao, a, bo, b))
                return 1;
        n = a->end - a->start;
        if (n == 0 || n != b->end - b->start ||
            a->nsucc != 2 || b->nsucc != 2 ||
            !is_invertible_jump(ao->text[a->end - 1]) ||
            !is_invertible_jump(bo->text[b->end - 1]))
                return 0;
        for (i = 0; i + 1 < n; i++)
                if (!word_at_equal(ao, a->start + i, bo, b->start + i))
                        return 0;
        aop = (unsigned int)
            ((ao->text[a->end - 1].lh >> 9) & 0777UL);
        bop = (unsigned int)
            ((bo->text[b->end - 1].lh >> 9) & 0777UL);
        if ((aop ^ 04U) != bop ||
            (ao->text[a->end - 1].lh & ~opmask) !=
            (bo->text[b->end - 1].lh & ~opmask) ||
            !local_control_target(ao, a->end - 1) ||
            !local_control_target(bo, b->end - 1))
                return 0;
        return 2;
}

struct cfg_seen {
        const struct block *a;
        const struct block *b;
};

static unsigned long
cfg_shape_score_rec(const struct dobj_object *ao, const struct block *a,
    const struct block *ab, unsigned long na,
    const struct dobj_object *bo, const struct block *b,
    const struct block *bb, unsigned long nb, unsigned int depth,
    struct cfg_seen *seen, unsigned int nseen, unsigned int *blocks,
    int *closed)
{
        unsigned long score, i;
        int relation;

        if (same_block_occurrence(ao, a, bo, b))
                return 0;
        if (depth == 0) {
                *closed = 0;
                return 0;
        }
        relation = block_shape_relation(ao, a, bo, b);
        if (relation == 0 || a->nsucc != b->nsucc) {
                *closed = 0;
                return 0;
        }
        for (i = 0; i < nseen; i++)
                if (seen[i].a == a && seen[i].b == b)
                        return 0;
        if (nseen >= 8) {
                *closed = 0;
                return 0;
        }
        seen[nseen].a = a;
        seen[nseen].b = b;
        nseen++;
        score = a->end - a->start;
        (*blocks)++;
        for (i = 0; i < a->nsucc; i++) {
                unsigned long bj = relation == 2 ? 1UL - i : i;
                const struct block *as = block_at(ab, na, a->succ[i]);
                const struct block *bs = block_at(bb, nb, b->succ[bj]);
                if (as == NULL || bs == NULL) {
                        if (as != bs)
                                *closed = 0;
                        continue;
                }
                if (same_block_occurrence(ao, as, bo, bs))
                        continue;
                if (block_shape_relation(ao, as, bo, bs) == 0 ||
                    as->nsucc != bs->nsucc) {
                        *closed = 0;
                        continue;
                }
                score += cfg_shape_score_rec(ao, as, ab, na,
                    bo, bs, bb, nb, depth - 1, seen, nseen, blocks, closed);
        }
        return score;
}

static unsigned long
cfg_shape_score(const struct dobj_object *ao, const struct block *a,
    const struct block *ab, unsigned long na,
    const struct dobj_object *bo, const struct block *b,
    const struct block *bb, unsigned long nb, unsigned int *blocks,
    int *closed_out)
{
        struct cfg_seen seen[8];
        int closed = 1;
        *blocks = 0;
        {
                unsigned long score = cfg_shape_score_rec(
                    ao, a, ab, na, bo, b, bb, nb,
                    4, seen, 0, blocks, &closed);
                *closed_out = closed;
                return score;
        }
}

static int
inverted_jump_blocks(const struct dobj_object *ao, const struct block *a,
    const struct block *ab, unsigned long na,
    const struct dobj_object *bo, const struct block *b,
    const struct block *bb, unsigned long nb)
{
        unsigned long i, an = a->end - a->start;
        unsigned int aop, bop;
        const struct block *at, *af, *bt, *bf;
        unsigned long opmask = 0777UL << 9;

        if (an != b->end - b->start || an == 0 ||
            a->nsucc != 2 || b->nsucc != 2)
                return 0;
        if (!is_invertible_jump(ao->text[a->end - 1]) ||
            !is_invertible_jump(bo->text[b->end - 1]))
                return 0;
        for (i = 0; i + 1 < an; i++)
                if (!word_at_equal(ao, a->start + i, bo, b->start + i))
                        return 0;
        aop = (unsigned int)((ao->text[a->end - 1].lh >> 9) & 0777UL);
        bop = (unsigned int)((bo->text[b->end - 1].lh >> 9) & 0777UL);
        if ((aop ^ 04U) != bop ||
            (ao->text[a->end - 1].lh & ~opmask) !=
            (bo->text[b->end - 1].lh & ~opmask))
                return 0;
        at = block_at(ab, na, a->succ[0]);
        af = block_at(ab, na, a->succ[1]);
        bt = block_at(bb, nb, b->succ[0]);
        bf = block_at(bb, nb, b->succ[1]);
        if (at == NULL || af == NULL || bt == NULL || bf == NULL)
                return 0;
        return block_shape_equal(ao, at, bo, bf) &&
            block_shape_equal(ao, af, bo, bt);
}

static unsigned long
inverted_jump_suffix_score(const struct dobj_object *ao,
    const struct block *a, const struct block *ab, unsigned long na,
    const struct dobj_object *bo, const struct block *b,
    const struct block *bb, unsigned long nb,
    unsigned long *taken_suffix, unsigned long *fall_suffix)
{
        unsigned long i, an = a->end - a->start;
        unsigned int aop, bop;
        const struct block *at, *af, *bt, *bf;
        unsigned long opmask = 0777UL << 9;

        *taken_suffix = *fall_suffix = 0;
        if (an != b->end - b->start || an == 0 ||
            a->nsucc != 2 || b->nsucc != 2)
                return 0;
        if (!is_invertible_jump(ao->text[a->end - 1]) ||
            !is_invertible_jump(bo->text[b->end - 1]))
                return 0;
        for (i = 0; i + 1 < an; i++)
                if (!word_at_equal(ao, a->start + i, bo, b->start + i))
                        return 0;
        aop = (unsigned int)((ao->text[a->end - 1].lh >> 9) & 0777UL);
        bop = (unsigned int)((bo->text[b->end - 1].lh >> 9) & 0777UL);
        if ((aop ^ 04U) != bop ||
            (ao->text[a->end - 1].lh & ~opmask) !=
            (bo->text[b->end - 1].lh & ~opmask))
                return 0;
        at = block_at(ab, na, a->succ[0]);
        af = block_at(ab, na, a->succ[1]);
        bt = block_at(bb, nb, b->succ[0]);
        bf = block_at(bb, nb, b->succ[1]);
        if (at == NULL || af == NULL || bt == NULL || bf == NULL)
                return 0;
        if (!same_block_occurrence(ao, at, bo, bf))
                *taken_suffix = common_block_suffix(ao, at, bo, bf);
        if (!same_block_occurrence(ao, af, bo, bt))
                *fall_suffix = common_block_suffix(ao, af, bo, bt);
        return *taken_suffix + *fall_suffix;
}

static unsigned long
popj_epilogue_start(const struct dobj_object *o, const struct block *b)
{
        unsigned long p;
        unsigned int op;

        if (b->end <= b->start)
                return b->end;
        p = b->end - 1;
        op = (unsigned int)((o->text[p].lh >> 9) & 0777UL);
        if (op != 0263) /* POPJ */
                return b->end;
        while (p > b->start) {
                op = (unsigned int)((o->text[p - 1].lh >> 9) & 0777UL);
                if (op != 0262) /* POP */
                        break;
                p--;
        }
        return p;
}

static unsigned long
common_epilogue_suffix(const struct dobj_object *ao, const struct block *a,
    const struct dobj_object *bo, const struct block *b)
{
        unsigned long as = popj_epilogue_start(ao, a);
        unsigned long bs = popj_epilogue_start(bo, b);
        unsigned long n;

        if (as == a->end || bs == b->end)
                return 0;
        n = common_block_suffix(ao, a, bo, b);
        if (n > a->end - as)
                n = a->end - as;
        if (n > b->end - bs)
                n = b->end - bs;
        return n;
}

static unsigned long
branch_targets_offset(const struct dobj_object *o, unsigned long off)
{
        unsigned long i, n = 0;
        for (i = 0; i < o->reloc_count; i++) {
                const struct dobj_reloc *r = &o->relocs[i];
                unsigned int op;
                if (r->loc_sec != DOBJ_SEC_TEXT ||
                    r->type != DOBJ_RELOC_LOCAL_RH18 ||
                    r->target_sec != DOBJ_SEC_TEXT ||
                    r->addend.rh != off || r->offset >= o->text_words)
                        continue;
                op = (unsigned int)((o->text[r->offset].lh >> 9) & 0777UL);
                if (op == 0254 ||
                    (op >= 0320 && op <= 0327) ||
                    (op >= 0340 && op <= 0347) ||
                    (op >= 0360 && op <= 0367))
                        n++;
        }
        return n;
}

static int
has_fallthrough_into(const struct dobj_object *o, unsigned long off)
{
        unsigned int op;
        if (off == 0)
                return 0;
        op = (unsigned int)((o->text[off - 1].lh >> 9) & 0777UL);
        /*
         * Be conservative.  JRST and POPJ are the only terminal forms this
         * analyzer currently proves cannot reach the following word.
         */
        return op != 0254 && op != 0263;
}

static int
retargetable_tail(const struct dobj_object *o, unsigned long off,
    unsigned long words)
{
        return branch_targets_offset(o, off) != 0 &&
            !has_fallthrough_into(o, off) && words != 0 &&
            unconditional_end_word(o->text[off + words - 1]);
}

static int
memory_id(const struct dobj_object *o, unsigned long off,
    int *kind, unsigned long *id)
{
        const struct dobj_reloc *r = rh_reloc(o, off);
        if (r == NULL) {
                *id = o->text[off].rh;
                *kind = *id < 020UL ? MEM_AC : DOBJ_SEC_ABS;
                return 1;
        }
        if (r->type == DOBJ_RELOC_LOCAL_RH18) {
                *kind = r->target_sec;
                *id = r->addend.rh;
                return 1;
        }
        /*
         * External-symbol operands need a canonical name/addend in the
         * comparison key, not the object-local symbol number.  Until that
         * representation is added, make them a barrier rather than risk a
         * false match.
         */
        return 0;
}

/*
 * PDP-6/PDP-10 effective addresses 0..17 name the accumulator file.  Once a
 * direct memory operand has been decoded as MEM_AC, fold that access into the
 * ordinary AC dependency masks.  Keeping it as an unrelated memory identity
 * would incorrectly allow a load/store through AC N to cross an instruction
 * which writes/reads AC N.
 */
static void
normalize_ac_memory(struct insn *d)
{
        unsigned long bit;

        if (d->mem_kind != MEM_AC)
                return;
        bit = 1UL << d->mem_id;
        if (d->mem_read)
                d->reads |= bit;
        if (d->mem_write)
                d->writes |= bit;
        d->mem_read = 0;
        d->mem_write = 0;
        d->mem_kind = 0;
        d->mem_id = 0UL;
}

/*
 * Admit only register/immediate operations with fully understood AC effects.
 * Memory-reference instructions are intentionally left for the next pass:
 * alias analysis must be correct before they may cross each other.
 */
static int
decode_safe(const struct dobj_object *o, unsigned long off, struct insn *d)
{
        unsigned long lh = o->text[off].lh;
        unsigned int op = (unsigned int)((lh >> 9) & 0777UL);
        unsigned int ac = (unsigned int)((lh >> 5) & 017UL);
        unsigned int ind = (unsigned int)((lh >> 4) & 1UL);
        unsigned int xr = (unsigned int)(lh & 017UL);
        unsigned long bit = 1UL << ac;

        if (ind || xr)
                return 0;
        d->word = o->text[off];
        d->obj = o;
        d->off = off;
        d->ac = ac;
        d->reads = d->writes = 0;
        d->mem_read = d->mem_write = 0;
        d->mem_kind = 0; d->mem_id = 0;
        d->mem_symbol = NULL;
        d->mem_addend.lh = d->mem_addend.rh = 0;
        d->flags = 0;

        switch (op) {
        case 0201: case 0205: case 0211: case 0215: /* MOVEI family */
        case 0400: case 0401:                         /* SETZ/SETZI */
        case 0474: case 0475:                         /* SETO/SETOI */
                d->writes = bit;
                return 1;
        /*
         * Plain halfword loads preserve the other half of the destination
         * AC, so both the memory and the old AC value are inputs.  The
         * immediate forms likewise preserve half of the AC.
         */
        case 0501: case 0505: case 0541: case 0545: /* HLLI/HRLI/HRRI/HLRI */
                d->reads = bit;
                d->writes = bit;
                return 1;
        case 0271: case 0275: /* ADDI/SUBI */
                d->reads = bit;
                d->writes = bit;
                return 1;
        case 0431: /* XORI */
        case 0435: /* IORI */
        case 0405: /* ANDI */
        case 0445: /* EQVI */
                d->reads = bit;
                d->writes = bit;
                return 1;
        case 0242: /* LSH */
                d->reads = bit;
                d->writes = bit;
                return 1;
        case 0135: /* LDB: byte-pointer target is dynamic, pointer unchanged */
                d->mem_read = 1;
                d->mem_kind = MEM_UNKNOWN;
                d->writes = bit;
                return 1;
        case 0137: /* DPB: byte-pointer target is dynamic, pointer unchanged */
                d->mem_write = 1;
                d->mem_kind = MEM_UNKNOWN;
                d->reads = bit;
                return 1;
        case 0200: case 0204: case 0210: case 0214: /* MOVE/MOVS/N/M */
        case 0270: case 0274:                         /* ADD/SUB */
        case 0404: case 0430: case 0434: case 0444: /* AND/XOR/IOR/EQV */
                if (!memory_id(o, off, &d->mem_kind, &d->mem_id))
                        return 0;
                d->mem_read = 1;
                if (op == 0270 || op == 0274 ||
                    op == 0404 || op == 0430 ||
                    op == 0434 || op == 0444)
                        d->reads = bit;
                d->writes = bit;
                normalize_ac_memory(d);
                return 1;
        case 0500: case 0504: case 0540: case 0544: /* HLL/HRL/HRR/HLR */
                if (!memory_id(o, off, &d->mem_kind, &d->mem_id))
                        return 0;
                d->mem_read = 1;
                d->reads = bit;
                d->writes = bit;
                normalize_ac_memory(d);
                return 1;
        /*
         * Z/O/E halfword loads determine the whole AC: the untouched half is
         * zero-filled, one-filled, or sign-extended.  They do not depend on
         * the old AC value.
         */
        case 0510: case 0514: case 0520: case 0524:
        case 0530: case 0534:
        case 0550: case 0554: case 0560: case 0564:
        case 0570: case 0574:
                if (!memory_id(o, off, &d->mem_kind, &d->mem_id))
                        return 0;
                d->mem_read = 1;
                d->writes = bit;
                normalize_ac_memory(d);
                return 1;
        case 0511: case 0515: case 0521: case 0525:
        case 0531: case 0535:
        case 0551: case 0555: case 0561: case 0565:
        case 0571: case 0575:
                d->writes = bit;
                return 1;
        case 0202: case 0206: case 0212: case 0216: /* MOVE*M */
        case 0502: case 0506: case 0542: case 0546: /* HLLM/HRLM/HRRM/HLRM */
                if (!memory_id(o, off, &d->mem_kind, &d->mem_id))
                        return 0;
                d->mem_write = 1;
                d->reads = bit;
                normalize_ac_memory(d);
                return 1;
        case 0402: case 0476: /* SETZM/SETOM */
                if (!memory_id(o, off, &d->mem_kind, &d->mem_id))
                        return 0;
                d->mem_write = 1;
                normalize_ac_memory(d);
                return 1;
        case 0403: case 0477: /* SETZB/SETOB */
                if (!memory_id(o, off, &d->mem_kind, &d->mem_id))
                        return 0;
                d->mem_write = 1;
                d->writes = bit;
                normalize_ac_memory(d);
                return 1;
        case 0406: case 0432: case 0436: case 0446: /* ANDM/XORM/IORM/EQVM */
                if (!memory_id(o, off, &d->mem_kind, &d->mem_id))
                        return 0;
                d->mem_read = d->mem_write = 1;
                d->reads = bit;
                normalize_ac_memory(d);
                return 1;
        case 0407: case 0433: case 0437: case 0447: /* ANDB/XORB/IORB/EQVB */
                if (!memory_id(o, off, &d->mem_kind, &d->mem_id))
                        return 0;
                d->mem_read = d->mem_write = 1;
                d->reads = bit;
                d->writes = bit;
                normalize_ac_memory(d);
                return 1;
        /*
         * Comparison/test/skip instructions are useful structural anchors.
         * They are admitted but never moved across another instruction.
         * CAI reads AC and an immediate; CAM reads AC and memory.  TR/TL
         * test AC against an immediate mask.  Their skip semantics make them
         * fixed nodes even though they do not write an AC.
         */
        case 0300: case 0301: case 0302: case 0303:
        case 0304: case 0305: case 0306: case 0307: /* CAI */
                d->reads = bit; d->flags = F_FIXED; return 1;
        case 0310: case 0311: case 0312: case 0313:
        case 0314: case 0315: case 0316: case 0317: /* CAM */
                if (!memory_id(o, off, &d->mem_kind, &d->mem_id)) return 0;
                d->reads = bit; d->mem_read = 1; d->flags = F_FIXED;
                normalize_ac_memory(d);
                return 1;
        case 0600: case 0601: case 0602: case 0603:
        case 0604: case 0605: case 0606: case 0607:
        case 0610: case 0611: case 0612: case 0613:
        case 0614: case 0615: case 0616: case 0617: /* TR/TL test family */
                d->reads = bit; d->flags = F_FIXED; return 1;
        default:
                return 0;
        }
}

/*
 * Prove that AC is a region-local temporary.  The incoming value must be
 * killed before any read inside the region, and the value produced by the
 * region must be killed before any later read.  Stop at unknown/control
 * instructions; uncertainty means the AC is not renameable.
 */
static int
ac_local_to_region(const struct dobj_object *o, unsigned long off,
    unsigned long n, unsigned int ac)
{
        unsigned long i;
        unsigned long bit = 1UL << ac;
        int killed_in = 0;

        if (ac == 0 || ac == 15)
                return 0;
        for (i = 0; i < n; i++) {
                struct insn d;
                if (!decode_safe(o, off + i, &d))
                        return 0;
                if (!killed_in && (d.reads & bit))
                        return 0;
                if (d.writes & bit)
                        killed_in = 1;
        }
        if (!killed_in)
                return 0;
        for (i = off + n; i < o->text_words; i++) {
                struct insn d;
                if (is_control_word(o->text[i]))
                        return 0;
                if (!decode_safe(o, i, &d))
                        return 0;
                if (d.reads & bit)
                        return 0;
                if (d.writes & bit)
                        return 1;
        }
        return 0;
}

static int
independent(const struct insn *a, const struct insn *b)
{
        int same_mem;
        if ((a->flags & F_FIXED) || (b->flags & F_FIXED))
                return 0;
        if ((a->writes & (b->reads | b->writes)) != 0 ||
            (b->writes & a->reads) != 0)
                return 0;
        if ((!a->mem_read && !a->mem_write) ||
            (!b->mem_read && !b->mem_write))
                return 1;
        if (a->mem_kind == MEM_UNKNOWN || b->mem_kind == MEM_UNKNOWN)
                return 0;
        same_mem = a->mem_kind == b->mem_kind && a->mem_id == b->mem_id;
        if (!same_mem)
                return 1;
        return !a->mem_write && !b->mem_write;
}

static void
build_pred(const struct insn *v, unsigned long n, unsigned int pred[MAX_REGION])
{
        unsigned long i, j;
        for (i = 0; i < n; i++) pred[i] = 0;
        for (i = 0; i < n; i++)
                for (j = i + 1; j < n; j++)
                        if (!independent(&v[i], &v[j]))
                                pred[j] |= 1U << i;
}

/*
 * Search whether two regions have a common legal instruction order.
 * This is deliberately exponential and host-only; MAX_REGION bounds it.
 * AC-normalized instruction words are compared at each choice.
 */
static int
common_schedule_rec(const struct region *a, const struct region *b,
    const unsigned int ap[MAX_REGION], const unsigned int bp[MAX_REGION],
    unsigned int adone, unsigned int bdone, unsigned long depth,
    int (*equal)(const struct insn *, const struct insn *),
    unsigned int aorder[MAX_REGION], unsigned int border[MAX_REGION])
{
        unsigned long i, j;
        if (depth == a->n) return 1;
        for (i = 0; i < a->n; i++) {
                if ((adone & (1U << i)) || (ap[i] & ~adone)) continue;
                for (j = 0; j < b->n; j++) {
                        if ((bdone & (1U << j)) || (bp[j] & ~bdone)) continue;
                        if (!equal(&a->insn[i], &b->insn[j])) continue;
                        if (aorder != NULL)
                                aorder[depth] = (unsigned int)i;
                        if (border != NULL)
                                border[depth] = (unsigned int)j;
                        if (common_schedule_rec(a, b, ap, bp,
                            adone | (1U << i), bdone | (1U << j), depth + 1,
                            equal, aorder, border))
                                return 1;
                }
        }
        return 0;
}

static int
find_common_schedule(const struct region *a, const struct region *b,
    int (*equal)(const struct insn *, const struct insn *),
    unsigned int aorder[MAX_REGION], unsigned int border[MAX_REGION])
{
        unsigned int ap[MAX_REGION], bp[MAX_REGION];

        if (a->n != b->n)
                return 0;
        build_pred(a->insn, a->n, ap);
        build_pred(b->insn, b->n, bp);
        return common_schedule_rec(a, b, ap, bp, 0, 0, 0, equal,
            aorder, border);
}

static int
mapped_insn_equal(const struct insn *a, const struct insn *b,
    int amap[16], int bmap[16])
{
        unsigned int aa = (unsigned int)((a->word.lh >> 5) & 017UL);
        unsigned int ba = (unsigned int)((b->word.lh >> 5) & 017UL);
        struct insn ac = *a, bc = *b;

        ac.word.lh &= ~0740UL;
        bc.word.lh &= ~0740UL;
        if (!insn_equal(&ac, &bc))
                return 0;
        if (aa == 0 || aa == 15 || ba == 0 || ba == 15)
                return aa == ba;
        if (amap[aa] >= 0 && amap[aa] != (int)ba) return 0;
        if (bmap[ba] >= 0 && bmap[ba] != (int)aa) return 0;
        amap[aa] = (int)ba; bmap[ba] = (int)aa;
        return 1;
}

static int
common_rename_rec(const struct region *a, const struct region *b,
    const unsigned int ap[MAX_REGION], const unsigned int bp[MAX_REGION],
    unsigned int adone, unsigned int bdone, unsigned long depth,
    int amap[16], int bmap[16], unsigned int aorder[MAX_REGION],
    unsigned int border[MAX_REGION], int out_amap[16], int out_bmap[16])
{
        unsigned long i, j;
        if (depth == a->n) {
                memcpy(out_amap, amap, 16U * sizeof(out_amap[0]));
                memcpy(out_bmap, bmap, 16U * sizeof(out_bmap[0]));
                return 1;
        }
        for (i = 0; i < a->n; i++) {
                if ((adone & (1U << i)) || (ap[i] & ~adone)) continue;
                for (j = 0; j < b->n; j++) {
                        int am[16], bm[16];
                        unsigned int aa, ba;
                        if ((bdone & (1U << j)) || (bp[j] & ~bdone)) continue;
                        memcpy(am, amap, sizeof(am)); memcpy(bm, bmap, sizeof(bm));
                        aa = a->insn[i].ac;
                        ba = b->insn[j].ac;
                        if (aa != ba &&
                            (((a->renameable >> aa) & 1UL) == 0 ||
                            ((b->renameable >> ba) & 1UL) == 0))
                                continue;
                        if (!mapped_insn_equal(&a->insn[i], &b->insn[j],
                            am, bm)) continue;
                        aorder[depth] = (unsigned int)i;
                        border[depth] = (unsigned int)j;
                        if (common_rename_rec(a, b, ap, bp,
                            adone | (1U << i), bdone | (1U << j), depth + 1,
                            am, bm, aorder, border, out_amap, out_bmap))
                                return 1;
                }
        }
        return 0;
}

static int
find_common_renamed_schedule(const struct region *a, const struct region *b,
    unsigned int aorder[MAX_REGION], unsigned int border[MAX_REGION],
    int out_amap[16], int out_bmap[16])
{
        unsigned int ap[MAX_REGION], bp[MAX_REGION];
        int amap[16], bmap[16];
        unsigned int i;
        if (a->n != b->n) return 0;
        build_pred(a->insn, a->n, ap); build_pred(b->insn, b->n, bp);
        for (i = 0; i < 16; i++) amap[i] = bmap[i] = -1;
        amap[0] = bmap[0] = 0; amap[15] = bmap[15] = 15;
        return common_rename_rec(a, b, ap, bp, 0, 0, 0, amap, bmap,
            aorder, border, out_amap, out_bmap);
}

static int
find_common_zero_schedule(const struct region *a, const struct region *b,
    unsigned int aorder[MAX_REGION], unsigned int border[MAX_REGION])
{
        return find_common_schedule(a, b, zero_form_equal, aorder, border);
}

static void
print_order(const unsigned int order[MAX_REGION], unsigned long n)
{
        unsigned long i;

        for (i = 0UL; i < n; i++)
                printf("%s+%o", i == 0UL ? "" : " ", order[i]);
}

static unsigned long
schedule_exact_span(const struct input *in, const struct region *a,
    const struct region *b, unsigned long *leftp, unsigned long *rightp)
{
        const struct dobj_object *ao = &in[a->input].obj;
        const struct dobj_object *bo = &in[b->input].obj;
        unsigned long left = 0UL;
        unsigned long right = 0UL;

        /*
         * The reconstructed common schedule proves the transformed region
         * itself equal.  Extend only through unchanged neighboring words
         * which are already relocation-equivalent.  This is deliberately a
         * lower bound: p10fold may later find a still larger relative-local
         * relocation match after actual source reassembly.
         */
        while (a->off > left && b->off > left &&
            word_at_equal(ao, a->off - left - 1UL,
                bo, b->off - left - 1UL))
                left++;
        while (a->off + a->n + right < ao->text_words &&
            b->off + b->n + right < bo->text_words &&
            word_at_equal(ao, a->off + a->n + right,
                bo, b->off + b->n + right))
                right++;
        if (leftp != NULL)
                *leftp = left;
        if (rightp != NULL)
                *rightp = right;
        return left + a->n + right;
}

static int
unconditional_end_at(const struct dobj_object *o, unsigned long off)
{
        unsigned int op;

        if (off >= o->text_words)
                return 0;
        op = (unsigned int)((o->text[off].lh >> 9) & 0777UL);
        return op == 0254U || op == 0263U; /* JRST / POPJ */
}

static int
isolated_text_block(const struct dobj_object *o, unsigned long off,
    unsigned long words)
{
        unsigned long i;

        if (words == 0UL || off + words > o->text_words)
                return 0;
        if (off != 0UL && !unconditional_end_at(o, off - 1UL))
                return 0;
        if (!unconditional_end_at(o, off + words - 1UL))
                return 0;
        for (i = 0UL; i < o->symbol_count; i++) {
                const struct dobj_symbol *s = &o->symbols[i];

                if (s->kind == DOBJ_SYM_DEF && s->sec == DOBJ_SEC_TEXT &&
                    s->value.rh > off && s->value.rh < off + words)
                        return 0;
        }
        for (i = 0UL; i < o->reloc_count; i++) {
                const struct dobj_reloc *r = &o->relocs[i];

                if ((r->type == DOBJ_RELOC_LOCAL_RH18 ||
                    r->type == DOBJ_RELOC_LOCAL_LH18) &&
                    r->target_sec == DOBJ_SEC_TEXT &&
                    r->addend.rh > off && r->addend.rh < off + words &&
                    (r->loc_sec != DOBJ_SEC_TEXT || r->offset < off ||
                    r->offset >= off + words))
                        return 0;
        }
        return 1;
}

static int
removable_text_block(const struct dobj_object *o, unsigned long off,
    unsigned long words)
{
        return !has_named_text_entry(o, off) &&
            isolated_text_block(o, off, words);
}

static int
ranges_overlap(unsigned long aoff, unsigned long an,
    unsigned long boff, unsigned long bn)
{
        return aoff < boff + bn && boff < aoff + an;
}

static unsigned long
schedule_zero_fold_saving(const struct input *in, const struct region *a,
    const struct region *b, unsigned long left, unsigned long right)
{
        const struct dobj_object *ao = &in[a->input].obj;
        const struct dobj_object *bo = &in[b->input].obj;
        unsigned long abase = a->off - left;
        unsigned long bbase = b->off - left;
        unsigned long span = left + a->n + right;
        unsigned long best = 0UL;
        unsigned long suboff;

        for (suboff = 0UL; suboff + 4UL <= span; suboff++) {
                unsigned long words;

                for (words = 4UL; suboff + words <= span; words++) {
                        int ar;
                        int br;

                        if (a->input == b->input &&
                            ranges_overlap(abase + suboff, words,
                                bbase + suboff, words))
                                continue;
                        ar = removable_text_block(ao, abase + suboff, words);
                        br = removable_text_block(bo, bbase + suboff, words);
                        if ((ar || br) && words > best)
                                best = words;
                }
        }
        return best;
}

static void
print_schedule_rewrite(const struct input *in, const struct region *a,
    const struct region *b, const unsigned int aorder[MAX_REGION],
    const unsigned int border[MAX_REGION])
{
        unsigned int identity[MAX_REGION];
        unsigned long left, right;
        unsigned long span = schedule_exact_span(in, a, b, &left, &right);
        unsigned long fold_save = schedule_zero_fold_saving(in, a, b,
            left, right);
        unsigned long i;

        for (i = 0UL; i < a->n; i++)
                identity[i] = (unsigned int)i;
        printf("REORDER ");
        print_source_location(stdout, &in[a->input], a->off);
        printf(" <=> ");
        print_source_location(stdout, &in[b->input], b->off);
        printf(" : SAVE 0; EXPECTED-FOLD-SAVE %lu; "
            "EXACT-RUN-POTENTIAL %lu (-%lu/+%lu); COST=ZERO; "
            "LEGAL=DEPENDENCY-DAG-COMMON-SCHEDULE-SINGLE-ENTRY\n",
            fold_save, span, left, right);
        printf("  A OLD ");
        print_order(identity, a->n);
        printf(" ; NEW ");
        print_order(aorder, a->n);
        putchar('\n');
        printf("  B OLD ");
        print_order(identity, b->n);
        printf(" ; NEW ");
        print_order(border, b->n);
        putchar('\n');
}

static void
print_zero_rewrite(const struct input *in, const struct region *a,
    const struct region *b, const unsigned int aorder[MAX_REGION],
    const unsigned int border[MAX_REGION])
{
        unsigned int identity[MAX_REGION];
        unsigned long i;

        for (i = 0UL; i < a->n; i++)
                identity[i] = (unsigned int)i;
        printf("CANON ");
        print_source_location(stdout, &in[a->input], a->off);
        printf(" <=> ");
        print_source_location(stdout, &in[b->input], b->off);
        printf(" : SAVE 0; FOLD-POTENTIAL %lu; COST=ZERO; "
            "LEGAL=DEPENDENCY-DAG-PLUS-UNLABELLED-ZERO-FORM\n", a->n);
        printf("  A OLD ");
        print_order(identity, a->n);
        printf(" ; NEW ");
        print_order(aorder, a->n);
        putchar('\n');
        printf("  B OLD ");
        print_order(identity, b->n);
        printf(" ; NEW ");
        print_order(border, b->n);
        putchar('\n');
        printf("  FORM MOVEI-AC-0 -> SETZ-AC\n");
}

static void
print_ac_map(const int amap[16])
{
        unsigned int ac;
        int any = 0;

        for (ac = 1U; ac < 15U; ac++) {
                if (amap[ac] < 0 || amap[ac] == (int)ac)
                        continue;
                printf("%s%o->%o", any ? " " : "", ac,
                    (unsigned int)amap[ac]);
                any = 1;
        }
        if (!any)
                printf("-");
}

static void
print_rename_rewrite(const struct input *in, const struct region *a,
    const struct region *b, const unsigned int aorder[MAX_REGION],
    const unsigned int border[MAX_REGION], const int amap[16])
{
        unsigned int identity[MAX_REGION];
        unsigned long i;

        for (i = 0UL; i < a->n; i++)
                identity[i] = (unsigned int)i;
        printf("RENAME-REWRITE ");
        print_source_location(stdout, &in[a->input], a->off);
        printf(" <=> ");
        print_source_location(stdout, &in[b->input], b->off);
        printf(" : SAVE 0; FOLD-POTENTIAL %lu; COST=ZERO; "
            "LEGAL=DEPENDENCY-DAG-PLUS-REGION-LOCAL-AC\n", a->n);
        printf("  A OLD ");
        print_order(identity, a->n);
        printf(" ; NEW ");
        print_order(aorder, a->n);
        printf(" ; RENAME ");
        print_ac_map(amap);
        putchar('\n');
        printf("  B OLD ");
        print_order(identity, b->n);
        printf(" ; NEW ");
        print_order(border, b->n);
        printf(" ; RENAME -\n");
}

static int
regions_overlap(const struct region *a, const struct region *b)
{
        if (a->input != b->input)
                return 0;
        return a->off < b->off + b->n && b->off < a->off + a->n;
}

static int
region_needs_zero_form(const struct region *r)
{
        unsigned long i;

        for (i = 0UL; i < r->n; i++) {
                const struct insn *d = &r->insn[i];
                unsigned int op = (unsigned int)((d->word.lh >> 9) & 0777UL);

                if (op == 0201U && d->word.rh == 0UL &&
                    (d->word.lh & 037UL) == 0UL &&
                    half_reloc(d->obj, d->off, 0) == NULL &&
                    !has_text_entry(d->obj, d->off))
                        return 1;
        }
        return 0;
}

static void
action_init(struct normalization_action *a, const struct region *r,
    const unsigned int order[MAX_REGION], int zero_form,
    const int rename[16])
{
        unsigned int i;

        memset(a, 0, sizeof(*a));
        a->input = r->input;
        a->off = r->off;
        a->n = r->n;
        for (i = 0U; i < (unsigned int)r->n; i++)
                a->order[i] = order[i];
        a->zero_form = zero_form;
        for (i = 0U; i < 16U; i++)
                a->rename[i] = rename == NULL ? -1 : rename[i];
}

static int
action_equal(const struct normalization_action *a,
    const struct normalization_action *b)
{
        unsigned int i;

        if (a->input != b->input || a->off != b->off || a->n != b->n ||
            a->zero_form != b->zero_form)
                return 0;
        for (i = 0U; i < (unsigned int)a->n; i++)
                if (a->order[i] != b->order[i])
                        return 0;
        for (i = 0U; i < 16U; i++)
                if (a->rename[i] != b->rename[i])
                        return 0;
        return 1;
}

static int
action_overlap(const struct normalization_action *a,
    const struct normalization_action *b)
{
        if (a->input != b->input)
                return 0;
        return a->off < b->off + b->n && b->off < a->off + a->n;
}

static int
candidate_conflict(const struct candidate *a, const struct candidate *b)
{
        unsigned int ai, bi;

        for (ai = 0U; ai < 2U; ai++)
                for (bi = 0U; bi < 2U; bi++) {
                        const struct normalization_action *aa = &a->action[ai];
                        const struct normalization_action *ba = &b->action[bi];

                        if (!action_overlap(aa, ba))
                                continue;
                        if (aa->off == ba->off && aa->n == ba->n &&
                            action_equal(aa, ba))
                                continue;
                        return 1;
                }
        return 0;
}

static int
score_better(const struct plan_score *a, const struct plan_score *b)
{
        if (a->fold_save != b->fold_save)
                return a->fold_save > b->fold_save;
        /*
         * Exact-run span is an exploration hint, not a size result.  Do not
         * recommend source churn when no selected candidate has a proven
         * zero-cost fold saving.  Once a plan is size-positive, use span and
         * candidate count only to break equal-saving ties.
         */
        if (a->fold_save == 0UL)
                return 0;
        if (a->span != b->span)
                return a->span > b->span;
        return a->count > b->count;
}

static int
candidate_fits(const struct plan_search *s, unsigned long index)
{
        unsigned long i;

        for (i = 0UL; i < index; i++)
                if (s->current[i] &&
                    candidate_conflict(&s->candidate[i],
                        &s->candidate[index]))
                        return 0;
        return 1;
}

static void
plan_search_rec(struct plan_search *s, unsigned long index,
    struct plan_score score)
{
        if (index == s->count) {
                if (score_better(&score, &s->best_score)) {
                        s->best_score = score;
                        memcpy(s->best, s->current,
                            (size_t)s->count * sizeof(s->best[0]));
                }
                return;
        }

        if (score.fold_save + s->suffix_fold[index] <
            s->best_score.fold_save)
                return;
        if (score.fold_save + s->suffix_fold[index] ==
            s->best_score.fold_save &&
            score.span + s->suffix_span[index] < s->best_score.span)
                return;

        if (candidate_fits(s, index)) {
                struct plan_score next = score;
                s->current[index] = 1U;
                next.fold_save += s->candidate[index].fold_save;
                next.span += s->candidate[index].span;
                next.count++;
                plan_search_rec(s, index + 1UL, next);
                s->current[index] = 0U;
        }
        plan_search_rec(s, index + 1UL, score);
}

static int
select_global_candidates(const struct candidate *candidate, unsigned long n,
    unsigned char **selectedp, struct plan_score *scorep)
{
        struct plan_search s;
        struct plan_score zero;
        unsigned long i;

        memset(&s, 0, sizeof(s));
        memset(&zero, 0, sizeof(zero));
        s.candidate = candidate;
        s.count = n;
        if (n == 0UL) {
                *selectedp = NULL;
                *scorep = zero;
                return 0;
        }
        s.current = (unsigned char *)calloc((size_t)n, 1U);
        s.best = (unsigned char *)calloc((size_t)n, 1U);
        s.suffix_fold = (unsigned long *)calloc((size_t)(n + 1UL),
            sizeof(*s.suffix_fold));
        s.suffix_span = (unsigned long *)calloc((size_t)(n + 1UL),
            sizeof(*s.suffix_span));
        if (s.current == NULL || s.best == NULL || s.suffix_fold == NULL ||
            s.suffix_span == NULL) {
                free(s.current); free(s.best);
                free(s.suffix_fold); free(s.suffix_span);
                return -1;
        }
        for (i = n; i-- > 0UL; ) {
                s.suffix_fold[i] = s.suffix_fold[i + 1UL] +
                    candidate[i].fold_save;
                s.suffix_span[i] = s.suffix_span[i + 1UL] +
                    candidate[i].span;
        }
        plan_search_rec(&s, 0UL, zero);
        *selectedp = s.best;
        *scorep = s.best_score;
        free(s.current);
        free(s.suffix_fold);
        free(s.suffix_span);
        return 0;
}

static int
candidate_add(struct candidate **cp, unsigned long *np, unsigned long *capp,
    enum candidate_kind kind, unsigned long ar, unsigned long br,
    const struct region *a, const struct region *b,
    const unsigned int aorder[MAX_REGION],
    const unsigned int border[MAX_REGION], const int amap[16],
    unsigned long fold_save, unsigned long span)
{
        struct candidate *c;
        unsigned long nc;
        int azero = 0;
        int bzero = 0;

        if (*np == *capp) {
                nc = *capp ? *capp * 2UL : 32UL;
                c = (struct candidate *)realloc(*cp,
                    (size_t)nc * sizeof(**cp));
                if (c == NULL)
                        return -1;
                *cp = c;
                *capp = nc;
        }
        c = &(*cp)[*np];
        memset(c, 0, sizeof(*c));
        c->kind = kind;
        c->a_region = ar;
        c->b_region = br;
        c->fold_save = fold_save;
        c->span = span;
        if (kind == CAND_ZERO) {
                azero = region_needs_zero_form(a);
                bzero = region_needs_zero_form(b);
        }
        action_init(&c->action[0], a, aorder, azero,
            kind == CAND_RENAME ? amap : NULL);
        action_init(&c->action[1], b, border, bzero, NULL);
        (*np)++;
        return 0;
}

static const char *
candidate_name(enum candidate_kind kind)
{
        return kind == CAND_SCHEDULE ? "REORDER" :
            (kind == CAND_ZERO ? "CANON" : "RENAME-REWRITE");
}

int
main(int argc, char **argv)
{
        struct input *in;
        struct region *r = NULL;
        unsigned long nr = 0, cap = 0, matches = 0, words = 0;
        unsigned long sched_matches = 0, sched_words = 0;
        unsigned long zero_matches = 0, zero_words = 0;
        unsigned long rename_matches = 0, rename_words = 0;
        unsigned long peephole_words = 0;
        struct candidate *candidate = NULL;
        unsigned long candidate_count = 0UL, candidate_cap = 0UL;
        unsigned char *selected = NULL;
        struct plan_score plan_score;
        int ni, ai;

        if (argc < 2) {
                usage();
                return 2;
        }
        ni = argc - 1;
        in = calloc((size_t)ni, sizeof(*in));
        if (in == NULL)
                return 1;
        for (ai = 0; ai < ni; ai++) {
                FILE *f;
                in[ai].name = argv[ai + 1];
                f = fopen(in[ai].name, "rb");
                if (f == NULL || dobj_read(f, &in[ai].obj) != 0) {
                        fprintf(stderr, "p10super: cannot read %s\n", in[ai].name);
                        if (f != NULL) fclose(f);
                        return 1;
                }
                fclose(f);
        }
        for (ai = 0; ai < ni; ai++) {
                unsigned long off;
                for (off = 0; off < in[ai].obj.text_words; off++) {
                        unsigned int replacement;
                        if (!cai0_jrst_replacement(&in[ai].obj, off,
                            &replacement))
                                replacement = 0;
                        else {
                                fprintf(stderr,
                                    "PEEP-CAI0-JRST %s+%06lo : SAVE 1; "
                                    "JUMP OP %03o\n",
                                    in[ai].name, off, replacement);
                                print_peephole_rewrite(&in[ai], off,
                                    replacement,
                                    "CAI-ZERO-PLAIN-JRST-COMPLEMENT");
                                peephole_words++;
                        }
                        if (incdec_jump_replacement(&in[ai].obj, off,
                            &replacement)) {
                                fprintf(stderr,
                                    "PEEP-%sI1-JUMP %s+%06lo : SAVE 1; "
                                    "%s OP %03o\n",
                                    (((in[ai].obj.text[off].lh >> 9) &
                                    0777UL) == 0271UL) ? "ADD" : "SUB",
                                    in[ai].name, off,
                                    (((in[ai].obj.text[off].lh >> 9) &
                                    0777UL) == 0271UL) ? "AOJ" : "SOJ",
                                    replacement);
                                print_peephole_rewrite(&in[ai], off,
                                    replacement,
                                    "INCDEC-ONE-SAME-AC-CONDITIONAL-JUMP");
                                peephole_words++;
                        }
                        if (incdec_jrst_replacement(&in[ai].obj, off,
                            &replacement)) {
                                fprintf(stderr,
                                    "PEEP-%sI1-JRST %s+%06lo : SAVE 1; "
                                    "%sA OP %03o\n",
                                    (((in[ai].obj.text[off].lh >> 9) &
                                    0777UL) == 0271UL) ? "ADD" : "SUB",
                                    in[ai].name, off,
                                    (((in[ai].obj.text[off].lh >> 9) &
                                    0777UL) == 0271UL) ? "AOJ" : "SOJ",
                                    replacement);
                                print_peephole_rewrite(&in[ai], off,
                                    replacement,
                                    "INCDEC-ONE-PLAIN-JRST");
                                peephole_words++;
                        }
                }
        }
        for (ai = 0; ai < ni; ai++) {
                struct block *blocks = NULL;
                unsigned long nb = build_blocks(&in[ai].obj, &blocks);
                unsigned long bi, max = 0, words = 0, invertible = 0;
                for (bi = 0; bi < nb; bi++) {
                        unsigned long len = blocks[bi].end - blocks[bi].start;
                        words += len;
                        if (len > max) max = len;
                        if (is_invertible_jump(
                            in[ai].obj.text[blocks[bi].end - 1]))
                                invertible++;
                }
                fprintf(stderr,
                    "CFG %s: %lu BLOCKS; %lu WORDS; MAX %lu WORDS; "
                    "%lu INVERTIBLE JUMPS\n",
                    in[ai].name, nb, words, max, invertible);
                free(blocks);
        }
        for (ai = 0; ai < ni; ai++) {
                int bi;
                struct block *ab = NULL;
                unsigned long na = build_blocks(&in[ai].obj, &ab);
                for (bi = ai; bi < ni; bi++) {
                        struct block *bb = NULL;
                        unsigned long nb = build_blocks(&in[bi].obj, &bb);
                        unsigned long x, y;
                        for (x = 0; x < na; x++)
                                for (y = (bi == ai ? x + 1 : 0);
                                    y < nb; y++) {
                                        struct guard_shape ag, bg;
                                        unsigned long gts = 0, gfs = 0;
                                        unsigned long gscore = 0;
                                        unsigned long its = 0, ifs = 0;
                                        unsigned long iscore =
                                            inverted_jump_suffix_score(
                                            &in[ai].obj, &ab[x], ab, na,
                                            &in[bi].obj, &bb[y], bb, nb,
                                            &its, &ifs);
                                        if (iscore >= 3 &&
                                            (its >= 2 || ifs >= 2))
                                                fprintf(stderr,
                                                    "CFG-INVERT-SUFFIX "
                                                    "%s+%06lo <=> %s+%06lo : "
                                                    "TAKEN %lu FALL %lu "
                                                    "SCORE %lu\n",
                                                    in[ai].name, ab[x].start,
                                                    in[bi].name, bb[y].start,
                                                    its, ifs, iscore);
                                        if (guard_shape_at(&in[ai].obj,
                                            &ab[x], ab, na, &ag) &&
                                            guard_shape_at(&in[bi].obj,
                                            &bb[y], bb, nb, &bg))
                                                gscore = guard_suffix_score(
                                                    &in[ai].obj, &ag,
                                                    &in[bi].obj, &bg,
                                                    &gts, &gfs);
                                        if (gscore >= 3 &&
                                            (gts >= 2 || gfs >= 2))
                                                fprintf(stderr,
                                                    "CFG-GUARD-SUFFIX "
                                                    "%s+%06lo <=> %s+%06lo : "
                                                    "TAKEN %lu FALL %lu "
                                                    "SCORE %lu\n",
                                                    in[ai].name, ab[x].start,
                                                    in[bi].name, bb[y].start,
                                                    gts, gfs, gscore);
                                        if (!same_block_occurrence(
                                            &in[ai].obj, &ab[x],
                                            &in[bi].obj, &bb[y]) &&
                                            block_shape_equal(
                                            &in[ai].obj, &ab[x],
                                            &in[bi].obj, &bb[y])) {
                                                unsigned long blen =
                                                    ab[x].end - ab[x].start;
                                                unsigned long exact =
                                                    common_block_suffix(
                                                    &in[ai].obj, &ab[x],
                                                    &in[bi].obj, &bb[y]);
                                                unsigned int sblocks = 0;
                                                int sclosed = 0;
                                                unsigned long sscore =
                                                    cfg_shape_score(
                                                    &in[ai].obj, &ab[x],
                                                    ab, na, &in[bi].obj,
                                                    &bb[y], bb, nb,
                                                    &sblocks, &sclosed);
                                                if (sblocks >= 2 &&
                                                    sscore >= 8 &&
                                                    sscore > blen)
                                                        fprintf(stderr,
                                                            "CFG-SUBGRAPH%s "
                                                            "%s+%06lo <=> "
                                                            "%s+%06lo : "
                                                            "%u BLOCKS; "
                                                            "%lu WORDS\n",
                                                            sclosed ?
                                                            "-CLOSED" : "",
                                                            in[ai].name,
                                                            ab[x].start,
                                                            in[bi].name,
                                                            bb[y].start,
                                                            sblocks, sscore);
                                                if (blen >= 3 &&
                                                    exact != blen)
                                                        fprintf(stderr,
                                                            "CFG-SHAPE-%s "
                                                            "%s+%06lo <=> "
                                                            "%s+%06lo : "
                                                            "%lu WORDS; "
                                                            "%u/%u SUCC\n",
                                                            (has_named_text_entry(
                                                            &in[ai].obj,
                                                            ab[x].start) ||
                                                            has_named_text_entry(
                                                            &in[bi].obj,
                                                            bb[y].start)) ?
                                                            "NAMED" : "INTERNAL",
                                                            in[ai].name,
                                                            ab[x].start,
                                                            in[bi].name,
                                                            bb[y].start,
                                                            blen,
                                                            ab[x].nsucc,
                                                            bb[y].nsucc);
                                        }
                                        if (inverted_jump_blocks(
                                            &in[ai].obj, &ab[x], ab, na,
                                            &in[bi].obj, &bb[y], bb, nb))
                                                fprintf(stderr,
                                                    "CFG-INVERT %s+%06lo <=> "
                                                    "%s+%06lo\n",
                                                    in[ai].name, ab[x].start,
                                                    in[bi].name, bb[y].start);
                                        unsigned long n = common_block_suffix(
                                            &in[ai].obj, &ab[x],
                                            &in[bi].obj, &bb[y]);
                                        unsigned long en =
                                            common_epilogue_suffix(
                                            &in[ai].obj, &ab[x],
                                            &in[bi].obj, &bb[y]);
                                        if (en >= 2)
                                                fprintf(stderr,
                                                    "EPILOGUE-SUFFIX%s %s+%06lo "
                                                    "<=> %s+%06lo : %lu WORDS\n",
                                                    (retargetable_tail(
                                                    &in[ai].obj,
                                                    ab[x].end - en, en) ||
                                                    retargetable_tail(
                                                    &in[bi].obj,
                                                    bb[y].end - en, en)) ?
                                                    "-RETARGETABLE" : "",
                                                    in[ai].name,
                                                    ab[x].end - en,
                                                    in[bi].name,
                                                    bb[y].end - en, en);
                                        if (n >= 3)
                                                fprintf(stderr,
                                                    "CFG-SUFFIX%s%s %s+%06lo <=> "
                                                    "%s+%06lo : %lu WORDS\n",
                                                    (retargetable_tail(
                                                    &in[ai].obj,
                                                    ab[x].end - n, n) ||
                                                    retargetable_tail(
                                                    &in[bi].obj,
                                                    bb[y].end - n, n)) ?
                                                    "-RETARGETABLE" : "",
                                                    retargetable_tail(
                                                    &in[ai].obj,
                                                    ab[x].end - n, n) ?
                                                    (retargetable_tail(
                                                    &in[bi].obj,
                                                    bb[y].end - n, n) ?
                                                    "-AB" : "-A") :
                                                    (retargetable_tail(
                                                    &in[bi].obj,
                                                    bb[y].end - n, n) ?
                                                    "-B" : ""),
                                                    in[ai].name,
                                                    ab[x].end - n,
                                                    in[bi].name,
                                                    bb[y].end - n, n);
                                }
                        free(bb);
                }
                free(ab);
        }
        for (ai = 0; ai < ni; ai++) {
                unsigned long start;
                for (start = 0; start < in[ai].obj.text_words; start++) {
                        struct insn v[MAX_REGION];
                        unsigned long n = 0, len;

                        while (start + n < in[ai].obj.text_words &&
                            n < MAX_REGION &&
                            decode_safe(&in[ai].obj, start + n, &v[n]))
                                n++;
                        /*
                         * Record every bounded prefix from this start point.
                         * This turns discovery into a sliding-window search:
                         * useful common subsequences are no longer hidden by
                         * unrelated instructions at either end of a larger
                         * analyzable region.
                         */
                        for (len = 2; len <= n; len++) {
                                struct region *q;
                                /*
                                 * Reordering across an alternate entry would
                                 * change what a branch or exported label sees.
                                 * Once a later word is an entry, no longer
                                 * prefix from this start is source-rewriteable.
                                 */
                                if (has_text_entry(&in[ai].obj,
                                    start + len - 1UL))
                                        break;
                                if (nr == cap) {
                                        unsigned long nc = cap ? cap * 2 : 256;
                                        q = realloc(r, (size_t)nc * sizeof(*r));
                                        if (q == NULL) return 1;
                                        r = q; cap = nc;
                                }
                                r[nr].input = ai;
                                r[nr].off = start;
                                r[nr].n = len;
                                r[nr].tail_candidate =
                                    start + len < in[ai].obj.text_words &&
                                    unconditional_end_word(
                                        in[ai].obj.text[start + len]);
                                r[nr].renameable = 0;
                                {
                                        unsigned int ac;
                                        for (ac = 1; ac < 15; ac++)
                                                if (ac_local_to_region(
                                                    &in[ai].obj, start, len,
                                                    ac))
                                                        r[nr].renameable |=
                                                            1UL << ac;
                                }
                                memcpy(r[nr].insn, v,
                                    (size_t)len * sizeof(v[0]));
                                nr++;
                        }
                }
        }
        for (unsigned long i = 0; i < nr; i++)
                for (unsigned long j = i + 1; j < nr; j++) {
                        int sched;
                        int zero;
                        int renamed;
                        unsigned int aorder[MAX_REGION];
                        unsigned int border[MAX_REGION];
                        int amap[16], bmap[16];
                        unsigned long left, right, span, fold_save;
                        enum candidate_kind kind;
                        if (regions_overlap(&r[i], &r[j]) ||
                            r[i].n != r[j].n)
                                continue;
                        sched = find_common_schedule(&r[i], &r[j],
                            insn_equal, aorder, border);
                        zero = !sched &&
                            find_common_zero_schedule(&r[i], &r[j],
                            aorder, border);
                        renamed = !sched && !zero &&
                            find_common_renamed_schedule(&r[i], &r[j],
                            aorder, border, amap, bmap);
                        if (!sched && !zero && !renamed) continue;
                        /* Ignore regions already byte-identical in source order. */
                        int identical = 1;
                        for (unsigned long k = 0; k < r[i].n; k++)
                                if (in[r[i].input].obj.text[r[i].off+k].lh !=
                                    in[r[j].input].obj.text[r[j].off+k].lh ||
                                    in[r[i].input].obj.text[r[i].off+k].rh !=
                                    in[r[j].input].obj.text[r[j].off+k].rh)
                                        identical = 0;
                        if (identical) continue;
                        span = schedule_exact_span(in, &r[i], &r[j],
                            &left, &right);
                        fold_save = schedule_zero_fold_saving(in, &r[i],
                            &r[j], left, right);
                        kind = sched ? CAND_SCHEDULE :
                            (zero ? CAND_ZERO : CAND_RENAME);
                        if (candidate_add(&candidate, &candidate_count,
                            &candidate_cap, kind, i, j, &r[i], &r[j],
                            aorder, border, amap, fold_save, span) != 0) {
                                fprintf(stderr, "p10super: out of memory\n");
                                return 1;
                        }
                        if (sched)
                                print_schedule_rewrite(in, &r[i], &r[j],
                                    aorder, border);
                        else if (zero)
                                print_zero_rewrite(in, &r[i], &r[j],
                                    aorder, border);
                        else
                                print_rename_rewrite(in, &r[i], &r[j],
                                    aorder, border, amap);
                        printf("%s%s %s+%06lo <=> %s+%06lo : %lu WORDS\n",
                            (r[i].tail_candidate && r[j].tail_candidate) ?
                                "TAIL-" : "",
                            sched ? "SCHEDULE" :
                            (zero ? "CANON-ZERO" : "RENAME"),
                            in[r[i].input].name, r[i].off,
                            in[r[j].input].name, r[j].off, r[i].n);
                        matches++; words += r[i].n;
                        if (sched) {
                                sched_matches++; sched_words += r[i].n;
                        } else if (zero) {
                                zero_matches++; zero_words += r[i].n;
                        } else {
                                rename_matches++; rename_words += r[i].n;
                        }
                }
        memset(&plan_score, 0, sizeof(plan_score));
        if (select_global_candidates(candidate, candidate_count, &selected,
            &plan_score) != 0) {
                fprintf(stderr, "p10super: out of memory\n");
                return 1;
        }
        printf("GLOBAL-PLAN: %lu/%lu CANDIDATES; EXPECTED-FOLD-SAVE-SCORE %lu; "
            "EXACT-RUN-SCORE %lu\n", plan_score.count, candidate_count,
            plan_score.fold_save, plan_score.span);
        for (unsigned long i = 0UL; i < candidate_count; i++) {
                const struct candidate *c;
                if (selected == NULL || !selected[i])
                        continue;
                c = &candidate[i];
                printf("  SELECT %lu %s ", i + 1UL,
                    candidate_name(c->kind));
                print_source_location(stdout,
                    &in[r[c->a_region].input], r[c->a_region].off);
                printf(" <=> ");
                print_source_location(stdout,
                    &in[r[c->b_region].input], r[c->b_region].off);
                printf(" : EXPECTED-FOLD-SAVE %lu; EXACT-RUN %lu\n",
                    c->fold_save, c->span);
        }
        printf("SUMMARY: %lu REORDERING MATCHES; %lu MATCHED WORDS (NON-ADDITIVE)\n",
            matches, words);
        printf("  SCHEDULE-ONLY: %lu MATCHES; %lu WORDS\n",
            sched_matches, sched_words);
        printf("  ZERO-FORM-CANON: %lu MATCHES; %lu WORDS\n",
            zero_matches, zero_words);
        printf("  AC-RENAME-DEPENDENT: %lu MATCHES; %lu WORDS\n",
            rename_matches, rename_words);
        printf("  PEEPHOLE-SHORTENING: %lu WORDS\n", peephole_words);
        for (ai = 0; ai < ni; ai++) dobj_free(&in[ai].obj);
        free(selected); free(candidate); free(r); free(in);
        return 0;
}
