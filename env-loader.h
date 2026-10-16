#ifndef ENV_LOADER_H
#define ENV_LOADER_H

/*
 * Minimal .env loader. Values are kept in a private table and are never
 * exported to the process environment. Lookups check the shell environment
 * first, then the .env file, so real environment variables take precedence.
 * An empty value counts as unset.
 *
 * Supported .env syntax: KEY=value, optional "export " prefix, '#' comments
 * (whole line, or after whitespace on unquoted values), "double" quotes with
 * \n \t \r \\ \" escapes and 'single' quotes taken literally. No expansion,
 * no multi-line values.
 */

// Loads `path`. Returns 0 on success, 1 if the file doesn't exist (not an
// error, the environment alone may be enough) and -1 on error.
int env_load_file(const char *path);

// Returns the value for `key` (shell env > .env), or NULL if unset or empty.
// The pointer stays valid until env_scrub(key) or env_free().
const char *env_get(const char *key);

// Typed lookups that return `def` when unset. Out of range or malformed
// numbers are reported and also yield `def`.
const char *env_get_str(const char *key, const char *def);
int env_get_int(const char *key, int def, int min, int max);
double env_get_double(const char *key, double def, double min, double max);

// Zeroes `key`'s value everywhere we can reach: the .env table and, if it
// came from the shell, the process environment block, which is also what
// /proc/<pid>/environ shows. Use it for secrets as soon as they are consumed.
void env_scrub(const char *key);

// Zeroes and frees everything. Safe to call more than once.
void env_free(void);

#endif
