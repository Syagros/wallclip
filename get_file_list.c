#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <dirent.h>
#include "../../../logging/log.h"

typedef struct {
  char **paths;
  int count;
  int capacity;
} file_list_t;

int main(int argc, char *argv[])
{
  int recursive = 0;

  // Parse flags & validate files/dir exist before (and !empty) proceeding. 
  for (int i = 1; i < argc; i++) {
    if (strcmp(argv[i], "-R") == 0 || strcmp(argv[i], "--recursive") == 0) {
      recursive = 1;
      continue;
    }

    if (argv[i][0] == '-') {
      Errorf(E_FAIL, "unknown option: %s", argv[i]);
      exit(EXIT_FAILURE);
    }

    struct stat st;
    if (stat(argv[i], &st) != 0) {
      Errnof(E_FAIL, "%s: does not exist", argv[i]);
      exit(EXIT_FAILURE);
    }
    if (S_ISREG(st.st_mode) && st.st_size == 0) {
      Errnof(E_FAIL, "%s: empty file", argv[i]);
      exit(EXIT_FAILURE);
    }

    if (S_ISDIR(st.st_mode)) {
      DIR *dir = opendir(argv[i]);

      if (!dir) {
        Errnof(E_FAIL, "%s: cannot open directory", argv[i]);
        exit(EXIT_FAILURE);
      }

      int count = 0;
      struct dirent *entry;
      while ((entry = readdir(dir)) != NULL) {
        if (++count > 2) break; // more than "." and ".."
      }
      closedir(dir);
      if (count <= 2) {
        Errnof(E_FAIL, "%s: empty directory", argv[i]);
        exit(EXIT_FAILURE);
      }
    }
  }
  return 0;
}
