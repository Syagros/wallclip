#define _GNU_SOURCE // needed for statx()
#include "wallclip.h"
#include "log.h"
#include "env-loader.h"
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
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <signal.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>
#include <getopt.h>

#define BUFFER_SIZE 4096
#define DISCARD_MAX (64 * 1024) // max oversized request bytes drained

struct file_entry {
	char *path;
	size_t fsize;
	time_t created_at;
	time_t modified_at;
};

struct file_list {
	struct file_entry *entries;
	int count;
};

struct image_ctx {
	struct file_entry *entry;
	struct clip_ctx *clip_ctx;
	PGconn *conn;
};

struct option long_options[] = {
	{ "verbose", no_argument,       0, 'v' },
	{ "delete",  required_argument, 0, 'd' },
	{ "query",  required_argument, 0, 'q' },
	{ "help",  no_argument, 0, 'h' },
	{ 0, 0, 0, 0 }
};

static void usage(FILE *out) {
	fprintf(out,
			"Usage:\n"
			"  wallclip [-v]				run server in foreground\n"
			"  wallclip [-v] FILE...			add images (space separated)\n"
			"  wallclip [-v] -d FILE [-d FILE2 ...]	delete images (both, db and fs; repeat -d per file)\n"
			"  wallclip [-v] -q, --query STR		query for images\n"
			"\n"
			"Options:\n"
			"  -v, --verbose\n"
			"  -d, --delete\n"
			"  -h, --help\n");
}

int init_setup(PGconn **conn, struct clip_ctx **clip_ctx)
{
	// unset/empty values are ignored by libpq, so it falls back to its own
	// defaults and PG* variables
	const char *keywords[] = { "dbname", "user", "password", "host", "port", NULL };
	const char *values[] = {
		env_get("WALLCLIP_DB_NAME"),
		env_get("WALLCLIP_DB_USER"),
		env_get("WALLCLIP_DB_PASSWORD"),
		env_get("WALLCLIP_DB_HOST"),
		env_get("WALLCLIP_DB_PORT"),
		NULL
	};

	// PQconnectdbParams() blocks until authentication is done (or failed), so
	// the credentials are not needed after it returns
	*conn = PQconnectdbParams(keywords, values, 0);
	env_scrub("WALLCLIP_DB_NAME");
	env_scrub("WALLCLIP_DB_USER");
	env_scrub("WALLCLIP_DB_PASSWORD");
	memset(values, 0, sizeof(values));

	if (PQstatus(*conn) != CONNECTION_OK) {
		Errorf(E_CRIT, "connection failed: %s", PQerrorMessage(*conn));
		return -1;
	}

	*clip_ctx = clip_model_load(model_path, 0);
	if (!*clip_ctx) {
		Errorf(E_CRIT, "failed to load clip model");
		return -1;
	}

	return 0;
}

// formats vector into `embedding_str` (of size `embedding_str_max`) as a
// pgvector literal. Returns 0 on success, -1 on error.
static int format_embedding(float *vec, int vec_dim,
		char *embedding_str, size_t embedding_str_max) {
	int rc = -1;

	size_t offset = 0;
	int written = snprintf(embedding_str + offset, embedding_str_max - offset, "[");
	if (written < 0 || (size_t)written >= embedding_str_max - offset) {
		goto cleanup;
	}
	offset += (size_t)written;

	for (int i = 0; i < vec_dim; i++) {
		written = snprintf(embedding_str + offset, embedding_str_max - offset,
				"%s%.6f", (i == 0) ? "" : ",", vec[i]);
		if (written < 0 || (size_t)written >= embedding_str_max - offset) {
			goto cleanup;
		}
		offset += (size_t)written;
	}

	written = snprintf(embedding_str + offset, embedding_str_max - offset, "]");
	if (written < 0 || (size_t)written >= embedding_str_max - offset) {
		goto cleanup;
	}

	rc = 0;

cleanup:
	return rc;
}

// Encodes the image at `path` with CLIP. Returns a malloc'd float vector of
// length *dim_out (caller frees), or NULL on error.
static float *encode_image_embedding(struct clip_ctx *ctx, const char *path,
		int *dim_out) {
	float *vec = NULL;
	bool ok = false;
	struct clip_image_u8 *img_u8 = clip_image_u8_make();
	struct clip_image_f32 *img_f32 = clip_image_f32_make();

	if (!img_u8 || !img_f32) {
		Errorf(E_CRIT, "failed to allocate clip image buffers");
		goto cleanup;
	}

	if (!clip_image_load_from_file(path, img_u8)) {
		Errorf(E_FAIL, "clip failed to load '%s'", path);
		goto cleanup;
	}

	if (!clip_image_preprocess(ctx, img_u8, img_f32)) {
		Errorf(E_FAIL, "clip preprocess failed for '%s'", path);
		goto cleanup;
	}

	int dim = clip_get_vision_hparams(ctx)->projection_dim;
	vec = malloc((size_t)dim * sizeof(float));
	if (!vec) {
		Errnof(E_CRIT, "malloc failed for embedding vector");
		goto cleanup;
	}

	if (!clip_image_encode(ctx, 4, img_f32, vec, true)) {
		Errorf(E_FAIL, "clip encode failed for '%s'", path);
		goto cleanup;
	}

	*dim_out = dim;
	ok = true;

cleanup:
	if (img_u8)  clip_image_u8_free(img_u8);
	if (img_f32) clip_image_f32_free(img_f32);
	if (!ok) {
		free(vec);
		vec = NULL;
	}
	return vec;
}

int query(char *query, struct clip_ctx *clip_ctx, PGconn *conn,
		char ***out_matches, int *out_count) {
	int rc = -1;
	char embedding_str[EMBED_STR_MAX];
	PGresult *res = NULL;
	char **match = NULL;
	int n_matches = 0;
	float *vec = NULL;
	struct clip_tokens tokens = {0}; // zeroed so clip_tokens_free is safe on early exit

	// Tokenize text
	if (!clip_tokenize(clip_ctx, query, &tokens)) {
		Errorf(E_FAIL, "failed to tokenize query");
		goto cleanup;
	}

	// the text encoder's buffers only fit CLIP_MAX_TOKENS, truncate like CLIP does
	if (tokens.size > CLIP_MAX_TOKENS) {
		tokens.data[CLIP_MAX_TOKENS - 1] = tokens.data[tokens.size - 1]; // end token
		tokens.size = CLIP_MAX_TOKENS;
	}

	// Encode text
	int vec_dim = clip_get_text_hparams(clip_ctx)->projection_dim;
	vec = malloc(sizeof(float) * (size_t)vec_dim);
	if (!vec) {
		Errnof(E_CRIT, "malloc failed for text vector");
		goto cleanup;
	}
	if (!clip_text_encode(clip_ctx, 4, &tokens, vec, true)) {
		Errorf(E_FAIL, "failed to encode query");
		goto cleanup;
	}

	// Convert vec to a pg array
	if (format_embedding(vec, vec_dim, embedding_str, EMBED_STR_MAX) < 0) {
		Errorf(E_FAIL, "failed to format query embedding (buffer too small)");
		goto cleanup;
	}

	// Compare embedding_str with pg database embeddings and get thumbnail paths
	char dist_buf[32], limit_buf[16];
	snprintf(dist_buf, sizeof dist_buf, "%.6f", 1.0 - similarity_threshold);
	snprintf(limit_buf, sizeof limit_buf, "%d", max_matches);

	const char *param_values[3] = { embedding_str, dist_buf, limit_buf };
	const char *sql =
		"SELECT thumbnail FROM wallpapers "
		"WHERE (embedding <=> $1::vector) < $2::float8 "
		"ORDER BY embedding <=> $1::vector "
		"LIMIT $3::int";

	res = PQexecParams(conn, sql, 3, NULL, param_values, NULL, NULL, 0);
	if (PQresultStatus(res) != PGRES_TUPLES_OK) {
		Errorf(E_FAIL, "similarity query failed: %s", PQerrorMessage(conn));
		goto cleanup;
	}

	n_matches = PQntuples(res);
	if (n_matches > 0) {
		match = calloc(n_matches, sizeof(char *));
		if (!match) {
			Errnof(E_CRIT, "calloc failed for match array");
			goto cleanup;
		}
		for (int i = 0; i < n_matches; i++) {
			match[i] = strdup(PQgetvalue(res, i, 0));
			if (!match[i]) {
				Errnof(E_CRIT, "strdup failed for matched thumbnail path");
				goto cleanup;
			}
		}
	}

	*out_matches = match; // may be NULL if n_matches == 0
	*out_count = n_matches;
	match = NULL; // ownership transferred to caller
	rc = 0;

cleanup:
	free(vec);
	if (match) {
		for (int i = 0; i < n_matches; i++)
			free(match[i]);
		free(match);
	}
	clip_tokens_free(&tokens);
	PQclear(res);
	return rc;
}

static int write_all(int fd, const char *buf, size_t len) {
	size_t total = 0;
	while (total < len) {
		ssize_t n = write(fd, buf + total, len - total);
		if (n < 0) {
			if (errno == EINTR)
				continue;
			return -1;
		}
		if (n == 0)
			return -1;
		total += (size_t)n;
	}
	return 0;
}

static int send_response(int client_fd, const char *status,
		const char *content_type, const char *body) {
	char header[512];
	char date[64];
	time_t now = time(NULL);
	struct tm tm;
	gmtime_r(&now, &tm);
	strftime(date, sizeof date, "%a, %d %b %Y %H:%M:%S GMT", &tm);

	size_t body_len = strlen(body);
	int header_len = snprintf(header, sizeof header,
			"HTTP/1.0 %s\r\n"
			"Date: %s\r\n"
			"Content-Type: %s\r\n"
			"Content-Length: %zu\r\n"
			"Connection: close\r\n\r\n",
			status, date, content_type, body_len);
	if (header_len < 0 || (size_t)header_len >= sizeof header)
		return -1;

	if (write_all(client_fd, header, (size_t)header_len) != 0)
		return -1;
	return write_all(client_fd, body, body_len);
}

static int hexval(int c)
{
	if (c >= '0' && c <= '9') return c - '0';
	if (c >= 'a' && c <= 'f') return c - 'a' + 10;
	if (c >= 'A' && c <= 'F') return c - 'A' + 10;
	return -1;
}

/*
 * Normalizes `s` in place: lowercases, turns runs of whitespace, control
 * characters, '-' and '_' into a single '-', and drops leading/trailing
 * separators. With `url_decode`, "%XX" escapes are decoded first and '+'
 * counts as a space.
 */
static void normalize_query(char *s, bool url_decode)
{
	char *w = s;
	bool pending_sep = false;

	for (char *r = s; *r; r++) {
		unsigned char c = (unsigned char)*r;

		if (url_decode && c == '%' &&
				hexval(r[1]) >= 0 && hexval(r[2]) >= 0) {
			c = (unsigned char)((hexval(r[1]) << 4) | hexval(r[2]));
			r += 2;
		}

		if ((url_decode && c == '+') || c == '-' || c == '_' ||
				isspace(c) || iscntrl(c)) {
			pending_sep = (w != s);   // drops leading separators
			continue;
		}
		if (pending_sep) {
			*w++ = '-';
			pending_sep = false;
		}
		*w++ = (char)tolower(c);
	}
	*w = '\0';
}

/*
 * Reads a request into `buf` (NUL-terminated, at most size-1 bytes) until the
 * client half-closes, or, for an HTTP GET, until the end of its headers.
 * Returns bytes read, or -1 on error or timeout.
 */
static ssize_t read_request(int fd, char *buf, size_t size) {
	size_t total = 0;

	while (total < size - 1) {
		ssize_t n = read(fd, buf + total, size - 1 - total);
		if (n < 0) {
			if (errno == EINTR)
				continue;
			return -1; // includes EAGAIN from SO_RCVTIMEO
		}
		if (n == 0)
			break;
		total += (size_t)n;

		// HTTP clients don't half-close, so stop at the end of the headers
		buf[total] = '\0';
		if (total >= 5 && strncmp(buf, "GET /", 5) == 0 &&
				(strstr(buf, "\r\n\r\n") || strstr(buf, "\n\n")))
			break;
	}

	// Oversized request: discard the rest (bounded) so close() doesn't send
	// an RST that makes the client lose our reply with ECONNRESET.
	if (total == size - 1) {
		char discard[BUFFER_SIZE];
		size_t dropped = 0;
		while (dropped < DISCARD_MAX) {
			ssize_t n = read(fd, discard, sizeof discard);
			if (n < 0 && errno == EINTR)
				continue;
			if (n <= 0)
				break;
			dropped += (size_t)n;
		}
	}

	buf[total] = '\0';
	return (ssize_t)total;
}

static void handle_client(int client_fd, struct clip_ctx *clip_ctx, PGconn *conn) {
	bool is_http = false;
	const char *err_status = NULL; // set before goto cleanup on HTTP failures
	char read_buffer[BUFFER_SIZE];
	char **matches = NULL;
	char *q = NULL;
	char *body = NULL;
	int count = 0;

	struct timeval tv = { .tv_sec = 5 };
	setsockopt(client_fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
	setsockopt(client_fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);

	ssize_t rc = read_request(client_fd, read_buffer, sizeof(read_buffer));
	if (rc <= 0) {
		if (rc < 0)
			Errnof(E_WARN, "read() failed");
		goto cleanup;
	}

	if (strncmp("GET /", read_buffer, 5) == 0) {
		is_http = true;

		char *start = read_buffer + 5;
		char *end = strstr(start, " HTTP");
		if (!end) {
			err_status = "400 Bad Request";
			goto cleanup;
		}

		q = strndup(start, (size_t)(end - start));
	} else {
		q = strdup(read_buffer);
	}

	if (!q) {
		Errnof(E_WARN, "strdup() failed");
		err_status = "500 Internal Server Error";
		goto cleanup;
	}

	normalize_query(q, is_http);
	if (q[0] == '\0') {
		err_status = "400 Bad Request";
		goto cleanup;
	}

	if (query(q, clip_ctx, conn, &matches, &count) != 0) {
		err_status = "500 Internal Server Error";
		goto cleanup;
	}

	if (is_http) {
		size_t len = 0;
		FILE *f = open_memstream(&body, &len);
		if (!f) {
			Errnof(E_CRIT, "open_memstream failed");
			err_status = "500 Internal Server Error";
			goto cleanup;
		}
		for (int i = 0; i < count; i++)
			fprintf(f, "%s\n", matches[i]);
		fclose(f);

		if (send_response(client_fd, "200 OK", "text/plain; charset=utf-8", body))
			Errorf(E_CRIT, "send_response failed"); // can't report to client
	} else {
		for (int i = 0; i < count; i++)
			if (dprintf(client_fd, "%s\n", matches[i]) <= 0) {
				Errnof(E_CRIT, "dprintf failed");
				goto cleanup;
			}
	}

cleanup:
	if (is_http && err_status)
		send_response(client_fd, err_status, "text/plain; charset=utf-8",
				err_status);
	if (matches) {
		for (int i = 0; i < count; i++)
			free(matches[i]);
		free(matches);
	}
	free(body);
	free(q);
	close(client_fd);
}

/*
 * Reads the file at `path` (whose size is already known as `fsize`), hashes
 * it with SHA-256 into `md_out`/`md_len_out`, and detects its MIME type into
 * a malloc'd string at `*mime_type_out` (caller frees).
 */
static int compute_checksum_and_mime(const char *path, size_t fsize,
		unsigned char *md_out, unsigned int *md_len_out,
		char **mime_type_out) {
	int rc = -1;
	FILE *f = NULL;
	unsigned char *img_buf = NULL;
	magic_t mime = NULL;

	f = fopen(path, "rb");
	if (!f) {
		Errnof(E_FAIL, "fopen failed for '%s'", path);
		goto cleanup;
	}

	img_buf = malloc(fsize);
	if (!img_buf || fread(img_buf, 1, fsize, f) != fsize) {
		Errorf(E_FAIL, "failed to read '%s'", path);
		goto cleanup;
	}
	fclose(f);
	f = NULL;

	if (!EVP_Digest(img_buf, fsize, md_out, md_len_out, EVP_sha256(), NULL)) {
		Errorf(E_FAIL, "EVP_Digest failed for '%s'", path);
		goto cleanup;
	}

	mime = magic_open(MAGIC_MIME_TYPE);
	if (!mime) {
		Errorf(E_FAIL, "magic_open failed for '%s'", path);
		goto cleanup;
	}
	if (magic_load(mime, NULL) != 0) {
		Errorf(E_FAIL, "magic_load failed for '%s': %s", path,
				magic_error(mime));
		goto cleanup;
	}

	const char *mime_result = magic_buffer(mime, img_buf, fsize);
	if (!mime_result) {
		Errorf(E_FAIL, "magic_file failed for '%s': %s", path,
				magic_error(mime));
		goto cleanup;
	}

	// magic_buffer's return value is owned by the magic_t handle and becomes
	// invalid once it is closed, so make our own copy before closing.
	*mime_type_out = strdup(mime_result);
	if (!*mime_type_out) {
		Errnof(E_CRIT, "strdup failed for mime_type");
		goto cleanup;
	}

	rc = 0;

cleanup:
	if (f)
		fclose(f);
	free(img_buf);
	if (mime)
		magic_close(mime);
	return rc;
}

// Checks whether an image with checksum `md` is already in the database.
// Returns 1 if it's a duplicate, 0 if not, -1 on error.
static int check_duplicate(PGconn *conn, const unsigned char *md,
		const char *path) {
	int rc = -1;
	PGresult *res = NULL;
	const char *param_values[1] = {(const char *)md};
	int param_lengths[1] = {32};
	int param_formats[1] = {1}; // binary

	res = PQexecParams(conn, "SELECT 1 FROM wallpapers WHERE checksum = $1",
			1, NULL, param_values, param_lengths, param_formats, 0);
	if (PQresultStatus(res) != PGRES_TUPLES_OK) {
		Errorf(E_FAIL, "duplicate check failed for '%s': %s", path,
				PQerrorMessage(conn));
		goto cleanup;
	}

	rc = (PQntuples(res) > 0) ? 1 : 0;

cleanup:
	PQclear(res);
	return rc;
}

// Formats `created_at` into `out` (of size `out_size`) as a Postgres
// timestamp string, falling back to 'now' if the conversion fails.
static void format_created_at(time_t created_at, char *out, size_t out_size) {
	struct tm tm_created;
	if (!localtime_r(&created_at, &tm_created) ||
			!strftime(out, out_size, "%Y-%m-%d %H:%M:%S", &tm_created)) {
		Errorf(E_WARN, "failed to format created_at, using current time");
		snprintf(out, out_size, "now"); // Postgres understands 'now'
	}
}

/*
 * Creates a resized thumbnail (fits within 640x360, never upscaled) for the
 * image at `path` inside a `.thumbnails` directory next to it. The thumbnail
 * keeps the source's file name, so ImageMagick writes it in the same format.
 * On success, fills *width_out and *height_out with the source image's original
 * dimensions and returns a malloc'd thumbnail path (caller frees). Returns
 * NULL on error.
 */
static char *generate_thumbnail(const char *path, size_t *width_out,
		size_t *height_out) {
	char *thumbnail = NULL, *path_copy1 = NULL, *path_copy2 = NULL;
	MagickWand *magick_wand = NULL;
	int ok = 0;

	path_copy1 = strdup(path);
	path_copy2 = strdup(path);
	if (!path_copy1 || !path_copy2) {
		Errnof(E_CRIT, "strdup failed for path");
		goto cleanup;
	}
	char *fname = basename(path_copy1);
	char *fdir = dirname(path_copy2);

	char thumb_dir[PATH_MAX];
	snprintf(thumb_dir, sizeof(thumb_dir), "%s/.thumbnails", fdir);
	if (mkdir(thumb_dir, 0755) != 0 && errno != EEXIST) {
		Errnof(E_FAIL, "mkdir failed for '%s'", thumb_dir);
		goto cleanup;
	}

	size_t thumb_len =
		strlen(fdir) + strlen("/.thumbnails/") + strlen(fname) + 1;
	thumbnail = malloc(thumb_len);
	if (!thumbnail) {
		Errnof(E_CRIT, "malloc failed for thumbnail path");
		goto cleanup;
	}

	// Same file name (hence same extension) as the source, so the thumbnail
	// is written in the source's format.
	snprintf(thumbnail, thumb_len, "%s/.thumbnails/%s", fdir, fname);

	magick_wand = NewMagickWand();
	if (MagickReadImage(magick_wand, path) == MagickFalse) {
		ExceptionType severity;
		char *magick_err = MagickGetException(magick_wand, &severity);
		Errorf(E_FAIL, "MagickReadImage failed for '%s': %s", path,
				magick_err);
		magick_err = (char *)MagickRelinquishMemory(magick_err);
		goto cleanup;
	}

	*width_out = MagickGetImageWidth(magick_wand);
	*height_out = MagickGetImageHeight(magick_wand);

	MagickResetIterator(magick_wand);
	while (MagickNextImage(magick_wand) != MagickFalse) {
		double scale = fmin(640.0 / (double)*width_out,
				360.0 / (double)*height_out);
		if (scale > 1.0)
			scale = 1.0;

		size_t new_w = (size_t)(*width_out * scale);
		size_t new_h = (size_t)(*height_out * scale);
		if (new_w == 0)
			new_w = 1;
		if (new_h == 0)
			new_h = 1;

		if (MagickResizeImage(magick_wand, new_w, new_h, LanczosFilter) ==
				MagickFalse) {
			ExceptionType severity;
			char *magick_err = MagickGetException(magick_wand, &severity);
			Errorf(E_FAIL, "MagickResizeImage failed for '%s': %s", path,
					magick_err);
			magick_err = (char *)MagickRelinquishMemory(magick_err);
			goto cleanup;
		}
	}

	if (MagickWriteImages(magick_wand, thumbnail, MagickTrue) == MagickFalse) {
		ExceptionType severity;
		char *magick_err = MagickGetException(magick_wand, &severity);
		Errorf(E_FAIL, "MagickWriteImages failed for '%s': %s", path,
				magick_err);
		magick_err = (char *)MagickRelinquishMemory(magick_err);
		goto cleanup;
	}

	ok = 1;

cleanup:
	free(path_copy1);
	free(path_copy2);
	if (magick_wand)
		DestroyMagickWand(magick_wand);
	if (!ok) {
		free(thumbnail);
		thumbnail = NULL;
	}
	return thumbnail;
}

// Returns a malloc'd title built from the file's basename with its
// extension stripped (caller frees), or NULL on error.
static char *build_title(const char *path) {
	char *path_copy = strdup(path);
	if (!path_copy) {
		Errnof(E_CRIT, "strdup failed building title");
		return NULL;
	}
	char *title_raw = basename(path_copy);
	char *dot = strrchr(title_raw, '.');
	if (dot && dot != title_raw)
		*dot = '\0';
	char *title = strdup(title_raw);
	free(path_copy);
	if (!title)
		Errnof(E_CRIT, "strdup failed building title");
	return title;
}

// Returns a malloc'd, lowercase, hyphen-separated slug built from `title`
// (caller frees), or NULL on error.
static char *slugify(const char *title) {
	size_t title_len = strlen(title);
	char *slug = malloc(title_len + 16); // room for the "untitled" fallback
	if (!slug) {
		Errnof(E_CRIT, "malloc failed for slug");
		return NULL;
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
	if (out == slug)
		strcpy(slug, "untitled");
	return slug;
}

/*
 * Checks `slug` against existing slugs and, if it collides, returns a new
 * malloc'd slug with a "-N" suffix appended to make it unique. Takes
 * ownership of `slug` either way (it is freed if a new one is allocated, or
 * on error) and returns the resulting slug (caller frees), or NULL on error.
 */
static char *resolve_slug_collision(PGconn *conn, char *slug,
		const char *path) {
	PGresult *check_res = NULL;
	const char *check_sql =
		"SELECT slug FROM wallpapers WHERE slug = $1 OR slug LIKE $1 || '-%'";
	const char *check_params[1] = {slug};

	check_res = PQexecParams(conn, check_sql, 1, NULL, check_params, NULL,
			NULL, 0);
	if (PQresultStatus(check_res) != PGRES_TUPLES_OK) {
		Errorf(E_FAIL, "slug collision query failed for '%s': %s", path,
				PQerrorMessage(conn));
		PQclear(check_res);
		free(slug);
		return NULL;
	}

	bool taken = false;
	int max_n = 1; // first suffix handed out is -1
	size_t base_len = strlen(slug);

	for (int i = 0; i < PQntuples(check_res); i++) {
		const char *existing = PQgetvalue(check_res, i, 0);

		if (strcmp(existing, slug) == 0) {
			taken = true;
			continue;
		}

		// only "<slug>-<digits>" counts as a numbered variant
		if (strncmp(existing, slug, base_len) == 0 &&
				existing[base_len] == '-') {
			const char *num = existing + base_len + 1;
			char *end;
			long n = strtol(num, &end, 10);
			if (*num && *end == '\0' && n >= max_n && n < INT_MAX - 1)
				max_n = (int)n + 1;
		}
	}
	PQclear(check_res);

	if (!taken)
		return slug;

	size_t new_len = base_len + 16;
	char *new_slug = malloc(new_len);
	if (!new_slug) {
		Errnof(E_CRIT, "malloc failed building unique slug");
		free(slug);
		return NULL;
	}
	snprintf(new_slug, new_len, "%s-%d", slug, max_n);
	free(slug);
	return new_slug;
}

// Inserts the wallpaper row.
static int insert_wallpaper(PGconn *conn, const struct image_ctx *img,
		const char *title, const char *slug, const char *thumbnail,
		const char *mime_type, size_t width, size_t height,
		const unsigned char *md, const char *embedding_str,
		const char *created_str) {
	int rc = -1;
	PGresult *res_insert = NULL;
	char fsize_str[32], width_str[32], height_str[32];
	snprintf(fsize_str, sizeof(fsize_str), "%zu", img->entry->fsize);
	snprintf(width_str, sizeof(width_str), "%zu", width);
	snprintf(height_str, sizeof(height_str), "%zu", height);

	const char *param_values[11] = {
		title,     slug,          img->entry->path, thumbnail,
		mime_type, fsize_str,     width_str,        height_str,
		(const char *)md, embedding_str, created_str,
	};
	int param_lengths[11] = {0, 0, 0, 0, 0, 0, 0, 0, 32, 0, 0};
	int param_formats[11] = {0, 0, 0, 0, 0, 0, 0, 0, 1, 0, 0}; // md is binary

	const char *insert =
		"INSERT INTO wallpapers "
		"(title, slug, image, thumbnail, mime_type, file_size, width, "
		"height, checksum, embedding, created_at) "
		"VALUES ($1,$2,$3,$4,$5,$6,$7,$8,$9,$10,$11)";

	res_insert = PQexecParams(conn, insert, 11, NULL, param_values,
			param_lengths, param_formats, 0);

	if (PQresultStatus(res_insert) != PGRES_COMMAND_OK) {
		Errorf(E_FAIL, "insert failed for '%s': %s", img->entry->path,
				PQresultErrorMessage(res_insert));
		goto cleanup;
	}

	rc = 0;

cleanup:
	PQclear(res_insert);
	return rc;
}

int process_img(struct image_ctx *img) {
	int rc = -1;
	unsigned char md[32];
	unsigned int md_len;
	char embedding_str[EMBED_STR_MAX];
	char created_str[32];
	size_t width = 0, height = 0;
	int dim = 0;
	float *vec = NULL;
	char *mime_type = NULL;
	char *thumbnail = NULL;
	char *title = NULL;
	char *slug = NULL;

	if (compute_checksum_and_mime(img->entry->path, img->entry->fsize, md,
				&md_len, &mime_type) != 0)
		goto cleanup;

	int dup = check_duplicate(img->conn, md, img->entry->path);
	if (dup < 0)
		goto cleanup;
	if (dup > 0) {
		Errorf(E_WARN, "Skipping %s, duplicate.", img->entry->path);
		rc = 1;
		goto cleanup;
	}

	// preprocessing and encoding
	vec = encode_image_embedding(img->clip_ctx, img->entry->path, &dim);
	if (!vec)
		goto cleanup;

	if (format_embedding(vec, dim, embedding_str, sizeof(embedding_str)) != 0) {
		Errorf(E_FAIL, "embedding string buffer too small for '%s'",
				img->entry->path);
		goto cleanup;
	}

	format_created_at(img->entry->created_at, created_str, sizeof(created_str));

	thumbnail = generate_thumbnail(img->entry->path, &width, &height);
	if (!thumbnail)
		goto cleanup;

	title = build_title(img->entry->path);
	if (!title)
		goto cleanup;

	slug = slugify(title);
	if (!slug)
		goto cleanup;

	slug = resolve_slug_collision(img->conn, slug, img->entry->path);
	if (!slug) // already freed by resolve_slug_collision on failure
		goto cleanup;

	if (insert_wallpaper(img->conn, img, title, slug, thumbnail, mime_type,
				width, height, md, embedding_str, created_str) != 0)
		goto cleanup;

	rc = 0;

cleanup:
	if (rc < 0 && thumbnail)
		unlink(thumbnail); // don't leave an orphan if a later step failed
	free(mime_type);
	free(thumbnail);
	free(title);
	free(slug);
	free(vec);
	return rc;
}

struct file_list *file_list_new(int count) {
	struct file_list *list = malloc(sizeof(*list));
	if (!list) {
		Errnof(E_CRIT, "malloc failed");
		goto cleanup;
	}
	list->count = count;
	list->entries = calloc(count, sizeof(*list->entries));
	if (!list->entries) {
		Errnof(E_FAIL, "calloc failed");
		goto cleanup;
	}

	return list;

cleanup:
	free(list);
	return NULL;
}

int create_socket(void) {
	int fd = socket(AF_UNIX, SOCK_STREAM, 0);
	if (fd < 0) {
		Errnof(E_CRIT, "socket() failed");
		return -1;
	}
	return fd;
}

int bind_socket(int fd, char *socket_path) {
	int rc = -1;
	struct sockaddr_un addr;
	memset(&addr, 0, sizeof(addr));

	if (strlen(socket_path) >= sizeof(addr.sun_path)) {
		Errorf(E_CRIT, "socket path too long");
		goto cleanup;
	}

	addr.sun_family = AF_UNIX;
	strncpy(addr.sun_path, socket_path, sizeof(addr.sun_path) - 1);

	// refuse to start if a live server already owns the socket
	int probe = socket(AF_UNIX, SOCK_STREAM, 0);
	if (probe >= 0) {
		if (connect(probe, (struct sockaddr *)&addr, sizeof(addr)) == 0) {
			close(probe);
			Errorf(E_CRIT, "server already running on '%s'", socket_path);
			goto cleanup;
		}
		close(probe);
	}
	unlink(socket_path); // stale socket

	if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
		Errnof(E_CRIT, "bind() failed");
		goto cleanup;
	}

	rc = 0;

cleanup:
	return rc;
}

int listen_socket(int fd) {
	if (listen(fd, SOMAXCONN) < 0) {
		Errnof(E_CRIT, "listen() failed");
		return -1;
	}
	return 0;
}

int connect_socket(int fd, const char *socket_path) {
	struct sockaddr_un addr;
	memset(&addr, 0, sizeof(addr));
	addr.sun_family = AF_UNIX;
	strncpy(addr.sun_path, socket_path, sizeof(addr.sun_path) - 1);

	if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
		Errnof(E_CRIT, "connect() failed");
		return -1;
	}
	return 0;
}

int write_query_socket(char *query, int fd) {
	if (write_all(fd, query, strlen(query)) != 0) {
		Errnof(E_CRIT, "Failed to write to socket.");
#if !defined(DEBUG)
		fprintf(stderr, "wallclip: Failed to write to socket.\n");
#endif
		return 1;
	}
	return 0;
}

/*
 * Loads the .env file (WALLCLIP_ENV_FILE, default ".env") and applies the
 * WALLCLIP_* settings over the defaults from wallclip.h. Shell environment
 * variables win over the .env file. -v flags (`cli_verbosity`) add to the
 * configured verbosity, capped at 3. Returns 0 on success, -1 on error.
 */
static int load_config(int cli_verbosity) {
	int base_verbosity = verbosity;

	verbosity = cli_verbosity; // so -v also shows the loader's own messages
	const char *env_file = getenv("WALLCLIP_ENV_FILE");
	if (env_load_file(env_file && *env_file ? env_file : ".env") < 0)
		return -1;

	socket_path = env_get_str("WALLCLIP_SOCKET_PATH", socket_path);
	model_path = env_get_str("WALLCLIP_MODEL_PATH", model_path);
	similarity_threshold = (float)env_get_double("WALLCLIP_SIMILARITY_THRESHOLD",
			similarity_threshold, 0.0, 1.0);
	max_matches = env_get_int("WALLCLIP_MAX_MATCHES", max_matches, 1, 100000);

	verbosity = env_get_int("WALLCLIP_VERBOSITY", base_verbosity, 0, 3) +
		cli_verbosity;
	if (verbosity > 3)
		verbosity = 3;
	return 0;
}

int main(int argc, char *argv[]) {
	int rc = -1;
	bool is_server = false;
	bool is_query = false;
	bool wand_ready = false;
	int c;
	int nfiles;
	const char *conflict = NULL;
	char *query_str = NULL;
	PGconn *conn = NULL;
	struct clip_ctx *clip_ctx = NULL;
	struct file_list *file_list = NULL;
	char *sockp = NULL;
	int cli_verbosity = 0;

	while ((c = getopt_long(argc, argv, "d:q:vh", long_options, NULL)) != -1) {
		switch (c) {
			case 'd':
				/* TODO: implement delete */
				fprintf(stderr, "wallclip: --delete/-d is not implemented yet\n");
				rc = -2;
				goto cleanup;
			case 'q':
				is_query = true;
				query_str = optarg;
				break;
			case 'v':
				if (cli_verbosity < 3)
					cli_verbosity++;
				break;
			case 'h':
				usage(stdout);
				rc = 0;
				goto cleanup;
			case '?':
				/* getopt already printed the specific error */
				usage(stderr);
				rc = -2;
				goto cleanup;
			default:
				fprintf(stderr, "wallclip: unexpected getopt code 0%o\n", c);
				rc = -2;
				goto cleanup;
		}
	}

	/* remaining non-option args are the files to process */
	nfiles = argc - optind;
	is_server = !is_query && nfiles == 0;

	if (is_query && nfiles > 0) {
		conflict = "--query cannot be combined with file arguments";
		fprintf(stderr, "wallclip: %s\n", conflict);
		usage(stderr);
		rc = -2;
		goto cleanup;
	}

	if (load_config(cli_verbosity) < 0)
		goto cleanup;

	sockp = strdup(socket_path);
	if (!sockp) {
		Errnof(E_CRIT, "strdup failed");
		goto cleanup;
	}

	if (nfiles > 0) {
		MagickWandGenesis();
		wand_ready = true;
		file_list = file_list_new(nfiles);
		if (!file_list)
			goto cleanup;
	}

	for (int i = 0; i < nfiles; i++) {
		const char *path = argv[optind + i];
		struct stat st;
		if (stat(path, &st) != 0) {
			Errnof(E_FAIL, "%s:", path);
			goto cleanup;
		}

		if (!S_ISREG(st.st_mode) || st.st_size == 0) {
			Errorf(E_FAIL, "%s: not a non-empty regular file", path);
			goto cleanup;
		}
		struct file_entry *e = &file_list->entries[i];
		char *resolved_path = realpath(path, NULL);
		if (!resolved_path) {
			Errnof(E_FAIL, "%s:", path);
			goto cleanup;
		}
		e->path = resolved_path;
		e->fsize = st.st_size;
		e->modified_at = st.st_mtime;

		struct statx stx;
		if (statx(AT_FDCWD, path, 0, STATX_BTIME, &stx) == 0 &&
				(stx.stx_mask & STATX_BTIME)) {
			e->created_at = stx.stx_btime.tv_sec;
		} else {
			e->created_at = e->modified_at; /* fallback for filesystems
											   without birth time */
		}
	}

	if (!is_query && init_setup(&conn, &clip_ctx) < 0)
		goto cleanup;

	if (is_server) {
		signal(SIGPIPE, SIG_IGN); // client hangups surface as EPIPE, not a kill

		int fd = create_socket();
		if (fd < 0) {
			goto cleanup;
		}
		if (bind_socket(fd, sockp) < 0 || listen_socket(fd) < 0) {
			close(fd);
			goto cleanup;
		}

		while (1) {
			int client_fd = accept(fd, NULL, NULL);
			if (client_fd < 0) {
				if (errno == EINTR)
					continue;
				Errnof(E_CRIT, "accept() failed");
				break;
			}
			handle_client(client_fd, clip_ctx, conn);
		}

		close(fd);
		unlink(sockp);
		rc = -1; // the loop only exits on error
		goto cleanup;
	}

	if (is_query) {
		int fd = create_socket();
		if (fd < 0) {
			goto cleanup;
		}

		if (connect_socket(fd, sockp) < 0) {
			close(fd);
			goto cleanup;
		}

		if (write_query_socket(query_str, fd) != 0) {
			close(fd);
			goto cleanup;
		}

		shutdown(fd, SHUT_WR); // signal end of request

		char buf[BUFFER_SIZE];
		while (1) {
			ssize_t n = read(fd, buf, sizeof(buf));
			if (n == 0)
				break; // server closed, done
			if (n < 0) {
				if (errno == EINTR)
					continue;
				Errnof(E_CRIT, "read() from socket failed");
				close(fd);
				goto cleanup;
			}
			if (fwrite(buf, 1, (size_t)n, stdout) != (size_t)n) {
				Errnof(E_CRIT, "fwrite to stdout failed");
				close(fd);
				goto cleanup;
			}
		}

		close(fd);
		rc = 0;
		goto cleanup;
	}

	int failed = 0;
	struct image_ctx img = {.clip_ctx = clip_ctx, .conn = conn};
	for (int i = 0; i < file_list->count; i++) {
		img.entry = &file_list->entries[i];
		int prc = process_img(&img);
		if (prc < 0) {
			puts(img.entry->path);
			failed++;
		}
	}

	rc = failed ? 1 : 0;
cleanup:
	free(sockp);
	env_free(); // zeroes any secrets still in memory
	if (wand_ready)
		MagickWandTerminus();
	if (clip_ctx)
		clip_free(clip_ctx);
	if (conn)
		PQfinish(conn);
	if (file_list) {
		for (int i = 0; i < file_list->count; i++)
			free(file_list->entries[i].path);
		free(file_list->entries);
		free(file_list);
	}
	return rc == 0 ? EXIT_SUCCESS : (rc == -2 ? 2 : 1); // 2 = usage error
}
