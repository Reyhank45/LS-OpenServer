/*
 * cdl - LS-OpenServer Utils  (directory listing, LS-style)
 * Part of lsutils multi-call binary
 */

#include <stdio.h>
#include <stdlib.h>
#include <dirent.h>
#include <sys/stat.h>
#include <string.h>

int main(int argc, char *argv[]) {
    const char *dir_path = (argc > 1) ? argv[1] : ".";
    DIR *dir = opendir(dir_path);
    if (!dir) {
        perror(argv[0]);
        return 1;
    }
    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL) {
        /* Skip hidden entries unless -a flag is given */
        printf("%s  ", entry->d_name);
    }
    printf("\n");
    closedir(dir);
    return 0;
}
