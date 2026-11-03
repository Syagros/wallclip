/*
 * Platform tests for the OS specific parts of wallclip. They run on Linux,
 * macOS and FreeBSD and need only a C compiler, no clip.cpp / ImageMagick /
 * PostgreSQL:
 *
 *   make test          (gmake on FreeBSD)
 *   make test-asan
 *
 * env-loader.c is included (not linked) so the static helpers can be tested.
 * It must stay the first include: it sets the feature macros (_GNU_SOURCE,
 * __STDC_WANT_LIB_EXT1__) that the system headers need to see first.
 */
#include "../env-loader.c"
#include "../platform.h"

#include <sys/time.h>
#include <unistd.h>

int verbosity = 0; // log.h expects the program to define it

#if defined(__APPLE__)
#define PLATFORM "macOS"
#define BSD_LIKE 1
#elif defined(__FreeBSD__)
#define PLATFORM "FreeBSD"
#define BSD_LIKE 1
#elif defined(__NetBSD__) || defined(__OpenBSD__) || defined(__DragonFly__)
#define PLATFORM "BSD"
#define BSD_LIKE 1
#elif defined(__linux__)
#define PLATFORM "Linux"
#define BSD_LIKE 0
#else
#define PLATFORM "other"
#define BSD_LIKE 0
#endif

static int checks = 0;
static int failures = 0;
static int skipped = 0;

#define CHECK(cond)                                                         \
	do {                                                                    \
		checks++;                                                           \
		if (!(cond)) {                                                      \
			failures++;                                                     \
			printf("  FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);        \
		}                                                                   \
	} while (0)

#define SKIP(why)                                                           \
	do {                                                                    \
		skipped++;                                                          \
		printf("  SKIP %s:%d: %s\n", __FILE__, __LINE__, why);              \
	} while (0)

// The code under test logs the deliberately bad inputs; keep that out of the
// test output.
static int muted_fd = -1;
static void mute_stderr(void) {
	fflush(stderr);
	muted_fd = dup(STDERR_FILENO);
	int nul = open("/dev/null", O_WRONLY);
	if (nul >= 0) {
		dup2(nul, STDERR_FILENO);
		close(nul);
	}
}
static void unmute_stderr(void) {
	fflush(stderr);
	if (muted_fd >= 0) {
		dup2(muted_fd, STDERR_FILENO);
		close(muted_fd);
		muted_fd = -1;
	}
}

static char tmpdir[256];

static void make_tmpdir(void) {
	const char *base = getenv("TMPDIR");
	snprintf(tmpdir, sizeof(tmpdir), "%s/wallclip-test-XXXXXX",
			base && *base ? base : "/tmp");
	if (!mkdtemp(tmpdir)) {
		perror("mkdtemp");
		exit(2);
	}
}

static void path_in_tmp(char *out, size_t n, const char *name) {
	snprintf(out, n, "%s/%s", tmpdir, name);
}

static void write_file(const char *path, const char *content) {
	FILE *f = fopen(path, "w");
	if (!f) {
		perror(path);
		exit(2);
	}
	fputs(content, f);
	fclose(f);
}

// --- secure_bzero ---------------------------------------------------------
// macOS uses memset_s(), the others explicit_bzero().
static void test_secure_bzero(void) {
	printf("secure_bzero (%s)\n",
#ifdef __APPLE__
			"memset_s"
#else
			"explicit_bzero"
#endif
	);

	unsigned char buf[64];
	volatile unsigned char *v = buf;

	memset(buf, 0xAA, sizeof(buf));
	secure_bzero(buf + 8, 32);
	int head_ok = 1, mid_ok = 1, tail_ok = 1;
	for (int i = 0; i < 8; i++)
		head_ok &= (v[i] == 0xAA);
	for (int i = 8; i < 40; i++)
		mid_ok &= (v[i] == 0);
	for (int i = 40; i < 64; i++)
		tail_ok &= (v[i] == 0xAA);
	CHECK(head_ok);  // nothing before the range is touched
	CHECK(mid_ok);   // the range is zeroed
	CHECK(tail_ok);  // nothing after the range is touched

	memset(buf, 0xAA, sizeof(buf));
	secure_bzero(buf, 0);
	CHECK(v[0] == 0xAA && v[63] == 0xAA); // n == 0 is a no-op

	char secret[] = "hunter2";
	wipe_str(secret);
	int all_zero = 1;
	for (size_t i = 0; i < sizeof(secret); i++)
		all_zero &= (((volatile char *)secret)[i] == 0);
	CHECK(all_zero);
	wipe_str(NULL); // must not crash
}

// --- .env parsing (getline, setvbuf with a stack buffer, strtol/strtod) ---
static void test_env_file(void) {
	printf("env_load_file / env_get\n");
	char path[300];
	path_in_tmp(path, sizeof(path), "t.env");
	write_file(path,
			"# comment\n"
			"WCT_PLAIN=hello\n"
			"export WCT_EXPORTED = spaced value  \n"
			"WCT_DQ=\"a\\tb\\n\\\"q\\\"\"\n"
			"WCT_SQ='lit\\n'\n"
			"WCT_TRAIL=value # trailing comment\n"
			"WCT_HASH=no#comment\n"
			"WCT_EMPTY=\n"
			"WCT_INT=42\n"
			"WCT_BADINT=4x\n"
			"WCT_BIG=99999\n"
			"WCT_DBL=0.27\n"
			"WCT_SHELL=fromfile\n"
			"1BAD=x\n"
			"WCT_NOQUOTE_END=\"unterminated\n"
			"WCT_LAST=ok");

	unsetenv("WCT_PLAIN");
	setenv("WCT_SHELL", "fromshell", 1);
	mute_stderr();
	CHECK(env_load_file(path) == 0);

	CHECK(strcmp(env_get_str("WCT_PLAIN", "?"), "hello") == 0);
	CHECK(strcmp(env_get_str("WCT_EXPORTED", "?"), "spaced value") == 0);
	CHECK(strcmp(env_get_str("WCT_DQ", "?"), "a\tb\n\"q\"") == 0);
	CHECK(strcmp(env_get_str("WCT_SQ", "?"), "lit\\n") == 0);
	CHECK(strcmp(env_get_str("WCT_TRAIL", "?"), "value") == 0);
	CHECK(strcmp(env_get_str("WCT_HASH", "?"), "no#comment") == 0);
	CHECK(env_get("WCT_EMPTY") == NULL);              // empty counts as unset
	CHECK(strcmp(env_get_str("WCT_SHELL", "?"), "fromshell") == 0); // shell wins
	CHECK(env_get("1BAD") == NULL);                   // malformed lines skipped
	CHECK(env_get("WCT_NOQUOTE_END") == NULL);
	CHECK(strcmp(env_get_str("WCT_LAST", "?"), "ok") == 0); // no trailing \n

	CHECK(env_get_int("WCT_INT", -1, 0, 100) == 42);
	CHECK(env_get_int("WCT_BADINT", -1, 0, 100) == -1);
	CHECK(env_get_int("WCT_BIG", -1, 0, 100) == -1);
	unmute_stderr();
	CHECK(env_get_int("WCT_MISSING", 7, 0, 100) == 7);
	double d = env_get_double("WCT_DBL", -1.0, 0.0, 1.0);
	CHECK(d > 0.2699 && d < 0.2701);

	char missing[300];
	path_in_tmp(missing, sizeof(missing), "does-not-exist.env");
	CHECK(env_load_file(missing) == 1);               // absent file isn't an error

	env_free();
	CHECK(env_get("WCT_PLAIN") == NULL);
	env_free();                                       // safe to call twice
	unsetenv("WCT_SHELL");
	unlink(path);
}

// --- env_scrub ------------------------------------------------------------
static void test_env_scrub(void) {
	printf("env_scrub\n");

	// a secret from the shell environment
	setenv("WCT_SECRET", "hunter2", 1);
	CHECK(env_get("WCT_SECRET") != NULL);
	env_scrub("WCT_SECRET");
	CHECK(getenv("WCT_SECRET") == NULL);
	CHECK(env_get("WCT_SECRET") == NULL);

	// a secret from the .env file: the table copy is zeroed in place
	char path[300];
	path_in_tmp(path, sizeof(path), "s.env");
	write_file(path, "WCT_FSECRET=abcdef\n");
	unsetenv("WCT_FSECRET");
	CHECK(env_load_file(path) == 0);
	const char *p = env_get("WCT_FSECRET");
	CHECK(p != NULL && strcmp(p, "abcdef") == 0);
	env_scrub("WCT_FSECRET");
	// the entry's memory is still allocated (only wiped), so reading it is fine
	CHECK(p != NULL && p[0] == '\0' && p[3] == '\0');
	CHECK(env_get("WCT_FSECRET") == NULL);
	env_free();

	env_scrub("WCT_NEVER_SET"); // unknown key must not crash
	unlink(path);
}

// --- log.h: errno text must not depend on glibc's %m -----------------------
static void test_errno_logging(void) {
	printf("Errnof formatting\n");
	fflush(stderr);
	int saved = dup(STDERR_FILENO);
	FILE *cap = tmpfile();
	CHECK(saved >= 0 && cap != NULL);
	if (saved < 0 || !cap)
		return;
	dup2(fileno(cap), STDERR_FILENO);

	errno = ENOENT;
	Errnof(E_FAIL, "probe-message");
	Errorf(E_FAIL, "plain-message");
	fflush(stderr);

	dup2(saved, STDERR_FILENO);
	close(saved);

	char out[2048] = {0};
	rewind(cap);
	size_t n = fread(out, 1, sizeof(out) - 1, cap);
	out[n] = '\0';
	fclose(cap);

	CHECK(strstr(out, "probe-message") != NULL);
	CHECK(strstr(out, "plain-message") != NULL);
	CHECK(strstr(out, strerror(ENOENT)) != NULL); // e.g. "No such file or directory"
	CHECK(strstr(out, "%m") == NULL);             // never print the raw specifier
	CHECK(strstr(out, ":m") == NULL);             // ...nor a mangled one
}

// --- file_birthtime ---------------------------------------------------------
static void test_birthtime(void) {
	printf("file_birthtime (%s)\n",
#if BSD_LIKE
			"st_birthtime"
#else
			"statx"
#endif
	);

	// 1. no usable birth time: fall back to mtime
	struct stat fake;
	memset(&fake, 0, sizeof(fake));
	fake.st_mtime = 12345;
	CHECK(file_birthtime(&fake, "/nonexistent/wallclip/test") == 12345);

#if BSD_LIKE
	// 2. BSD/macOS read the value straight from struct stat
	fake.st_birthtime = 777;
	CHECK(file_birthtime(&fake, "/nonexistent/wallclip/test") == 777);
#endif

	// 3. a real file whose mtime is pushed far away from its creation: the
	//    birth time must not follow it. The mtime goes into the FUTURE on
	//    purpose: macOS and the BSDs also move the birth time back when the
	//    mtime is set to something earlier than it.
	char path[300];
	path_in_tmp(path, sizeof(path), "bt.dat");
	time_t before = time(NULL);
	write_file(path, "x");

	const time_t far_mtime = before + 100000; // about 27 hours ahead
	struct timeval tv[2] = {{far_mtime, 0}, {far_mtime, 0}};
	CHECK(utimes(path, tv) == 0);

	struct stat st;
	CHECK(stat(path, &st) == 0);
	CHECK(st.st_mtime == far_mtime);

	time_t bt = file_birthtime(&st, path);
	long delta = (long)(bt - before);
	if (delta < 0)
		delta = -delta;

#if BSD_LIKE
	if (st.st_birthtime <= 0) {
		SKIP("this filesystem reports no birth time");
	} else {
		CHECK(bt == st.st_birthtime);
		CHECK(bt != far_mtime);
		CHECK(delta < 120);
	}
#else
	// Linux: when the filesystem has a btime (ext4, xfs, btrfs, newer tmpfs)
	// it must be returned; otherwise (older tmpfs, overlayfs) the mtime
	// fallback is expected.
	struct statx stx;
	if (statx(AT_FDCWD, path, 0, STATX_BTIME, &stx) == 0 &&
			(stx.stx_mask & STATX_BTIME)) {
		CHECK(bt == stx.stx_btime.tv_sec);
		CHECK(bt != far_mtime);
		CHECK(delta < 120);
	} else {
		SKIP("this filesystem reports no btime (mtime fallback used)");
		CHECK(bt == far_mtime);
	}
#endif

	// 4. a path that vanished: still returns a sane value
	unlink(path);
#if !BSD_LIKE
	CHECK(file_birthtime(&st, path) == far_mtime); // statx fails -> mtime
#endif
}

int main(void) {
	printf("wallclip platform tests on %s\n", PLATFORM);
	make_tmpdir();

	test_secure_bzero();
	test_env_file();
	test_env_scrub();
	test_errno_logging();
	test_birthtime();

	rmdir(tmpdir);
	printf("\n%d checks, %d failed, %d skipped\n", checks, failures, skipped);
	return failures ? 1 : 0;
}
