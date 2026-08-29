#define _GNU_SOURCE // needed for statx()
#include <clip.h>
#include <ctype.h>
#include <fcntl.h>
#include <errno.h>
#include <inttypes.h>
#include <libgen.h>
#include <libpq-fe.h>
#include <magic.h>
#include <MagickWand/MagickWand.h>
#include <openssl/evp.h>
#include <sys/stat.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#include "../logging/log.h"

typedef struct {
  char **paths;
  int count;
  int capacity;
  int had_errors;
} file_list_t;

typedef struct {
  const char *path;
  struct clip_ctx *clip_ctx;
  PGconn *conn;
} image_ctx_t;

int process_img(image_ctx_t *img) {
  int vec_dim = clip_get_vision_hparams(img->clip_ctx)->projection_dim;

  // Load image from disk
  struct clip_image_u8 *img0 = clip_image_u8_make();
  if (!clip_image_load_from_file(img->path, img0)) {
    Errnof(E_FAIL, "failed to load image from '%s'", img->path);
    clip_image_u8_free(img0);
    return -1;
  }
  // Preprocess image
  struct clip_image_f32 *img_res = clip_image_f32_make();
  if (!clip_image_preprocess(img->clip_ctx, img0, img_res)) {
    Errorf(E_FAIL, "failed to preprocess image '%s'", img->path);
    clip_image_f32_free(img_res);
    clip_image_u8_free(img0);
    return -1;
  }
  // Encode image
  float img_vec[vec_dim];
  if (!clip_image_encode(img->clip_ctx, 4, img_res, img_vec, true)) {
    Errorf(E_FAIL, "failed to encode image '%s'", img->path);
    clip_image_u8_free(img0);
    clip_image_f32_free(img_res);
    return -1;
  }
  clip_image_u8_free(img0);
  clip_image_f32_free(img_res);

  size_t cap = (size_t)vec_dim * 64 + 16;  // generous, safe upper bound per value
  char *embedding_str = malloc(cap);
  if (!embedding_str) {
    Errnof(E_CRIT, "malloc failed for embedding_str (%d dims)", vec_dim);
    return -1;
  }

  size_t used = 0;
  int n = snprintf(embedding_str + used, cap - used, "[");
  used += n;

  for (int i = 0; i < vec_dim; i++) {
    n = snprintf(embedding_str + used, cap - used, "%f%s",
                 img_vec[i], (i < vec_dim - 1) ? "," : "");
    if (n < 0 || (size_t)n >= cap - used) {
      Errorf(E_CRIT, "embedding string truncated/overflowed for '%s'", img->path);
      free(embedding_str);
      return -1;
    }
    used += n;
  }
  snprintf(embedding_str + used, cap - used, "]");

  time_t created_at, modified_at;

  // get_file_times
  {
    struct stat st;
    if (stat(img->path, &st) != 0) {
      Errorf(E_FAIL, "stat failed for '%s'", img->path);
      free(embedding_str);
      return -1;
    }
    modified_at = st.st_mtime;
  }

  // get_birthtime
  {
    struct statx stx;
    if (statx(AT_FDCWD, img->path, 0, STATX_BTIME, &stx) != 0) {
      Errorf(E_FAIL, "statx failed for '%s'", img->path);
      created_at = 0;
    } else if (!(stx.stx_mask & STATX_BTIME)) {
      Errorf(E_FAIL, "filesystem doesn't support btime for '%s'", img->path);
      created_at = 0;
    } else {
      created_at = stx.stx_btime.tv_sec;
    }
  }

  char created_str[32], updated_str[32];

  struct tm tm_created, tm_modified;
  if (!localtime_r(&created_at, &tm_created) ||
      !strftime(created_str, sizeof(created_str), "%Y-%m-%d %H:%M:%S", &tm_created)) {
    snprintf(created_str, sizeof(created_str), "unknown");
  }
  if (!localtime_r(&modified_at, &tm_modified) ||
      !strftime(updated_str, sizeof(updated_str), "%Y-%m-%d %H:%M:%S", &tm_modified)) {
    snprintf(updated_str, sizeof(updated_str), "unknown");
  }

  // thumbnail + dimensions
  char *thumbnail;
  size_t width, height;
  {
    char *temp_path = strdup(img->path);
    if (!temp_path) {
      Errnof(E_CRIT, "strdup failed building thumbnail path");
      free(embedding_str);
      return -1;
    }
    char *dir = dirname(temp_path);

    const char *base = strrchr(img->path, '/');
    base = (base) ? base + 1 : img->path;

    // Thumbnails are always re-encoded as JPEG below (MagickSetImageFormat),
    // so name the thumbnail file .jpg regardless of the source image's own
    // extension (.png, .jpeg, .webp, ...) instead of reusing it verbatim.
    char *base_noext = strdup(base);
    if (!base_noext) {
      Errnof(E_CRIT, "strdup failed building thumbnail path");
      free(temp_path);
      free(embedding_str);
      return -1;
    }
    char *base_ext = strrchr(base_noext, '.');
    if (base_ext && base_ext != base_noext) *base_ext = '\0';

    size_t thumbnail_size = strlen(dir) + strlen("/.thumbnails/") + strlen(base_noext) + strlen(".jpg") + 1;
    thumbnail = malloc(thumbnail_size);
    if (!thumbnail) {
      Errnof(E_CRIT, "malloc failed building thumbnail path");
      free(base_noext);
      free(temp_path);
      free(embedding_str);
      return -1;
    }

    size_t thumbnail_dir_size = strlen(dir) + strlen("/.thumbnails") + 1;
    char *thumbnail_dir = malloc(thumbnail_dir_size);
    if (!thumbnail_dir) {
      Errnof(E_CRIT, "malloc failed building thumbnail dir");
      free(thumbnail);
      free(base_noext);
      free(temp_path);
      free(embedding_str);
      return -1;
    }
    snprintf(thumbnail_dir, thumbnail_dir_size, "%s/.thumbnails", dir);

    struct stat dst = {0};
    if (stat(thumbnail_dir, &dst) == -1) {
      if (mkdir(thumbnail_dir, 0750) != 0 && errno != EEXIST) {
        Errnof(E_FAIL, "mkdir failed for '%s'", thumbnail_dir);
        free(thumbnail_dir);
        free(thumbnail);
        free(base_noext);
        free(temp_path);
        free(embedding_str);
        return -1;
      }
    }
    free(thumbnail_dir);

    snprintf(thumbnail, thumbnail_size, "%s/.thumbnails/%s.jpg", dir, base_noext);
    free(base_noext);
    free(temp_path);

    MagickWand *magick_wand = NewMagickWand();
    if (MagickReadImage(magick_wand, img->path) == MagickFalse) {
      Errorf(E_FAIL, "MagickReadImage failed for '%s'", img->path);
      magick_wand = DestroyMagickWand(magick_wand);
      free(thumbnail);
      free(embedding_str);
      return -1;
    }

    width = MagickGetImageWidth(magick_wand);
    height = MagickGetImageHeight(magick_wand);

    MagickResetIterator(magick_wand);
    while (MagickNextImage(magick_wand) != MagickFalse)
      MagickResizeImage(magick_wand, 640, 360, LanczosFilter);
    MagickSetImageFormat(magick_wand, "JPEG");

    if (MagickWriteImages(magick_wand, thumbnail, MagickTrue) == MagickFalse) {
      Errorf(E_FAIL, "MagickWriteImages failed for '%s'", img->path);
      magick_wand = DestroyMagickWand(magick_wand);
      free(thumbnail);
      free(embedding_str);
      return -1;
    }
    magick_wand = DestroyMagickWand(magick_wand);
  }

  // checksum + mime type
  char hex[65];
  size_t fsize;
  char *mime_type;
  {
    FILE *f = fopen(img->path, "rb");
    if (!f) {
      Errnof(E_FAIL, "fopen failed for '%s'", img->path);
      free(thumbnail);
      free(embedding_str);
      return -1;
    }

    if (fseek(f, 0L, SEEK_END) != 0) {
      Errnof(E_FAIL, "fseek (end) failed for '%s'", img->path);
      fclose(f);
      free(thumbnail);
      free(embedding_str);
      return -1;
    }

    long fsize_l = ftell(f);
    if (fsize_l < 0) {
      Errnof(E_FAIL, "ftell failed for '%s'", img->path);
      fclose(f);
      free(thumbnail);
      free(embedding_str);
      return -1;
    }
    fsize = (size_t)fsize_l;

    if (fseek(f, 0L, SEEK_SET) != 0) {
      Errnof(E_FAIL, "fseek (start) failed for '%s'", img->path);
      fclose(f);
      free(thumbnail);
      free(embedding_str);
      return -1;
    }

    unsigned char *img_buf = malloc(fsize);
    if (!img_buf || fread(img_buf, 1, fsize, f) != fsize) {
      Errorf(E_FAIL, "failed to read '%s'", img->path);
      free(img_buf);
      fclose(f);
      free(thumbnail);
      free(embedding_str);
      return -1;
    }
    fclose(f);

    unsigned char md[32];
    unsigned int md_len;
    EVP_Digest(img_buf, fsize, md, &md_len, EVP_sha256(), NULL);
    for (unsigned int i = 0; i < md_len; i++)
      sprintf(hex + i * 2, "%02x", md[i]);
    hex[64] = '\0';
    free(img_buf);

    magic_t mime = magic_open(MAGIC_MIME_TYPE);
    if (!mime) {
      Errorf(E_FAIL, "magic_open failed for '%s'", img->path);
      free(thumbnail);
      free(embedding_str);
      return -1;
    }
    if (magic_load(mime, NULL) != 0) {
      Errorf(E_FAIL, "magic_load failed for '%s': %s", img->path, magic_error(mime));
      magic_close(mime);
      free(thumbnail);
      free(embedding_str);
      return -1;
    }
    const char *mime_result = magic_file(mime, img->path);
    if (!mime_result) {
      Errorf(E_FAIL, "magic_file failed for '%s': %s", img->path, magic_error(mime));
      magic_close(mime);
      free(thumbnail);
      free(embedding_str);
      return -1;
    }
    // magic_file's return value is owned by the magic_t handle and becomes
    // invalid once it is closed, so make our own copy before closing.
    mime_type = strdup(mime_result);
    magic_close(mime);
    if (!mime_type) {
      Errnof(E_CRIT, "strdup failed for mime_type");
      free(thumbnail);
      free(embedding_str);
      return -1;
    }
  }

  // slug
  char *title;
  {
    char *path_copy = strdup(img->path);
    if (!path_copy) {
      Errnof(E_CRIT, "strdup failed building title");
      free(mime_type);
      free(thumbnail);
      free(embedding_str);
      return -1;
    }
    char *title_raw = basename(path_copy);
    char *dot = strrchr(title_raw, '.');
    if (dot && dot != title_raw) *dot = '\0';
    title = strdup(title_raw);
    free(path_copy);
    if (!title) {
      free(mime_type);
      free(thumbnail);
      free(embedding_str);
      return -1;
    }
  }

  size_t title_len = strlen(title);
  char *slug = malloc(title_len + 1);
  if (!slug) {
    free(title);
    free(mime_type);
    free(thumbnail);
    free(embedding_str);
    return -1;
  }

  char *out = slug;
  int last_was_hyphen = 1; /* suppress leading hyphens */
  for (const unsigned char *p = (const unsigned char *)title; *p; p++) {
    if (isalnum(*p)) {
      *out++ = (char)tolower(*p);
      last_was_hyphen = 0;
    } else if (!last_was_hyphen) {
      *out++ = '-';
      last_was_hyphen = 1;
    }
  }
  if (out > slug && *(out - 1) == '-')
    out--;
  *out = '\0';

  {
    const char *check_sql =
      "SELECT slug FROM wallpapers WHERE slug = $1 OR slug LIKE $1 || '-%'";
    const char *check_params[1] = { slug };
    PGresult *check_res = PQexecParams(img->conn, check_sql, 1, NULL,
                                       check_params, NULL, NULL, 0);

    if (PQresultStatus(check_res) == PGRES_TUPLES_OK && PQntuples(check_res) > 0) {
      int max_n = 0;
      size_t base_len = strlen(slug);
      for (int i = 0; i < PQntuples(check_res); i++) {
        char *existing = PQgetvalue(check_res, i, 0);
        size_t existing_len = strlen(existing);
        if (existing_len == base_len && strcmp(existing, slug) == 0) {
          if (max_n < 1) max_n = 1;
          continue;
        }
        if (existing_len > base_len &&
            strncmp(existing, slug, base_len) == 0 &&
            existing[base_len] == '-') {
          int n = atoi(existing + base_len + 1);
          if (n >= max_n) max_n = n + 1;
        }
      }
      if (max_n > 0) {
        char *new_slug = malloc(base_len + 16);
        sprintf(new_slug, "%s-%d", slug, max_n);
        free(slug);
        slug = new_slug;
      }
    }
    PQclear(check_res);
  }

  // insert
  char file_size_str[32], width_str[16], height_str[16];
  snprintf(file_size_str, sizeof(file_size_str), "%zu", fsize);
  snprintf(width_str, sizeof(width_str), "%zu", width);
  snprintf(height_str, sizeof(height_str), "%zu", height);

  const char *paramValues[12] = {
    title, slug, img->path, thumbnail,
    mime_type, file_size_str, width_str, height_str,
    hex, embedding_str, created_str, updated_str
  };

  const char *insert_sql =
    "INSERT INTO wallpapers (title, slug, image, thumbnail, mime_type, "
    "file_size, width, height, checksum, embedding, created_at, updated_at) "
    "VALUES ($1,$2,$3,$4,$5,$6,$7,$8,$9,$10::vector,$11,$12)";

  PGresult *res_insert = PQexecParams(img->conn, insert_sql, 12, NULL,
                                      paramValues, NULL, NULL, 0);
  int ret = 0;
  if (PQresultStatus(res_insert) != PGRES_COMMAND_OK) {
    Errorf(E_FAIL, "insert failed for '%s': %s", img->path, PQresultErrorMessage(res_insert));
    ret = -1;
  }
  PQclear(res_insert);

  free(thumbnail);
  free(embedding_str);
  free(mime_type);
  free(slug);
  free(title);

  return ret;
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
  FILE *envf = fopen(".env", "r");
  char line[512];
  if (!envf) {
    Errorf(E_CRIT, "Failed to open .env file.");
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
  struct clip_ctx *ctx = clip_model_load("/usr/local/src/clip.cpp/models/clip-vit-base-patch32_ggml-model-f32.gguf", 1);
  if (!ctx) {
    Errorf(E_FAIL, "Unable to load model from %s", "/usr/local/src/clip.cpp/models/clip-vit-base-patch32_ggml-model-f32.gguf");
	exit(EXIT_FAILURE);
  }

  const char *keywords[] = { "user", "password", "dbname", NULL };
  const char *values[]   = { pguser, pgpass, pgdbname, NULL };

  PGconn *conn = PQconnectdbParams(keywords, values, 0);
  free(pgpass);
  free(pguser);
  free(pgdbname);
  if (PQstatus(conn) == CONNECTION_BAD) {       
    Errorf(E_FAIL, "Connection to database failed: %s", PQerrorMessage(conn));
    PQfinish(conn);
    clip_free(ctx);
    file_list_free(list);
    exit(EXIT_FAILURE);
  }
  else {
    int ver = PQserverVersion(conn);
    printf("Server version: %d\n", ver);
  }

  MagickWandGenesis();
  file_list_t *failed = malloc(sizeof(*failed));
  if (!failed) {
    Errnof(E_FAIL, "malloc failed");
    MagickWandTerminus();
    clip_free(ctx);
    PQfinish(conn);
    file_list_free(list);
    exit(EXIT_FAILURE);
  }
  failed->capacity = 16;
  failed->count = 0;
  failed->had_errors = 0;
  failed->paths = reallocarray(NULL, failed->capacity, sizeof(*failed->paths));
  if (!failed->paths) {
    Errnof(E_FAIL, "reallocarray failed");
    free(failed);
    MagickWandTerminus();
    clip_free(ctx);
    PQfinish(conn);
    file_list_free(list);
    exit(EXIT_FAILURE);
  }

  for (int i = 0; i < list->count; i++) {
    image_ctx_t img = {
      .path     = list->paths[i],
      .clip_ctx = ctx,     /* from clip_model_load(...) */
      .conn     = conn,    /* from PQconnectdb(...) */
    };

    if (process_img(&img) != 0) {
      Errnof(E_FAIL, "failed to process image '%s'", img.path);
      list->had_errors = 1;
      if (file_list_push(failed, img.path) != 0) {
        Errorf(E_FAIL, "failed to record '%s' in failed-files list", img.path);
      }
      continue;
    }
  }
  MagickWandTerminus();
  clip_free(ctx);
  PQfinish(conn);

  if (failed->count > 0) {
    fprintf(stderr, "Failed to process %d file%s:\n",
            failed->count, failed->count == 1 ? "" : "s");
    for (int i = 0; i < failed->count; i++) {
      fprintf(stderr, "  %s\n", failed->paths[i]);
    }
  }

  int had_errors = list->had_errors;
  file_list_free(failed);
  file_list_free(list);
  return had_errors ? EXIT_FAILURE : EXIT_SUCCESS;
}
