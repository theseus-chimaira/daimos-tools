#include "dobj_native.h"

#define DARC_MAX_INPUTS 16U

struct darc_input {
        kword_t path[U_PATH_WORDS];
        struct nd_obj obj;
        kword_t member_off;
};

struct index_ctx {
        int outfd;
        kword_t member_off;
        kword_t count;
        kword_t words;
};

static struct darc_input inputs[DARC_MAX_INPUTS];

static int
copy_s6(kword_t *dst, const kword_t *src)
{
        unsigned int i;
        unsigned int n;
        n = (unsigned int)(src[0] & 0777777UL);
        if (1U + (n + 5U) / 6U > U_PATH_WORDS)
                return -1;
        for (i = 0U; i < U_PATH_WORDS; ++i)
                dst[i] = 0UL;
        for (i = 0U; i < 1U + (n + 5U) / 6U; ++i)
                dst[i] = src[i];
        return 0;
}

static int
count_def(unsigned int index, const struct nd_symbol *sym, void *arg)
{
        struct index_ctx *ctx;
        unsigned int len;
        (void)index;
        ctx = (struct index_ctx *)arg;
        if (sym->kind != ND_SYM_DEF)
                return 0;
        len = nd_strlen(sym->name);
        ctx->count++;
        ctx->words += 1UL + (kword_t)nd_name_words(len);
        return 0;
}

static int
write_def(unsigned int index, const struct nd_symbol *sym, void *arg)
{
        struct index_ctx *ctx;
        kword_t h;
        unsigned int len;
        (void)index;
        ctx = (struct index_ctx *)arg;
        if (sym->kind != ND_SYM_DEF)
                return 0;
        len = nd_strlen(sym->name);
        h = nd_halves(ctx->member_off, (kword_t)len);
        if (nd_write_exact(ctx->outfd, &h, 1U) != 0 ||
            nd_write_name(ctx->outfd, sym->name) != 0)
                return -1;
        return 0;
}

static int
usage(void)
{
        (void)u_puts(2, "USAGE: DARC -O ARCHIVE.DARC OBJECT.DOBJ ...");
        (void)u_crlf(2);
        return 1;
}

int
main(int argc, kword_t **argv)
{
        struct index_ctx ctx;
        kword_t header[2];
        kword_t member_header;
        kword_t member_base;
        kword_t index_count;
        kword_t index_words;
        kword_t total_members;
        unsigned int first;
        unsigned int count;
        unsigned int i;
        int fd;
        int outfd;

        if (argc < 4 || !u_s6_eq(argv[1], "-O"))
                return usage();
        first = 3U;
        count = (unsigned int)argc - first;
        if (count == 0U || count > DARC_MAX_INPUTS)
                return usage();
        index_count = 0UL;
        index_words = 0UL;
        for (i = 0U; i < count; ++i) {
                if (copy_s6(inputs[i].path, argv[first + i]) != 0)
                        return 1;
                fd = dsys_open(inputs[i].path, SYS_O_RDONLY);
                if (fd < 0 || nd_obj_parse(fd, 0UL, &inputs[i].obj) != 0) {
                        if (fd >= 0) (void)dsys_close(fd);
                        (void)u_puts(2, "DARC: INVALID DOBJ: ");
                        (void)u_put_s6(2, inputs[i].path);
                        (void)u_crlf(2);
                        return 1;
                }
                ctx.count = 0UL;
                ctx.words = 0UL;
                if (nd_obj_each_symbol(fd, &inputs[i].obj,
                    count_def, &ctx) != 0 || dsys_close(fd) != 0)
                        return 1;
                index_count += ctx.count;
                index_words += ctx.words;
        }
        total_members = (kword_t)count;
        member_base = 2UL + index_words;
        for (i = 0U; i < count; ++i) {
                inputs[i].member_off = member_base;
                member_base += 1UL + inputs[i].obj.object_words;
        }
        outfd = dsys_open(argv[2], SYS_O_WRONLY | SYS_O_CREAT | SYS_O_TRUNC);
        if (outfd < 0)
                return 1;
        header[0] = nd_magic("DARC1 ");
        header[1] = nd_halves(total_members, index_count);
        if (nd_write_exact(outfd, header, 2U) != 0)
                goto bad;
        for (i = 0U; i < count; ++i) {
                fd = dsys_open(inputs[i].path, SYS_O_RDONLY);
                if (fd < 0)
                        goto bad;
                ctx.outfd = outfd;
                ctx.member_off = inputs[i].member_off;
                if (nd_obj_each_symbol(fd, &inputs[i].obj,
                    write_def, &ctx) != 0 || dsys_close(fd) != 0)
                        goto bad;
        }
        for (i = 0U; i < count; ++i) {
                member_header = nd_halves(0UL, inputs[i].obj.object_words);
                if (nd_write_exact(outfd, &member_header, 1U) != 0)
                        goto bad;
                fd = dsys_open(inputs[i].path, SYS_O_RDONLY);
                if (fd < 0 || nd_copy_words(fd, 0UL, outfd,
                    inputs[i].obj.object_words) != 0) {
                        if (fd >= 0) (void)dsys_close(fd);
                        goto bad;
                }
                if (dsys_close(fd) != 0)
                        goto bad;
        }
        if (dsys_close(outfd) != 0)
                return 1;
        return 0;
bad:
        (void)dsys_close(outfd);
        (void)dsys_unlink(argv[2]);
        (void)u_puts(2, "DARC: ARCHIVE WRITE FAILED");
        (void)u_crlf(2);
        return 1;
}
