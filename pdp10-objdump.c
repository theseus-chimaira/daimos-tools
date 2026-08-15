#include "dobj.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define HALF_MASK 0777777UL

struct opcode_name {
    unsigned int op;
    const char *name;
};

static const struct opcode_name opcode_names[] = {
    {0200U, "MOVE"}, {0201U, "MOVEI"}, {0202U, "MOVEM"}, {0203U, "MOVES"},
    {0204U, "MOVS"}, {0205U, "MOVSI"}, {0206U, "MOVSM"}, {0207U, "MOVSS"},
    {0210U, "MOVN"}, {0211U, "MOVNI"}, {0212U, "MOVNM"}, {0213U, "MOVNS"},
    {0214U, "MOVM"}, {0215U, "MOVMI"}, {0216U, "MOVMM"}, {0217U, "MOVMS"},
    {0254U, "JRST"}, {0255U, "JFCL"}, {0260U, "PUSHJ"}, {0261U, "PUSH"},
    {0262U, "POP"}, {0263U, "POPJ"}, {0264U, "JSR"}, {0265U, "JSP"},
    {0266U, "JSA"}, {0267U, "JRA"}, {0270U, "ADD"}, {0271U, "ADDI"},
    {0272U, "ADDM"}, {0273U, "ADDB"}, {0274U, "SUB"}, {0275U, "SUBI"},
    {0276U, "SUBM"}, {0277U, "SUBB"}, {0400U, "SETZ"}, {0401U, "SETZI"},
    {0402U, "SETZM"}, {0403U, "SETZB"}, {0474U, "SETO"}, {0475U, "SETOI"},
    {0476U, "SETOM"}, {0477U, "SETOB"}, {0500U, "HLL"}, {0501U, "HLLI"},
    {0502U, "HLLM"}, {0503U, "HLLS"}, {0540U, "HRR"}, {0541U, "HRRI"},
    {0542U, "HRRM"}, {0543U, "HRRS"}, {0550U, "HLR"}, {0551U, "HLRI"},
    {0552U, "HLRM"}, {0553U, "HLRS"}, {0560U, "HRL"}, {0561U, "HRLI"},
    {0562U, "HRLM"}, {0563U, "HRLS"}
};

static const char *opcode_name(unsigned int op)
{
    size_t i;

    for (i = 0U; i < sizeof(opcode_names) / sizeof(opcode_names[0]); i++)
        if (opcode_names[i].op == op)
            return opcode_names[i].name;
    return NULL;
}

static const char *section_name(int sec)
{
    switch (sec) {
    case DOBJ_SEC_ABS: return "ABS";
    case DOBJ_SEC_TEXT: return ".text";
    case DOBJ_SEC_DATA: return ".data";
    case DOBJ_SEC_BSS: return ".bss";
    default: return "?";
    }
}

static void print_word(struct dobj_word w)
{
    printf("%06lo,,%06lo", w.lh & HALF_MASK, w.rh & HALF_MASK);
}

static void disassemble_word(unsigned long addr, struct dobj_word w)
{
    unsigned int op;
    unsigned int ac;
    unsigned int ind;
    unsigned int xr;
    unsigned long y;
    const char *name;

    op = (unsigned int)((w.lh >> 9) & 0777UL);
    ac = (unsigned int)((w.lh >> 5) & 017UL);
    ind = (unsigned int)((w.lh >> 4) & 01UL);
    xr = (unsigned int)(w.lh & 017UL);
    y = w.rh & HALF_MASK;
    name = opcode_name(op);

    printf("%06lo:  ", addr & HALF_MASK);
    print_word(w);
    printf("  ");
    if (name != NULL)
        printf("%-7s", name);
    else
        printf("OP%03o  ", op);
    printf(" %o,", ac);
    if (ind != 0U)
        putchar('@');
    printf("%06lo", y);
    if (xr != 0U)
        printf("(%o)", xr);
    putchar('\n');
}

static void dump_header(const struct dobj_object *obj)
{
    printf("sections: text=%lu data=%lu bss=%lu symbols=%lu relocs=%lu\n",
           obj->text_words, obj->data_words, obj->bss_words,
           obj->symbol_count, obj->reloc_count);
}

static void dump_symbols(const struct dobj_object *obj)
{
    unsigned long i;

    printf("\nSYMBOL TABLE:\n");
    for (i = 0UL; i < obj->symbol_count; i++) {
        const struct dobj_symbol *s;
        s = &obj->symbols[i];
        printf("%4lu %-6s %-5s ", i + 1UL,
               s->kind == DOBJ_SYM_DEF ? "DEF" :
               s->kind == DOBJ_SYM_UNDEF ? "UNDEF" : "?",
               section_name(s->sec));
        print_word(s->value);
        printf(" %s\n", s->name);
    }
}

static void dump_relocs(const struct dobj_object *obj)
{
    unsigned long i;

    printf("\nRELOCATIONS:\n");
    for (i = 0UL; i < obj->reloc_count; i++) {
        const struct dobj_reloc *r;
        r = &obj->relocs[i];
        printf("%s+%06lo type=%d ", section_name(r->loc_sec),
               r->offset & HALF_MASK, r->type);
        if (r->type == DOBJ_RELOC_LOCAL_RH18)
            printf("target=%s", section_name(r->target_sec));
        else if (r->type == DOBJ_RELOC_SYMBOL_RH18 &&
                 r->symbol > 0UL && r->symbol <= obj->symbol_count)
            printf("symbol=%s", obj->symbols[r->symbol - 1UL].name);
        else
            printf("symbol=%lu", r->symbol);
        printf(" addend=");
        print_word(r->addend);
        putchar('\n');
    }
}

static void dump_disassembly(const struct dobj_object *obj)
{
    unsigned long i;

    printf("\nDisassembly of section .text:\n");
    for (i = 0UL; i < obj->text_words; i++)
        disassemble_word(i, obj->text[i]);
}

static void usage(const char *prog)
{
    fprintf(stderr,
            "usage: %s [-h] [-t] [-r] [-d] object.dobj\n"
            "  -h  show object header\n"
            "  -t  show symbol table\n"
            "  -r  show relocations\n"
            "  -d  disassemble .text\n",
            prog);
}

int main(int argc, char **argv)
{
    int show_header;
    int show_symbols;
    int show_relocs;
    int show_disasm;
    int i;
    const char *path;
    FILE *f;
    struct dobj_object obj;

    show_header = 0;
    show_symbols = 0;
    show_relocs = 0;
    show_disasm = 0;
    path = NULL;

    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-h") == 0)
            show_header = 1;
        else if (strcmp(argv[i], "-t") == 0)
            show_symbols = 1;
        else if (strcmp(argv[i], "-r") == 0)
            show_relocs = 1;
        else if (strcmp(argv[i], "-d") == 0)
            show_disasm = 1;
        else if (argv[i][0] == '-') {
            usage(argv[0]);
            return 2;
        } else if (path == NULL)
            path = argv[i];
        else {
            usage(argv[0]);
            return 2;
        }
    }
    if (path == NULL) {
        usage(argv[0]);
        return 2;
    }
    if (!show_header && !show_symbols && !show_relocs && !show_disasm)
        show_header = show_symbols = show_relocs = show_disasm = 1;

    f = fopen(path, "rb");
    if (f == NULL) {
        perror(path);
        return 1;
    }
    if (!dobj_is_object(f) || fseek(f, 0L, SEEK_SET) != 0 ||
        dobj_read(f, &obj) != 0) {
        fprintf(stderr, "%s: not a DOBJ1 object\n", path);
        fclose(f);
        return 1;
    }
    fclose(f);

    if (show_header)
        dump_header(&obj);
    if (show_symbols)
        dump_symbols(&obj);
    if (show_relocs)
        dump_relocs(&obj);
    if (show_disasm)
        dump_disassembly(&obj);

    dobj_free(&obj);
    return 0;
}
