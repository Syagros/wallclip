#define CLIP_EMBED_DIM 512
// generous fixed bound: 512 floats, each up to ~24 chars incl. comma/sign/decimals, plus brackets/NUL
#define EMBED_STR_MAX (CLIP_EMBED_DIM * 24 + 16)
#define CLIP_MODEL_PATH "/usr/local/src/clip.cpp/models/clip-vit-base-patch32_ggml-model-f32.gguf"

const char conninfo[256] = "dbname=CHANGEME! user=CHANGEME! password=CHANGEME!";
const char *socket_path = "/run/wallclip/socket";
const float similarity_threshold = 0.27;
