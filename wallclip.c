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
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

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

int init_startup(PGconn **conn, struct clip_ctx **clip_ctx)
{
	int rc = 0;

	*conn = PQconnectdb(conninfo);
	if (PQstatus(*conn) != CONNECTION_OK) {
		Errorf(E_FAIL, "connection failed: %s", PQerrorMessage(*conn));
		rc = -1;
	}

	*clip_ctx = clip_model_load(CLIP_MODEL_PATH, 0);
	if (!*clip_ctx) {
		Errorf(E_CRIT, "failed to load clip model\n");
		rc = -1;
	}

	return rc;
}

int create_socket() {
	struct sockaddr_un addr;
	memset(&addr, 0, sizeof(addr));

	if (strlen(socket_path) >= sizeof(addr.sun_path)) {
		Errorf(E_CRIT, "socket path too long");
		return -1;
	}

	char socket_dir[PATH_MAX];
	strncpy(socket_dir, socket_path, sizeof(socket_dir) - 1);
	socket_dir[sizeof(socket_dir) - 1] = '\0';
	mkdir(dirname(socket_dir), 0700);

	addr.sun_family = AF_UNIX;
	strncpy(addr.sun_path, socket_path, sizeof(addr.sun_path) - 1);

	int fd = socket(AF_UNIX, SOCK_STREAM, 0);
	if (fd < 0) {
		Errnof(E_CRIT, "socket() failed");
		return -1;
	}

	unlink(socket_path); // clear stale socket

	if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
		Errnof(E_CRIT, "bind() failed");
		close(fd);
		return -1;
	}

	if (listen(fd, SOMAXCONN) < 0) {
		Errnof(E_CRIT, "listen() failed");
		close(fd);
		return -1;
	}

	while (1) {
		int client_fd = accept(fd, NULL, NULL);
		if (client_fd < 0) {
			if (errno == EINTR)
				continue;
			break;
		}
		// todo: client handling
		close(client_fd);
	}

	close(fd);
	return 0;
}

int query(char *query, struct clip_ctx *clip_ctx, PGconn *conn) {
	int rc = -1;
	int n_matches = 0;
	char embedding_str[EMBED_STR_MAX];
	struct image_ctx img;
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
		goto cleanup;
	}
	offset += written;

	for (int i = 0; i < vec_dim; i++) {
		written = snprintf(embedding_str + offset, EMBED_STR_MAX - offset, "%s%.6f",
				(i == 0) ? "" : ",", vec[i]);
		if (written < 0 || (size_t)written >= EMBED_STR_MAX - offset) {
			Errorf(E_CRIT, "Failed to format embedding vector (buffer too small)");
			goto cleanup;
		}
		offset += written;
	}

	written = snprintf(embedding_str + offset, EMBED_STR_MAX - offset, "]");
	if (written < 0 || (size_t)written >= EMBED_STR_MAX - offset) {
		Errorf(E_CRIT, "Failed to format embedding vector (buffer too small)");
		goto cleanup;
	}

	// Compare embedding_str with pg database embeddings and get thumbnail paths
	const char *param_values[1] = {embedding_str};
	const char *sql = "SELECT thumbnail FROM wallpapers "
		"WHERE 1 - (embedding <=> $1::vector(512)) > 0.27 "
		"ORDER BY embedding <=> $1::vector(512)";

	PGresult *res = PQexecParams(conn, sql, 1, NULL, param_values, NULL, NULL, 0);
	if (PQresultStatus(res) != PGRES_TUPLES_OK) {
		Errorf(E_FAIL, "similarity query failed: %s", PQerrorMessage(conn));
		goto cleanup;
	}

	n_matches = PQntuples(res);
	char **match = NULL;
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

	for (int i = 0; i < n_matches; i++) {
		printf("%s\n", match[i]);
	}

	rc = 0;
cleanup:
	for (int i = 0; i < n_matches; i++) {
		free(match[i]);
	}
	if(match)
		free(match);
	clip_tokens_free(&tokens);
	PQclear(res);
	return rc;
}

int process_img(struct image_ctx *img) {
	int rc = -1;
	char md[32];
	char embedding_str[EMBED_STR_MAX];
	unsigned int md_len;
	char created_str[32];
	size_t width = 0, height = 0;

	// everything that needs releasing lives here
	FILE *f = NULL;
	unsigned char *img_buf = NULL;
	magic_t mime = NULL;
	char *mime_type = NULL;
	PGresult *res = NULL;
	struct clip_image_u8 *img0 = NULL;
	struct clip_image_f32 *img_res = NULL;
	char *path_copy1 = NULL, *path_copy2 = NULL, *fname_noext = NULL;
	char *thumbnail = NULL;
	MagickWand *magick_wand = NULL;
	char *title = NULL;
	char *slug = NULL;
	PGresult *check_res = NULL;
	PGresult *res_insert = NULL;

	// checksum + mime type
	{
		f = fopen(img->entry->path, "rb");
		if (!f) {
			Errnof(E_FAIL, "fopen failed for '%s'", img->entry->path);
			goto cleanup;
		}

		// fsize is already known from the upfront stat() in main, no need to
		// fseek/ftell here.
		size_t size_f = img->entry->fsize;

		img_buf = malloc(size_f);
		if (!img_buf || fread(img_buf, 1, size_f, f) != size_f) {
			Errorf(E_FAIL, "failed to read '%s'", img->entry->path);
			goto cleanup;
		}
		fclose(f);
		f = NULL;

		if (!EVP_Digest(img_buf, size_f, (unsigned char *)md, &md_len, EVP_sha256(),
					NULL)) {
			Errorf(E_FAIL, "EVP_Digest failed for '%s'", img->entry->path);
			goto cleanup;
		}

		mime = magic_open(MAGIC_MIME_TYPE);
		if (!mime) {
			Errorf(E_FAIL, "magic_open failed for '%s'", img->entry->path);
			goto cleanup;
		}
		if (magic_load(mime, NULL) != 0) {
			Errorf(E_FAIL, "magic_load failed for '%s': %s", img->entry->path,
					magic_error(mime));
			goto cleanup;
		}

		const char *mime_result = magic_buffer(mime, img_buf, size_f);
		if (!mime_result) {
			Errorf(E_FAIL, "magic_file failed for '%s': %s", img->entry->path,
					magic_error(mime));
			goto cleanup;
		}

		// magic_buffer's return value is owned by the magic_t handle and becomes
		// invalid once it is closed, so make our own copy before closing.
		mime_type = strdup(mime_result);
		magic_close(mime);
		mime = NULL;
		free(img_buf);
		img_buf = NULL;
		if (!mime_type) {
			Errnof(E_CRIT, "strdup failed for mime_type");
			goto cleanup;
		}
	}

	// deduplication check using checksum
	{
		const char *param_values[1];
		int param_lengths[1];
		int param_formats[1];

		param_values[0] = (const char *)md;
		param_lengths[0] = 32;
		param_formats[0] = 1; // binary
		res = PQexecParams(img->conn,
				"SELECT 1 FROM wallpapers WHERE checksum = $1", 1, NULL,
				param_values, param_lengths, param_formats, 0);
		if (PQresultStatus(res) != PGRES_TUPLES_OK) {
			Errorf(E_FAIL, "duplicate check failed for '%s': %s",
					img->entry->path, PQerrorMessage(img->conn));
			goto cleanup;
		}
		if (PQntuples(res) > 0) {
			Errorf(E_WARN, "Skipping %s, duplicate.", img->entry->path);
			rc = 1;
			goto cleanup;
		}
		PQclear(res);
		res = NULL;
	}

	int vec_dim = clip_get_vision_hparams(img->clip_ctx)->projection_dim;

	{
		size_t offset = 0;
		int written;

		// Load image from disk
		img0 = clip_image_u8_make();
		if (!clip_image_load_from_file(img->entry->path, img0)) {
			Errnof(E_FAIL, "failed to load image from '%s'", img->entry->path);
			goto cleanup;
		}
		// Preprocess image
		img_res = clip_image_f32_make();
		if (!clip_image_preprocess(img->clip_ctx, img0, img_res)) {
			Errorf(E_FAIL, "failed to preprocess image '%s'", img->entry->path);
			goto cleanup;
		}
		// Encode image
		float img_vec[vec_dim];
		if (!clip_image_encode(img->clip_ctx, 4, img_res, img_vec, true)) {
			Errorf(E_FAIL, "failed to encode image '%s'", img->entry->path);
			goto cleanup;
		}
		clip_image_u8_free(img0);
		img0 = NULL;
		clip_image_f32_free(img_res);
		img_res = NULL;

		written = snprintf(embedding_str + offset, EMBED_STR_MAX - offset, "[");
		if (written < 0 || (size_t)written >= EMBED_STR_MAX - offset) {
			Errorf(E_CRIT,
					"failed to format embedding vector for '%s' (buffer too small)",
					img->entry->path);
			goto cleanup;
		}
		offset += written;

		for (int i = 0; i < vec_dim; i++) {
			written = snprintf(embedding_str + offset, EMBED_STR_MAX - offset,
					"%s%.6f", (i == 0) ? "" : ",", img_vec[i]);
			if (written < 0 || (size_t)written >= EMBED_STR_MAX - offset) {
				Errorf(E_CRIT,
						"failed to format embedding vector for '%s' (buffer too small)",
						img->entry->path);
				goto cleanup;
			}
			offset += written;
		}

		written = snprintf(embedding_str + offset, EMBED_STR_MAX - offset, "]");
		if (written < 0 || (size_t)written >= EMBED_STR_MAX - offset) {
			Errorf(E_CRIT,
					"failed to format embedding vector for '%s' (buffer too small)",
					img->entry->path);
			goto cleanup;
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
		path_copy1 = strdup(img->entry->path);
		path_copy2 = strdup(img->entry->path);
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
			strlen(fdir) + strlen("/.thumbnails/") + strlen(fname) + 16;
		thumbnail = malloc(thumb_len);
		if (!thumbnail) {
			Errnof(E_CRIT, "malloc failed for thumbnail path");
			goto cleanup;
		}
		// Thumbnails are always re-encoded as JPEG below (MagickSetImageFormat),
		// so name the thumbnail file .jpg regardless of the source image's own
		// extension. If a thumbnail with that base name already exists (e.g. two
		// source files sharing a name but differing in extension), suffix with
		// -1, -2, ... until a free path is found.
		fname_noext = strdup(fname);
		if (!fname_noext) {
			Errnof(E_CRIT, "strdup failed building thumbnail path");
			goto cleanup;
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

		magick_wand = NewMagickWand();
		if (MagickReadImage(magick_wand, img->entry->path) == MagickFalse) {
			ExceptionType severity;
			char *magick_err = MagickGetException(magick_wand, &severity);
			Errorf(E_FAIL, "MagickReadImage failed for '%s': %s", img->entry->path,
					magick_err);
			magick_err = (char *)MagickRelinquishMemory(magick_err);
			goto cleanup;
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
				magick_err = (char *)MagickRelinquishMemory(magick_err);
				goto cleanup;
			}
		}

		if (MagickSetImageFormat(magick_wand, "JPEG") == MagickFalse) {
			ExceptionType severity;
			char *magick_err = MagickGetException(magick_wand, &severity);
			Errorf(E_FAIL, "MagickSetImageFormat failed for '%s': %s",
					img->entry->path, magick_err);
			magick_err = (char *)MagickRelinquishMemory(magick_err);
			goto cleanup;
		}

		if (MagickWriteImages(magick_wand, thumbnail, MagickTrue) == MagickFalse) {
			ExceptionType severity;
			char *magick_err = MagickGetException(magick_wand, &severity);
			Errorf(E_FAIL, "MagickWriteImages failed for '%s': %s", img->entry->path,
					magick_err);
			magick_err = (char *)MagickRelinquishMemory(magick_err);
			goto cleanup;
		}
		magick_wand = DestroyMagickWand(magick_wand);
		magick_wand = NULL;

		free(path_copy1);
		path_copy1 = NULL;
		free(path_copy2);
		path_copy2 = NULL;
		free(fname_noext);
		fname_noext = NULL;
	}

	// slug
	{
		char *path_copy = strdup(img->entry->path);
		if (!path_copy) {
			Errnof(E_CRIT, "strdup failed building title");
			goto cleanup;
		}
		char *title_raw = basename(path_copy);
		char *dot = strrchr(title_raw, '.');
		if (dot && dot != title_raw)
			*dot = '\0';
		title = strdup(title_raw);
		free(path_copy);
		if (!title) {
			Errnof(E_CRIT, "strdup failed building title");
			goto cleanup;
		}
	}

	size_t title_len = strlen(title);
	slug = malloc(title_len + 1);
	if (!slug) {
		Errnof(E_CRIT, "malloc failed for slug");
		goto cleanup;
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
		check_res = PQexecParams(img->conn, check_sql, 1, NULL, check_params, NULL,
				NULL, 0);

		if (PQresultStatus(check_res) != PGRES_TUPLES_OK) {
			Errorf(E_FAIL, "slug collision query failed for '%s': %s",
					img->entry->path, PQerrorMessage(img->conn));
			goto cleanup;
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
					goto cleanup;
				}
				snprintf(new_slug, base_len + 16, "%s-%d", slug, max_n);
				free(slug);
				slug = new_slug;
			}
		}
		PQclear(check_res);
		check_res = NULL;
	}

	// insert
	{
		char fsize_str[32], width_str[32], height_str[32];
		snprintf(fsize_str, sizeof(fsize_str), "%zu", img->entry->fsize);
		snprintf(width_str, sizeof(width_str), "%zu", width);
		snprintf(height_str, sizeof(height_str), "%zu", height);

		const char *param_values[11] = {
			title,     slug,          img->entry->path, thumbnail,
			mime_type, fsize_str,     width_str,        height_str,
			md,        embedding_str, created_str,
		};
		int param_lengths[11] = {0, 0, 0, 0, 0, 0, 0, 0, 32, 0, 0};
		int param_formats[11] = {0, 0, 0, 0, 0, 0, 0, 0, 1, 0, 0}; // md is binary

		const char *insert =
			"INSERT INTO wallpapers "
			"(title, slug, image, thumbnail, mime_type, file_size, width, "
			"height, checksum, embedding, created_at) "
			"VALUES ($1,$2,$3,$4,$5,$6,$7,$8,$9,$10,$11)";

		res_insert = PQexecParams(img->conn, insert, 11, NULL, param_values,
				param_lengths, param_formats, 0);

		if (PQresultStatus(res_insert) != PGRES_COMMAND_OK) {
			Errorf(E_FAIL, "insert failed for '%s': %s", img->entry->path,
					PQresultErrorMessage(res_insert));
			goto cleanup;
		}
	}

	rc = 0;
cleanup:
	if (f)
		fclose(f);
	free(img_buf);
	if (mime)
		magic_close(mime);
	free(mime_type);
	if (res)
		PQclear(res);
	if (check_res)
		PQclear(check_res);
	if (res_insert)
		PQclear(res_insert);
	if (img0)
		clip_image_u8_free(img0);
	if (img_res)
		clip_image_f32_free(img_res);
	if (magick_wand)
		DestroyMagickWand(magick_wand);
	free(path_copy1);
	free(path_copy2);
	free(fname_noext);
	free(thumbnail);
	free(title);
	free(slug);
	return rc;
}

struct file_list *file_list_new(size_t count) {
	struct file_list *list = malloc(sizeof(*list));
	if (!list) {
		Errnof(E_FAIL, "malloc failed");
		goto cleanup;
	}
	list->count = count;
	list->entries = calloc(count, sizeof(*list->entries));
	if (!list->entries) {
		Errnof(E_FAIL, "malloc failed");
		goto cleanup;
	}

	return list;

cleanup:
	free(list);
	return NULL;
}

void file_list_free(struct file_list *list) {
	if (!list)
		return;
	for (int i = 0; i < list->count; i++)
		free(list->entries[i].path);
	free(list->entries);
	free(list);
}

int main(int argc, char *argv[]) {
	int rc = -1;
	bool is_query = 0;
	char *query_str = NULL;
	PGconn *conn = NULL;
	struct clip_ctx *clip_ctx = NULL;
	struct file_list *file_list = file_list_new(argc - 1);
	struct file_list *file_list_failures = file_list_new(argc - 1);
	struct file_list *file_list_duplicates = file_list_new(argc - 1);
	if (!file_list || !file_list_failures || !file_list_duplicates)
		exit(EXIT_FAILURE);

	if (argc == 1) {
		if (create_socket() < 0) { 
			Errorf(E_CRIT, "Failed to create socket.");
			goto cleanup;
		}
		goto cleanup;
	}

	for (int i = 1; i < argc; i++) {
		if (strcmp(argv[i], "-q") == 0 || strcmp(argv[i], "--query") == 0) {
			if (i + 1 >= argc) {
				fprintf(stderr, "wallclip: %s requires an argument\n", argv[i]);
				goto cleanup;
			}
			is_query = 1;
			query_str = argv[i + 1];
			rc = init_startup(&conn, &clip_ctx);
			if (rc < 0)
				goto cleanup;
			break;
		}

		struct stat st;
		rc = stat(argv[i], &st);
		if (rc < 0) {
			Errnof(E_FAIL, "%s", argv[i]);
			goto cleanup;
		}

		struct file_entry *e = &file_list->entries[i - 1];
		char *resolved_path = realpath(argv[i], NULL);
		if (!resolved_path) {
			Errnof(E_FAIL, "%s:", argv[i]);
			goto cleanup;
		}
		e->path = resolved_path;
		e->fsize = st.st_size;
		e->modified_at = st.st_mtime;

		struct statx stx;
		if (statx(AT_FDCWD, argv[i], 0, STATX_BTIME, &stx) == 0 &&
				(stx.stx_mask & STATX_BTIME)) {
			e->created_at = stx.stx_btime.tv_sec;
		} else {
			e->created_at = e->modified_at; /* fallback for when filesystem doesn't
											   support birth time. */
		}
	}

	if (is_query == 1) {
		rc = query(query_str, clip_ctx, conn);
		goto cleanup;
	}

	// track failures and duplicates
	// TODO: Rewrite this once process_img() is split into many functions
	int failures = 0;
	int duplicates = 0;
	for (int i = 0; i < file_list->count; i++) {
		struct image_ctx img = {
			.entry = &file_list->entries[i],
			.clip_ctx = clip_ctx,
			.conn = conn,
		};
		int rcp = process_img(&img);
		if (rcp < 0) {
			Errorf(E_WARN, "failed to process '%s'", file_list->entries[i].path);
			file_list_failures->entries[failures].path = strdup(file_list->entries[i].path);
			if (!file_list_failures->entries[failures].path) {
				Errnof(E_CRIT, "strdup failed recording failed path");
			}
			failures++;
		} else if (rcp > 0) {
			Errorf(E_WARN, "'%s' is a duplicate, skipping", file_list->entries[i].path);
			file_list_duplicates->entries[duplicates].path = strdup(file_list->entries[i].path);
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

	rc = 0;
cleanup:
	if (clip_ctx)
		clip_free(clip_ctx);
	if (conn)
		PQfinish(conn);
	file_list_free(file_list);
	file_list_free(file_list_failures);
	file_list_free(file_list_duplicates);
	return rc;
}
