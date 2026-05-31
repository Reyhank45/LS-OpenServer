/*
 * cat - LS-OpenServer Utils
 * Part of lsutils multi-call binary
 */

#include <stdio.h>
#include <stdlib.h>

int main(int argc, char *argv[]) {
    if (argc < 2) {
        fprintf(stderr, "Usage: cat <file> [file ...]\n");
        return 1;
    }
    for (int i = 1; i < argc; i++) {
        FILE *f = fopen(argv[i], "r");
        if (!f) {
            perror(argv[i]);
            return 1;
        }
        char buf[4096];
        size_t n;
        while ((n = fread(buf, 1, sizeof(buf), f)) > 0) {
            fwrite(buf, 1, n, stdout);
        }
        if (ferror(f)) {
            perror(argv[i]);
            fclose(f);
            return 1;
        }
        fclose(f);
    }
    return 0;
}
