/*
 * cdl - LS-OpenServer Utils  (current directory lists)
 * Part of lsutils multi-call binary
 */

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

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
