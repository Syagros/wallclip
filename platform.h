#ifndef PLATFORM_H
#define PLATFORM_H
/*
 * OS specific helpers, kept in a header of their own so they can be unit
 * tested without clip.cpp, ImageMagick or PostgreSQL (see tests/).
 * The including file must define _GNU_SOURCE before any system header (Linux
 * needs it for statx()).
 */
#include <fcntl.h>
#include <sys/stat.h>
#include <time.h>

// Birth time of `path` with per-OS support and an mtime fallback for
// filesystems without one.
static inline time_t file_birthtime(const struct stat *st, const char *path) {
#if defined(__APPLE__) || defined(__FreeBSD__) || defined(__NetBSD__) || \
		defined(__OpenBSD__) || defined(__DragonFly__)
	(void)path;
	if (st->st_birthtime > 0)
		return st->st_birthtime;
#else
	struct statx stx;
	if (statx(AT_FDCWD, path, 0, STATX_BTIME, &stx) == 0 &&
			(stx.stx_mask & STATX_BTIME)) {
		return stx.stx_btime.tv_sec;
	}
#endif
	return st->st_mtime;
}

#endif
