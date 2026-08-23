#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <unistd.h>
#include <inttypes.h>
#include <magic.h>
#include <MagickWand/MagickWand.h>

typedef struct {
  size_t width;
  size_t height;
} ImageDims;

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
  char mime_type_buf[32];
  FILE *f = fopen(argv[1], "r");
  fseek(f, 0L, SEEK_END);
  long int res = ftell(f);

  magic_t mime = magic_open(MAGIC_MIME_TYPE);
  magic_load(mime,NULL);
  const char *mime_type = magic_file(mime,argv[1]);

  format_bytes(res, mime_type_buf, sizeof(mime_type_buf));

  printf("%s %s %s\n", mime_type_buf, argv[1], mime_type);
  printf("width: %zu\nheight: %zu", imageDims->width, imageDims->height);

  fclose(f);
  free(imageDims);
  magic_close(mime);
  return 0;
}
