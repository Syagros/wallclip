#define _GNU_SOURCE // needed for statx()
#include "wallclip.h"
#include "../logging/log.h"
#include <MagickWand/MagickWand.h>
#include <clip.h>
#include <fcntl.h>
#include <libgen.h>
#include <libpq-fe.h>
#include <limits.h>
#include <magic.h>
#include <openssl/evp.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

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

int process_img(image_ctx_t *img) {
  // checksum + mime type
  char md[32];
  char *embedding_str;
  char *mime_type;
  char *thumbnail;
  unsigned int md_len;
  size_t width, height;
  {
    FILE *f = fopen(img->entry->path, "rb");
    if (!f) {
      Errnof(E_FAIL, "fopen failed for '%s'", img->entry->path);
      return -1;
    }

    // fsize is already known from the upfront stat() in main(), no need to
    // fseek/ftell here.
    size_t size_f = img->entry->fsize;

    unsigned char *img_buf = malloc(size_f);
    if (!img_buf || fread(img_buf, 1, size_f, f) != size_f) {
      Errorf(E_FAIL, "failed to read '%s'", img->entry->path);
      free(img_buf);
      fclose(f);
      return -1;
    }
    fclose(f);

    EVP_Digest(img_buf, size_f, (unsigned char *)md, &md_len, EVP_sha256(),
               NULL);

    magic_t mime = magic_open(MAGIC_MIME_TYPE);
    if (!mime) {
      Errorf(E_FAIL, "magic_open failed for '%s'", img->entry->path);
      free(img_buf);
      return -1;
    }
    if (magic_load(mime, NULL) != 0) {
      Errorf(E_FAIL, "magic_load failed for '%s': %s", img->entry->path,
             magic_error(mime));
      magic_close(mime);
      free(img_buf);
      return -1;
    }

    const char *mime_result = magic_buffer(mime, img_buf, size_f);
    if (!mime_result) {
      Errorf(E_FAIL, "magic_file failed for '%s': %s", img->entry->path,
             magic_error(mime));
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
      return -1;
    }
    printf("%s\n", mime_type);
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
    // Load image from disk
    struct clip_image_u8 *img0 = clip_image_u8_make();
    if (!clip_image_load_from_file(img->entry->path, img0)) {
      Errnof(E_FAIL, "failed to load image from '%s'", img->entry->path);
      clip_image_u8_free(img0);
      free(mime_type);
      return -1;
    }
    // Preprocess image
    struct clip_image_f32 *img_res = clip_image_f32_make();
    if (!clip_image_preprocess(img->clip_ctx, img0, img_res)) {
      Errorf(E_FAIL, "failed to preprocess image '%s'", img->entry->path);
      clip_image_f32_free(img_res);
      clip_image_u8_free(img0);
      free(mime_type);
      return -1;
    }
    // Encode image
    float img_vec[vec_dim];
    if (!clip_image_encode(img->clip_ctx, 4, img_res, img_vec, true)) {
      Errorf(E_FAIL, "failed to encode image '%s'", img->entry->path);
      clip_image_u8_free(img0);
      clip_image_f32_free(img_res);
      free(mime_type);
      return -1;
    }
    clip_image_u8_free(img0);
    clip_image_f32_free(img_res);

    size_t cap =
        (size_t)vec_dim * 64 + 16; // generous, safe upper bound per value
    embedding_str = malloc(cap);
    if (!embedding_str) {
      Errnof(E_CRIT, "malloc failed for embedding_str (%d dims)", vec_dim);
      free(mime_type);
      return -1;
    }

    size_t used = 0;
    int n = snprintf(embedding_str + used, cap - used, "[");
    used += n;

    for (int i = 0; i < vec_dim; i++) {
      n = snprintf(embedding_str + used, cap - used, "%f%s", img_vec[i],
                   (i < vec_dim - 1) ? "," : "");
      if (n < 0 || (size_t)n >= cap - used) {
        Errorf(E_CRIT, "embedding string truncated/overflowed for '%s'",
               img->entry->path);
        free(embedding_str);
        free(mime_type);
        return -1;
      }
      used += n;
    }
    snprintf(embedding_str + used, cap - used, "]");
  }

  {
    char created_str[32], updated_str[32];

    struct tm tm_created, tm_modified;
    if (!localtime_r(&img->entry->created_at, &tm_created) ||
        !strftime(created_str, sizeof(created_str), "%Y-%m-%d %H:%M:%S",
                  &tm_created)) {
      snprintf(created_str, sizeof(created_str), "unknown");
    }
    if (!localtime_r(&img->entry->modified_at, &tm_modified) ||
        !strftime(updated_str, sizeof(updated_str), "%Y-%m-%d %H:%M:%S",
                  &tm_modified)) {
      snprintf(updated_str, sizeof(updated_str), "unknown");
    }
  }

  // thumbnail + dimensions
  {
  }

  // insert
  {
    const char *paramValues[3] = {mime_type, md, embedding_str};
    int paramLengths[3] = {0, 32, 0}; // 0 cus ignored for text param
    int paramFormats[3] = {0, 1, 0};  // 0 = text, 1 = binary
    const char *insert = "INSERT INTO wallpapers (mime_type, checksum, "
                         "embedding) VALUES ($1, $2, $3)";
    PGresult *res_insert = PQexecParams(img->conn, insert, 3, NULL, paramValues,
                                        paramLengths, paramFormats, 0);

    if (PQresultStatus(res_insert) != PGRES_COMMAND_OK) {
      Errorf(E_FAIL, "insert failed for '%s': %s", img->entry->path,
             PQresultErrorMessage(res_insert));
      PQclear(res_insert);
      free(embedding_str);
      free(mime_type);
      return -1;
    }
    PQclear(res_insert);
    free(embedding_str);
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
  list->entries = malloc(count * sizeof(*list->entries));
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
  file_list_t *file_list = file_list_new(argc - 1);
  file_list_t *file_list_failures = file_list_new(argc - 1);
  file_list_t *file_list_duplicates = file_list_new(argc - 1);
  if (!file_list || !file_list_failures || !file_list_duplicates)
    exit(EXIT_FAILURE);

  for (int i = 1; i < argc; i++) {
    struct stat st;
    if (stat(argv[i], &st) != 0) {
      Errnof(E_FAIL, "%s: does not exist", argv[i]);
      file_list_free(file_list);
      exit(EXIT_FAILURE);
    }

    file_entry_t *e = &file_list->entries[i - 1];
    e->path = strdup(argv[i]);
    e->fsize = st.st_size;
    e->modified_at = st.st_mtime;

    struct statx stx;
    if (statx(AT_FDCWD, argv[i], 0, STATX_BTIME, &stx) == 0 &&
        (stx.stx_mask & STATX_BTIME)) {
      e->created_at = stx.stx_btime.tv_sec;
    } else {
      e->created_at = 0;
    }
  }

  PGconn *conn = PQconnectdb(conninfo);
  if (PQstatus(conn) != CONNECTION_OK) {
    Errorf(E_FAIL, "connection failed: %s", PQerrorMessage(conn));
    PQfinish(conn);
    Errorf(E_CRIT, "Failed to make postgresql db connection.");
    file_list_free(file_list);
    return 1;
  }

  struct clip_ctx *clip_ctx = clip_model_load(CLIP_MODEL_PATH, 0);
  if (!clip_ctx) {
    Errorf(E_FAIL, "Unable to load model from %s", CLIP_MODEL_PATH);
    PQfinish(conn);
    file_list_free(file_list);
    exit(EXIT_FAILURE);
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
