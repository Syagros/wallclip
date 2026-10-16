#define CLIP_EMBED_DIM 512
#define CLIP_MAX_TOKENS 77 // CLIP text context length, incl. start/end tokens
// generous fixed bound: 512 floats, each up to ~24 chars incl. comma/sign/decimals, plus brackets/NUL
#define EMBED_STR_MAX (CLIP_EMBED_DIM * 24 + 16)

/*
 * Defaults for the settings below. They can be overridden at runtime, with
 * precedence: these < .env file < shell environment (see load_config()).
 * The three macros above stay compile-time, they are tied to the model and
 * to the vector(512) column in schema.sql.
 */
#define CLIP_MODEL_PATH "/usr/local/src/clip.cpp/models/clip-vit-base-patch32_ggml-model-f32.gguf"
#define MAX_MATCHES 100

const char *socket_path = "/run/wallclip/socket";
const char *model_path = CLIP_MODEL_PATH;
int verbosity = 0; // not static, log.h reads it from env-loader.c too
float similarity_threshold = 0.27;
int max_matches = MAX_MATCHES;
