#include "dobj.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void usage(void)
{
    fprintf(stderr, "usage: darc -o archive.darc object.dobj ...\n");
}

int main(int argc, char **argv)
{
    const char *outname;
    struct dobj_object *objects;
    FILE *out;
    int first;
    int i;
    int n;
    int rc;

    outname = NULL;
    first = 1;
    if (argc > 2 && strcmp(argv[1], "-o") == 0) {
        outname = argv[2];
        first = 3;
    }
    if (outname == NULL || first >= argc) {
        usage();
        return 1;
    }
    n = argc - first;
    objects = (struct dobj_object *)calloc((size_t)n, sizeof(*objects));
    if (objects == NULL) {
        fprintf(stderr, "darc: out of memory\n");
        return 1;
    }
    rc = 1;
    for (i = 0; i < n; i++) {
        FILE *f;
        f = fopen(argv[first + i], "rb");
        if (f == NULL) {
            perror(argv[first + i]);
            goto done;
        }
        if (dobj_read(f, &objects[i]) != 0) {
            fprintf(stderr, "darc: invalid DOBJ: %s\n", argv[first + i]);
            fclose(f);
            goto done;
        }
        fclose(f);
    }
    out = fopen(outname, "wb");
    if (out == NULL) {
        perror(outname);
        goto done;
    }
    if (dobj_archive_write(out, objects, (unsigned long)n) != 0) {
        fprintf(stderr, "darc: cannot write archive\n");
        fclose(out);
        remove(outname);
        goto done;
    }
    if (fclose(out) != 0) {
        perror(outname);
        remove(outname);
        goto done;
    }
    rc = 0;
done:
    for (i = 0; i < n; i++)
        dobj_free(&objects[i]);
    free(objects);
    return rc;
}
