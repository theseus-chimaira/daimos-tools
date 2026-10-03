#include "dobj_native.h"

struct opcode_name {
        unsigned int op;
        const char *name;
};

static const struct opcode_name opnames[] = {
        {0200U,"MOVE"},{0201U,"MOVEI"},{0202U,"MOVEM"},{0203U,"MOVES"},
        {0204U,"MOVS"},{0205U,"MOVSI"},{0206U,"MOVSM"},{0207U,"MOVSS"},
        {0210U,"MOVN"},{0211U,"MOVNI"},{0212U,"MOVNM"},{0213U,"MOVNS"},
        {0214U,"MOVM"},{0215U,"MOVMI"},{0216U,"MOVMM"},{0217U,"MOVMS"},
        {0254U,"JRST"},{0255U,"JFCL"},{0260U,"PUSHJ"},{0261U,"PUSH"},
        {0262U,"POP"},{0263U,"POPJ"},{0264U,"JSR"},{0265U,"JSP"},
        {0266U,"JSA"},{0267U,"JRA"},{0270U,"ADD"},{0271U,"ADDI"},
        {0272U,"ADDM"},{0273U,"ADDB"},{0274U,"SUB"},{0275U,"SUBI"},
        {0276U,"SUBM"},{0277U,"SUBB"},{0400U,"SETZ"},{0401U,"SETZI"},
        {0402U,"SETZM"},{0403U,"SETZB"},{0474U,"SETO"},{0475U,"SETOI"},
        {0476U,"SETOM"},{0477U,"SETOB"},{0500U,"HLL"},{0501U,"HLLI"},
        {0502U,"HLLM"},{0503U,"HLLS"},{0540U,"HRR"},{0541U,"HRRI"},
        {0542U,"HRRM"},{0543U,"HRRS"},{0550U,"HLR"},{0551U,"HLRI"},
        {0552U,"HLRM"},{0553U,"HLRS"},{0560U,"HRL"},{0561U,"HRLI"},
        {0562U,"HRLM"},{0563U,"HRLS"}
};

static const char *
opname(unsigned int op)
{
        unsigned int i;
        for (i = 0U; i < sizeof(opnames) / sizeof(opnames[0]); ++i)
                if (opnames[i].op == op)
                        return opnames[i].name;
        return 0;
}

static const char *
secname(unsigned int sec)
{
        if (sec == ND_SEC_ABS) return "ABS";
        if (sec == ND_SEC_TEXT) return ".TEXT";
        if (sec == ND_SEC_DATA) return ".DATA";
        if (sec == ND_SEC_BSS) return ".BSS";
        return "?";
}

static int
put_word(kword_t w)
{
        return u_put_octal(1, nd_lh(w), 6U) != 0 ||
            u_puts(1, ",,") != 0 || u_put_octal(1, nd_rh(w), 6U) != 0;
}

static int
dump_header(const struct nd_obj *o)
{
        return u_puts(1, "TEXT ") != 0 || u_put_uint(1, o->text_words) != 0 ||
            u_puts(1, " DATA ") != 0 || u_put_uint(1, o->data_words) != 0 ||
            u_puts(1, " BSS ") != 0 || u_put_uint(1, o->bss_words) != 0 ||
            u_puts(1, " SYMBOLS ") != 0 || u_put_uint(1, o->symbol_count) != 0 ||
            u_puts(1, " RELOCS ") != 0 || u_put_uint(1, o->reloc_count) != 0 ||
            u_crlf(1) != 0 ? -1 : 0;
}

static int
dump_symbols(int fd, const struct nd_obj *o)
{
        struct nd_symbol s;
        unsigned int i;
        if (u_puts(1, "SYMBOL TABLE:") != 0 || u_crlf(1) != 0)
                return -1;
        for (i = 0U; i < (unsigned int)o->symbol_count; ++i) {
                if (nd_obj_symbol(fd, o, i, &s) != 0 ||
                    u_put_uint(1, i + 1U) != 0 || u_putc(1, ' ') != 0 ||
                    u_puts(1, s.kind == ND_SYM_DEF ? "DEF " : "UNDEF ") != 0 ||
                    u_puts(1, secname(s.sec)) != 0 || u_putc(1, ' ') != 0 ||
                    put_word(s.value) != 0 || u_putc(1, ' ') != 0 ||
                    nd_put_name(1, s.name) != 0 || u_crlf(1) != 0)
                        return -1;
        }
        return 0;
}

static int
dump_relocs(int fd, const struct nd_obj *o)
{
        struct nd_reloc r;
        struct nd_symbol s;
        unsigned int i;
        if (u_puts(1, "RELOCATIONS:") != 0 || u_crlf(1) != 0)
                return -1;
        for (i = 0U; i < (unsigned int)o->reloc_count; ++i) {
                if (nd_obj_reloc(fd, o, i, &r) != 0 ||
                    u_puts(1, secname(r.loc_sec)) != 0 || u_putc(1, '+') != 0 ||
                    u_put_octal(1, r.offset, 6U) != 0 ||
                    u_puts(1, " TYPE ") != 0 || u_put_uint(1, r.type) != 0)
                        return -1;
                if (r.type == ND_RELOC_LOCAL_RH18 ||
                    r.type == ND_RELOC_LOCAL_LH18) {
                        if (u_puts(1, " TARGET ") != 0 ||
                            u_puts(1, secname(r.target_sec)) != 0)
                                return -1;
                } else if (r.symbol != 0UL && r.symbol <= o->symbol_count) {
                        if (nd_obj_symbol(fd, o, (unsigned int)r.symbol - 1U,
                            &s) != 0 || u_puts(1, " SYMBOL ") != 0 ||
                            nd_put_name(1, s.name) != 0)
                                return -1;
                }
                if (u_puts(1, " ADDEND ") != 0 || put_word(r.addend) != 0 ||
                    u_crlf(1) != 0)
                        return -1;
        }
        return 0;
}

static int
dump_disasm(int fd, const struct nd_obj *o)
{
        kword_t buf[ND_IO_WORDS];
        kword_t pos;
        unsigned int n;
        unsigned int i;
        unsigned int op;
        unsigned int ac;
        unsigned int ind;
        unsigned int xr;
        kword_t lh;
        const char *name;

        if (u_puts(1, "DISASSEMBLY .TEXT:") != 0 || u_crlf(1) != 0 ||
            nd_seek(fd, o->text_off) != 0)
                return -1;
        pos = 0UL;
        while (pos < o->text_words) {
                n = o->text_words - pos > ND_IO_WORDS ? ND_IO_WORDS :
                    (unsigned int)(o->text_words - pos);
                if (nd_read_exact(fd, buf, n) != 0)
                        return -1;
                for (i = 0U; i < n; ++i) {
                        lh = nd_lh(buf[i]);
                        op = (unsigned int)((lh >> 9) & 0777UL);
                        ac = (unsigned int)((lh >> 5) & 017UL);
                        ind = (unsigned int)((lh >> 4) & 1UL);
                        xr = (unsigned int)(lh & 017UL);
                        name = opname(op);
                        if (u_put_octal(1, pos + i, 6U) != 0 ||
                            u_puts(1, ": ") != 0 || put_word(buf[i]) != 0 ||
                            u_putc(1, ' ') != 0)
                                return -1;
                        if (name != 0) {
                                if (u_puts(1, name) != 0)
                                        return -1;
                        } else if (u_puts(1, "OP") != 0 ||
                            u_put_octal(1, op, 3U) != 0)
                                return -1;
                        if (u_putc(1, ' ') != 0 || u_put_octal(1, ac, 1U) != 0 ||
                            u_putc(1, ',') != 0 ||
                            (ind && u_putc(1, '@') != 0) ||
                            u_put_octal(1, nd_rh(buf[i]), 6U) != 0)
                                return -1;
                        if (xr != 0U && (u_putc(1, '(') != 0 ||
                            u_put_octal(1, xr, 1U) != 0 || u_putc(1, ')') != 0))
                                return -1;
                        if (u_crlf(1) != 0)
                                return -1;
                }
                pos += (kword_t)n;
        }
        return 0;
}

static int
usage(void)
{
        (void)u_puts(2, "USAGE: OBJDUMP [-H] [-T] [-R] [-D] OBJECT.DOBJ");
        (void)u_crlf(2);
        return 1;
}

int
main(int argc, kword_t **argv)
{
        struct nd_obj obj;
        int show_h;
        int show_t;
        int show_r;
        int show_d;
        unsigned int i;
        kword_t *path;
        int fd;
        int rc;

        show_h = show_t = show_r = show_d = 0;
        path = 0;
        for (i = 1U; i < (unsigned int)argc; ++i) {
                if (u_s6_eq(argv[i], "-H")) show_h = 1;
                else if (u_s6_eq(argv[i], "-T")) show_t = 1;
                else if (u_s6_eq(argv[i], "-R")) show_r = 1;
                else if (u_s6_eq(argv[i], "-D")) show_d = 1;
                else if (path == 0) path = argv[i];
                else return usage();
        }
        if (path == 0)
                return usage();
        if (!show_h && !show_t && !show_r && !show_d)
                show_h = show_t = show_r = show_d = 1;
        fd = dsys_open(path, SYS_O_RDONLY);
        if (fd < 0 || nd_obj_parse(fd, 0UL, &obj) != 0) {
                if (fd >= 0) (void)dsys_close(fd);
                (void)u_puts(2, "OBJDUMP: NOT A DOBJ1 OBJECT");
                (void)u_crlf(2);
                return 1;
        }
        rc = 0;
        if (show_h && dump_header(&obj) != 0) rc = 1;
        if (!rc && show_t && dump_symbols(fd, &obj) != 0) rc = 1;
        if (!rc && show_r && dump_relocs(fd, &obj) != 0) rc = 1;
        if (!rc && show_d && dump_disasm(fd, &obj) != 0) rc = 1;
        if (dsys_close(fd) != 0) rc = 1;
        return rc;
}
