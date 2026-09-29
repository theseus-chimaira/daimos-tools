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

static int
unconditional_end_word(struct dobj_word w)
{
        unsigned long op = (w.lh >> 9) & 0777UL;
        return op == 0254UL || op == 0263UL; /* JRST, POPJ */
}

static int
is_skip_opcode(unsigned int op)
{
        if (op >= 0300 && op <= 0317)
                return (op & 07U) != 0;
        if (op >= 0600 && op <= 0677)
                return (op & 07U) != 0;
        if ((op >= 0330 && op <= 0337) || /* SKIP */
            (op >= 0350 && op <= 0357) || /* AOS */
            (op >= 0370 && op <= 0377))   /* SOS */
                return (op & 07U) != 0;
        return 0;
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
        return is_skip_opcode(op);
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
                if (is_skip_opcode((unsigned int)
                    ((o->text[i].lh >> 9) & 0777UL)) &&
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
                } else if (is_skip_opcode(op)) {
                        if (b[i].end < o->text_words)
                                b[i].succ[b[i].nsucc++] = b[i].end;
                        if (b[i].end + 1 < o->text_words && b[i].nsucc < 2)
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
cai0_jrst_replacement(const struct dobj_object *o, unsigned long off,
    unsigned int *replacement)
{
        unsigned int op, nextop;
        if (off + 1 >= o->text_words || rh_reloc(o, off) != NULL)
                return 0;
        op = (unsigned int)((o->text[off].lh >> 9) & 0777UL);
        nextop = (unsigned int)((o->text[off + 1].lh >> 9) & 0777UL);
        if (op < 0301 || op > 0307 || o->text[off].rh != 0 ||
            nextop != 0254)
                return 0;
        /*
         * CAIx AC,0 skips the JRST when condition x is true.  A single
         * JUMP with the complementary condition therefore has identical
         * control flow.  PDP-10 condition complements differ by bit 04.
         */
        *replacement = 0320U + (((op - 0300U) ^ 04U) & 07U);
        return 1;
}

static int
incdec_jump_replacement(const struct dobj_object *o, unsigned long off,
    unsigned int *replacement)
{
        unsigned long lh, nlh;
        unsigned int op, nop, ac, nac;

        if (off + 1 >= o->text_words || rh_reloc(o, off) != NULL)
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

        if (off + 1 >= o->text_words || rh_reloc(o, off) != NULL)
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
                if (a->symbol >= ao->symbol_count || b->symbol >= bo->symbol_count)
                        return 0;
                return strcmp(ao->symbols[a->symbol].name,
                    bo->symbols[b->symbol].name) == 0;
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
                *kind = DOBJ_SEC_ABS;
                *id = o->text[off].rh;
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
        case 0401: case 0475:                         /* SETZI/SETOI */
        case 0501: case 0541: case 0551: case 0561: /* halfword I */
                d->writes = bit;
                return 1;
        case 0271: case 0275: /* ADDI/SUBI */
                d->reads = bit;
                d->writes = bit;
                return 1;
        case 0431: /* XORI */
        case 0435: /* IORI */
        case 0441: /* ANDI */
        case 0471: /* EQVI */
                d->reads = bit;
                d->writes = bit;
                return 1;
        case 0242: /* LSH */
                d->reads = bit;
                d->writes = bit;
                return 1;
        case 0200: case 0204: case 0210: case 0214: /* MOVE/MOVS/N/M */
        case 0270: case 0274:                         /* ADD/SUB */
        case 0500: case 0540: case 0550: case 0560: /* halfword reads */
        case 0510: case 0520: case 0530: case 0570: /* halfword Z reads */
                if (!memory_id(o, off, &d->mem_kind, &d->mem_id))
                        return 0;
                d->mem_read = 1;
                if (op == 0270 || op == 0274) d->reads = bit;
                d->writes = bit;
                return 1;
        case 0202: case 0206: case 0212: case 0216: /* MOVE*M */
        case 0502: case 0542: case 0552: case 0562: /* halfword stores */
                if (!memory_id(o, off, &d->mem_kind, &d->mem_id))
                        return 0;
                d->mem_write = 1;
                d->reads = bit;
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
                d->reads = bit; d->mem_read = 1; d->flags = F_FIXED; return 1;
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
    unsigned int adone, unsigned int bdone, unsigned long depth)
{
        unsigned long i, j;
        if (depth == a->n) return 1;
        for (i = 0; i < a->n; i++) {
                struct dobj_word aw;
                if ((adone & (1U << i)) || (ap[i] & ~adone)) continue;
                aw = a->insn[i].word;
                for (j = 0; j < b->n; j++) {
                        struct dobj_word bw;
                        if ((bdone & (1U << j)) || (bp[j] & ~bdone)) continue;
                        bw = b->insn[j].word;
                        /*
                         * Compare opcode/address exactly here.  AC-renaming
                         * search is a separate transformation and must not be
                         * conflated with scheduling legality.
                         */
                        (void)aw;
                        (void)bw;
                        if (!insn_equal(&a->insn[i], &b->insn[j])) continue;
                        if (common_schedule_rec(a, b, ap, bp,
                            adone | (1U << i), bdone | (1U << j), depth + 1))
                                return 1;
                }
        }
        return 0;
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
    int amap[16], int bmap[16])
{
        unsigned long i, j;
        if (depth == a->n) return 1;
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
                        if (common_rename_rec(a, b, ap, bp,
                            adone | (1U << i), bdone | (1U << j), depth + 1,
                            am, bm))
                                return 1;
                }
        }
        return 0;
}

static int
has_common_renamed_schedule(const struct region *a, const struct region *b)
{
        unsigned int ap[MAX_REGION], bp[MAX_REGION];
        int amap[16], bmap[16];
        unsigned int i;
        if (a->n != b->n) return 0;
        build_pred(a->insn, a->n, ap); build_pred(b->insn, b->n, bp);
        for (i = 0; i < 16; i++) amap[i] = bmap[i] = -1;
        amap[0] = bmap[0] = 0; amap[15] = bmap[15] = 15;
        return common_rename_rec(a, b, ap, bp, 0, 0, 0, amap, bmap);
}

static int
has_common_schedule(const struct region *a, const struct region *b)
{
        unsigned int ap[MAX_REGION], bp[MAX_REGION];
        if (a->n != b->n) return 0;
        build_pred(a->insn, a->n, ap);
        build_pred(b->insn, b->n, bp);
        return common_schedule_rec(a, b, ap, bp, 0, 0, 0);
}

static int
regions_overlap(const struct region *a, const struct region *b)
{
        if (a->input != b->input)
                return 0;
        return a->off < b->off + b->n && b->off < a->off + a->n;
}

int
main(int argc, char **argv)
{
        struct input *in;
        struct region *r = NULL;
        unsigned long nr = 0, cap = 0, matches = 0, words = 0;
        unsigned long sched_matches = 0, sched_words = 0;
        unsigned long rename_matches = 0, rename_words = 0;
        unsigned long peephole_words = 0;
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
                        int renamed;
                        if (regions_overlap(&r[i], &r[j]) ||
                            r[i].n != r[j].n)
                                continue;
                        sched = has_common_schedule(&r[i], &r[j]);
                        renamed = !sched &&
                            has_common_renamed_schedule(&r[i], &r[j]);
                        if (!sched && !renamed) continue;
                        /* Ignore regions already byte-identical in source order. */
                        int identical = 1;
                        for (unsigned long k = 0; k < r[i].n; k++)
                                if (in[r[i].input].obj.text[r[i].off+k].lh !=
                                    in[r[j].input].obj.text[r[j].off+k].lh ||
                                    in[r[i].input].obj.text[r[i].off+k].rh !=
                                    in[r[j].input].obj.text[r[j].off+k].rh)
                                        identical = 0;
                        if (identical) continue;
                        printf("%s%s %s+%06lo <=> %s+%06lo : %lu WORDS\n",
                            (r[i].tail_candidate && r[j].tail_candidate) ?
                                "TAIL-" : "",
                            sched ? "SCHEDULE" : "RENAME",
                            in[r[i].input].name, r[i].off,
                            in[r[j].input].name, r[j].off, r[i].n);
                        matches++; words += r[i].n;
                        if (sched) {
                                sched_matches++; sched_words += r[i].n;
                        } else {
                                rename_matches++; rename_words += r[i].n;
                        }
                }
        printf("SUMMARY: %lu REORDERING MATCHES; %lu MATCHED WORDS (NON-ADDITIVE)\n",
            matches, words);
        printf("  SCHEDULE-ONLY: %lu MATCHES; %lu WORDS\n",
            sched_matches, sched_words);
        printf("  AC-RENAME-DEPENDENT: %lu MATCHES; %lu WORDS\n",
            rename_matches, rename_words);
        printf("  PEEPHOLE-SHORTENING: %lu WORDS\n", peephole_words);
        for (ai = 0; ai < ni; ai++) dobj_free(&in[ai].obj);
        free(r); free(in);
        return 0;
}
