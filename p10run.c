#define _POSIX_C_SOURCE 199309L

/*
 * p10run.c - minimal PDP-6/PDP-10 bare-metal semantic test runner.
 *
 * The runner deliberately knows nothing about assembler symbol resolution or
 * libgcc source structure.  GCC/DAS produce DOBJ objects, dlink resolves them
 * against the canonical DARC libgcc archive and emits DXR plus a link map.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <ctype.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <signal.h>
#include <time.h>
#include <unistd.h>

#define P10RUN_VERSION "p10run-dobj-v1_20260911"
#define PATHSZ 4096
#define MAX_NAME 80
#define MASK18 0777777UL

/* A PDP-10 word is 36 bits, so unsigned long is insufficient on ILP32 hosts.
 * Keep the representation C89-compatible by storing two 18-bit halves. */
struct p10_word {
    unsigned long high;
    unsigned long low;
};

struct string_list {
    char **v;
    int n;
    int cap;
};

struct expectation {
    char name[MAX_NAME];
    struct p10_word value;
};

struct expect_list {
    struct expectation *v;
    int n;
    int cap;
};

struct label {
    char name[MAX_NAME];
    unsigned long value;
};

struct label_list {
    struct label *v;
    int n;
    int cap;
};

struct options {
    const char *machine;
    const char *mode;
    const char *exec_mode;
    const char *workdir;
    const char *name;
    const char *start_text;
    const char *step_limit;
    const char *pc_reg;
    const char *report;
    int timeout;
    int no_default_ini;
    struct string_list gcc_extra;
    struct string_list examine;
    struct string_list set_cmd;
    struct string_list ini;
    struct string_list sources;
    struct expect_list expects;
};

static void die(const char *s)
{
    fprintf(stderr, "p10run: %s\n", s);
    exit(1);
}

static void die_path(const char *s, const char *p)
{
    fprintf(stderr, "p10run: %s: %s\n", s, p);
    exit(1);
}

static char *xstrdup(const char *s)
{
    char *p;
    p = (char *)malloc(strlen(s) + 1U);
    if (p == NULL)
        die("out of memory");
    memcpy(p, s, strlen(s) + 1U);
    return p;
}

static void *xrealloc(void *p, size_t n)
{
    void *q;
    q = realloc(p, n);
    if (q == NULL)
        die("out of memory");
    return q;
}

static int grow_capacity(int cap)
{
    if (cap == 0)
        return 8;
    if (cap > 16384)
        die("too many arguments");
    return cap * 2;
}


static void copy_text(char *out, size_t cap, const char *s)
{
    size_t n;
    n = strlen(s);
    if (n >= cap)
        die("string too long");
    memcpy(out, s, n + 1U);
}

static void cat_text(char *out, size_t cap, const char *s)
{
    size_t n;
    size_t m;
    n = strlen(out);
    m = strlen(s);
    if (n + m >= cap)
        die("string too long");
    memcpy(out + n, s, m + 1U);
}

static void make_path2(char *out, size_t cap, const char *a, const char *b)
{
    size_t n;
    copy_text(out, cap, a);
    n = strlen(out);
    if (n != 0U && out[n - 1U] != '/')
        cat_text(out, cap, "/");
    cat_text(out, cap, b);
}

static int path_exists(const char *p)
{
    struct stat st;
    return stat(p, &st) == 0;
}

static int path_executable(const char *p)
{
    return access(p, X_OK) == 0;
}

static void join_path(char *out, const char *a, const char *b)
{
    make_path2(out, PATHSZ, a, b);
}

static const char *base_name(const char *p)
{
    const char *q;
    q = strrchr(p, '/');
    return q == NULL ? p : q + 1;
}

static void stem_name(char *out, const char *p)
{
    const char *b;
    const char *dot;
    size_t n;
    b = base_name(p);
    dot = strrchr(b, '.');
    n = dot == NULL ? strlen(b) : (size_t)(dot - b);
    if (n >= MAX_NAME)
        n = MAX_NAME - 1U;
    memcpy(out, b, n);
    out[n] = 0;
}

static unsigned long parse_octal(const char *s)
{
    char *end;
    unsigned long v;
    errno = 0;
    v = strtoul(s, &end, 8);
    if (errno != 0 || *s == 0 || *end != 0)
        die("invalid octal value");
    return v;
}

static int parse_p10_word(const char *s, struct p10_word *word)
{
    struct p10_word value;
    const unsigned char *p;
    if (*s == 0)
        return 0;
    value.high = 0UL;
    value.low = 0UL;
    p = (const unsigned char *)s;
    while (*p != 0) {
        unsigned long digit;
        if (*p < (unsigned char)'0' || *p > (unsigned char)'7')
            return 0;
        if ((value.high & 0700000UL) != 0UL)
            return 0;
        digit = (unsigned long)(*p - (unsigned char)'0');
        value.high = ((value.high << 3) | (value.low >> 15)) & MASK18;
        value.low = ((value.low << 3) | digit) & MASK18;
        p++;
    }
    *word = value;
    return 1;
}

static void list_add(struct string_list *l, const char *s)
{
    if (l->n == l->cap) {
        l->cap = grow_capacity(l->cap);
        l->v = (char **)xrealloc(l->v,
                                (size_t)l->cap * sizeof(l->v[0]));
    }
    l->v[l->n++] = xstrdup(s);
}

static void expect_add(struct expect_list *l, const char *s)
{
    const char *eq;
    size_t n;
    if (l->n == l->cap) {
        l->cap = grow_capacity(l->cap);
        l->v = (struct expectation *)xrealloc(
            l->v, (size_t)l->cap * sizeof(l->v[0]));
    }
    eq = strchr(s, '=');
    if (eq == NULL || eq == s)
        die("--expect requires LABEL=OCTAL");
    n = (size_t)(eq - s);
    if (n >= MAX_NAME)
        die("expectation label too long");
    memcpy(l->v[l->n].name, s, n);
    l->v[l->n].name[n] = 0;
    if (!parse_p10_word(eq + 1, &l->v[l->n].value))
        die("invalid 36-bit octal value");
    l->n++;
}

static void list_free(struct string_list *l)
{
    int i;
    for (i = 0; i < l->n; i++)
        free(l->v[i]);
    free(l->v);
}

static void options_free(struct options *o)
{
    list_free(&o->gcc_extra);
    list_free(&o->examine);
    list_free(&o->set_cmd);
    list_free(&o->ini);
    list_free(&o->sources);
    free(o->expects.v);
}

static void usage(void)
{
    fprintf(stderr,
        "usage: p10run [options] source...\n"
        "  --machine pdp6|ka10|ki10|ks10|kl10\n"
        "  --mode rim|deposit\n"
        "  --start OCTAL\n"
        "  --workdir DIR\n"
        "  --name NAME\n"
        "  --exec-mode step|go\n"
        "  --step-limit N\n"
        "  --timeout N\n"
        "  --pc-reg NAME\n"
        "  --expect LABEL=OCTAL\n"
        "  --examine LABEL|ADDRESS\n"
        "  --set COMMAND\n"
        "  --ini FILE\n"
        "  --no-default-ini\n"
        "  --gcc-extra FLAG\n"
        "  --report FILE\n"
        "  --version\n");
}

static const char *need_arg(int argc, char **argv, int *ip)
{
    if (*ip + 1 >= argc)
        die("missing option argument");
    (*ip)++;
    return argv[*ip];
}

static void parse_args(struct options *o, int argc, char **argv)
{
    int i;
    const char *env;
    memset(o, 0, sizeof(*o));
    o->machine = "ka10";
    o->mode = "rim";
    o->exec_mode = "step";
    env = getenv("STEP_LIMIT");
    o->step_limit = env == NULL ? "2000000" : env;
    env = getenv("SIMH_PC_REG");
    o->pc_reg = env == NULL ? "PC" : env;
    env = getenv("TIMEOUT");
    o->timeout = env == NULL ? 120 : atoi(env);
    for (i = 1; i < argc; i++) {
        const char *a;
        a = argv[i];
        if (strcmp(a, "--version") == 0) {
            printf("%s\n", P10RUN_VERSION);
            exit(0);
        } else if (strcmp(a, "--machine") == 0) {
            o->machine = need_arg(argc, argv, &i);
        } else if (strcmp(a, "--mode") == 0) {
            o->mode = need_arg(argc, argv, &i);
        } else if (strcmp(a, "--start") == 0) {
            o->start_text = need_arg(argc, argv, &i);
        } else if (strcmp(a, "--workdir") == 0) {
            o->workdir = need_arg(argc, argv, &i);
        } else if (strcmp(a, "--name") == 0) {
            o->name = need_arg(argc, argv, &i);
        } else if (strcmp(a, "--exec-mode") == 0) {
            o->exec_mode = need_arg(argc, argv, &i);
        } else if (strcmp(a, "--step-limit") == 0) {
            o->step_limit = need_arg(argc, argv, &i);
        } else if (strcmp(a, "--timeout") == 0) {
            o->timeout = atoi(need_arg(argc, argv, &i));
        } else if (strcmp(a, "--pc-reg") == 0) {
            o->pc_reg = need_arg(argc, argv, &i);
        } else if (strcmp(a, "--expect") == 0) {
            expect_add(&o->expects, need_arg(argc, argv, &i));
        } else if (strcmp(a, "--examine") == 0) {
            list_add(&o->examine, need_arg(argc, argv, &i));
        } else if (strcmp(a, "--set") == 0) {
            list_add(&o->set_cmd, need_arg(argc, argv, &i));
        } else if (strcmp(a, "--ini") == 0) {
            list_add(&o->ini, need_arg(argc, argv, &i));
        } else if (strcmp(a, "--no-default-ini") == 0) {
            o->no_default_ini = 1;
        } else if (strcmp(a, "--gcc-extra") == 0) {
            list_add(&o->gcc_extra, need_arg(argc, argv, &i));
        } else if (strcmp(a, "--report") == 0) {
            o->report = need_arg(argc, argv, &i);
        } else if (a[0] == '-') {
            usage();
            die("unknown option");
        } else {
            list_add(&o->sources, a);
        }
    }
    if (o->sources.n == 0)
        die("sources required");
    if (strcmp(o->mode, "rim") != 0 && strcmp(o->mode, "deposit") != 0)
        die("invalid --mode");
    if (strcmp(o->exec_mode, "step") != 0 && strcmp(o->exec_mode, "go") != 0)
        die("invalid --exec-mode");
}

static void resolve_tool(char *out, const char *prefix, const char *envname,
                         const char *installed, const char *local)
{
    const char *e;
    char p[PATHSZ];
    e = getenv(envname);
    if (e != NULL && *e != 0) {
        copy_text(out, PATHSZ, e);
        return;
    }
    join_path(p, prefix, "bin");
    join_path(out, p, installed);
    if (path_executable(out))
        return;
    copy_text(out, PATHSZ, local);
}

static const char *machine_march(const char *m)
{
    if (strcmp(m, "pdp6") == 0) return "166";
    if (strcmp(m, "ka10") == 0) return "ka10";
    if (strcmp(m, "ki10") == 0) return "ki10";
    if (strcmp(m, "ks10") == 0) return "ks10";
    if (strcmp(m, "kl10") == 0) return "kl10";
    die("invalid machine");
    return "";
}

static const char *machine_simh_name(const char *m)
{
    if (strcmp(m, "pdp6") == 0) return "pdp6";
    if (strcmp(m, "ka10") == 0) return "pdp10-ka";
    if (strcmp(m, "ki10") == 0) return "pdp10-ki";
    if (strcmp(m, "ks10") == 0) return "pdp10-ks";
    if (strcmp(m, "kl10") == 0) return "pdp10-kl";
    die("invalid machine");
    return "";
}

static const char *machine_simh_env(const char *m)
{
    if (strcmp(m, "pdp6") == 0) return "SIMH_PDP6";
    if (strcmp(m, "ka10") == 0) return "SIMH_KA";
    if (strcmp(m, "ki10") == 0) return "SIMH_KI";
    if (strcmp(m, "ks10") == 0) return "SIMH_KS";
    return "SIMH_KL";
}

static const char *machine_ini_name(const char *m)
{
    if (strcmp(m, "pdp6") == 0) return "pdp6.ini";
    if (strcmp(m, "ka10") == 0) return "pdp10-ka.ini";
    if (strcmp(m, "ki10") == 0) return "pdp10-ki.ini";
    if (strcmp(m, "ks10") == 0) return "pdp10-ks.ini";
    return "pdp10-kl.ini";
}

static int run_wait(char *const argv[], const char *cwd, const char *log,
                    int timeout)
{
    pid_t pid;
    int status;
    int fd;
    long elapsed_ms;
    struct timespec poll_delay;
    pid = fork();
    if (pid < 0)
        return -1;
    if (pid == 0) {
        if (cwd != NULL && chdir(cwd) != 0)
            _exit(126);
        if (log != NULL) {
            FILE *f;
            f = fopen(log, "w");
            if (f == NULL)
                _exit(126);
            fd = fileno(f);
            if (dup2(fd, 1) < 0 || dup2(fd, 2) < 0)
                _exit(126);
        }
        execv(argv[0], argv);
        _exit(127);
    }
    elapsed_ms = 0;
    poll_delay.tv_sec = 0;
    poll_delay.tv_nsec = 10000000L;
    for (;;) {
        pid_t r;
        r = waitpid(pid, &status, WNOHANG);
        if (r == pid)
            break;
        if (r < 0)
            return -1;
        if (timeout > 0 && elapsed_ms >= (long)timeout * 1000L) {
            kill(pid, SIGKILL);
            waitpid(pid, &status, 0);
            return 124;
        }
        while (nanosleep(&poll_delay, &poll_delay) != 0 && errno == EINTR)
            ;
        poll_delay.tv_sec = 0;
        poll_delay.tv_nsec = 10000000L;
        elapsed_ms += 10L;
    }
    if (WIFEXITED(status))
        return WEXITSTATUS(status);
    if (WIFSIGNALED(status))
        return 128 + WTERMSIG(status);
    return 1;
}

static void mkdir_one(const char *p)
{
    if (mkdir(p, 0777) != 0 && errno != EEXIST)
        die_path("cannot create directory", p);
}

static void object_path(char *out, const char *work, int n, const char *src)
{
    char stem[MAX_NAME];
    stem_name(stem, src);
    {
        char leaf[MAX_NAME + 32];
        if (snprintf(leaf, sizeof(leaf), "p10run-%03d-%s.dobj",
                     n, stem) >= (int)sizeof(leaf))
            die("temporary object name too long");
        make_path2(out, PATHSZ, work, leaf);
    }
}

static void compile_source(const struct options *o, const char *gcc,
                           const char *as, const char *work, int n,
                           const char *src, char *obj)
{
    const char *dot;
    char march[64];
    char **av;
    int ac;
    int i;
    object_path(obj, work, n, src);
    dot = strrchr(src, '.');
    av = (char **)malloc((size_t)(o->gcc_extra.n + 16) * sizeof(av[0]));
    if (av == NULL)
        die("out of memory");
    ac = 0;
    if (dot != NULL && strcmp(dot, ".c") == 0) {
        char asm_path[PATHSZ];
        char stem[MAX_NAME];
        char leaf[MAX_NAME + 32];
        char *asv[6];

        stem_name(stem, src);
        if (snprintf(leaf, sizeof(leaf), "p10run-%03d-%s.s",
                     n, stem) >= (int)sizeof(leaf))
            die("temporary assembly name too long");
        make_path2(asm_path, sizeof(asm_path), work, leaf);
        copy_text(march, sizeof(march), "-march=");
        cat_text(march, sizeof(march), machine_march(o->machine));
        av[ac++] = (char *)gcc;
        av[ac++] = (char *)"-O2";
        av[ac++] = (char *)"-fno-builtin";
        av[ac++] = (char *)"-fno-common";
        av[ac++] = march;
        for (i = 0; i < o->gcc_extra.n; i++) av[ac++] = o->gcc_extra.v[i];
        av[ac++] = (char *)"-S";
        av[ac++] = (char *)src;
        av[ac++] = (char *)"-o";
        av[ac++] = asm_path;
        av[ac] = NULL;
        if (run_wait(av, NULL, NULL, 30) != 0) {
            free(av);
            die_path("compile failed", src);
        }
        asv[0] = (char *)as;
        asv[1] = (char *)"-c";
        asv[2] = (char *)"-o";
        asv[3] = obj;
        asv[4] = asm_path;
        asv[5] = NULL;
        if (run_wait(asv, NULL, NULL, 30) != 0) {
            free(av);
            die_path("assemble failed", src);
        }
        free(av);
        return;
    } else {
        av[ac++] = (char *)as;
        av[ac++] = (char *)"-c";
        av[ac++] = (char *)"-o";
        av[ac++] = obj;
        av[ac++] = (char *)src;
        av[ac] = NULL;
    }
    if (run_wait(av, NULL, NULL, 30) != 0) {
        free(av);
        die_path("compile/assemble failed", src);
    }
    free(av);
}

static void find_libgcc(char *out, const char *gcc, const char *work)
{
    char tmp[PATHSZ];
    char *av[3];
    FILE *f;
    av[0] = (char *)gcc;
    av[1] = (char *)"-print-libgcc-file-name";
    av[2] = NULL;
    make_path2(tmp, sizeof(tmp), work, "p10run-libgcc-path.txt");
    if (run_wait(av, NULL, tmp, 30) != 0)
        die("cannot locate libgcc");
    f = fopen(tmp, "r");
    if (f == NULL || fgets(out, PATHSZ, f) == NULL)
        die("cannot read libgcc path");
    fclose(f);
    out[strcspn(out, "\r\n")] = 0;
    remove(tmp);
    if (!path_exists(out))
        die_path("libgcc archive not found", out);
}

static void link_objects(const char *dlink, const char *work, const char *name,
                         char **objects, int nobj, const char *libc,
                         const char *libgcc, unsigned long base,
                         char *dxr, char *map)
{
    char **av;
    char bopt[32];
    int ac;
    int i;
    {
        char leaf[MAX_NAME + 8];
        copy_text(leaf, sizeof(leaf), name);
        cat_text(leaf, sizeof(leaf), ".dxr");
        make_path2(dxr, PATHSZ, work, leaf);
        copy_text(leaf, sizeof(leaf), name);
        cat_text(leaf, sizeof(leaf), ".map");
        make_path2(map, PATHSZ, work, leaf);
    }
    if (snprintf(bopt, sizeof(bopt), "%lo", base) >= (int)sizeof(bopt))
        die("link base address too long");
    av = (char **)malloc((size_t)(nobj + 11) * sizeof(av[0]));
    if (av == NULL)
        die("out of memory");
    ac = 0;
    av[ac++] = (char *)dlink;
    av[ac++] = (char *)"-b";
    av[ac++] = bopt;
    av[ac++] = (char *)"-o";
    av[ac++] = dxr;
    av[ac++] = (char *)"-M";
    av[ac++] = map;
    for (i = 0; i < nobj; i++)
        av[ac++] = objects[i];
    if (libc != NULL && *libc != 0)
        av[ac++] = (char *)libc;
    av[ac++] = (char *)libgcc;
    av[ac] = NULL;
    if (run_wait(av, NULL, NULL, 30) != 0) {
        free(av);
        die("link failed");
    }
    free(av);
}

static void read_map(const char *path, unsigned long base, struct label_list *l)
{
    FILE *f;
    char line[256];
    char name[MAX_NAME];
    unsigned long v;
    l->n = 0;
    f = fopen(path, "r");
    if (f == NULL)
        die_path("cannot read linker map", path);
    while (fgets(line, sizeof(line), f) != NULL) {
        if (sscanf(line, "%79s %lo", name, &v) != 2)
            continue;
        if (l->n == l->cap) {
            l->cap = grow_capacity(l->cap);
            l->v = (struct label *)xrealloc(
                l->v, (size_t)l->cap * sizeof(l->v[0]));
        }
        memcpy(l->v[l->n].name, name, strlen(name) + 1U);
        l->v[l->n].value = base + v;
        l->n++;
    }
    fclose(f);
}

static int find_label(const struct label_list *l, const char *name,
                      unsigned long *v)
{
    int i;
    for (i = 0; i < l->n; i++) {
        if (strcmp(l->v[i].name, name) == 0) {
            *v = l->v[i].value;
            return 1;
        }
    }
    return 0;
}

static unsigned long dxr_entry(const char *path, unsigned long base)
{
    FILE *f;
    unsigned char b[8];
    unsigned long rh;
    f = fopen(path, "rb");
    if (f == NULL || fread(b, 1U, 8U, f) != 8U)
        die("cannot read DXR header");
    fclose(f);
    rh = (unsigned long)b[0] |
         ((unsigned long)b[1] << 8) |
         (((unsigned long)b[2] & 03UL) << 16);
    rh &= MASK18;
    return base + rh;
}

static void convert_dxr(const char *conv, const char *dxr, const char *out,
                        const char *kind, unsigned long base)
{
    char bopt[32];
    char mode[20];
    char *av[8];
    int ac;
    if (snprintf(bopt, sizeof(bopt), "%lo", base) >= (int)sizeof(bopt))
        die("DXR base address too long");
    copy_text(mode, sizeof(mode), "--");
    cat_text(mode, sizeof(mode), kind);
    ac = 0;
    av[ac++] = (char *)conv;
    av[ac++] = mode;
    av[ac++] = (char *)"-b";
    av[ac++] = bopt;
    if (strcmp(kind, "simh") == 0)
        av[ac++] = (char *)"--no-go";
    av[ac++] = (char *)dxr;
    av[ac++] = (char *)out;
    av[ac] = NULL;
    if (run_wait(av, NULL, NULL, 30) != 0)
        die("dxrconvert failed");
}

static void copy_labels(const char *map, const char *labels,
                        unsigned long base)
{
    FILE *in;
    FILE *out;
    char line[256];
    char name[MAX_NAME];
    unsigned long v;
    in = fopen(map, "r");
    out = fopen(labels, "w");
    if (in == NULL || out == NULL)
        die("cannot write labels");
    while (fgets(line, sizeof(line), in) != NULL) {
        if (sscanf(line, "%79s %lo", name, &v) == 2)
            fprintf(out, "%-32s %06lo\n", name, base + v);
    }
    fclose(in);
    fclose(out);
}

static void add_default_ini(FILE *f, const char *work, const char *tools,
                            const char *prefix, const char *machine)
{
    char p[PATHSZ];
    char q[PATHSZ];
    const char *name;
    name = machine_ini_name(machine);
    join_path(p, work, name);
    if (path_exists(p)) {
        fprintf(f, "echo __P10RUN_INI__ %s\ndo %s\n", p, p);
        return;
    }
    join_path(p, tools, "simh");
    join_path(q, p, name);
    if (path_exists(q)) {
        fprintf(f, "echo __P10RUN_INI__ %s\ndo %s\n", q, q);
        return;
    }
    join_path(p, prefix, "share/pdp10-tools/simh");
    join_path(q, p, name);
    if (path_exists(q))
        fprintf(f, "echo __P10RUN_INI__ %s\ndo %s\n", q, q);
}

static void write_run_script(const struct options *o, const char *path,
                             const char *image, unsigned long entry,
                             const struct label_list *labels,
                             const char *work, const char *tools,
                             const char *prefix)
{
    FILE *f;
    int i;
    f = fopen(path, "w");
    if (f == NULL)
        die_path("cannot write SIMH script", path);
    fprintf(f, "echo __P10RUN_START__ machine=%s mode=%s entry=%lo\n",
            o->machine, o->mode, entry);
    if (!o->no_default_ini)
        add_default_ini(f, work, tools, prefix, o->machine);
    for (i = 0; i < o->ini.n; i++)
        fprintf(f, "echo __P10RUN_INI__ %s\ndo %s\n", o->ini.v[i], o->ini.v[i]);
    for (i = 0; i < o->set_cmd.n; i++)
        fprintf(f, "%s\n", o->set_cmd.v[i]);
    if (strcmp(o->mode, "rim") == 0)
        fprintf(f, "load %s\n", image);
    else
        fprintf(f, "do %s\n", image);
    fprintf(f, "echo __P10RUN_LOW_CORE_BREAK__ 20\nbreak 20\n");
    if (strcmp(o->exec_mode, "step") == 0) {
        fprintf(f, "echo __P10RUN_RUN__ STEP %s PC=%lo\n", o->step_limit, entry);
        fprintf(f, "deposit %s %lo\nexamine %s\nstep %s\n",
                o->pc_reg, entry, o->pc_reg, o->step_limit);
    } else {
        fprintf(f, "echo __P10RUN_RUN__ GO %lo\ngo %lo\n", entry, entry);
    }
    fprintf(f, "echo __P10RUN_AFTER_RUN__\nexamine %s\n", o->pc_reg);
    for (i = 0; i < o->expects.n; i++) {
        unsigned long addr;
        if (!find_label(labels, o->expects.v[i].name, &addr))
            die_path("expected label not found", o->expects.v[i].name);
        fprintf(f, "echo __P10RUN_EXPECT__ %s %lo %06lo%06lo\nexamine %lo\n",
                o->expects.v[i].name, addr, o->expects.v[i].value.high,
                o->expects.v[i].value.low, addr);
    }
    for (i = 0; i < o->examine.n; i++) {
        unsigned long addr;
        if (find_label(labels, o->examine.v[i], &addr))
            fprintf(f, "echo __P10RUN_EXAMINE__ %s %lo\nexamine %lo\n",
                    o->examine.v[i], addr, addr);
        else
            fprintf(f, "echo __P10RUN_EXAMINE__ %s\nexamine %s\n",
                    o->examine.v[i], o->examine.v[i]);
    }
    fprintf(f, "quit\n");
    if (fclose(f) != 0)
        die("cannot close SIMH script");
}

static char *read_file(const char *path)
{
    FILE *f;
    long n;
    char *p;
    f = fopen(path, "rb");
    if (f == NULL)
        return NULL;
    if (fseek(f, 0L, SEEK_END) != 0) { fclose(f); return NULL; }
    n = ftell(f);
    if (n < 0L || fseek(f, 0L, SEEK_SET) != 0) { fclose(f); return NULL; }
    p = (char *)malloc((size_t)n + 1U);
    if (p == NULL) { fclose(f); return NULL; }
    if (n != 0L && fread(p, 1U, (size_t)n, f) != (size_t)n) {
        free(p); fclose(f); return NULL;
    }
    p[n] = 0;
    fclose(f);
    return p;
}

static int contains_ci(const char *s, const char *needle)
{
    size_t n;
    n = strlen(needle);
    while (*s) {
        size_t i;
        for (i = 0U; i < n; i++) {
            if (s[i] == 0 || tolower((unsigned char)s[i]) !=
                tolower((unsigned char)needle[i]))
                break;
        }
        if (i == n)
            return 1;
        s++;
    }
    return 0;
}

static int parse_examine_after(const char *text, const char *marker,
                               const char *name, struct p10_word *got)
{
    char key[160];
    const char *p;
    const char *end;
    copy_text(key, sizeof(key), marker);
    cat_text(key, sizeof(key), " ");
    cat_text(key, sizeof(key), name);
    cat_text(key, sizeof(key), " ");
    p = strstr(text, key);
    if (p == NULL)
        return 0;
    end = strchr(p, '\n');
    if (end == NULL)
        return 0;
    p = end + 1;
    while (*p != 0) {
        char line[256];
        size_t n;
        char *tok;
        struct p10_word last;
        int have;
        end = strchr(p, '\n');
        n = end == NULL ? strlen(p) : (size_t)(end - p);
        if (n >= sizeof(line)) n = sizeof(line) - 1U;
        memcpy(line, p, n); line[n] = 0;
        last.high = 0UL; last.low = 0UL; have = 0;
        tok = strtok(line, " \t:=");
        while (tok != NULL) {
            struct p10_word value;
            if (parse_p10_word(tok, &value)) { last = value; have = 1; }
            tok = strtok(NULL, " \t:=");
        }
        if (have) { *got = last; return 1; }
        if (end == NULL) break;
        p = end + 1;
    }
    return 0;
}

static int evaluate(const struct options *o, const char *text)
{
    static const char *bad[] = {
        "Format error", "Non-existent memory", "Unknown command",
        "Invalid argument", "Command not allowed", "Illegal instruction"
    };
    int failures;
    int i;
    failures = 0;
    for (i = 0; i < (int)(sizeof(bad) / sizeof(bad[0])); i++) {
        if (contains_ci(text, bad[i])) {
            fprintf(stderr, "FAIL: SIMH output contains: %s\n", bad[i]);
            failures++;
        }
    }
    if (strstr(text, "Breakpoint, PC: 000020") != NULL) {
        fprintf(stderr, "FAIL: low-core break reached PC 000020\n");
        failures++;
    }
    for (i = 0; i < o->expects.n; i++) {
        struct p10_word got;
        if (!parse_examine_after(text, "__P10RUN_EXPECT__",
                                 o->expects.v[i].name, &got)) {
            fprintf(stderr, "FAIL: %s: no readable examine output\n",
                    o->expects.v[i].name);
            failures++;
        } else if (got.high != o->expects.v[i].value.high ||
                   got.low != o->expects.v[i].value.low) {
            fprintf(stderr,
                    "FAIL: %s: got %06lo%06lo expected %06lo%06lo\n",
                    o->expects.v[i].name, got.high, got.low,
                    o->expects.v[i].value.high, o->expects.v[i].value.low);
            failures++;
        }
    }
    return failures;
}

static void write_report(const char *path, const struct options *o,
                         const char *simh, const char *gcc, const char *as,
                         const char *dlink, const char *libgcc,
                         unsigned long start, unsigned long entry,
                         int simh_rc, int failures, const char *output)
{
    FILE *f;
    f = fopen(path, "w");
    if (f == NULL)
        return;
    fprintf(f, "========== p10run report ==========\n");
    fprintf(f, "version=%s\n", P10RUN_VERSION);
    fprintf(f, "machine=%s\nmode=%s\nexec_mode=%s\n", o->machine, o->mode, o->exec_mode);
    fprintf(f, "start=%lo\nentry=%lo\ntimeout=%d\n", start, entry, o->timeout);
    fprintf(f, "gcc=%s\nassembler=%s\ndlink=%s\nlibgcc=%s\nsimh=%s\n",
            gcc, as, dlink, libgcc, simh);
    fprintf(f, "rc=%d\nresult=%s\n", simh_rc,
            simh_rc == 0 && failures == 0 ? "PASS" : "FAIL");
    fprintf(f, "\n========== simh output ==========\n%s", output == NULL ? "" : output);
    fclose(f);
}

int main(int argc, char **argv)
{
    struct options o;
    struct label_list labels;
    const char *prefix;
    char cwd[PATHSZ];
    char tools[PATHSZ];
    char work[PATHSZ];
    char name[MAX_NAME];
    char gcc[PATHSZ], as[PATHSZ], dlink[PATHSZ], conv[PATHSZ], simh[PATHSZ];
    char libc[PATHSZ], libgcc[PATHSZ];
    char **objects;
    char dxr[PATHSZ], map[PATHSZ], image[PATHSZ], runini[PATHSZ], log[PATHSZ];
    char labels_path[PATHSZ], report[PATHSZ];
    char local[PATHSZ], envsimh[PATHSZ];
    unsigned long start;
    unsigned long entry;
    int i;
    int simh_rc;
    int failures;
    char *output;
    char *simhav[3];

    objects = NULL;
    memset(&labels, 0, sizeof(labels));
    parse_args(&o, argc, argv);
    prefix = getenv("PDP10_PREFIX");
    if (prefix == NULL || *prefix == 0)
        die("PDP10_PREFIX must be set");
    if (getcwd(cwd, sizeof(cwd)) == NULL)
        die("cannot determine current directory");
    copy_text(tools, sizeof(tools), argv[0]);
    {
        char *slash;
        slash = strrchr(tools, '/');
        if (slash == NULL) copy_text(tools, sizeof(tools), ".");
        else if (slash == tools) slash[1] = 0;
        else *slash = 0;
    }
    copy_text(work, sizeof(work), o.workdir == NULL ? cwd : o.workdir);
    mkdir_one(work);
    if (o.name != NULL) copy_text(name, sizeof(name), o.name);
    else stem_name(name, o.sources.v[0]);
    start = parse_octal(o.start_text == NULL ?
                        (strcmp(o.machine, "kl10") == 0 ? "500" : "1000") :
                        o.start_text);

    join_path(local, tools, "pdp10-dec-none-gcc");
    resolve_tool(gcc, prefix, "PDP10_GCC", "pdp10-dec-none-gcc", local);
    join_path(local, tools, "pdp10-dec-none-as");
    resolve_tool(as, prefix, "PDP10_AS", "pdp10-dec-none-as", local);
    join_path(local, tools, "dlink");
    resolve_tool(dlink, prefix, "PDP10_DLINK", "dlink", local);
    join_path(local, tools, "dxrconvert");
    resolve_tool(conv, prefix, "DXRCONVERT", "dxrconvert", local);
    join_path(local, tools, machine_simh_name(o.machine));
    copy_text(envsimh, sizeof(envsimh), machine_simh_env(o.machine));
    resolve_tool(simh, prefix, envsimh, machine_simh_name(o.machine), local);
    if (!path_executable(gcc)) die_path("missing gcc", gcc);
    if (!path_executable(as)) die_path("missing assembler", as);
    if (!path_executable(dlink)) die_path("missing dlink", dlink);
    if (!path_executable(conv)) die_path("missing dxrconvert", conv);
    if (!path_executable(simh)) die_path("missing SIMH", simh);

    objects = (char **)malloc((size_t)o.sources.n * sizeof(objects[0]));
    if (objects == NULL)
        die("out of memory");
    for (i = 0; i < o.sources.n; i++) {
        char obj[PATHSZ];
        if (!path_exists(o.sources.v[i]))
            die_path("source not found", o.sources.v[i]);
        compile_source(&o, gcc, as, work, i + 1, o.sources.v[i], obj);
        objects[i] = xstrdup(obj);
    }
    join_path(libc, prefix, "lib/libc.a");
    if (!path_exists(libc)) libc[0] = 0;
    find_libgcc(libgcc, gcc, work);
    /*
     * Link at the actual load address.  A zero-based DXR relocation bitmap
     * records right-half relocations only, so post-link relocation cannot
     * correctly adjust packed LH18 references such as symbol,,symbol.
     * Fix both halves at link time and use DXR conversion only to place the
     * already-linked image at START.
     */
    link_objects(dlink, work, name, objects, o.sources.n, libc, libgcc,
                 start, dxr, map);
    read_map(map, 0UL, &labels);
    entry = dxr_entry(dxr, start);
    { char leaf[MAX_NAME + 20]; copy_text(leaf, sizeof(leaf), name); cat_text(leaf, sizeof(leaf), ".labels"); make_path2(labels_path, sizeof(labels_path), work, leaf); }
    copy_labels(map, labels_path, 0UL);
    if (strcmp(o.mode, "rim") == 0) {
        { char leaf[MAX_NAME + 8]; copy_text(leaf, sizeof(leaf), name); cat_text(leaf, sizeof(leaf), ".rim"); make_path2(image, sizeof(image), work, leaf); }
        convert_dxr(conv, dxr, image, "rim", start);
    } else {
        { char leaf[MAX_NAME + 8]; copy_text(leaf, sizeof(leaf), name); cat_text(leaf, sizeof(leaf), ".simh"); make_path2(image, sizeof(image), work, leaf); }
        convert_dxr(conv, dxr, image, "simh", start);
    }
    { char leaf[MAX_NAME + 20]; copy_text(leaf, sizeof(leaf), name); cat_text(leaf, sizeof(leaf), "-run.ini"); make_path2(runini, sizeof(runini), work, leaf); }
    { char leaf[MAX_NAME + 20]; copy_text(leaf, sizeof(leaf), name); cat_text(leaf, sizeof(leaf), ".out"); make_path2(log, sizeof(log), work, leaf); }
    { char leaf[MAX_NAME + 20]; copy_text(leaf, sizeof(leaf), name); cat_text(leaf, sizeof(leaf), ".report.txt"); make_path2(report, sizeof(report), work, leaf); }
    if (o.report != NULL) copy_text(report, sizeof(report), o.report);
    write_run_script(&o, runini, image, entry, &labels, work, tools, prefix);

    simhav[0] = simh;
    simhav[1] = runini;
    simhav[2] = NULL;
    simh_rc = run_wait(simhav, cwd, log, o.timeout);
    output = read_file(log);
    if (output == NULL) output = xstrdup("");
    failures = simh_rc == 0 ? 0 : 1;
    if (simh_rc != 0)
        fprintf(stderr, "FAIL: SIMH exited with status %d\n", simh_rc);
    failures += evaluate(&o, output);
    write_report(report, &o, simh, gcc, as, dlink, libgcc, start, entry,
                 simh_rc, failures, output);
    free(output);
    for (i = 0; i < o.sources.n; i++)
        free(objects[i]);
    free(objects);
    free(labels.v);
    options_free(&o);
    if (failures != 0) {
        fprintf(stderr, "see report: %s\n", report);
        return 1;
    }
    printf("PASS %s on %s\n", name, o.machine);
    return 0;
}
