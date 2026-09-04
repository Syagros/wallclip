#include "wallclip.h"
#include "../logging/log.h"
#include <MagickWand/MagickWand.h>
#include <clip.h>
#include <libpq-fe.h>
#include <magic.h>
#include <openssl/evp.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
  const char *path;
  struct clip_ctx *clip_ctx;
  PGconn *conn;
} image_ctx_t;

int process_img(image_ctx_t *img) {
  // checksum + mime type
  char md[32];
  unsigned int md_len;
  unsigned long size_f;
  char *embedding_str;
  char *mime_type;
  {
    FILE *f = fopen(img->path, "rb");
    if (!f) {
      Errnof(E_FAIL, "fopen failed for '%s'", img->path);
      return -1;
    }

    fseek(f, 0L, SEEK_END);

    size_f = ftell(f);

    fseek(f, 0L, SEEK_SET);

    unsigned char *img_buf = malloc(size_f);
    if (!img_buf || fread(img_buf, 1, size_f, f) != size_f) {
      Errorf(E_FAIL, "failed to read '%s'", img->path);
      free(img_buf);
      fclose(f);
      return -1;
    }
    fclose(f);

    EVP_Digest(img_buf, size_f, (unsigned char *)md, &md_len, EVP_sha256(),
               NULL);

    magic_t mime = magic_open(MAGIC_MIME_TYPE);
    if (!mime) {
      Errorf(E_FAIL, "magic_open failed for '%s'", img->path);
      return -1;
    }
    if (magic_load(mime, NULL) != 0) {
      Errorf(E_FAIL, "magic_load failed for '%s': %s", img->path,
             magic_error(mime));
      magic_close(mime);
      return -1;
    }

    const char *mime_result = magic_buffer(mime, img_buf, size_f);
    if (!mime_result) {
      Errorf(E_FAIL, "magic_file failed for '%s': %s", img->path,
             magic_error(mime));
      magic_close(mime);
      return -1;
    }

    // magic_file's return value is owned by the magic_t handle and becomes
    // invalid once it is closed, so make our own copy before closing.
    mime_type = strdup(mime_result);
    magic_close(mime);
    if (!mime_type) {
      Errnof(E_CRIT, "strdup failed for mime_type");
      return -1;
    }
    printf("%s\n", mime_type);
    free(img_buf);
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
      Errorf(E_WARN, "Skipping %s, duplicate.", img->path);
      return 1;
    }
    PQclear(res);
  }

  int vec_dim = clip_get_vision_hparams(img->clip_ctx)->projection_dim;

  {
    // Load image from disk
    struct clip_image_u8 *img0 = clip_image_u8_make();
    if (!clip_image_load_from_file(img->path, img0)) {
      Errnof(E_FAIL, "failed to load image from '%s'", img->path);
      clip_image_u8_free(img0);
      free(mime_type);
      return -1;
    }
    // Preprocess image
    struct clip_image_f32 *img_res = clip_image_f32_make();
    if (!clip_image_preprocess(img->clip_ctx, img0, img_res)) {
      Errorf(E_FAIL, "failed to preprocess image '%s'", img->path);
      clip_image_f32_free(img_res);
      clip_image_u8_free(img0);
      free(mime_type);
      return -1;
    }
    // Encode image
    float img_vec[vec_dim];
    if (!clip_image_encode(img->clip_ctx, 4, img_res, img_vec, true)) {
      Errorf(E_FAIL, "failed to encode image '%s'", img->path);
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
      free(embedding_str);
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
               img->path);
        free(embedding_str);
        free(mime_type);
        return -1;
      }
      used += n;
    }
    snprintf(embedding_str + used, cap - used, "]");
  }

  // insert
  {
    const char *paramValues[3] = {mime_type, md, embedding_str};
    int paramLengths[3] = {0, 32, 0}; // 0 cus ignored for text param
    int paramFormats[3] = {0, 1, 0};  // 0 = text, 1 = binary
    const char *insert = "INSERT INTO wallpapers (mime_type, checksum, embedding) VALUES ($1, $2, $3)";
    PGresult *res_insert = PQexecParams(img->conn, insert, 3, NULL, paramValues,
                                        paramLengths, paramFormats, 0);

    if (PQresultStatus(res_insert) != PGRES_COMMAND_OK) {
      Errorf(E_FAIL, "insert failed for '%s': %s", img->path,
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

int main(int argc, char *argv[]) {
  printf("%d\n", argc);
  image_ctx_t img;
  img.path = argv[1];
  img.conn = PQconnectdb(conninfo);
  if (PQstatus(img.conn) != CONNECTION_OK) {
    Errorf(E_FAIL, "connection failed: %s", PQerrorMessage(img.conn));
    PQfinish(img.conn);
    Errorf(E_CRIT, "Failed to make postgresql db connection.");
    return 1;
  }

  img.clip_ctx = clip_model_load(CLIP_MODEL_PATH, 0);
  if (!img.clip_ctx) {
    Errorf(E_FAIL, "Unable to load model from %s", CLIP_MODEL_PATH);
    exit(EXIT_FAILURE);
  }

  process_img(&img); // error handling? what's that?
  PQfinish(img.conn);
  clip_free(img.clip_ctx);
  return 0;
}
