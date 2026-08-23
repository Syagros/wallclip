#define _GNU_SOURCE
#include <clip.h>
#include <ctype.h>
#include <fcntl.h>
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

typedef struct {
  size_t width;
  size_t height;
} ImageDims;

int get_birthtime_linux(const char *path, time_t *created_at) {
  struct statx stx;
  if (statx(AT_FDCWD, path, 0, STATX_BTIME, &stx) != 0) {
    return -1;
  }
  if (!(stx.stx_mask & STATX_BTIME)) {
    return -1; // filesystem doesn't support btime
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

void format_bytes(uint64_t bytes, char *out, size_t out_size) {
  const char *units[] = {"B", "KiB", "MiB", "GiB", "TiB", "PiB"};
  int unit_index = 0;
  double size = (double)bytes;

  while (size >= 1024.0 && unit_index < 5) {
    size /= 1024.0;
    unit_index++;
  }

  if (unit_index == 0) {
    snprintf(out, out_size, "%" PRIu64 " %s", bytes, units[unit_index]);
  } else {
    snprintf(out, out_size, "%.2f %s", size, units[unit_index]);
  }
}

static char *make_slug(const char *title) {
  size_t len = strlen(title);
  char *slug = malloc(len + 1);
  if (!slug) return NULL;

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
  return slug;
}

void *getDimensions(const char *image, const char *thumbnail) {
#define ThrowWandException(wand) \
{ \
  char \
  *description; \
  \
  ExceptionType \
  severity; \
  \
  description=MagickGetException(wand,&severity); \
  (void) fprintf(stderr,"%s %s %lu %s\n",GetMagickModule(),description); \
  description=(char *) MagickRelinquishMemory(description); \
  magick_wand=DestroyMagickWand(magick_wand); \
  MagickWandTerminus(); \
  exit(0); \
  }

ImageDims *imageDims = malloc(sizeof(ImageDims));
  
MagickWand
*magick_wand;

if (access(image, F_OK) != 0) {
  printf("File %s not found.", image);
  exit(0);
}

MagickBooleanType
status;

  
MagickWandGenesis();
magick_wand=NewMagickWand();
status=MagickReadImage(magick_wand,image);

if (status == MagickFalse)
  ThrowWandException(magick_wand);

imageDims->width = MagickGetImageWidth(magick_wand);
imageDims->height = MagickGetImageHeight(magick_wand);

// Generate the images into a thumbnail.
MagickResetIterator(magick_wand);

while (MagickNextImage(magick_wand) != MagickFalse)
  MagickResizeImage(magick_wand,640,360,LanczosFilter);

status=MagickWriteImages(magick_wand,thumbnail,MagickTrue);

if (status == MagickFalse)
  ThrowWandException(magick_wand);

magick_wand=DestroyMagickWand(magick_wand);
MagickWandTerminus();

return (void *)imageDims;
}

int main(int argc, char **argv) {
  if (argc < 3) {
    fprintf(stderr, "Usage: %s image thumbnail\n", argv[0]);
    return 1;
  }

  ImageDims *imageDims = (ImageDims *)getDimensions(argv[1], argv[2]);
  char *pgpass = NULL, *pguser = NULL, *pgdbname = NULL;
  char mime_type_buf[32];
  FILE *envf = fopen(".env", "r");
  char line[512];
  if (!envf) {
    perror("fopen .env");
    return 1;
  }
  FILE *f = fopen(argv[1], "rb");
  if (!f) {
    perror("fopen");
    return 1;
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
    fprintf(stderr, "PGPASS/PGUSER/PGDBNAME missing from .env\n");
    free(pgpass);
    free(pguser);
    free(pgdbname);
    return 1;
  }
  fseek(f, 0L, SEEK_END);
  size_t res = ftell(f);
  fseek(f, 0L, SEEK_SET);

  // Load CLIP model
  struct clip_ctx * ctx = clip_model_load("/usr/local/src/clip.cpp/models/clip-vit-base-patch32_ggml-model-f32.gguf", 1);
  if (!ctx) {
	printf("%s: Unable to load model from /usr/local/src/clip.cpp/models/clip-vit-base-patch32_ggml-model-f32.gguf", __func__);
	return 1;
  }

  int vec_dim = clip_get_vision_hparams(ctx)->projection_dim;

  // Load image from disk
  struct clip_image_u8 * img0 = clip_image_u8_make();
  if (!clip_image_load_from_file(argv[1], img0)) {
	fprintf(stderr, "%s: failed to load image from '%s'\n", __func__, argv[1]);        clip_image_u8_free(img0);
	return 1;
  }

  // Preprocess image
  struct clip_image_f32 * img_res = clip_image_f32_make();
  if (!clip_image_preprocess(ctx, img0, img_res)) {
	fprintf(stderr, "%s: failed to preprocess image\n", __func__);
    clip_image_f32_free(img_res);
    clip_image_u8_free(img0);
	return 1;
  }

  // Encode image
  float img_vec[vec_dim];
  if (!clip_image_encode(ctx, 4, img_res, img_vec, true)) {
	fprintf(stderr, "%s: failed to encode image\n", __func__);
    clip_image_u8_free(img0);
    clip_image_f32_free(img_res);
	return 1;
  }

  for (int i = 0; i < vec_dim; i++) {
	printf("%f%s", img_vec[i], (i < vec_dim - 1) ? ", " : "");
  }
  printf("]\n");

  if (!pgpass || !pguser || !pgdbname) {
	fprintf(stderr, "PGPASS/PGUSER/PGDBNAME missing from .env\n");
	free(pgpass);
	free(pguser);
	free(pgdbname);
	return 1;
  }

  char conninfo[256];
  snprintf(conninfo, sizeof(conninfo), "user=%s password=%s dbname=%s", pguser, pgpass, pgdbname);

  PGconn *conn = PQconnectdb(conninfo);
  free(pgpass);
  free(pguser);
  free(pgdbname);
  if (PQstatus(conn) == CONNECTION_BAD) {       
	fprintf(stderr, "Connection to database failed: %s\n",
			PQerrorMessage(conn));
	PQfinish(conn);
  }

  int ver = PQserverVersion(conn);
  printf("Server version: %d\n", ver);

  time_t created_at, modified_at, changed_at, accessed_at;
  if (get_file_times(argv[1], &modified_at, &changed_at, &accessed_at) != 0) {
    perror("get_file_times");
    return 1;
  }

  if (get_birthtime_linux(argv[1], &created_at) != 0) {
    perror("get_birthtime_linux");
    created_at = 0;
  }

  unsigned char *img_buf = malloc(res);
  if (!img_buf)
    printf("Failed to allocate space for img_buf");

  int nread = fread(img_buf, 1, res, f);
  if (nread == 0) {
    printf("Failed to read file.");
    return 1;
  }
  unsigned char md[32];
  unsigned int md_len;

  EVP_Digest(img_buf, res, md, &md_len, EVP_sha256(), NULL);
  char hex[65];
  for (unsigned int i = 0; i < md_len; i++)
    sprintf(hex + i * 2, "%02x", md[i]);
  hex[64] = '\0';
  free(img_buf);

  magic_t mime = magic_open(MAGIC_MIME_TYPE);
  magic_load(mime,NULL);
  const char *mime_type = magic_file(mime,argv[1]);

  format_bytes(res, mime_type_buf, sizeof(mime_type_buf));

  printf("%s %s %s\n", mime_type_buf, argv[1], mime_type);
  printf("width: %zu\nheight: %zu\n", imageDims->width, imageDims->height);

  char *title = basename(argv[1]);
  char *dot = strrchr(title, '.');
  if (dot && dot != title ) {
    *dot = '\0';
  }
  char *slug = make_slug(title);
  printf("title: %s\n",title);
  printf("slug: %s\n",slug);
  printf("hash: %s\n", hex);
  printf("Created: %s",  ctime(&created_at));
  printf("Modified: %s", ctime(&modified_at));
  printf("Changed:  %s", ctime(&changed_at));
  printf("Accessed: %s", ctime(&accessed_at));
  PQfinish(conn);

  // Cleanup
  clip_free(ctx);
  clip_image_u8_free(img0);
  clip_image_f32_free(img_res);
  fclose(f);
  free(imageDims);
  free(slug);
  magic_close(mime);
  return 0;
}
