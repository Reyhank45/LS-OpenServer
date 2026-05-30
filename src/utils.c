#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <dirent.h>
#include <sys/stat.h>
#include <libgen.h>

void do_ls(int argc, char *argv[]) {
    const char *dir_path = (argc > 1) ? argv[1] : ".";
    DIR *dir = opendir(dir_path);
    if (!dir) {
        perror("ls");
        return;
    }
    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL) {
        printf("%s  ", entry->d_name);
    }
    printf("\n");
    closedir(dir);
}

void do_cat(int argc, char *argv[]) {
    if (argc < 2) {
        printf("Usage: cat <file>\n");
        return;
    }
    FILE *f = fopen(argv[1], "r");
    if (!f) {
        perror("cat");
        return;
    }
    char buf[1024];
    while (fgets(buf, sizeof(buf), f)) {
        printf("%s", buf);
    }
    fclose(f);
}

void do_mkdir(int argc, char *argv[]) {
    if (argc < 2) {
        printf("Usage: mkdir <dir>\n");
        return;
    }
    if (mkdir(argv[1], 0777) != 0) {
        perror("mkdir");
    }
}

void do_echo(int argc, char *argv[]) {
    for (int i = 1; i < argc; i++) {
        printf("%s ", argv[i]);
    }
    printf("\n");
}

int main(int argc, char *argv[]) {
    char *prog_name = basename(argv[0]);
    
    if (strcmp(prog_name, "cdl") == 0) {
        do_ls(argc, argv);
    } else if (strcmp(prog_name, "cat") == 0) {
        do_cat(argc, argv);
    } else if (strcmp(prog_name, "mkdir") == 0) {
        do_mkdir(argc, argv);
    } else if (strcmp(prog_name, "echo") == 0) {
        do_echo(argc, argv);
    } else {
        printf("lsutils: multi-call binary\n");
        printf("Available commands: cdl, cat, mkdir, echo\n");
    }
    return 0;
}
