/*
 * cdir - LS-OpenServer Utils  (create directory)
 * Part of lsutils multi-call binary
 */

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

/* Recursively create directories, like mkdir -p */
static int mkdir_p(const char *path, mode_t mode) {
  char tmp[4096];
  char *p = NULL;
  size_t len;

  snprintf(tmp, sizeof(tmp), "%s", path);
  len = strlen(tmp);
  if (len > 0 && tmp[len - 1] == '/')
    tmp[len - 1] = '\0';

  for (p = tmp + 1; *p; p++) {
    if (*p == '/') {
      *p = '\0';
      if (mkdir(tmp, mode) != 0 && errno != EEXIST)
        return -1;
      *p = '/';
    }
  }
  if (mkdir(tmp, mode) != 0 && errno != EEXIST)
    return -1;
  return 0;
}

int main(int argc, char *argv[]) {
  int pflag = 0;
  int start = 1;

  if (argc > 1 && strcmp(argv[1], "-p") == 0) {
    pflag = 1;
    start = 2;
  }

  if (argc <= start) {
    fprintf(stderr, "Usage: cdir [-p] <dir> [dir ...]\n");
    return 1;
  }

  int ret = 0;
  for (int i = start; i < argc; i++) {
    int r;
    if (pflag) {
      r = mkdir_p(argv[i], 0777);
    } else {
      r = mkdir(argv[i], 0777);
    }
    if (r != 0) {
      perror(argv[i]);
      ret = 1;
    }
  }
  return ret;
}
