/* mkinitfs0.c - build a hierarchical DAIMON initfs0 image */

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define INITFS0_MAGIC       0051646060ULL
#define INITFS0_VERSION     1ULL
#define INITFS0_HDR_WORDS   8
#define INITFS0_ENT_WORDS   8
#define INITFS0_MAX_ENTRIES 24
#define INITFS0_REG         1
#define INITFS0_DIR         4

#define WORD_MASK    0777777777777ULL
#define MAX_PARTS    16
#define MAX_PART_LEN 64

typedef unsigned long long word_t;

#define ENC_NONE   (-1)
#define ENC_BINARY  0
#define ENC_WORDS   1
#define ENC_ASM     2
#define ENC_ALIAS   3
#define ENC_DXR     4

/* One element of the flattened data section: either a 36-bit word or an
   asm passthrough line (for .asm-encoded entries). */
struct data_item {
	int    is_asm;
	word_t word;
	char  *line; /* strdup'd; only valid when is_asm != 0 */
};

struct entry {
	char  parts[MAX_PARTS][MAX_PART_LEN + 1];
	int   nparts;
	char  alias_parts[MAX_PARTS][MAX_PART_LEN + 1];
	int   nalias_parts;
	char  path[4096];
	int   mode;
	int   type;       /* INITFS0_REG or INITFS0_DIR */
	int   encoding;   /* ENC_* */
	int   flags;

	/* populated by expand_entries / build_image */
	int   parent;          /* 1-based entry index, 0 = root */
	int   name_off;        /* nonet offset in string section */
	int   data_off;        /* word offset in data section */
	int   data_word_count;
	int   ndata_nonets;

	unsigned int     *data_nonets; /* owned; NULL for dir/alias */
	struct data_item *data_items;  /* owned */
	int               ndata_items;
};

static struct entry g_entries[INITFS0_MAX_ENTRIES];
static int          g_nentries;

/* --- error and memory helpers --- */

static void die(const char *fmt, ...)
{
	va_list ap;
	fprintf(stderr, "mkinitfs0: ");
	va_start(ap, fmt);
	vfprintf(stderr, fmt, ap);
	va_end(ap);
	fputc('\n', stderr);
	exit(1);
}

static void *xmalloc(size_t n)
{
	void *p = malloc(n);
	if (!p) { perror("mkinitfs0"); exit(1); }
	return p;
}

static void *xrealloc(void *p, size_t n)
{
	p = realloc(p, n);
	if (!p) { perror("mkinitfs0"); exit(1); }
	return p;
}

/* --- SIXBIT V1 name validation and uppercase --- */

static void sixbit_validate(const char *src, char *dst)
{
	const char *p;
	char *q = dst;
	for (p = src; *p; p++) {
		char ch = *p;
		if (ch >= 'a' && ch <= 'z')
			ch = (char)(ch - ('a' - 'A'));
		if (!((ch >= 'A' && ch <= 'Z') ||
		      (ch >= '0' && ch <= '9') ||
		      ch == '.' || ch == '_' || ch == '-' ||
		      ch == '$' || ch == '%'))
			die("entry name '%s' is not a SIXBIT V1 name", src);
		*q++ = ch;
	}
	*q = '\0';
}

/* --- image-path normalization --- */

static int do_normalize_path(const char *text,
    char parts[][MAX_PART_LEN + 1])
{
	char buf[4096];
	char *p, *start;
	int n = 0;

	if (strlen(text) >= sizeof(buf))
		die("path too long: %.64s", text);
	memcpy(buf, text, strlen(text) + 1U);
	p = buf;
	while (*p == '/') p++;
	if (!*p)
		die("entry path must not be empty or root");

	while (*p) {
		start = p;
		while (*p && *p != '/') p++;
		if (p > start) {
			char saved = *p;
			if (n >= MAX_PARTS)
				die("path too deep: %s", text);
			*p = '\0';
			if (strlen(start) > MAX_PART_LEN)
				die("path component too long: %s", start);
			sixbit_validate(start, parts[n++]);
			*p = saved;
		}
		if (*p) p++;
	}
	if (n == 0)
		die("entry path must not be empty or root");
	return n;
}

/* --- entry lookup by path parts --- */

static int parts_eq(
    const char a[][MAX_PART_LEN + 1], int na,
    const char b[][MAX_PART_LEN + 1], int nb)
{
	int i;
	if (na != nb) return 0;
	for (i = 0; i < na; i++)
		if (strcmp(a[i], b[i]) != 0) return 0;
	return 1;
}

static int find_entry(const char parts[][MAX_PART_LEN + 1], int nparts)
{
	int i;
	for (i = 0; i < g_nentries; i++)
		if (parts_eq(g_entries[i].parts, g_entries[i].nparts,
		             parts, nparts))
			return i;
	return -1;
}

static void fmt_path(const char parts[][MAX_PART_LEN + 1], int nparts,
    char *buf, size_t bufsz)
{
	int i;
	buf[0] = '\0';
	for (i = 0; i < nparts; i++) {
		size_t used = strlen(buf);
		size_t partlen = strlen(parts[i]);
		if (used + partlen + 2 >= bufsz) break;
		buf[used++] = '/';
		memcpy(buf + used, parts[i], partlen + 1U);
	}
}

/* --- parse one CLI spec: image-path:host-path:mode[:encoding] --- */

static void parse_entry_spec(const char *spec, struct entry *e)
{
	char buf[8192];
	char *f[4];
	int n = 0;
	char *p;
	unsigned long m;
	char *endp;
	const char *enc;

	if (strlen(spec) >= sizeof(buf))
		die("entry spec too long");
	memcpy(buf, spec, strlen(spec) + 1U);

	f[n++] = buf;
	for (p = buf; *p; p++) {
		if (*p == ':' && n < 4) {
			*p = '\0';
			f[n++] = p + 1;
		}
	}
	if (n < 3 || n > 4)
		die("bad entry '%s'; expected image-path:host-path:mode[:encoding]",
		    spec);

	memset(e, 0, sizeof(*e));
	e->type = INITFS0_REG;

	enc = (n == 4) ? f[3] : "binary";
	if      (strcmp(enc, "binary") == 0) e->encoding = ENC_BINARY;
	else if (strcmp(enc, "words")  == 0) e->encoding = ENC_WORDS;
	else if (strcmp(enc, "asm")    == 0) e->encoding = ENC_ASM;
	else if (strcmp(enc, "alias")  == 0) e->encoding = ENC_ALIAS;
	else if (strcmp(enc, "dxr")    == 0) e->encoding = ENC_DXR;
	else die("entry '%s' has unsupported encoding '%s'", f[0], enc);

	m = strtoul(f[2], &endp, 8);
	if (*endp != '\0' || endp == f[2])
		die("entry '%s' mode '%s' is not octal", f[0], f[2]);
	e->mode = (int)m;

	e->nparts = do_normalize_path(f[0], e->parts);

	if (e->encoding == ENC_ALIAS) {
		e->nalias_parts = do_normalize_path(f[1], e->alias_parts);
	} else {
		if (strlen(f[1]) >= sizeof(e->path))
			die("host path too long: %s", f[1]);
		memcpy(e->path, f[1], strlen(f[1]) + 1U);
	}
}

/* --- expand entries: insert auto-created parent directories --- */

static void expand_entries(struct entry *raw, int nraw)
{
	int i, d, idx;

	for (i = 0; i < nraw; i++) {
		for (d = 1; d < raw[i].nparts; d++) {
			char pparts[MAX_PARTS][MAX_PART_LEN + 1];
			int j;
			for (j = 0; j < d; j++)
				memcpy(pparts[j], raw[i].parts[j], MAX_PART_LEN + 1);

			idx = find_entry(pparts, d);
			if (idx == -1) {
				struct entry *de;
				if (g_nentries >= INITFS0_MAX_ENTRIES)
					die("image has too many entries; maximum is %d",
					    INITFS0_MAX_ENTRIES);
				de = &g_entries[g_nentries++];
				memset(de, 0, sizeof(*de));
				for (j = 0; j < d; j++)
					memcpy(de->parts[j], pparts[j], MAX_PART_LEN + 1);
				de->nparts = d;
				de->mode = 0555;
				de->type = INITFS0_DIR;
				de->encoding = ENC_NONE;
			} else if (g_entries[idx].type != INITFS0_DIR) {
				char pb[512];
				fmt_path(pparts, d, pb, sizeof(pb));
				die("path '%s' is both file and directory", pb);
			}
		}

		idx = find_entry(raw[i].parts, raw[i].nparts);
		if (idx != -1) {
			char pb[512];
			fmt_path(raw[i].parts, raw[i].nparts, pb, sizeof(pb));
			die("duplicate image path '%s'", pb);
		}
		if (g_nentries >= INITFS0_MAX_ENTRIES)
			die("image has too many entries; maximum is %d",
			    INITFS0_MAX_ENTRIES);
		g_entries[g_nentries++] = raw[i];
	}

	for (i = 0; i < g_nentries; i++) {
		if (g_entries[i].nparts <= 1) {
			g_entries[i].parent = 0;
		} else {
			char pparts[MAX_PARTS][MAX_PART_LEN + 1];
			int j, np = g_entries[i].nparts - 1;
			for (j = 0; j < np; j++)
				memcpy(pparts[j], g_entries[i].parts[j], MAX_PART_LEN + 1);
			idx = find_entry(pparts, np);
			if (idx == -1) die("internal: parent not found");
			g_entries[i].parent = idx + 1;
		}
	}
}

/* --- nonet packing: ceil(n/4) 36-bit words --- */

static word_t *pack_nonets(const unsigned int *nonets, int n, int *nwords_out)
{
	int nw = (n + 3) / 4;
	word_t *words;
	int i, j;
	if (nw == 0) { *nwords_out = 0; return NULL; }
	words = (word_t *)xmalloc((size_t)nw * sizeof(word_t));
	for (i = 0; i < nw; i++) {
		word_t w = 0;
		for (j = 0; j < 4; j++) {
			unsigned int v = (i * 4 + j < n) ? nonets[i * 4 + j] : 0;
			if (v > 0x1FFU) die("nonet out of range: %u", v);
			w = (w << 9) | v;
		}
		words[i] = w;
	}
	*nwords_out = nw;
	return words;
}

/* --- data loaders --- */

static unsigned int *load_binary_nonets(const char *path, int *nout)
{
	FILE *f;
	unsigned char *buf;
	unsigned int *out;
	long sz;
	int i;

	f = fopen(path, "rb");
	if (!f) { perror(path); exit(1); }
	if (fseek(f, 0, SEEK_END) != 0 || (sz = ftell(f)) < 0) {
		perror(path); exit(1);
	}
	rewind(f);
	buf = (unsigned char *)xmalloc((size_t)(sz ? sz : 1));
	if (sz > 0 && (long)fread(buf, 1, (size_t)sz, f) != sz) {
		perror(path); exit(1);
	}
	fclose(f);
	out = (unsigned int *)xmalloc((size_t)(sz ? sz : 1) * sizeof(unsigned int));
	for (i = 0; i < sz; i++) out[i] = buf[i];
	free(buf);
	*nout = (int)sz;
	return out;
}

static unsigned int *load_word_file_nonets(const char *path, int *nout)
{
	FILE *f;
	char line[256];
	unsigned int *out = NULL;
	int n = 0, cap = 0, lineno = 0;

	f = fopen(path, "r");
	if (!f) { perror(path); exit(1); }

	while (fgets(line, sizeof(line), f)) {
		char *p = line;
		char *ep;
		word_t w;
		char *semi, *hash, *end;
		lineno++;

		semi = strchr(p, ';'); if (semi) *semi = '\0';
		hash = strchr(p, '#'); if (hash) *hash = '\0';
		end = p + strlen(p);
		while (end > p && (end[-1] == ' ' || end[-1] == '\t' ||
		                   end[-1] == '\n' || end[-1] == '\r'))
			*--end = '\0';
		while (*p == ' ' || *p == '\t') p++;
		if (!*p) continue;

		w = 0;
		ep = p;
		while (*ep >= '0' && *ep <= '7')
			w = (w << 3) | (word_t)(*ep++ - '0');
		if (*ep != '\0' || ep == p)
			die("%s:%d: word '%s' is not octal", path, lineno, p);
		if (w >= (1ULL << 36))
			die("%s:%d: word '%s' is outside 36 bits", path, lineno, p);

		if (n + 4 > cap) {
			cap = cap ? cap * 2 : 256;
			out = (unsigned int *)xrealloc(out,
			    (size_t)cap * sizeof(unsigned int));
		}
		out[n++] = (unsigned int)((w >> 27) & 0x1FFU);
		out[n++] = (unsigned int)((w >> 18) & 0x1FFU);
		out[n++] = (unsigned int)((w >>  9) & 0x1FFU);
		out[n++] = (unsigned int)( w        & 0x1FFU);
	}
	fclose(f);
	*nout = n;
	return out;
}

/* Read a DXR1/DXR2 executable and return its words as nonets.
   Each DXR container word is a 36-bit PDP-10 word in the low 36 bits of an
   8-byte little-endian value.  Layout: bits 0-17 = rh, bits 18-31 = lh low
   14, bits 32-35 = lh high 4; bits 36-63 must be zero. */
static unsigned int *load_dxr_nonets(const char *path, int *nout)
{
	FILE *f;
	unsigned char *buf;
	long sz;
	int i, nwords;
	word_t *words;
	unsigned int *out;
	word_t dxr_lh;
	int image_words, reloc_words;
	int dxr_flags;

	f = fopen(path, "rb");
	if (!f) { perror(path); exit(1); }
	if (fseek(f, 0, SEEK_END) != 0 || (sz = ftell(f)) < 0) {
		perror(path); exit(1);
	}
	rewind(f);
	if (sz < 16 || sz % 8 != 0)
		die("%s: DXR size is not a whole number of 36-bit container words",
		    path);

	buf = (unsigned char *)xmalloc((size_t)sz);
	if (fread(buf, 1, (size_t)sz, f) != (size_t)sz) { perror(path); exit(1); }
	fclose(f);

	nwords = (int)(sz / 8);
	words = (word_t *)xmalloc((size_t)nwords * sizeof(word_t));
	for (i = 0; i < nwords; i++) {
		unsigned char *p = buf + i * 8;
		unsigned long lo =  (unsigned long)p[0]
		                 | ((unsigned long)p[1] <<  8)
		                 | ((unsigned long)p[2] << 16)
		                 | ((unsigned long)p[3] << 24);
		unsigned long hi =  (unsigned long)p[4]
		                 | ((unsigned long)p[5] <<  8)
		                 | ((unsigned long)p[6] << 16)
		                 | ((unsigned long)p[7] << 24);
		if (hi & ~0xFUL)
			die("%s: DXR word at byte %d has bits outside 36 bits",
			    path, i * 8);
		{
			word_t rh = lo & 0777777UL;
			word_t lh = ((lo >> 18) | (hi << 14)) & 0777777UL;
			words[i] = (lh << 18) | rh;
		}
	}
	free(buf);

	/* DXR has a two-word base header.  The current extension adds one
	 * word containing text_words,,SIXBIT/TX2/.  Length distinguishes the
	 * legacy and extended forms without stealing another BSS flag bit. */
	dxr_lh = ((word_t)('D' - 040) << 12)
	        | ((word_t)('X' - 040) <<  6)
	        |  (word_t)('R' - 040);
	if (((words[0] >> 18) & 0777777ULL) != dxr_lh)
		die("%s: not a DXR executable", path);
	image_words = (int)((words[1] >> 18) & 0777777ULL);
	dxr_flags = (int)(words[1] & 0700000ULL);
	reloc_words = (image_words + 35) / 36;
	if ((dxr_flags & 0100000) != 0) {
		word_t tx2 = ((word_t)('T' - 040) << 12)
		           | ((word_t)('X' - 040) << 6)
		           |  (word_t)('2' - 040);
		word_t text_words;
		if (nwords <= 3 + reloc_words)
			die("%s: compressed DXR payload is empty", path);
		text_words = (words[2] >> 18) & 0777777ULL;
		if ((words[2] & 0777777ULL) != tx2 ||
		    text_words > (word_t)image_words)
			die("%s: compressed DXR requires valid DXR2 metadata", path);
	} else if (nwords == 2 + image_words + reloc_words)
		;
	else if (nwords == 3 + image_words + reloc_words) {
		word_t tx2 = ((word_t)('T' - 040) << 12)
		           | ((word_t)('X' - 040) << 6)
		           |  (word_t)('2' - 040);
		word_t text_words = (words[2] >> 18) & 0777777ULL;
		if ((words[2] & 0777777ULL) != tx2 ||
		    text_words > (word_t)image_words)
			die("%s: invalid DXR2 text metadata", path);
		;
	} else
		die("%s: DXR length is %d words, expected %d or %d",
		    path, nwords, 2 + image_words + reloc_words,
		    3 + image_words + reloc_words);

	out = (unsigned int *)xmalloc((size_t)nwords * 4 * sizeof(unsigned int));
	for (i = 0; i < nwords; i++) {
		out[i*4+0] = (unsigned int)((words[i] >> 27) & 0x1FFU);
		out[i*4+1] = (unsigned int)((words[i] >> 18) & 0x1FFU);
		out[i*4+2] = (unsigned int)((words[i] >>  9) & 0x1FFU);
		out[i*4+3] = (unsigned int)( words[i]        & 0x1FFU);
	}
	free(words);
	*nout = nwords * 4;
	return out;
}

/* Strip trailing whitespace in-place. */
static void rtrim(char *s)
{
	char *end = s + strlen(s);
	while (end > s && (end[-1] == '\n' || end[-1] == '\r' ||
	                   end[-1] == ' '  || end[-1] == '\t'))
		*--end = '\0';
}

/* Parse an asm file; returns data_items (labels + indented instructions)
   and nwords (count of non-label, non-directive lines = word count). */
static void load_asm_file(const char *path,
    struct data_item **items_out, int *nitems_out, int *nwords_out)
{
	FILE *f;
	char line[4096];
	struct data_item *items = NULL;
	int n = 0, cap = 0, nwords = 0;

	f = fopen(path, "r");
	if (!f) { perror(path); exit(1); }

	while (fgets(line, sizeof(line), f)) {
		char *p = line;
		struct data_item it;
		char *semi = strchr(p, ';');
		size_t plen;

		if (semi) *semi = '\0';
		rtrim(p);
		while (*p == ' ' || *p == '\t') p++;
		if (!*p) continue;

		if (strncmp(p, ".text",   5) == 0 ||
		    strncmp(p, ".data",   5) == 0 ||
		    strncmp(p, ".globl ", 7) == 0 ||
		    strncmp(p, ".global ", 8) == 0)
			continue;

		plen = strlen(p);
		memset(&it, 0, sizeof(it));
		it.is_asm = 1;

		if (plen > 0 && p[plen - 1] == ':') {
			/* label */
			it.line = (char *)xmalloc(plen + 1);
			memcpy(it.line, p, plen + 1);
		} else {
			char indented[4096 + 2];
			if (p[0] == '.' && strncmp(p, ".word", 5) != 0)
				die("%s: unsupported asm directive '%s'", path, p);
			indented[0] = '\t';
			memcpy(indented + 1, p, plen + 1);
			it.line = (char *)xmalloc(plen + 2);
			memcpy(it.line, indented, plen + 2);
			nwords++;
		}

		if (n >= cap) {
			cap = cap ? cap * 2 : 64;
			items = (struct data_item *)xrealloc(items,
			    (size_t)cap * sizeof(struct data_item));
		}
		items[n++] = it;
	}
	fclose(f);
	*items_out  = items;
	*nitems_out = n;
	*nwords_out = nwords;
}

/* --- load entry data (fill data_nonets, data_items, counts) --- */

static void load_entry(struct entry *e)
{
	int nw, i;
	word_t *words;

	if (e->type == INITFS0_DIR || e->encoding == ENC_ALIAS) {
		e->data_nonets   = NULL;
		e->ndata_nonets  = 0;
		e->data_items    = NULL;
		e->ndata_items   = 0;
		e->data_word_count = 0;
		return;
	}

	if (e->encoding == ENC_ASM) {
		load_asm_file(e->path, &e->data_items, &e->ndata_items,
		              &e->data_word_count);
		e->ndata_nonets = e->data_word_count * 4;
		e->data_nonets  = (unsigned int *)xmalloc(
		    (size_t)(e->ndata_nonets + 1) * sizeof(unsigned int));
		memset(e->data_nonets, 0,
		    (size_t)e->ndata_nonets * sizeof(unsigned int));
		return;
	}

	if (e->encoding == ENC_WORDS)
		e->data_nonets = load_word_file_nonets(e->path, &e->ndata_nonets);
	else if (e->encoding == ENC_DXR)
		e->data_nonets = load_dxr_nonets(e->path, &e->ndata_nonets);
	else
		e->data_nonets = load_binary_nonets(e->path, &e->ndata_nonets);

	words = pack_nonets(e->data_nonets, e->ndata_nonets, &nw);
	e->data_word_count = nw;
	e->data_items = (struct data_item *)xmalloc(
	    (size_t)(nw ? nw : 1) * sizeof(struct data_item));
	e->ndata_items = nw;
	for (i = 0; i < nw; i++) {
		e->data_items[i].is_asm = 0;
		e->data_items[i].word   = words[i];
		e->data_items[i].line   = NULL;
	}
	free(words);
}

/* --- build image: assign offsets, assemble all sections --- */

static struct data_item *g_data_items;
static int               g_ndata_items;
static int               g_data_item_cap;
static int               g_data_word_count;
static word_t           *g_str_words;
static int               g_nstr_words;
static word_t            g_header[INITFS0_HDR_WORDS];
static word_t            g_entry_words[INITFS0_MAX_ENTRIES * INITFS0_ENT_WORDS];

static void data_push(const struct data_item *it)
{
	if (g_ndata_items >= g_data_item_cap) {
		g_data_item_cap = g_data_item_cap ? g_data_item_cap * 2 : 256;
		g_data_items = (struct data_item *)xrealloc(g_data_items,
		    (size_t)g_data_item_cap * sizeof(struct data_item));
	}
	g_data_items[g_ndata_items++] = *it;
}

static void build_image(void)
{
	unsigned int name_nonets[INITFS0_MAX_ENTRIES * (MAX_PART_LEN + 2)];
	int n_name_nonets = 0;
	int i, j;

	for (i = 0; i < g_nentries; i++) {
		struct entry *e = &g_entries[i];
		const char *name = e->parts[e->nparts - 1];
		e->name_off = n_name_nonets;
		for (j = 0; name[j]; j++) {
			unsigned char c = (unsigned char)name[j];
			if (c > 127) die("entry name '%s' is not ASCII", name);
			name_nonets[n_name_nonets++] = c;
		}
		name_nonets[n_name_nonets++] = 0;
		load_entry(e);
	}

	g_str_words = pack_nonets(name_nonets, n_name_nonets, &g_nstr_words);

	g_ndata_items    = 0;
	g_data_word_count = 0;

	for (i = 0; i < g_nentries; i++) {
		struct entry *e = &g_entries[i];
		if (e->type == INITFS0_DIR || e->encoding == ENC_ALIAS) {
			e->data_off = 0;
			continue;
		}
		e->data_off = g_data_word_count;
		for (j = 0; j < e->ndata_items; j++)
			data_push(&e->data_items[j]);
		g_data_word_count += e->data_word_count;
	}

	/* resolve aliases */
	for (i = 0; i < g_nentries; i++) {
		struct entry *e = &g_entries[i];
		struct entry *tgt;
		int ti;
		if (e->encoding != ENC_ALIAS) continue;
		ti = find_entry(e->alias_parts, e->nalias_parts);
		if (ti < 0) {
			char pb[512];
			fmt_path(e->alias_parts, e->nalias_parts, pb, sizeof(pb));
			die("alias target '%s' is not a file", pb);
		}
		tgt = &g_entries[ti];
		if (tgt->type != INITFS0_REG) {
			char pb[512];
			fmt_path(e->alias_parts, e->nalias_parts, pb, sizeof(pb));
			die("alias target '%s' is not a file", pb);
		}
		if (tgt->encoding == ENC_ALIAS) {
			char pb[512];
			fmt_path(e->alias_parts, e->nalias_parts, pb, sizeof(pb));
			die("alias target '%s' is another alias", pb);
		}
		e->data_off        = tgt->data_off;
		e->data_word_count = tgt->data_word_count;
		e->ndata_nonets    = tgt->ndata_nonets;
	}

	g_header[0] = INITFS0_MAGIC;
	g_header[1] = INITFS0_VERSION;
	g_header[2] = (word_t)g_nentries;
	g_header[3] = INITFS0_ENT_WORDS;
	g_header[4] = (word_t)g_nstr_words;
	g_header[5] = (word_t)g_data_word_count;
	g_header[6] = 0;
	g_header[7] = 0;

	for (i = 0; i < g_nentries; i++) {
		struct entry *e = &g_entries[i];
		word_t *ew = &g_entry_words[i * INITFS0_ENT_WORDS];
		ew[0] = (word_t)e->name_off;
		ew[1] = (word_t)e->type;
		ew[2] = (word_t)e->mode;
		ew[3] = (word_t)e->data_off;
		ew[4] = (word_t)e->data_word_count;
		ew[5] = (word_t)e->ndata_nonets;
		ew[6] = (word_t)e->parent;
		ew[7] = (word_t)e->flags;
	}
}

/* --- output --- */

static void emit_word(FILE *out, word_t w, const char *comment)
{
	if (w > WORD_MASK) die("word out of range");
	if (comment)
		fprintf(out, "\t.word\t0%012llo\t; %s\n", w, comment);
	else
		fprintf(out, "\t.word\t0%012llo\n", w);
}

static void emit_image(FILE *out, const char *label)
{
	int i, pos;
	word_t total;

	fprintf(out, "\t.text\n");
	fprintf(out, "\t.globl\t%s\n", label);
	fprintf(out, "\t.globl\t%s_end\n", label);
	fprintf(out, "%s:\n", label);

	for (i = 0; i < INITFS0_HDR_WORDS; i++) {
		char comment[32];
		snprintf(comment, sizeof(comment), "hdr[%d]", i);
		emit_word(out, g_header[i], comment);
	}

	pos = 0;
	for (i = 0; i < g_nentries; i++) {
		struct entry *e = &g_entries[i];
		char pb[512];
		int f;
		fmt_path(e->parts, e->nparts, pb, sizeof(pb));
		for (f = 0; f < INITFS0_ENT_WORDS; f++) {
			char comment[sizeof(pb) + 16];
			snprintf(comment, sizeof(comment), "%s ent[%d]", pb, f);
			emit_word(out, g_entry_words[pos++], comment);
		}
	}

	fprintf(out, "%s_strings:\n", label);
	for (i = 0; i < g_nstr_words; i++)
		emit_word(out, g_str_words[i], NULL);

	fprintf(out, "%s_data:\n", label);
	for (i = 0; i < g_ndata_items; i++) {
		if (g_data_items[i].is_asm)
			fprintf(out, "%s\n", g_data_items[i].line);
		else
			emit_word(out, g_data_items[i].word, NULL);
	}

	fprintf(out, "%s_end:\n", label);
	total = (word_t)(INITFS0_HDR_WORDS +
	    g_nentries * INITFS0_ENT_WORDS +
	    g_nstr_words + g_data_word_count);
	fprintf(out, "\t; initfs0 words: %llu\n", total);
}

static void emit_words(FILE *out)
{
	int i;
	for (i = 0; i < INITFS0_HDR_WORDS; i++)
		fprintf(out, "%012llo\n", g_header[i]);
	for (i = 0; i < g_nentries * INITFS0_ENT_WORDS; i++)
		fprintf(out, "%012llo\n", g_entry_words[i]);
	for (i = 0; i < g_nstr_words; i++)
		fprintf(out, "%012llo\n", g_str_words[i]);
	for (i = 0; i < g_ndata_items; i++) {
		if (g_data_items[i].is_asm)
			die("raw word output does not support asm-encoded entries");
		fprintf(out, "%012llo\n", g_data_items[i].word);
	}
}

/* --- main --- */

static void usage(void)
{
	fprintf(stderr,
	    "usage: mkinitfs0 [-o output] [--label label] "
	    "[--format asm|words] entry...\n");
	exit(2);
}

int main(int argc, char **argv)
{
	const char *output = NULL;
	const char *label  = "initfs0_image";
	int fmt_words = 0;
	struct entry raw[INITFS0_MAX_ENTRIES];
	int nraw = 0;
	FILE *out;
	int i;

	for (i = 1; i < argc; i++) {
		if ((strcmp(argv[i], "-o") == 0 ||
		     strcmp(argv[i], "--output") == 0) && i + 1 < argc) {
			output = argv[++i];
		} else if (strcmp(argv[i], "--label") == 0 && i + 1 < argc) {
			label = argv[++i];
		} else if (strcmp(argv[i], "--format") == 0 && i + 1 < argc) {
			const char *fmt = argv[++i];
			if      (strcmp(fmt, "asm")   == 0) fmt_words = 0;
			else if (strcmp(fmt, "words") == 0) fmt_words = 1;
			else usage();
		} else if (argv[i][0] == '-') {
			usage();
		} else {
			if (nraw >= INITFS0_MAX_ENTRIES)
				die("too many entries specified");
			parse_entry_spec(argv[i], &raw[nraw++]);
		}
	}

	expand_entries(raw, nraw);
	build_image();

	if (output) {
		out = fopen(output, "w");
		if (!out) { perror(output); return 1; }
	} else {
		out = stdout;
	}

	if (fmt_words)
		emit_words(out);
	else
		emit_image(out, label);

	if (output && fclose(out) != 0) { perror(output); return 1; }
	return 0;
}
