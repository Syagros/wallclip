#include <errno.h>
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
  int had_errors;
} file_list_t;

void file_list_free(file_list_t *list) {
  if (!list) return;
  for (int i = 0; i < list->count; i++) {
    free(list->paths[i]);
  }
  free(list->paths);
  free(list);
}

int file_list_push(file_list_t *list, const char *path) {
  if (list->count == list->capacity) {
    size_t new_cap = list->capacity * 2;
    char **new_paths = reallocarray(list->paths, new_cap, sizeof(*list->paths));
    if (!new_paths) {
      Errnof(E_FAIL, "reallocarray failed");
      return -1;
    }
    list->paths = new_paths;
    list->capacity = new_cap;
  }

  char *path_copy = strdup(path);
  if (!path_copy) {
    Errnof(E_FAIL, "strdup failed");
    return -1;
  }

  list->paths[list->count++] = path_copy;
  return 0;
}

int main(int argc, char *argv[])
{
  // create file list
  file_list_t *list = malloc(sizeof(*list));
  if (!list) {
    Errnof(E_FAIL, "malloc failed");
    exit(EXIT_FAILURE);
  }
  list->capacity = 16;
  list->count = 0;
  list->had_errors = 0;
  list->paths = reallocarray(NULL, list->capacity, sizeof(*list->paths));
  if (!list->paths) {
    Errnof(E_FAIL, "reallocarray failed");
    free(list);
    exit(EXIT_FAILURE);
  }

  // Parse flags & validate files exist before (and !empty) proceeding.
  for (int i = 1; i < argc; i++) {
    if (argv[i][0] == '-') {
      Errorf(E_FAIL, "unknown option: %s", argv[i]);
      file_list_free(list);
      exit(EXIT_FAILURE);
    }

    struct stat st;
    if (stat(argv[i], &st) != 0) {
      Errnof(E_FAIL, "%s: does not exist", argv[i]);
      file_list_free(list);
      exit(EXIT_FAILURE);
    }
    if (S_ISREG(st.st_mode) && st.st_size == 0) {
      Errnof(E_FAIL, "%s: empty file", argv[i]);
      file_list_free(list);
      exit(EXIT_FAILURE);
    }
    if (S_ISDIR(st.st_mode)) {
      Errnof(E_FAIL, "%s: cannot open directory", argv[i]);
      file_list_free(list);
      exit(EXIT_FAILURE);
    }

    // push list
    if (file_list_push(list, argv[i]) != 0) {
      list->had_errors = 1;
    }
  }
  printf("count=%d capacity=%d had_errors=%d\n",
         list->count, list->capacity, list->had_errors);
  for (int i = 0; i < list->count; i++) {
    printf("  [%d] %s\n", i, list->paths[i]);
  }
  file_list_free(list);
  return 0;
}
