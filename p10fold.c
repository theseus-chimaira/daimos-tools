#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "dobj.h"

#define DEFAULT_MIN_WORDS 4UL

struct input {
        const char *name;
        struct dobj_object obj;
        const struct dobj_reloc **reloc_at;
};

struct occurrence {
        int input;
        unsigned long off;
        int removable;
};

struct group {
        unsigned long words;
        struct occurrence *occ;
        unsigned long count;
        unsigned long cap;
};

static void
usage(void)
{
        fprintf(stderr,
            "usage: p10fold [-m min-words] [-P fold.plan] object.dobj ...\n");
}

static int
reloc_half(int type)
{
        if (type == DOBJ_RELOC_LOCAL_RH18 ||
            type == DOBJ_RELOC_SYMBOL_RH18)
                return 0;
        if (type == DOBJ_RELOC_LOCAL_LH18 ||
            type == DOBJ_RELOC_SYMBOL_LH18)
                return 1;
        return -1;
}

static int
half_equal(const struct input *a, unsigned long ao,
    const struct input *b, unsigned long bo, unsigned long abase,
    unsigned long bbase, unsigned long span, int half)
{
        const struct dobj_reloc *ar;
        const struct dobj_reloc *br;
        unsigned long aw;
        unsigned long bw;

        ar = a->reloc_at[ao * 2UL + (unsigned long)half];
        br = b->reloc_at[bo * 2UL + (unsigned long)half];
        aw = half ? a->obj.text[ao].lh : a->obj.text[ao].rh;
        bw = half ? b->obj.text[bo].lh : b->obj.text[bo].rh;
        if (ar == NULL || br == NULL)
                return ar == br && aw == bw;
        if (ar->type != br->type)
                return 0;
        if (ar->type == DOBJ_RELOC_SYMBOL_RH18 ||
            ar->type == DOBJ_RELOC_SYMBOL_LH18) {
                const struct dobj_symbol *as;
                const struct dobj_symbol *bs;

                if (ar->symbol == 0UL || ar->symbol > a->obj.symbol_count ||
                    br->symbol == 0UL || br->symbol > b->obj.symbol_count)
                        return 0;
                as = &a->obj.symbols[ar->symbol - 1UL];
                bs = &b->obj.symbols[br->symbol - 1UL];
                if (strcmp(as->name, bs->name) != 0 ||
                    ar->addend.lh != br->addend.lh ||
                    ar->addend.rh != br->addend.rh)
                        return 0;
        } else {
                if (ar->target_sec != br->target_sec)
                        return 0;
                if (ar->target_sec == DOBJ_SEC_TEXT &&
                    ar->addend.rh >= abase &&
                    ar->addend.rh < abase + span &&
                    br->addend.rh >= bbase &&
                    br->addend.rh < bbase + span) {
                        if (ar->addend.rh - abase != br->addend.rh - bbase ||
                            ar->addend.lh != br->addend.lh)
                                return 0;
                } else if (ar->addend.lh != br->addend.lh ||
                    ar->addend.rh != br->addend.rh) {
                        return 0;
                }
        }
        return 1;
}

static int
word_equal(const struct input *a, unsigned long ao,
    const struct input *b, unsigned long bo, unsigned long abase,
    unsigned long bbase, unsigned long span)
{
        return half_equal(a, ao, b, bo, abase, bbase, span, 0) &&
            half_equal(a, ao, b, bo, abase, bbase, span, 1);
}

static int
range_equal(const struct input *a, unsigned long abase,
    const struct input *b, unsigned long bbase, unsigned long words)
{
        unsigned long i;

        for (i = 0UL; i < words; i++)
                if (!word_equal(a, abase + i, b, bbase + i,
                    abase, bbase, words))
                        return 0;
        return 1;
}

static const char *
symbol_at(const struct input *in, unsigned long off)
{
        unsigned long i;

        for (i = 0UL; i < in->obj.symbol_count; i++) {
                const struct dobj_symbol *s = &in->obj.symbols[i];

                if (s->kind == DOBJ_SYM_DEF && s->sec == DOBJ_SEC_TEXT &&
                    s->value.rh == off)
                        return s->name;
        }
        return NULL;
}

static unsigned long
next_symbol(const struct input *in, unsigned long off)
{
        unsigned long i;
        unsigned long next = in->obj.text_words;

        for (i = 0UL; i < in->obj.symbol_count; i++) {
                const struct dobj_symbol *s = &in->obj.symbols[i];

                if (s->kind == DOBJ_SYM_DEF && s->sec == DOBJ_SEC_TEXT &&
                    s->value.rh > off && s->value.rh < next)
                        next = s->value.rh;
        }
        return next;
}

static unsigned long
opcode_at(const struct input *in, unsigned long off)
{
        return (in->obj.text[off].lh >> 9) & 0777UL;
}

static int
unconditional_end(const struct input *in, unsigned long off)
{
        unsigned long op = opcode_at(in, off);

        /*
         * JRST and POPJ end the current sequential path.  Keep this list
         * deliberately short: a missed opportunity is preferable to calling
         * a conditional or skip-dependent path free.
         */
        return op == 0254UL || op == 0263UL;
}

static int
isolated_block(const struct input *in, unsigned long off,
    unsigned long words)
{
        unsigned long i;

        if (words == 0UL)
                return 0;
        if (off != 0UL && !unconditional_end(in, off - 1UL))
                return 0;
        if (!unconditional_end(in, off + words - 1UL))
                return 0;
        /*
         * A removed copy must have a single legal entry.  Reject labels and
         * local TEXT relocations into its interior; otherwise some control
         * path could bypass the entry we intend to retarget.
         */
        for (i = 0UL; i < in->obj.symbol_count; i++) {
                const struct dobj_symbol *s = &in->obj.symbols[i];

                if (s->kind == DOBJ_SYM_DEF && s->sec == DOBJ_SEC_TEXT &&
                    s->value.rh > off && s->value.rh < off + words)
                        return 0;
        }
        for (i = 0UL; i < in->obj.reloc_count; i++) {
                const struct dobj_reloc *r = &in->obj.relocs[i];

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
removable_block(const struct input *in, unsigned long off,
    unsigned long words)
{
        /*
         * Keep named entries address-stable.  Unnamed isolated blocks may be
         * removed by retargeting their existing incoming transfers.
         */
        return symbol_at(in, off) == NULL &&
            isolated_block(in, off, words);
}

static int
same_occurrence(const struct occurrence *a, const struct occurrence *b)
{
        return a->input == b->input && a->off == b->off;
}

static int
group_add(struct group *g, int input, unsigned long off, int removable)
{
        struct occurrence o;
        unsigned long i;

        o.input = input;
        o.off = off;
        o.removable = removable;
        for (i = 0UL; i < g->count; i++)
                if (same_occurrence(&g->occ[i], &o)) {
                        if (removable)
                                g->occ[i].removable = 1;
                        return 0;
                }
        if (g->count == g->cap) {
                unsigned long cap = g->cap ? g->cap * 2UL : 4UL;
                struct occurrence *p = (struct occurrence *)realloc(g->occ,
                    (size_t)cap * sizeof(*p));

                if (p == NULL)
                        return -1;
                g->occ = p;
                g->cap = cap;
        }
        g->occ[g->count++] = o;
        return 0;
}

static int
group_equivalent(const struct group *g, const struct input *inputs,
    int ai, unsigned long ao, int bi, unsigned long bo, unsigned long words)
{
        if (g->words != words || g->count == 0UL)
                return 0;
        return range_equal(&inputs[g->occ[0].input], g->occ[0].off,
            &inputs[ai], ao, words) &&
            range_equal(&inputs[g->occ[0].input], g->occ[0].off,
            &inputs[bi], bo, words);
}

static int
add_zero_group(struct group **groupsp, unsigned long *countp,
    unsigned long *capp, const struct input *inputs, int ai, unsigned long ao,
    int ar, int bi, unsigned long bo, int br, unsigned long words)
{
        struct group *groups = *groupsp;
        unsigned long i;

        for (i = 0UL; i < *countp; i++) {
                if (!group_equivalent(&groups[i], inputs, ai, ao, bi, bo,
                    words))
                        continue;
                if (group_add(&groups[i], ai, ao, ar) != 0 ||
                    group_add(&groups[i], bi, bo, br) != 0)
                        return -1;
                return 0;
        }
        if (*countp == *capp) {
                unsigned long cap = *capp ? *capp * 2UL : 16UL;
                struct group *p = (struct group *)realloc(groups,
                    (size_t)cap * sizeof(*p));

                if (p == NULL)
                        return -1;
                groups = p;
                *groupsp = groups;
                *capp = cap;
        }
        memset(&groups[*countp], 0, sizeof(groups[*countp]));
        groups[*countp].words = words;
        if (group_add(&groups[*countp], ai, ao, ar) != 0 ||
            group_add(&groups[*countp], bi, bo, br) != 0)
                return -1;
        (*countp)++;
        return 0;
}

static int
group_cmp(const void *av, const void *bv)
{
        const struct group *a = (const struct group *)av;
        const struct group *b = (const struct group *)bv;
        unsigned long i, ar = 0UL, br = 0UL;
        unsigned long asave, bsave;
        for (i = 0UL; i < a->count; i++) ar += a->occ[i].removable != 0;
        for (i = 0UL; i < b->count; i++) br += b->occ[i].removable != 0;
        if (ar >= a->count) ar = a->count - 1UL;
        if (br >= b->count) br = b->count - 1UL;
        asave = a->words * ar;
        bsave = b->words * br;

        if (asave < bsave)
                return 1;
        if (asave > bsave)
                return -1;
        return 0;
}

static int
load_input(struct input *in, const char *name)
{
        FILE *f;
        unsigned long i;

        memset(in, 0, sizeof(*in));
        in->name = name;
        f = fopen(name, "rb");
        if (f == NULL) {
                perror(name);
                return -1;
        }
        if (dobj_read(f, &in->obj) != 0) {
                fprintf(stderr, "p10fold: cannot read DOBJ1 object: %s\n",
                    name);
                fclose(f);
                return -1;
        }
        fclose(f);
        if (in->obj.text_words == 0UL)
                return 0;
        in->reloc_at = (const struct dobj_reloc **)calloc(
            (size_t)(in->obj.text_words * 2UL), sizeof(*in->reloc_at));
        if (in->reloc_at == NULL)
                return -1;
        for (i = 0UL; i < in->obj.reloc_count; i++) {
                const struct dobj_reloc *r = &in->obj.relocs[i];

                int half;

                if (r->loc_sec != DOBJ_SEC_TEXT ||
                    r->offset >= in->obj.text_words)
                        continue;
                half = reloc_half(r->type);
                if (half < 0) {
                        fprintf(stderr,
                            "p10fold: unsupported TEXT relocation at %s+%06lo\n",
                            name, r->offset);
                        return -1;
                }
                if (in->reloc_at[r->offset * 2UL +
                    (unsigned long)half] != NULL) {
                        fprintf(stderr,
                            "p10fold: duplicate half relocation at %s+%06lo\n",
                            name, r->offset);
                        return -1;
                }
                in->reloc_at[r->offset * 2UL + (unsigned long)half] = r;
        }
        return 0;
}

static void
free_input(struct input *in)
{
        free(in->reloc_at);
        dobj_free(&in->obj);
}

int
main(int argc, char **argv)
{
        struct input *inputs;
        unsigned long min_words = DEFAULT_MIN_WORDS;
        unsigned long candidates = 0UL;
        unsigned long whole_words = 0UL;
        unsigned long zero_cost_candidates = 0UL;
        unsigned long one_jump_candidates = 0UL;
        unsigned long address_sensitive_candidates = 0UL;
        struct group *groups = NULL;
        unsigned long group_count = 0UL;
        unsigned long group_cap = 0UL;
        int first = 1;
        const char *plan_name = NULL;
        int count;
        int ai;
        int bi;

        while (first < argc && argv[first][0] == '-') {
                if (strcmp(argv[first], "-m") == 0 && first + 1 < argc) {
                        char *end;
                        min_words = strtoul(argv[first + 1], &end, 0);
                        if (*argv[first + 1] == '\0' || *end != '\0' ||
                            min_words < 1UL) {
                                usage();
                                return 2;
                        }
                        first += 2;
                } else if (strcmp(argv[first], "-P") == 0 &&
                    first + 1 < argc) {
                        plan_name = argv[first + 1];
                        first += 2;
                } else {
                        usage();
                        return 2;
                }
        }
        if (argc - first < 1) {
                usage();
                return 2;
        }
        count = argc - first;
        inputs = (struct input *)calloc((size_t)count, sizeof(*inputs));
        if (inputs == NULL)
                return 1;
        for (ai = 0; ai < count; ai++) {
                if (load_input(&inputs[ai], argv[first + ai]) != 0) {
                        while (ai-- > 0)
                                free_input(&inputs[ai]);
                        free(inputs);
                        return 1;
                }
        }

        printf("P10FOLD DOBJ1 DUPLICATE TEXT ANALYSIS\n");
        printf("MINIMUM RUN: %lu WORDS\n\n", min_words);
        for (ai = 0; ai < count; ai++) {
                unsigned long ao;

                for (ao = 0UL; ao + min_words <= inputs[ai].obj.text_words;
                    ao++) {
                        for (bi = ai; bi < count; bi++) {
                                unsigned long bo;
                                unsigned long start = bi == ai ? ao + 1UL : 0UL;

                                for (bo = start;
                                    bo + min_words <= inputs[bi].obj.text_words;
                                    bo++) {
                                        unsigned long n;
                                        unsigned long max;
                                        const char *as;
                                        const char *bs;
                                        int whole;

                                        if (!range_equal(&inputs[ai], ao,
                                            &inputs[bi], bo, min_words))
                                                continue;
                                        if (ao != 0UL && bo != 0UL &&
                                            range_equal(&inputs[ai], ao - 1UL,
                                            &inputs[bi], bo - 1UL,
                                            min_words + 1UL))
                                                continue;
                                        max = inputs[ai].obj.text_words - ao;
                                        if (inputs[bi].obj.text_words - bo < max)
                                                max = inputs[bi].obj.text_words - bo;
                                        n = min_words;
                                        while (n < max &&
                                            range_equal(&inputs[ai], ao,
                                            &inputs[bi], bo, n + 1UL))
                                                n++;
                                        as = symbol_at(&inputs[ai], ao);
                                        bs = symbol_at(&inputs[bi], bo);
                                        whole = as != NULL && bs != NULL &&
                                            next_symbol(&inputs[ai], ao) == ao + n &&
                                            next_symbol(&inputs[bi], bo) == bo + n;
                                        int zero_block;
                                        int ar;
                                        int br;
                                        const char *cost;
                                        unsigned long pair_net;

                                        /*
                                         * A named entry may have observable
                                         * address identity.  It can anchor a
                                         * fold, but do not remove/alias it in
                                         * the strict zero-cost accounting.
                                         */
                                        ar = removable_block(
                                            &inputs[ai], ao, n);
                                        br = removable_block(
                                            &inputs[bi], bo, n);
                                        zero_block = ar || br;
                                        cost = zero_block ? "ZERO" :
                                            (whole ? "ADDRESS_ALIAS" :
                                            "ONE_JRST");
                                        pair_net = zero_block ? n :
                                            (whole ? 0UL : n - 1UL);
                                        printf("%s %s+%06lo%s%s%s <=> "
                                            "%s+%06lo%s%s%s : %lu WORDS; "
                                            "COST=%s; PAIR_NET=%lu\n",
                                            whole ? "WHOLE" :
                                            (zero_block ? "BLOCK" : "RUN  "),
                                            inputs[ai].name, ao,
                                            as ? " (" : "", as ? as : "",
                                            as ? ")" : "",
                                            inputs[bi].name, bo,
                                            bs ? " (" : "", bs ? bs : "",
                                            bs ? ")" : "", n,
                                            cost, pair_net);
                                        candidates++;
                                        if (zero_block) {
                                                whole_words += n;
                                                zero_cost_candidates++;
                                                if (add_zero_group(&groups,
                                                    &group_count, &group_cap,
                                                    inputs, ai, ao, ar,
                                                    bi, bo, br, n) != 0) {
                                                        fprintf(stderr,
                                                            "p10fold: out of memory\n");
                                                        return 1;
                                                }
                                        } else {
                                                if (whole)
                                                        address_sensitive_candidates++;
                                                else
                                                        one_jump_candidates++;
                                        }

                                        /*
                                         * The maximal duplicate run need not
                                         * itself be removable.  A shorter
                                         * aligned subrun may start/end on
                                         * legal transfer boundaries.  Add
                                         * those exact subruns to the fold
                                         * groups as well so the chosen
                                         * minimum length cannot hide a valid
                                         * zero-cost fold.
                                         */
                                        {
                                                unsigned long suboff;
                                                for (suboff = 0UL;
                                                    suboff + min_words <= n;
                                                    suboff++) {
                                                        unsigned long subn;
                                                        for (subn = min_words;
                                                            suboff + subn <= n;
                                                            subn++) {
                                                                int sar, sbr;
                                                                if (suboff == 0UL &&
                                                                    subn == n)
                                                                        continue;
                                                                if (!range_equal(
                                                                    &inputs[ai],
                                                                    ao + suboff,
                                                                    &inputs[bi],
                                                                    bo + suboff,
                                                                    subn))
                                                                        continue;
                                                                sar = removable_block(
                                                                    &inputs[ai],
                                                                    ao + suboff,
                                                                    subn);
                                                                sbr = removable_block(
                                                                    &inputs[bi],
                                                                    bo + suboff,
                                                                    subn);
                                                                if (!sar && !sbr)
                                                                        continue;
                                                                if (add_zero_group(
                                                                    &groups,
                                                                    &group_count,
                                                                    &group_cap,
                                                                    inputs, ai,
                                                                    ao + suboff,
                                                                    sar, bi,
                                                                    bo + suboff,
                                                                    sbr, subn) != 0) {
                                                                        fprintf(stderr,
                                                                            "p10fold: out of memory\n");
                                                                        return 1;
                                                                }
                                                        }
                                                }
                                        }
                                }
                        }
                }
        }
        printf("\nSUMMARY: %lu CANDIDATES; %lu ZERO-COST CANDIDATE WORDS\n",
            candidates, whole_words);
        printf("COST CLASSES: %lu ZERO-EXECUTION-COST; "
            "%lu ADDRESS-SENSITIVE WHOLE ENTRIES; "
            "%lu REQUIRE ONE EXTRA JRST PER FOLDED OCCURRENCE.\n",
            zero_cost_candidates, address_sensitive_candidates,
            one_jump_candidates);
        printf("RUNS MAY OVERLAP; RUN LENGTHS ARE NOT ADDITIVE SAVINGS.\n");
        printf("PAIR_NET ASSUMES ONE DUPLICATE COPY IS REPLACED; "
            "IT IS NOT AN ADDITIVE TOTAL.\n");
        if (group_count != 0UL) {
                unsigned char **used;
                unsigned long accepted = 0UL;
                unsigned long saved = 0UL;
                unsigned long gi;
                FILE *plan = NULL;

                if (plan_name != NULL) {
                        plan = fopen(plan_name, "w");
                        if (plan == NULL) {
                                perror(plan_name);
                                return 1;
                        }
                        if (fprintf(plan, "P10FOLD1\n") < 0) {
                                fclose(plan);
                                remove(plan_name);
                                return 1;
                        }
                }

                used = (unsigned char **)calloc((size_t)count, sizeof(*used));
                if (used == NULL)
                        return 1;
                for (ai = 0; ai < count; ai++) {
                        used[ai] = (unsigned char *)calloc(
                            (size_t)inputs[ai].obj.text_words, 1U);
                        if (used[ai] == NULL)
                                return 1;
                }
                qsort(groups, (size_t)group_count, sizeof(*groups), group_cmp);
                printf("\nZERO-COST EQUIVALENCE GROUPS "
                    "(GREEDY NON-OVERLAPPING):\n");
                for (gi = 0UL; gi < group_count; gi++) {
                        struct group *g = &groups[gi];
                        unsigned long oi;
                        unsigned long usable = 0UL;
                        unsigned long removable = 0UL;
                        unsigned long group_saved;
                        unsigned long anchor = ~0UL;
                        int blocked = 0;

                        for (oi = 0UL; oi < g->count; oi++) {
                                unsigned long j;
                                struct occurrence *o = &g->occ[oi];
                                int occ_blocked = 0;

                                for (j = 0UL; j < g->words; j++)
                                        if (used[o->input][o->off + j])
                                                occ_blocked = 1;
                                if (occ_blocked)
                                        blocked = 1;
                                else {
                                        usable++;
                                        if (o->removable)
                                                removable++;
                                }
                        }
                        if (blocked || usable < 2UL)
                                continue;
                        if (removable >= usable)
                                removable = usable - 1UL;
                        if (removable == 0UL)
                                continue;
                        group_saved = g->words * removable;
                        for (oi = 0UL; oi < g->count; oi++) {
                                struct occurrence *o = &g->occ[oi];
                                unsigned long j;
                                int occ_blocked = 0;
                                for (j = 0UL; j < g->words; j++)
                                        if (used[o->input][o->off + j])
                                                occ_blocked = 1;
                                if (!occ_blocked && !o->removable) {
                                        anchor = oi;
                                        break;
                                }
                        }
                        if (anchor == ~0UL)
                                for (oi = 0UL; oi < g->count; oi++) {
                                        struct occurrence *o = &g->occ[oi];
                                        unsigned long j;
                                        int occ_blocked = 0;
                                        for (j = 0UL; j < g->words; j++)
                                                if (used[o->input][o->off + j])
                                                        occ_blocked = 1;
                                        if (!occ_blocked) {
                                                anchor = oi;
                                                break;
                                        }
                                }
                        if (anchor == ~0UL)
                                continue;
                        printf("GROUP %lu: %lu WORDS x %lu COPIES; SAVE %lu\n",
                            accepted + 1UL, g->words, g->count,
                            group_saved);
                        for (oi = 0UL; oi < g->count; oi++) {
                                unsigned long j;
                                struct occurrence *o = &g->occ[oi];
                                const char *s = symbol_at(&inputs[o->input],
                                    o->off);

                                printf("  %s %s+%06lo%s%s%s\n",
                                    o->removable ? "DONOR " : "ANCHOR",
                                    inputs[o->input].name, o->off,
                                    s ? " (" : "", s ? s : "",
                                    s ? ")" : "");
                                if (plan != NULL && oi != anchor &&
                                    o->removable &&
                                    fprintf(plan,
                                    "FOLD\t%s\t%lo\t%s\t%lo\t%lo\n",
                                    inputs[o->input].name, o->off,
                                    inputs[g->occ[anchor].input].name,
                                    g->occ[anchor].off, g->words) < 0) {
                                        fclose(plan);
                                        remove(plan_name);
                                        return 1;
                                }
                                for (j = 0UL; j < g->words; j++)
                                        used[o->input][o->off + j] = 1U;
                        }
                        saved += group_saved;
                        accepted++;
                }
                printf("ZERO-COST NON-OVERLAPPING TOTAL: %lu GROUPS; "
                    "%lu WORDS SAVED\n", accepted, saved);
                if (plan != NULL && fclose(plan) != 0) {
                        remove(plan_name);
                        return 1;
                }
                for (ai = 0; ai < count; ai++)
                        free(used[ai]);
                free(used);
        } else if (plan_name != NULL) {
                FILE *plan = fopen(plan_name, "w");
                if (plan == NULL) {
                        perror(plan_name);
                        return 1;
                }
                if (fprintf(plan, "P10FOLD1\n") < 0 || fclose(plan) != 0) {
                        remove(plan_name);
                        return 1;
                }
        }
        for (ai = 0; ai < (int)group_count; ai++)
                free(groups[ai].occ);
        free(groups);
        for (ai = 0; ai < count; ai++)
                free_input(&inputs[ai]);
        free(inputs);
        return 0;
}
