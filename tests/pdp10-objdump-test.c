#include "dobj.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static struct dobj_word instruction(unsigned int op, unsigned int ac,
                                    unsigned long y)
{
    return dobj_word_halves(((unsigned long)(op & 0777U) << 9) |
                            ((unsigned long)(ac & 017U) << 5), y);
}

int main(int argc, char **argv)
{
    struct dobj_object obj;
    FILE *f;

    if (argc != 2)
        return 2;
    memset(&obj, 0, sizeof(obj));
    obj.text_words = 2UL;
    obj.text = (struct dobj_word *)calloc(2U, sizeof(*obj.text));
    if (obj.text == NULL)
        return 1;
    obj.text[0] = instruction(0201U, 1U, 043UL);
    obj.text[1] = instruction(0263U, 017U, 0UL);
    f = fopen(argv[1], "wb");
    if (f == NULL) {
        free(obj.text);
        return 1;
    }
    if (dobj_write(f, &obj) != 0) {
        fclose(f);
        free(obj.text);
        return 1;
    }
    if (fclose(f) != 0) {
        free(obj.text);
        return 1;
    }
    free(obj.text);
    return 0;
}
