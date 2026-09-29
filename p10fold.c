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

static void
usage(void)
{
        fprintf(stderr, "usage: p10fold [-m min-words] object.dobj ...\n");
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
        if (words == 0UL)
                return 0;
        if (off != 0UL && !unconditional_end(in, off - 1UL))
                return 0;
        return unconditional_end(in, off + words - 1UL);
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
        int first = 1;
        int count;
        int ai;
        int bi;

        if (argc > 3 && strcmp(argv[1], "-m") == 0) {
                char *end;

                min_words = strtoul(argv[2], &end, 0);
                if (*argv[2] == '\0' || *end != '\0' || min_words < 2UL) {
                        usage();
                        return 2;
                }
                first = 3;
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
                                        const char *cost;
                                        unsigned long pair_net;

                                        zero_block =
                                            isolated_block(&inputs[ai], ao, n) &&
                                            isolated_block(&inputs[bi], bo, n);
                                        cost = (whole || zero_block) ?
                                            "ZERO" : "ONE_JRST";
                                        pair_net = (whole || zero_block) ?
                                            n : n - 1UL;
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
                                        if (whole || zero_block) {
                                                whole_words += n;
                                                zero_cost_candidates++;
                                        } else {
                                                one_jump_candidates++;
                                        }
                                }
                        }
                }
        }
        printf("\nSUMMARY: %lu CANDIDATES; %lu ZERO-COST CANDIDATE WORDS\n",
            candidates, whole_words);
        printf("COST CLASSES: %lu ZERO-EXECUTION-COST; "
            "%lu REQUIRE ONE EXTRA JRST PER FOLDED OCCURRENCE.\n",
            zero_cost_candidates, one_jump_candidates);
        printf("RUNS MAY OVERLAP; RUN LENGTHS ARE NOT ADDITIVE SAVINGS.\n");
        printf("PAIR_NET ASSUMES ONE DUPLICATE COPY IS REPLACED; "
            "IT IS NOT AN ADDITIVE TOTAL.\n");
        for (ai = 0; ai < count; ai++)
                free_input(&inputs[ai]);
        free(inputs);
        return 0;
}
