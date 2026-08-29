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

typedef struct {
  size_t width;
  size_t height;
  char *thumb_path;
} MagicResults;

int get_birthtime_linux(const char *path, time_t *created_at) {
  struct statx stx;
  if (statx(AT_FDCWD, path, 0, STATX_BTIME, &stx) != 0) {
    return -1;
  }
  if (!(stx.stx_mask & STATX_BTIME)) {
    printf("Filesystem doesn't support btime.");
    return -1; 
  }
  *created_at = stx.stx_btime.tv_sec;
  return 0;
}

int get_file_times(const char *path, time_t *modified_at, time_t *changed_at, time_t *accessed_at) {
  struct stat st;
  if (stat(path, &st) != 0) {
    return -1;
  }
  *modified_at = st.st_mtime;
  *changed_at  = st.st_ctime;
  *accessed_at = st.st_atime;
  return 0;
}
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

  // Postgresql creds parsing from .env file
  char *pgpass = NULL, *pguser = NULL, *pgdbname = NULL;
  char mime_type_buf[32];
  FILE *envf = fopen(".env", "r");
  char line[512];
  if (!envf) {
    Errorf("E_ERROR", "Failed to open .env file.");
    exit(EXIT_FAILURE);
  }

  while (fgets(line, sizeof(line), envf)) {
    line[strcspn(line, "\r\n")] = '\0';
    char *p = line;
    while (isspace((unsigned char)*p)) p++;
    if (*p == '\0' || *p == '#') continue;

    char *eq = strchr(p, '=');
    if (!eq) continue;
    *eq = '\0';
    char *key = p;
    char *val = eq + 1;

    size_t len = strlen(val);
    if (len >= 2 && (val[0] == '"' || val[0] == '\'') && val[len-1] == val[0]) {
      val[len-1] = '\0';
      val++;
    }

    if (!strcmp(key, "PGPASS"))        pgpass = strdup(val);
    else if (!strcmp(key, "PGUSER"))   pguser = strdup(val);
    else if (!strcmp(key, "PGDBNAME")) pgdbname = strdup(val);
  }
  fclose(envf);
  if (!pgpass || !pguser || !pgdbname) {
    Errorf(E_FAIL, "PGPASS/PGUSER/PGDBNAME missing from .env");
    free(pgpass);
    free(pguser);
    free(pgdbname);
    exit(EXIT_FAILURE);
  }

  // Load CLIP model
  struct clip_ctx * ctx = clip_model_load("/usr/local/src/clip.cpp/models/clip-vit-base-patch32_ggml-model-f32.gguf", 1);
  if (!ctx) {
    Errorf(E_FAIL, "Unable to load model from %s", "/usr/local/src/clip.cpp/models/clip-vit-base-patch32_ggml-model-f32.gguf");
	exit(EXIT_FAILURE);
  }

  int vec_dim = clip_get_vision_hparams(ctx)->projection_dim;

  // Load image from disk
  struct clip_image_u8 * img0 = clip_image_u8_make();
  if (!clip_image_load_from_file(list->paths[i], img0)) {
    Errnof(E_FAIL, "failed to load image from '%s'", list->paths[i]);
    clip_image_u8_free(img0);
    exit(EXIT_FAILURE);
  }

  // Preprocess image
  struct clip_image_f32 * img_res = clip_image_f32_make();
  if (!clip_image_preprocess(ctx, img0, img_res)) {
    Errorf(E_FAIL, "failed to preprocess image");
    clip_image_f32_free(img_res);
    clip_image_u8_free(img0);
    exit(EXIT_FAILURE);
  }

  // Encode image
  float img_vec[vec_dim];
  if (!clip_image_encode(ctx, 4, img_res, img_vec, true)) {
    Errorf(E_FAIL, "failed to encode image");
    clip_image_u8_free(img0);
    clip_image_f32_free(img_res);
    exit(EXIT_FAILURE);
  }

  char *embedding_str = malloc(vec_dim * 16 + 16);
  if (!embedding_str) {
    Errnof(E_CRIT, "malloc failed for embedding_str (%d dims)", vec_dim);
    // Cleanup everything allocated up to this point
    file_list_free(list);
    free(pgpass);
    free(pguser);
    free(pgdbname);
    clip_image_u8_free(img0);
    clip_image_f32_free(img_res);
    clip_free(ctx);
    exit(EXIT_FAILURE);
  }
  char *ep = embedding_str;
  ep += sprintf(ep, "[");
  for (int i = 0; i < vec_dim; i++) {
    ep += sprintf(ep, "%f%s", img_vec[i], (i < vec_dim - 1) ? "," : "");
  }
  sprintf(ep, "]");

  time_t created_at, modified_at, changed_at, accessed_at;
  if (get_file_times(list->paths[i], &modified_at, &changed_at, &accessed_at) != 0) {
    Errorf("E_FAIL", "get_file_times");
    exit(EXIT_FAILURE);
  }

  if (get_birthtime_linux(list->paths[i], &created_at) != 0) {
    Errorf("E_FAIL", "get_birthtime_linux");
    created_at = 0;
  }

  char created_str[32], updated_str[32];
  strftime(created_str, sizeof(created_str), "%Y-%m-%d %H:%M:%S", localtime(&created_at));
  strftime(updated_str, sizeof(updated_str), "%Y-%m-%d %H:%M:%S", localtime(&modified_at));

  char conninfo[256];
  snprintf(conninfo, sizeof(conninfo), "user=%s password=%s dbname=%s", pguser, pgpass, pgdbname);

  PGconn *conn = PQconnectdb(conninfo);
  free(pgpass);
  free(pguser);
  free(pgdbname);
  if (PQstatus(conn) == CONNECTION_BAD) {       
    Errorf(E_FAIL, "Connection to database failed: %s", PQerrorMessage(conn));
    PQfinish(conn);
  }
  else
    int ver = PQserverVersion(conn);

  printf("Server version: %d\n", ver);
  /* printf("count=%d capacity=%d had_errors=%d\n", */
  /*        list->count, list->capacity, list->had_errors); */
  /* for (int i = 0; i < list->count; i++) { */
  /*   printf("  [%d] %s\n", i, list->paths[i]); */
  /* } */
  file_list_free(list);
  return 0;
}
