#define _GNU_SOURCE // needed for statx()
#include "wallclip.h"
#include "../logging/log.h"
#include <MagickWand/MagickWand.h>
#include <clip.h>
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <libgen.h>
#include <libpq-fe.h>
#include <limits.h>
#include <magic.h>
#include <math.h>
#include <openssl/evp.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

typedef struct {
  char *path;
  size_t fsize;
  time_t created_at;
  time_t modified_at;
} file_entry_t;

typedef struct {
  file_entry_t *entries;
  int count;
} file_list_t;

typedef struct {
  file_entry_t *entry;
  struct clip_ctx *clip_ctx;
  PGconn *conn;
} image_ctx_t;


int query(char *query, struct clip_ctx *clip_ctx, PGconn *conn) {
  char embedding_str[EMBED_STR_MAX];
  image_ctx_t img;
  img.clip_ctx = clip_ctx;
  img.conn = conn;
  size_t offset = 0;
  int written;

  // Tokenize text
  struct clip_tokens tokens;
  clip_tokenize(img.clip_ctx, query, &tokens);

  // Encode text
  int vec_dim = clip_get_text_hparams(clip_ctx)->projection_dim;
  float vec[vec_dim];
  clip_text_encode(clip_ctx, 4, &tokens, vec, true);

  // Convert vec to a pg array
  written = snprintf(embedding_str + offset, EMBED_STR_MAX - offset, "[");
  if (written < 0 || (size_t)written >= EMBED_STR_MAX - offset) {
    Errorf(E_CRIT, "Failed to format embedding vector (buffer too small)");
    clip_tokens_free(&tokens);
    clip_free(clip_ctx);
    PQfinish(conn);
    return -1;
  }
  offset += written;

  for (int i = 0; i < vec_dim; i++) {
    written = snprintf(embedding_str + offset, EMBED_STR_MAX - offset, "%s%.6f",
                       (i == 0) ? "" : ",", vec[i]);
    if (written < 0 || (size_t)written >= EMBED_STR_MAX - offset) {
      Errorf(E_CRIT, "Failed to format embedding vector (buffer too small)");
      clip_tokens_free(&tokens);
      clip_free(clip_ctx);
      PQfinish(conn);
      return -1;
    }
    offset += written;
  }

  written = snprintf(embedding_str + offset, EMBED_STR_MAX - offset, "]");
  if (written < 0 || (size_t)written >= EMBED_STR_MAX - offset) {
    Errorf(E_CRIT, "Failed to format embedding vector (buffer too small)");
    clip_tokens_free(&tokens);
    clip_free(clip_ctx);
    PQfinish(conn);
    return -1;
  }

  // Compare embedding_str with pg database embeddings and get thumbnail paths
  const char *paramValues[1] = {embedding_str};
  const char *sql = "SELECT thumbnail FROM wallpapers "
                    "WHERE 1 - (embedding <=> $1::vector(512)) > 0.27 "
                    "ORDER BY embedding <=> $1::vector(512)";

  PGresult *res = PQexecParams(conn, sql, 1, NULL, paramValues, NULL, NULL, 0);
  if (PQresultStatus(res) != PGRES_TUPLES_OK) {
    Errorf(E_FAIL, "similarity query failed: %s", PQerrorMessage(conn));
    clip_tokens_free(&tokens);
    clip_free(clip_ctx);
    PQfinish(conn);
    PQclear(res);
    return -1;
  }

  int n_matches = PQntuples(res);
  char **match = NULL;
  if (n_matches > 0) {
    match = calloc(n_matches, sizeof(char *));
    if (!match) {
      Errnof(E_CRIT, "calloc failed for match array");
      clip_tokens_free(&tokens);
      clip_free(clip_ctx);
      PQfinish(conn);
      PQclear(res);
      return -1;
    }
    for (int i = 0; i < n_matches; i++) {
      match[i] = strdup(PQgetvalue(res, i, 0));
      if (!match[i]) {
        Errnof(E_CRIT, "strdup failed for matched thumbnail path");
        clip_tokens_free(&tokens);
        clip_free(clip_ctx);
        PQfinish(conn);
        PQclear(res);
        return -1;
      }
    }
  }

  clip_tokens_free(&tokens);
  clip_free(clip_ctx);
  PQfinish(conn);
  PQclear(res);

  for (int i = 0; i < n_matches; i++) {
    printf("%s\n", match[i]);
  }

  for (int i = 0; i < n_matches; i++) {
    free(match[i]);
  }
  free(match);

  return 0;
}

int process_img(image_ctx_t *img) {
  // checksum + mime type
  char md[32];
  char embedding_str[EMBED_STR_MAX];
  char *mime_type;
  char *thumbnail;
  unsigned int md_len;
  char created_str[32];
  size_t width, height;
  {
    FILE *f = fopen(img->entry->path, "rb");
    if (!f) {
      Errnof(E_FAIL, "fopen failed for '%s'", img->entry->path);
#if !defined(DEBUG)
      fprintf(stderr, "wallclip: %s: %s\n", img->entry->path, strerror(errno));
#endif
      return -1;
    }

    // fsize is already known from the upfront stat() in main, no need to
    // fseek/ftell here.
    size_t size_f = img->entry->fsize;

    unsigned char *img_buf = malloc(size_f);
    if (!img_buf || fread(img_buf, 1, size_f, f) != size_f) {
      Errorf(E_FAIL, "failed to read '%s'", img->entry->path);
#if !defined(DEBUG)
      fprintf(stderr, "wallclip: failed to read '%s'\n", img->entry->path);
#endif
      free(img_buf);
      fclose(f);
      return -1;
    }
    fclose(f);

    if (!EVP_Digest(img_buf, size_f, (unsigned char *)md, &md_len, EVP_sha256(),
                    NULL)) {
      Errorf(E_FAIL, "EVP_Digest failed for '%s'", img->entry->path);
#if !defined(DEBUG)
      fprintf(stderr, "wallclip: failed to hash '%s'\n", img->entry->path);
#endif
      free(img_buf);
      return -1;
    }

    magic_t mime = magic_open(MAGIC_MIME_TYPE);
    if (!mime) {
      Errorf(E_FAIL, "magic_open failed for '%s'", img->entry->path);
#if !defined(DEBUG)
      fprintf(stderr,
              "wallclip: failed to initialize mime detection for '%s'\n",
              img->entry->path);
#endif
      free(img_buf);
      return -1;
    }
    if (magic_load(mime, NULL) != 0) {
      Errorf(E_FAIL, "magic_load failed for '%s': %s", img->entry->path,
             magic_error(mime));
#if !defined(DEBUG)
      fprintf(stderr, "wallclip: magic_load failed for '%s': %s\n",
              img->entry->path, magic_error(mime));
#endif
      magic_close(mime);
      free(img_buf);
      return -1;
    }

    const char *mime_result = magic_buffer(mime, img_buf, size_f);
    if (!mime_result) {
      Errorf(E_FAIL, "magic_file failed for '%s': %s", img->entry->path,
             magic_error(mime));
#if !defined(DEBUG)
      fprintf(stderr, "wallclip: failed to detect mime type for '%s': %s\n",
              img->entry->path, magic_error(mime));
#endif
      magic_close(mime);
      free(img_buf);
      return -1;
    }

    // magic_buffer's return value is owned by the magic_t handle and becomes
    // invalid once it is closed, so make our own copy before closing.
    mime_type = strdup(mime_result);
    magic_close(mime);
    free(img_buf);
    if (!mime_type) {
      Errnof(E_CRIT, "strdup failed for mime_type");
#if !defined(DEBUG)
      fprintf(stderr, "wallclip: out of memory processing '%s'\n",
              img->entry->path);
#endif
      return -1;
    }
  }

  // deduplication check using checksum
  {
    const char *paramValues[1];
    int paramLengths[1];
    int paramFormats[1];

    paramValues[0] = (const char *)md;
    paramLengths[0] = 32;
    paramFormats[0] = 1; // binary
    PGresult *res =
        PQexecParams(img->conn, "SELECT 1 FROM wallpapers WHERE checksum = $1",
                     1, NULL, paramValues, paramLengths, paramFormats, 0);
    if (PQntuples(res) > 0) {
      PQclear(res);
      free(mime_type);
      Errorf(E_WARN, "Skipping %s, duplicate.", img->entry->path);
      return 1;
    }
    PQclear(res);
  }

  int vec_dim = clip_get_vision_hparams(img->clip_ctx)->projection_dim;

  {
    char embedding_str[EMBED_STR_MAX];
    size_t offset = 0;
    int written;

    // Load image from disk
    struct clip_image_u8 *img0 = clip_image_u8_make();
    if (!clip_image_load_from_file(img->entry->path, img0)) {
      Errnof(E_FAIL, "failed to load image from '%s'", img->entry->path);
#if !defined(DEBUG)
      fprintf(stderr, "wallclip: failed to load image '%s'\n",
              img->entry->path);
#endif
      clip_image_u8_free(img0);
      free(mime_type);
      return -1;
    }
    // Preprocess image
    struct clip_image_f32 *img_res = clip_image_f32_make();
    if (!clip_image_preprocess(img->clip_ctx, img0, img_res)) {
      Errorf(E_FAIL, "failed to preprocess image '%s'", img->entry->path);
#if !defined(DEBUG)
      fprintf(stderr, "wallclip: failed to preprocess image '%s'\n",
              img->entry->path);
#endif
      clip_image_f32_free(img_res);
      clip_image_u8_free(img0);
      free(mime_type);
      return -1;
    }
    // Encode image
    float img_vec[vec_dim];
    if (!clip_image_encode(img->clip_ctx, 4, img_res, img_vec, true)) {
      Errorf(E_FAIL, "failed to encode image '%s'", img->entry->path);
#if !defined(DEBUG)
      fprintf(stderr, "wallclip: failed to encode image '%s'\n",
              img->entry->path);
#endif
      clip_image_u8_free(img0);
      clip_image_f32_free(img_res);
      free(mime_type);
      return -1;
    }
    clip_image_u8_free(img0);
    clip_image_f32_free(img_res);

    written = snprintf(embedding_str + offset, EMBED_STR_MAX - offset, "[");
    if (written < 0 || (size_t)written >= EMBED_STR_MAX - offset) {
      Errorf(E_CRIT,
             "failed to format embedding vector for '%s' (buffer too small)",
             img->entry->path);
#if !defined(DEBUG)
      fprintf(stderr, "wallclip: failed to format embedding vector for '%s'\n",
              img->entry->path);
#endif
      free(mime_type);
      return -1;
    }
    offset += written;

    for (int i = 0; i < vec_dim; i++) {
      written = snprintf(embedding_str + offset, EMBED_STR_MAX - offset,
                         "%s%.6f", (i == 0) ? "" : ",", img_vec[i]);
      if (written < 0 || (size_t)written >= EMBED_STR_MAX - offset) {
        Errorf(E_CRIT,
               "failed to format embedding vector for '%s' (buffer too small)",
               img->entry->path);
#if !defined(DEBUG)
        fprintf(stderr,
                "wallclip: failed to format embedding vector for '%s'\n",
                img->entry->path);
#endif
        free(mime_type);
        return -1;
      }
      offset += written;
    }

    written = snprintf(embedding_str + offset, EMBED_STR_MAX - offset, "]");
    if (written < 0 || (size_t)written >= EMBED_STR_MAX - offset) {
      Errorf(E_CRIT,
             "failed to format embedding vector for '%s' (buffer too small)",
             img->entry->path);
#if !defined(DEBUG)
      fprintf(stderr, "wallclip: failed to format embedding vector for '%s'\n",
              img->entry->path);
#endif
      free(mime_type);
      return -1;
    }
  }

  {
    struct tm tm_created;
    if (!localtime_r(&img->entry->created_at, &tm_created) ||
        !strftime(created_str, sizeof(created_str), "%Y-%m-%d %H:%M:%S",
                  &tm_created)) {
      Errorf(E_WARN, "failed to format created_at for '%s', using current time",
             img->entry->path);
      snprintf(created_str, sizeof(created_str),
               "now"); // Postgres understands 'now'
    }
  }

  // thumbnail + dimensions
  {
    char *path_copy1 = strdup(img->entry->path);
    char *path_copy2 = strdup(img->entry->path);
    if (!path_copy1 || !path_copy2) {
      Errnof(E_CRIT, "strdup failed for path");
#if !defined(DEBUG)
      fprintf(stderr, "wallclip: out of memory processing '%s'\n",
              img->entry->path);
#endif
      free(path_copy1);
      free(path_copy2);
      free(mime_type);
      return -1;
    }
    char *fname = basename(path_copy1);
    char *fdir = dirname(path_copy2);

    char thumb_dir[PATH_MAX];
    snprintf(thumb_dir, sizeof(thumb_dir), "%s/.thumbnails", fdir);
    if (mkdir(thumb_dir, 0755) != 0 && errno != EEXIST) {
      Errnof(E_FAIL, "mkdir failed for '%s'", thumb_dir);
#if !defined(DEBUG)
      fprintf(stderr, "wallclip: mkdir failed for '%s': %s\n", thumb_dir,
              strerror(errno));
#endif
      free(path_copy1);
      free(path_copy2);
      free(mime_type);
      return -1;
    }

    size_t thumb_len =
        strlen(fdir) + strlen("/.thumbnails/") + strlen(fname) + 16;
    thumbnail = malloc(thumb_len);
    if (!thumbnail) {
      Errnof(E_CRIT, "malloc failed for thumbnail path");
#if !defined(DEBUG)
      fprintf(stderr, "wallclip: out of memory processing '%s'\n",
              img->entry->path);
#endif
      free(path_copy1);
      free(path_copy2);
      free(mime_type);
      return -1;
    }
    // Thumbnails are always re-encoded as JPEG below (MagickSetImageFormat),
    // so name the thumbnail file .jpg regardless of the source image's own
    // extension. If a thumbnail with that base name already exists (e.g. two
    // source files sharing a name but differing in extension), suffix with
    // -1, -2, ... until a free path is found.
    char *fname_noext = strdup(fname);
    if (!fname_noext) {
      Errnof(E_CRIT, "strdup failed building thumbnail path");
#if !defined(DEBUG)
      fprintf(stderr, "wallclip: out of memory processing '%s'\n",
              img->entry->path);
#endif
      free(path_copy1);
      free(path_copy2);
      free(thumbnail);
      free(mime_type);
      return -1;
    }
    char *base_ext = strrchr(fname_noext, '.');
    if (base_ext && base_ext != fname_noext)
      *base_ext = '\0';

    int suffix = 0;
    for (;;) {
      if (suffix == 0) {
        snprintf(thumbnail, thumb_len, "%s/.thumbnails/%s.jpg", fdir,
                 fname_noext);
      } else {
        snprintf(thumbnail, thumb_len, "%s/.thumbnails/%s-%d.jpg", fdir,
                 fname_noext, suffix);
      }
      if (access(thumbnail, F_OK) != 0)
        break; // path is free
      suffix++;
    }

    MagickWand *magick_wand = NewMagickWand();
    if (MagickReadImage(magick_wand, img->entry->path) == MagickFalse) {
      ExceptionType severity;
      char *magick_err = MagickGetException(magick_wand, &severity);
      Errorf(E_FAIL, "MagickReadImage failed for '%s': %s", img->entry->path,
             magick_err);
#if !defined(DEBUG)
      fprintf(stderr, "wallclip: %s\n", magick_err);
#endif
      magick_err = (char *)MagickRelinquishMemory(magick_err);
      magick_wand = DestroyMagickWand(magick_wand);
      free(thumbnail);
      free(mime_type);
      free(path_copy1);
      free(path_copy2);
      free(fname_noext);
      return -1;
    }

    width = MagickGetImageWidth(magick_wand);
    height = MagickGetImageHeight(magick_wand);

    MagickResetIterator(magick_wand);
    while (MagickNextImage(magick_wand) != MagickFalse) {
      double scale = fmin(640.0 / (double)width, 360.0 / (double)height);
      size_t new_w = (size_t)(width * scale);
      size_t new_h = (size_t)(height * scale);
      if (new_w == 0)
        new_w = 1;
      if (new_h == 0)
        new_h = 1;

      if (MagickResizeImage(magick_wand, new_w, new_h, LanczosFilter) ==
          MagickFalse) {
        ExceptionType severity;
        char *magick_err = MagickGetException(magick_wand, &severity);
        Errorf(E_FAIL, "MagickResizeImage failed for '%s': %s",
               img->entry->path, magick_err);
#if !defined(DEBUG)
        fprintf(stderr, "wallclip: %s\n", magick_err);
#endif
        magick_err = (char *)MagickRelinquishMemory(magick_err);
        magick_wand = DestroyMagickWand(magick_wand);
        free(path_copy1);
        free(path_copy2);
        free(fname_noext);
        free(thumbnail);
        free(mime_type);
        return -1;
      }
    }

    if (MagickSetImageFormat(magick_wand, "JPEG") == MagickFalse) {
      ExceptionType severity;
      char *magick_err = MagickGetException(magick_wand, &severity);
      Errorf(E_FAIL, "MagickSetImageFormat failed for '%s': %s",
             img->entry->path, magick_err);
#if !defined(DEBUG)
      fprintf(stderr, "wallclip: %s\n", magick_err);
#endif
      magick_err = (char *)MagickRelinquishMemory(magick_err);
      magick_wand = DestroyMagickWand(magick_wand);
      free(path_copy1);
      free(path_copy2);
      free(fname_noext);
      free(thumbnail);
      free(mime_type);
      return -1;
    }

    if (MagickWriteImages(magick_wand, thumbnail, MagickTrue) == MagickFalse) {
      ExceptionType severity;
      char *magick_err = MagickGetException(magick_wand, &severity);
      Errorf(E_FAIL, "MagickWriteImages failed for '%s': %s", img->entry->path,
             magick_err);
#if !defined(DEBUG)
      fprintf(stderr, "wallclip: %s\n", magick_err);
#endif
      magick_err = (char *)MagickRelinquishMemory(magick_err);
      magick_wand = DestroyMagickWand(magick_wand);
      free(thumbnail);
      free(mime_type);
      free(path_copy1);
      free(path_copy2);
      free(fname_noext);
      return -1;
    }
    magick_wand = DestroyMagickWand(magick_wand);

    free(path_copy1);
    free(path_copy2);
    free(fname_noext);
  }

  // slug
  char *title;
  {
    char *path_copy = strdup(img->entry->path);
    if (!path_copy) {
      Errnof(E_CRIT, "strdup failed building title");
#if !defined(DEBUG)
      fprintf(stderr, "wallclip: out of memory processing '%s'\n",
              img->entry->path);
#endif
      free(mime_type);
      free(thumbnail);
      return -1;
    }
    char *title_raw = basename(path_copy);
    char *dot = strrchr(title_raw, '.');
    if (dot && dot != title_raw)
      *dot = '\0';
    title = strdup(title_raw);
    free(path_copy);
    if (!title) {
      Errnof(E_CRIT, "strdup failed building title");
#if !defined(DEBUG)
      fprintf(stderr, "wallclip: out of memory processing '%s'\n",
              img->entry->path);
#endif
      free(mime_type);
      free(thumbnail);
      return -1;
    }
  }

  size_t title_len = strlen(title);
  char *slug = malloc(title_len + 1);
  if (!slug) {
    Errnof(E_CRIT, "malloc failed for slug");
#if !defined(DEBUG)
    fprintf(stderr, "wallclip: out of memory processing '%s'\n",
            img->entry->path);
#endif
    free(title);
    free(mime_type);
    free(thumbnail);
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
    const char *check_params[1] = {slug};
    PGresult *check_res = PQexecParams(img->conn, check_sql, 1, NULL,
                                       check_params, NULL, NULL, 0);

    if (PQresultStatus(check_res) != PGRES_TUPLES_OK) {
      Errorf(E_FAIL, "slug collision query failed for '%s': %s",
             img->entry->path, PQerrorMessage(img->conn));
#if !defined(DEBUG)
      fprintf(stderr, "wallclip: slug collision query failed for '%s': %s",
              img->entry->path, PQerrorMessage(img->conn));
#endif
      PQclear(check_res);
      free(slug);
      free(title);
      free(mime_type);
      free(thumbnail);
      return -1;
    }

    if (PQntuples(check_res) > 0) {
      int max_n = 0;
      size_t base_len = strlen(slug);
      for (int i = 0; i < PQntuples(check_res); i++) {
        char *existing = PQgetvalue(check_res, i, 0);
        size_t existing_len = strlen(existing);
        if (existing_len == base_len && strcmp(existing, slug) == 0) {
          if (max_n < 1)
            max_n = 1;
          continue;
        }
        if (existing_len > base_len && strncmp(existing, slug, base_len) == 0 &&
            existing[base_len] == '-') {
          int n = atoi(existing + base_len + 1);
          if (n >= max_n)
            max_n = n + 1;
        }
      }
      if (max_n > 0) {
        char *new_slug = malloc(base_len + 16);
        if (!new_slug) {
          Errnof(E_CRIT, "malloc failed building unique slug");
#if !defined(DEBUG)
          fprintf(stderr, "wallclip: out of memory processing '%s'\n",
                  img->entry->path);
#endif
          PQclear(check_res);
          free(slug);
          free(title);
          free(mime_type);
          free(thumbnail);
          return -1;
        }
        snprintf(new_slug, base_len + 16, "%s-%d", slug, max_n);
        free(slug);
        slug = new_slug;
      }
    }
    PQclear(check_res);
  }

  // insert
  {
    char fsize_str[32], width_str[32], height_str[32];
    snprintf(fsize_str, sizeof(fsize_str), "%zu", img->entry->fsize);
    snprintf(width_str, sizeof(width_str), "%zu", width);
    snprintf(height_str, sizeof(height_str), "%zu", height);

    const char *paramValues[11] = {
        title,     slug,          img->entry->path, thumbnail,
        mime_type, fsize_str,     width_str,        height_str,
        md,        embedding_str, created_str,
    };
    int paramLengths[11] = {0, 0, 0, 0, 0, 0, 0, 0, 32, 0, 0};
    int paramFormats[11] = {0, 0, 0, 0, 0, 0, 0, 0, 1, 0, 0}; // md is binary

    const char *insert =
        "INSERT INTO wallpapers "
        "(title, slug, image, thumbnail, mime_type, file_size, width, "
        "height, checksum, embedding, created_at) "
        "VALUES ($1,$2,$3,$4,$5,$6,$7,$8,$9,$10,$11)";

    PGresult *res_insert =
        PQexecParams(img->conn, insert, 11, NULL, paramValues, paramLengths,
                     paramFormats, 0);

    if (PQresultStatus(res_insert) != PGRES_COMMAND_OK) {
      Errorf(E_FAIL, "insert failed for '%s': %s", img->entry->path,
             PQresultErrorMessage(res_insert));
#if !defined(DEBUG)
      fprintf(stderr, "wallclip: insert failed for '%s': %s\n",
              img->entry->path, PQresultErrorMessage(res_insert));
#endif
      PQclear(res_insert);
      free(slug);
      free(title);
      free(thumbnail);
      free(mime_type);
      return -1;
    }
    PQclear(res_insert);
    free(slug);
    free(title);
    free(thumbnail);
    free(mime_type);
    return 0;
  }
}

file_list_t *file_list_new(size_t count) {
  file_list_t *list = malloc(sizeof(*list));
  if (!list) {
    Errnof(E_FAIL, "malloc failed");
    return NULL;
  }
  list->count = count;
  list->entries = calloc(count, sizeof(*list->entries));
  if (!list->entries) {
    Errnof(E_FAIL, "malloc failed");
    free(list);
    return NULL;
  }

  return list;
}

void file_list_free(file_list_t *list) {
  if (!list)
    return;
  for (int i = 0; i < list->count; i++)
    free(list->entries[i].path);
  free(list->entries);
  free(list);
}

int main(int argc, char *argv[]) {
  bool is_query = 0;
  file_list_t *file_list = file_list_new(argc - 1);
  file_list_t *file_list_failures = file_list_new(argc - 1);
  file_list_t *file_list_duplicates = file_list_new(argc - 1);
  if (!file_list || !file_list_failures || !file_list_duplicates)
    exit(EXIT_FAILURE);

  for (int i = 1; i < argc; i++) {
    if (strcmp(argv[i], "-q") == 0 || strcmp(argv[i], "--query") == 0) {
      is_query = 1;
      goto common;
    }
    struct stat st;
    if (stat(argv[i], &st) != 0) {
      Errnof(E_FAIL, "%s: does not exist", argv[i]);
#if !defined(DEBUG)
      fprintf(stderr, "wallclip: %s: %s\n", argv[i], strerror(errno));
#endif
      file_list_free(file_list);
      file_list_free(file_list_failures);
      file_list_free(file_list_duplicates);
      exit(EXIT_FAILURE);
    }

    file_entry_t *e = &file_list->entries[i - 1];
    char *resolved_path = realpath(argv[i], NULL);
    if (!resolved_path) {
      Errnof(E_FAIL, "realpath failed for '%s'", argv[i]);
#if !defined(DEBUG)
      fprintf(stderr, "wallclip: %s: %s\n", argv[i], strerror(errno));
#endif
      file_list_free(file_list);
      file_list_free(file_list_failures);
      file_list_free(file_list_duplicates);
      exit(EXIT_FAILURE);
    }
    e->path = resolved_path;
    e->fsize = st.st_size;
    e->modified_at = st.st_mtime;

    struct statx stx;
    if (statx(AT_FDCWD, argv[i], 0, STATX_BTIME, &stx) == 0 &&
        (stx.stx_mask & STATX_BTIME)) {
      e->created_at = stx.stx_btime.tv_sec;
    } else {
      e->created_at = e->modified_at; // fallback for when filesystem doesn't
                                      // support birth time.
    }
  }

common:
  PGconn *conn = PQconnectdb(conninfo);
  if (PQstatus(conn) != CONNECTION_OK) {
    Errorf(E_FAIL, "connection failed: %s", PQerrorMessage(conn));
    PQfinish(conn);
    Errorf(E_CRIT, "Failed to make postgresql db connection.");
    file_list_free(file_list);
    file_list_free(file_list_failures);
    file_list_free(file_list_duplicates);
    return 1;
  }

  struct clip_ctx *clip_ctx = clip_model_load(CLIP_MODEL_PATH, 0);
  if (!clip_ctx) {
    Errorf(E_FAIL, "Unable to load model from %s", CLIP_MODEL_PATH);
    PQfinish(conn);
    file_list_free(file_list);
    exit(EXIT_FAILURE);
  }

  int rc;
  if (is_query == 1) {
    rc = query(argv[2], clip_ctx, conn);
    file_list_free(file_list);
    file_list_free(file_list_failures);
    file_list_free(file_list_duplicates);
    return rc;
  }

  int failures = 0;
  int duplicates = 0;
  for (int i = 0; i < file_list->count; i++) {
    image_ctx_t img = {
        .entry = &file_list->entries[i],
        .clip_ctx = clip_ctx,
        .conn = conn,
    };
    int rc = process_img(&img);
    if (rc < 0) {
      Errorf(E_WARN, "failed to process '%s'", file_list->entries[i].path);
#if !defined(DEBUG)
      fprintf(stderr, "wallclip: failed to process %s\n",
              file_list->entries[i].path);
#endif
      file_list_failures->entries[failures].path =
          strdup(file_list->entries[i].path);
      if (!file_list_failures->entries[failures].path) {
        Errnof(E_CRIT, "strdup failed recording failed path");
      }
      failures++;
    } else if (rc > 0) {
      Errorf(E_WARN, "'%s' is a duplicate, skipping",
             file_list->entries[i].path);
#if !defined(DEBUG)
      fprintf(stderr, "wallclip: '%s' is a duplicate, skipping\n",
              file_list->entries[i].path);
#endif
      file_list_duplicates->entries[duplicates].path =
          strdup(file_list->entries[i].path);
      if (!file_list_duplicates->entries[duplicates].path) {
        Errnof(E_CRIT, "strdup failed recording duplicate path");
      }
      duplicates++;
    }
  }
  file_list_failures->count = failures;
  file_list_duplicates->count = duplicates;

  if (failures > 0) {
    printf("%d image(s) failed processing:\n", failures);
    for (int i = 0; i < file_list_failures->count; i++)
      printf("  %s\n", file_list_failures->entries[i].path);
  }
  if (duplicates > 0) {
    printf("%d image(s) skipped as duplicates:\n", duplicates);
    for (int i = 0; i < file_list_duplicates->count; i++)
      printf("  %s\n", file_list_duplicates->entries[i].path);
  }

  PQfinish(conn);
  clip_free(clip_ctx);
  file_list_free(file_list);
  file_list_free(file_list_failures);
  file_list_free(file_list_duplicates);
  return failures > 0 ? EXIT_FAILURE : EXIT_SUCCESS;
}
