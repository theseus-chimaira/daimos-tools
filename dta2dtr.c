/*
 * dta2dtr.c - convert a SIMH PDP-6 DECtape image to aap raw mark-track DTR.
 *
 * SIMH stores each 36-bit word as two little-endian 32-bit containers holding
 * 18-bit halves. The aap Type-555 model instead consumes physical tape cells
 * containing three data bits plus one mark-track bit. Short SIMH images are
 * valid for boot tapes; unwritten blocks are emitted as zero data.
 *
 * This is intentionally streaming and keeps only one 36-bit word as payload
 * working storage.
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#define NUM_BLOCKS      01102U
#define WORDS_PER_BLOCK 0200U
#define END_CELLS       2700U

enum {
	TAPE_END_F = 022,
	TAPE_END_R = 055,
	BLOCK_SPACE = 025,
	BLOCK_END_F = 026,
	BLOCK_END_R = 045,
	DATA_SYNC = 032,
	BLOCK_SYNC = 051,
	DATA_END_F = 073,
	DATA_END_R = 010,
	DATA_MARK = 070
};

static void
die(const char *what)
{
	perror(what);
	exit(1);
}

static void
put_cell_word(FILE *out, unsigned int mark, int *check, unsigned int data)
{
	unsigned int i;
	unsigned char cell;

	if (check != NULL) {
		*check ^= (int)(~data & 077U);
		*check ^= (int)(~(data >> 6) & 077U);
		*check ^= (int)(~(data >> 12) & 077U);
	}
	for (i = 0; i < 6; i++) {
		cell = (unsigned char)(((mark & 040U) != 0U) << 3);
		cell |= (unsigned char)((data >> 15) & 07U);
		if (fputc(cell, out) == EOF)
			die("dta2dtr: write");
		mark <<= 1;
		data = (data << 3) & 0777777U;
	}
}

static uint64_t
reverse_block_word(unsigned int block)
{
	uint64_t value;
	unsigned int i;

	value = 0;
	for (i = 0; i < 12; i++)
		value |= (((uint64_t)block >> (i * 3)) & UINT64_C(07))
		    << ((11 - i) * 3);
	return value ^ UINT64_C(0777777777777);
}

static uint64_t
interblock_word(unsigned int block)
{
	if (block < 075U)
		return UINT64_C(0721200220107);
	if (block == 075U)
		return UINT64_C(0577777777777);
	return UINT64_C(0721200233107);
}

static uint64_t
reverse_interblock_word(unsigned int block)
{
	if (block < 073U)
		return UINT64_C(0721200223107);
	return UINT64_C(0721200230107);
}

/*
 * Return 1 for a word, 0 for clean EOF at a word boundary, and -1 for a
 * truncated or invalid eight-byte SIMH word.
 */
static int
read_simh_word(FILE *in, uint64_t *word)
{
	unsigned char b[8];
	size_t n;
	uint32_t left;
	uint32_t right;

	n = fread(b, 1, sizeof(b), in);
	if (n == 0)
		return 0;
	if (n != sizeof(b))
		return -1;
	left = (uint32_t)b[0] | (uint32_t)b[1] << 8 |
	    (uint32_t)b[2] << 16 | (uint32_t)b[3] << 24;
	right = (uint32_t)b[4] | (uint32_t)b[5] << 8 |
	    (uint32_t)b[6] << 16 | (uint32_t)b[7] << 24;
	if ((left & ~0777777U) != 0U || (right & ~0777777U) != 0U)
		return -1;
	*word = ((uint64_t)left << 18) | right;
	return 1;
}

static void
write_pair(FILE *out, unsigned int mark1, unsigned int mark2, uint64_t word,
	int *check)
{
	put_cell_word(out, mark1, check, (unsigned int)((word >> 18) & 0777777U));
	put_cell_word(out, mark2, check, (unsigned int)(word & 0777777U));
}

static int
convert(FILE *in, FILE *out)
{
	uint64_t word;
	uint64_t bm;
	uint64_t ib;
	unsigned int block;
	unsigned int wi;
	unsigned int i;
	int check;
	int state;
	int input_done;

	for (i = 0; i < END_CELLS; i++) {
		put_cell_word(out, TAPE_END_R, NULL, 0);
		put_cell_word(out, TAPE_END_R, NULL, 0);
	}

	input_done = 0;
	for (block = 0; block < NUM_BLOCKS; block++) {
		bm = block;
		write_pair(out, BLOCK_SPACE, BLOCK_END_F, bm, NULL);

		ib = interblock_word(block);
		write_pair(out, DATA_SYNC, DATA_END_R, ib, NULL);
		put_cell_word(out, DATA_END_R, NULL, 0);

		check = 077;
		for (wi = 0; wi < WORDS_PER_BLOCK; wi++) {
			if (!input_done) {
				state = read_simh_word(in, &word);
				if (state < 0) {
					fprintf(stderr,
					    "dta2dtr: invalid SIMH word at block %o word %o\n",
					    block, wi);
					return 1;
				}
				if (state == 0)
					input_done = 1;
			}
			if (input_done)
				word = 0;

			if (wi == 0)
				write_pair(out, DATA_END_R, DATA_END_R, word, &check);
			else if (wi == WORDS_PER_BLOCK - 1)
				write_pair(out, DATA_END_F, DATA_END_F, word, &check);
			else
				write_pair(out, DATA_MARK, DATA_MARK, word, &check);
		}
		put_cell_word(out, DATA_END_F, NULL,
		    ((unsigned int)check & 077U) << 12 | 07777U);

		ib = reverse_interblock_word(block);
		write_pair(out, DATA_END_F, BLOCK_SYNC, ib, NULL);
		bm = reverse_block_word(block);
		write_pair(out, BLOCK_END_R, BLOCK_SPACE, bm, NULL);
	}

	state = read_simh_word(in, &word);
	if (state != 0) {
		fprintf(stderr, "dta2dtr: input contains data beyond block %o\n",
		    NUM_BLOCKS - 1);
		return 1;
	}

	for (i = 0; i < END_CELLS; i++) {
		put_cell_word(out, TAPE_END_F, NULL, 0);
		put_cell_word(out, TAPE_END_F, NULL, 0);
	}
	return ferror(out) ? 1 : 0;
}

static void
usage(void)
{
	fprintf(stderr, "usage: dta2dtr input.dta output.dtr\n");
	exit(2);
}

int
main(int argc, char **argv)
{
	FILE *in;
	FILE *out;
	int rc;

	if (argc != 3)
		usage();
	in = fopen(argv[1], "rb");
	if (in == NULL)
		die(argv[1]);
	out = fopen(argv[2], "wb");
	if (out == NULL)
		die(argv[2]);

	rc = convert(in, out);
	if (fclose(out) == EOF)
		rc = 1;
	if (fclose(in) == EOF)
		rc = 1;
	if (rc != 0)
		(void)remove(argv[2]);
	return rc;
}
