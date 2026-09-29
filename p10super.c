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

struct input {
        const char *name;
        struct dobj_object obj;
};

struct insn {
        struct dobj_word word;
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
        struct dobj_word canon[MAX_REGION];
        struct insn insn[MAX_REGION];
        int tail_candidate;
};

static int
unconditional_end_word(struct dobj_word w)
{
        unsigned long op = (w.lh >> 9) & 0777UL;
        return op == 0254UL || op == 0263UL; /* JRST, POPJ */
}

static unsigned long
canon_lh(struct dobj_word w, const unsigned int map[16])
{
        unsigned long ac = (w.lh >> 5) & 017UL;
        return (w.lh & ~0740UL) | ((unsigned long)map[ac] << 5);
}

/*
 * Rename ACs by first appearance.  AC0 and P (17) retain their ABI meaning;
 * the other AC numbers are scratch names for this advisory comparison.
 * Final acceptance still requires source reassembly and exact p10fold proof.
 */
static void
normalize_acs(struct dobj_word *v, unsigned long n)
{
        unsigned int map[16];
        unsigned int next = 1;
        unsigned long i;
        unsigned int a;

        for (a = 0; a < 16; a++) map[a] = 16;
        map[0] = 0; map[15] = 15;
        for (i = 0; i < n; i++) {
                a = (unsigned int)((v[i].lh >> 5) & 017UL);
                if (map[a] == 16) {
                        while (next == 15) next++;
                        map[a] = next++;
                }
                v[i].lh = canon_lh(v[i], map);
        }
}

static void
usage(void)
{
        fprintf(stderr, "usage: p10super object.dobj ...\n");
}

static const struct dobj_reloc *
rh_reloc(const struct dobj_object *o, unsigned long off)
{
        unsigned long i;
        for (i = 0; i < o->reloc_count; i++)
                if (o->relocs[i].loc_sec == DOBJ_SEC_TEXT &&
                    o->relocs[i].offset == off &&
                    (o->relocs[i].type == DOBJ_RELOC_LOCAL_RH18 ||
                    o->relocs[i].type == DOBJ_RELOC_SYMBOL_RH18))
                        return &o->relocs[i];
        return NULL;
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

static int
word_cmp(struct dobj_word a, struct dobj_word b)
{
        if (a.lh != b.lh)
                return a.lh < b.lh ? -1 : 1;
        if (a.rh != b.rh)
                return a.rh < b.rh ? -1 : 1;
        return 0;
}

/*
 * Lexicographically smallest topological schedule.  Dependencies preserve
 * the original order whenever two instructions are not proven independent.
 */
static void
canonicalize(const struct insn *v, unsigned long n, struct dobj_word *out)
{
        unsigned int pred[MAX_REGION] = {0};
        unsigned int done = 0;
        unsigned long i, j, k;

        for (i = 0; i < n; i++)
                for (j = i + 1; j < n; j++)
                        if (!independent(&v[i], &v[j]))
                                pred[j] |= 1U << i;
        for (k = 0; k < n; k++) {
                int best = -1;
                for (i = 0; i < n; i++) {
                        if ((done & (1U << i)) != 0 ||
                            (pred[i] & ~done) != 0)
                                continue;
                        if (best < 0 ||
                            word_cmp(v[i].word, v[(unsigned long)best].word) < 0)
                                best = (int)i;
                }
                out[k] = v[(unsigned long)best].word;
                done |= 1U << (unsigned int)best;
        }
}

static int
same_canon(const struct region *a, const struct region *b)
{
        unsigned long i;
        if (a->n != b->n)
                return 0;
        for (i = 0; i < a->n; i++)
                if (word_cmp(a->canon[i], b->canon[i]) != 0)
                        return 0;
        return 1;
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
                        if (word_cmp(aw, bw) != 0) continue;
                        if (common_schedule_rec(a, b, ap, bp,
                            adone | (1U << i), bdone | (1U << j), depth + 1))
                                return 1;
                }
        }
        return 0;
}

static int
mapped_word_equal(struct dobj_word a, struct dobj_word b,
    int amap[16], int bmap[16])
{
        unsigned int aa = (unsigned int)((a.lh >> 5) & 017UL);
        unsigned int ba = (unsigned int)((b.lh >> 5) & 017UL);
        if ((a.lh & ~0740UL) != (b.lh & ~0740UL) || a.rh != b.rh)
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
                        if ((bdone & (1U << j)) || (bp[j] & ~bdone)) continue;
                        memcpy(am, amap, sizeof(am)); memcpy(bm, bmap, sizeof(bm));
                        if (!mapped_word_equal(a->insn[i].word, b->insn[j].word,
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

int
main(int argc, char **argv)
{
        struct input *in;
        struct region *r = NULL;
        unsigned long nr = 0, cap = 0, matches = 0, words = 0;
        unsigned long sched_matches = 0, sched_words = 0;
        unsigned long rename_matches = 0, rename_words = 0;
        int ni, ai;

        if (argc < 3) {
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
                                memcpy(r[nr].insn, v,
                                    (size_t)len * sizeof(v[0]));
                                canonicalize(v, len, r[nr].canon);
                                normalize_acs(r[nr].canon, len);
                                nr++;
                        }
                }
        }
        for (unsigned long i = 0; i < nr; i++)
                for (unsigned long j = i + 1; j < nr; j++) {
                        int sched;
                        int renamed;
                        if (r[i].input == r[j].input ||
                            r[i].n != r[j].n)
                                continue;
                        sched = same_canon(&r[i], &r[j]) ||
                            has_common_schedule(&r[i], &r[j]);
                        renamed = !sched &&
                            has_common_renamed_schedule(&r[i], &r[j]);
                        if (!sched && !renamed) continue;
                        /* Ignore regions already byte-identical in source order. */
                        int identical = 1;
                        for (unsigned long k = 0; k < r[i].n; k++)
                                if (word_cmp(in[r[i].input].obj.text[r[i].off+k],
                                    in[r[j].input].obj.text[r[j].off+k]) != 0)
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
        for (ai = 0; ai < ni; ai++) dobj_free(&in[ai].obj);
        free(r); free(in);
        return 0;
}
