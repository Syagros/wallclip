#define CLIP_MAX_TOKENS 77 // CLIP text context length, incl. start/end tokens
// upper bound of one "%.6f" float in a pgvector literal: sign, 39 integer
// digits, '.', 6 decimals, and the separating comma
#define EMBED_FLOAT_MAX 48

/*
 * Defaults for the settings below. They can be overridden at runtime, with
 * precedence: these < .env file < shell environment (see load_config()).
 * CLIP_MAX_TOKENS stays compile-time, it is tied to the CLIP text encoder.
 * The embedding dimension comes from the loaded model at runtime and must
 * match the vector(N) column created by schema.sql (see its embed_dim).
 */
#define CLIP_MODEL_PATH "/usr/local/src/clip.cpp/models/clip-vit-base-patch32_ggml-model-f32.gguf"
#define MAX_MATCHES 100

const char *socket_path = "/run/wallclip/socket";
const char *model_path = CLIP_MODEL_PATH;
int verbosity = 0; // not static, log.h reads it from env-loader.c too
float similarity_threshold = 0.27;
int max_matches = MAX_MATCHES;
