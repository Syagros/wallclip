#include <stdio.h>
#include <stdarg.h>
#include <errno.h>

#define C_TEXT_ "\033[37m"
#define C_OKAY_ "\033[34m"
#define C_HINT_ "\033[33m"
#define C_WARN_ "\033[32m"
#define C_FAIL_ "\033[31m"
#define C_CRIT_ "\033[91m"
#define C_CLEAR "\033[0m"
static char* __ErrStr__[] = {
	"%s "C_TEXT_"%s %s"C_TEXT_"from : "C_WARN_"%s"C_OKAY_" @ "C_HINT_"%s"C_CLEAR":"C_WARN_"%d"C_CLEAR"\n",
	"("C_HINT_"%d"C_TEXT_":"C_WARN_"%m"C_TEXT_") ",
	C_OKAY_ "[  OK  ]" C_CLEAR,
	C_HINT_ "[ HINT ]" C_CLEAR,
	C_WARN_ "[ WARN ]" C_CLEAR,
	C_FAIL_ "[ FAIL ]" C_CLEAR,
	C_CRIT_ "[ CRIT ]" C_CLEAR
};
#define __ERRNO_STR__ 

#define E_OKAY __ErrStr__[2]
#define E_HINT __ErrStr__[3]
#define E_WARN __ErrStr__[4]
#define E_FAIL __ErrStr__[5]
#define E_CRIT __ErrStr__[6]

static void
Errorf_impl(const char * const type, const char * const func, const char * const file, const int line,
            const char * const fmt, ...)
{
    char msg[512] = {0};
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof(msg), fmt, ap);
    va_end(ap);

    fprintf(stderr, __ErrStr__[0], type, msg, "", func, file, line);
}

static void
Errnof_impl(const char * const type, const char * const func, const char * const file, const int line,
            const char * const fmt, ...)
{
    char msg[512] = {0};
    char eno[64]  = {0};
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof(msg), fmt, ap);
    va_end(ap);

    snprintf(eno, sizeof(eno), __ErrStr__[1], errno, errno);
    fprintf(stderr, __ErrStr__[0], type, msg, eno, func, file, line);
}

#define Errorf(TYPE, ...) \
    Errorf_impl(TYPE, __func__, __FILE__, __LINE__, __VA_ARGS__)

#define Errnof(TYPE, ...) \
    Errnof_impl(TYPE, __func__, __FILE__, __LINE__, __VA_ARGS__)
