#define _GNU_SOURCE // explicit_bzero(), getline()
#include "env-loader.h"
#include "log.h"
#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

struct env_entry {
	char *key;
	char *val;
};

static struct env_entry *entries = NULL;
static size_t n_entries = 0;
static size_t cap_entries = 0;

static void wipe_str(char *s) {
	if (s)
		explicit_bzero(s, strlen(s));
}

// Sets `key` to a copy of `val`, replacing (and wiping) any previous value.
// Returns 0 on success, -1 on error.
static int entry_set(const char *key, const char *val) {
	char *v = strdup(val);
	if (!v) {
		Errnof(E_CRIT, "strdup failed for the value of '%s'", key);
		return -1;
	}

	for (size_t i = 0; i < n_entries; i++) {
		if (strcmp(entries[i].key, key) == 0) {
			wipe_str(entries[i].val);
			free(entries[i].val);
			entries[i].val = v;
			return 0;
		}
	}

	if (n_entries == cap_entries) {
		size_t ncap = cap_entries ? cap_entries * 2 : 16;
		struct env_entry *tmp = realloc(entries, ncap * sizeof(*tmp));
		if (!tmp) {
			Errnof(E_CRIT, "realloc failed for the env table");
			goto fail;
		}
		entries = tmp;
		cap_entries = ncap;
	}

	char *k = strdup(key);
	if (!k) {
		Errnof(E_CRIT, "strdup failed for key '%s'", key);
		goto fail;
	}

	entries[n_entries].key = k;
	entries[n_entries].val = v;
	n_entries++;
	return 0;

fail:
	wipe_str(v);
	free(v);
	return -1;
}

// Parses one line of `path` (modified in place). Malformed lines are reported
// (by line number only, never by content) and skipped. Returns 0, or -1 on
// out of memory.
static int parse_line(const char *path, char *line, int lineno) {
	char *p = line;
	while (isspace((unsigned char)*p))
		p++;
	if (*p == '\0' || *p == '#')
		return 0;

	if (strncmp(p, "export", 6) == 0 && isspace((unsigned char)p[6])) {
		p += 6;
		while (isspace((unsigned char)*p))
			p++;
	}

	char *key = p;
	if (!isalpha((unsigned char)*p) && *p != '_')
		goto malformed;
	while (isalnum((unsigned char)*p) || *p == '_')
		p++;
	char *key_end = p;
	while (*p == ' ' || *p == '\t')
		p++;
	if (*p != '=')
		goto malformed;
	*key_end = '\0';
	p++;
	while (*p == ' ' || *p == '\t')
		p++;

	char *val = p;
	if (*p == '"' || *p == '\'') {
		char quote = *p++;
		char *w = p;
		val = w;
		while (*p && *p != quote) {
			if (quote == '"' && *p == '\\' && p[1]) {
				p++;
				switch (*p) {
					case 'n': *w++ = '\n'; break;
					case 't': *w++ = '\t'; break;
					case 'r': *w++ = '\r'; break;
					default:  *w++ = *p;   break;
				}
				p++;
				continue;
			}
			*w++ = *p++;
		}
		if (*p != quote)
			goto malformed; // unterminated
		*w = '\0';
		p++;
		while (isspace((unsigned char)*p))
			p++;
		if (*p != '\0' && *p != '#')
			goto malformed; // junk after the closing quote
	} else {
		for (char *c = val; *c; c++) {
			if (*c == '#' && (c[-1] == ' ' || c[-1] == '\t')) {
				*c = '\0';
				break;
			}
		}
		size_t n = strlen(val);
		while (n > 0 && isspace((unsigned char)val[n - 1]))
			val[--n] = '\0';
	}

	return entry_set(key, val);

malformed:
	Errorf(E_FAIL, "%s:%d: ignoring malformed line", path, lineno);
	return 0;
}

int env_load_file(const char *path) {
	int rc = -1;
	FILE *f = NULL;
	char *line = NULL;
	size_t cap = 0;
	int lineno = 0;
	char iobuf[BUFSIZ]; // our own stdio buffer, so it can be wiped afterwards
	struct stat st;

	f = fopen(path, "r");
	if (!f) {
		if (errno == ENOENT) {
			Errorf(E_HINT, "'%s' not found, using the environment only", path);
			rc = 1;
			goto cleanup;
		}
		Errnof(E_CRIT, "failed to open '%s'", path);
		goto cleanup;
	}
	setvbuf(f, iobuf, _IOFBF, sizeof(iobuf));

	if (fstat(fileno(f), &st) == 0 && (st.st_mode & (S_IRWXG | S_IRWXO)))
		Errorf(E_WARN, "'%s' is accessible by group/others, consider chmod 600",
				path);

	while (getline(&line, &cap, f) != -1) {
		lineno++;
		if (parse_line(path, line, lineno) < 0)
			goto cleanup;
	}
	if (ferror(f)) {
		Errnof(E_CRIT, "failed to read '%s'", path);
		goto cleanup;
	}

	rc = 0;

cleanup:
	if (line) {
		explicit_bzero(line, cap);
		free(line);
	}
	if (f)
		fclose(f);
	explicit_bzero(iobuf, sizeof(iobuf));
	return rc;
}

const char *env_get(const char *key) {
	const char *v = getenv(key);
	if (v && *v)
		return v;

	for (size_t i = 0; i < n_entries; i++) {
		if (strcmp(entries[i].key, key) == 0)
			return *entries[i].val ? entries[i].val : NULL;
	}
	return NULL;
}

const char *env_get_str(const char *key, const char *def) {
	const char *v = env_get(key);
	return v ? v : def;
}

int env_get_int(const char *key, int def, int min, int max) {
	const char *v = env_get(key);
	if (!v)
		return def;

	char *end = NULL;
	errno = 0;
	long n = strtol(v, &end, 10);
	if (errno != 0 || end == v || *end != '\0' || n < min || n > max) {
		Errorf(E_FAIL, "invalid %s (expected an integer in %d..%d), using %d",
				key, min, max, def);
		return def;
	}
	return (int)n;
}

double env_get_double(const char *key, double def, double min, double max) {
	const char *v = env_get(key);
	if (!v)
		return def;

	char *end = NULL;
	errno = 0;
	double d = strtod(v, &end);
	if (errno != 0 || end == v || *end != '\0' || !(d >= min && d <= max)) {
		Errorf(E_FAIL, "invalid %s (expected a number in %g..%g), using %g",
				key, min, max, def);
		return def;
	}
	return d;
}

void env_scrub(const char *key) {
	for (size_t i = 0; i < n_entries; i++) {
		if (strcmp(entries[i].key, key) == 0)
			wipe_str(entries[i].val);
	}

	// getenv() points into the environment block, which is writable memory
	// owned by this process
	char *v = getenv(key);
	if (v) {
		wipe_str(v);
		unsetenv(key);
	}
}

void env_free(void) {
	for (size_t i = 0; i < n_entries; i++) {
		wipe_str(entries[i].val);
		free(entries[i].key);
		free(entries[i].val);
	}
	free(entries);
	entries = NULL;
	n_entries = 0;
	cap_entries = 0;
}
