#define _GNU_SOURCE

#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <linux/openat2.h>
#include <linux/stat.h>
#include <unistd.h>

typedef int (*open_fn)(const char *path, int flags, ...);
typedef int (*openat_fn)(int dirfd, const char *path, int flags, ...);
typedef FILE *(*fopen_fn)(const char *path, const char *mode);
typedef int (*access_fn)(const char *path, int mode);
typedef int (*faccessat_fn)(int dirfd, const char *path, int mode, int flags);
typedef int (*stat_fn)(const char *path, struct stat *buf);
typedef int (*stat64_fn)(const char *path, struct stat64 *buf);
typedef int (*fstatat_fn)(int dirfd, const char *path, struct stat *buf, int flags);
typedef int (*xstat_fn)(int ver, const char *path, struct stat *buf);
typedef int (*xstat64_fn)(int ver, const char *path, struct stat64 *buf);
typedef long (*syscall_fn)(long number, ...);

struct redirect_rule {
    char dll[64];
    char target[PATH_MAX];
    bool used;
};

static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static bool g_initialized;
static bool g_debug;
static bool g_trace_open;
static bool g_trace_all_open;
static bool g_one_shot = true;
static FILE *g_log;
static struct redirect_rule g_rules[16];
static size_t g_rule_count;

static open_fn real_open_fn;
static open_fn real_open64_fn;
static openat_fn real_openat_fn;
static openat_fn real_openat64_fn;
static fopen_fn real_fopen_fn;
static fopen_fn real_fopen64_fn;
static access_fn real_access_fn;
static faccessat_fn real_faccessat_fn;
static stat_fn real_stat_fn;
static stat64_fn real_stat64_fn;
static stat_fn real_lstat_fn;
static stat64_fn real_lstat64_fn;
static fstatat_fn real_fstatat_fn;
static fstatat_fn real_newfstatat_fn;
static xstat_fn real_xstat_fn;
static xstat64_fn real_xstat64_fn;
static xstat_fn real_lxstat_fn;
static xstat64_fn real_lxstat64_fn;
static syscall_fn real_syscall_fn;

static void debug_log(const char *fmt, ...)
{
    if (!g_debug && !g_log)
        return;

    va_list ap;
    va_start(ap, fmt);
    FILE *out = g_log ? g_log : stderr;
    fputs("wine-dll-redirect: ", out);
    vfprintf(out, fmt, ap);
    fputc('\n', out);
    fflush(out);
    va_end(ap);
}

static void resolve_real_functions(void)
{
    real_open_fn = (open_fn)dlsym(RTLD_NEXT, "open");
    real_open64_fn = (open_fn)dlsym(RTLD_NEXT, "open64");
    real_openat_fn = (openat_fn)dlsym(RTLD_NEXT, "openat");
    real_openat64_fn = (openat_fn)dlsym(RTLD_NEXT, "openat64");
    real_fopen_fn = (fopen_fn)dlsym(RTLD_NEXT, "fopen");
    real_fopen64_fn = (fopen_fn)dlsym(RTLD_NEXT, "fopen64");
    real_access_fn = (access_fn)dlsym(RTLD_NEXT, "access");
    real_faccessat_fn = (faccessat_fn)dlsym(RTLD_NEXT, "faccessat");
    real_stat_fn = (stat_fn)dlsym(RTLD_NEXT, "stat");
    real_stat64_fn = (stat64_fn)dlsym(RTLD_NEXT, "stat64");
    real_lstat_fn = (stat_fn)dlsym(RTLD_NEXT, "lstat");
    real_lstat64_fn = (stat64_fn)dlsym(RTLD_NEXT, "lstat64");
    real_fstatat_fn = (fstatat_fn)dlsym(RTLD_NEXT, "fstatat");
    real_newfstatat_fn = (fstatat_fn)dlsym(RTLD_NEXT, "newfstatat");
    real_xstat_fn = (xstat_fn)dlsym(RTLD_NEXT, "__xstat");
    real_xstat64_fn = (xstat64_fn)dlsym(RTLD_NEXT, "__xstat64");
    real_lxstat_fn = (xstat_fn)dlsym(RTLD_NEXT, "__lxstat");
    real_lxstat64_fn = (xstat64_fn)dlsym(RTLD_NEXT, "__lxstat64");
    real_syscall_fn = (syscall_fn)dlsym(RTLD_NEXT, "syscall");
}

static bool env_enabled(const char *name)
{
    const char *value = getenv(name);
    return value && *value && strcmp(value, "0") != 0;
}

static bool env_disabled(const char *name)
{
    const char *value = getenv(name);
    return value && (!strcmp(value, "0") || !strcasecmp(value, "false") || !strcasecmp(value, "no"));
}

static bool has_dll_suffix(const char *path, const char *dll)
{
    size_t path_len = strlen(path);
    size_t dll_len = strlen(dll);

    if (path_len < dll_len)
        return false;

    const char *tail = path + path_len - dll_len;
    if (strcasecmp(tail, dll) != 0)
        return false;

    if (tail == path)
        return true;

    return tail[-1] == '/' || tail[-1] == '\\';
}

static bool looks_relevant_path(const char *path)
{
    if (!path)
        return false;
    if (g_trace_all_open)
        return true;
    if (strcasestr(path, ".dll"))
        return true;

    for (size_t i = 0; i < g_rule_count; ++i) {
        if (strcasestr(path, g_rules[i].dll))
            return true;
    }
    return false;
}

static void trace_open_path(const char *api, const char *path)
{
    if (g_trace_open && looks_relevant_path(path))
        debug_log("trace %s %s", api, path ? path : "(null)");
}

static bool is_absolute_path(const char *path)
{
    return path && (path[0] == '/' || path[0] == '\\');
}

static void add_rule(const char *dll, const char *target)
{
    if (!dll || !*dll || !target || !*target || g_rule_count >= sizeof(g_rules) / sizeof(g_rules[0]))
        return;

    if (strchr(dll, '.'))
        snprintf(g_rules[g_rule_count].dll, sizeof(g_rules[g_rule_count].dll), "%s", dll);
    else
        snprintf(g_rules[g_rule_count].dll, sizeof(g_rules[g_rule_count].dll), "%s.dll", dll);
    snprintf(g_rules[g_rule_count].target, sizeof(g_rules[g_rule_count].target), "%s", target);
    g_rules[g_rule_count].used = false;
    debug_log("rule %s -> %s", g_rules[g_rule_count].dll, g_rules[g_rule_count].target);
    ++g_rule_count;
}

static void parse_rules(void)
{
    const char *env = getenv("WINE_DLL_REDIRECTS");
    if (!env || !*env)
        return;

    char *copy = strdup(env);
    if (!copy)
        return;

    char *saveptr = NULL;
    for (char *item = strtok_r(copy, ";", &saveptr);
         item;
         item = strtok_r(NULL, ";", &saveptr)) {
        char *eq = strchr(item, '=');
        if (!eq)
            continue;
        *eq = '\0';
        add_rule(item, eq + 1);
    }

    free(copy);
}

static void init_once(void)
{
    pthread_mutex_lock(&g_lock);
    if (g_initialized) {
        pthread_mutex_unlock(&g_lock);
        return;
    }

    resolve_real_functions();

    g_debug = env_enabled("WINE_DLL_REDIRECT_DEBUG");
    g_trace_open = env_enabled("WINE_DLL_REDIRECT_TRACE_OPEN");
    g_trace_all_open = getenv("WINE_DLL_REDIRECT_TRACE_OPEN") &&
                       !strcasecmp(getenv("WINE_DLL_REDIRECT_TRACE_OPEN"), "all");
    g_one_shot = !env_disabled("WINE_DLL_REDIRECT_ONESHOT");

    const char *log_path = getenv("WINE_DLL_REDIRECT_LOG");
    if (log_path && *log_path && real_open_fn) {
        int fd = real_open_fn(log_path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0666);
        if (fd >= 0)
            g_log = fdopen(fd, "a");
    }

    debug_log("init pid=%ld exe=%s trace_open=%d one_shot=%d",
              (long)getpid(), program_invocation_short_name ? program_invocation_short_name : "(unknown)",
              g_trace_open ? 1 : 0, g_one_shot ? 1 : 0);
    parse_rules();
    g_initialized = true;

    pthread_mutex_unlock(&g_lock);
}

static const char *redirect_path(const char *path)
{
    if (!path)
        return path;

    init_once();

    pthread_mutex_lock(&g_lock);
    for (size_t i = 0; i < g_rule_count; ++i) {
        if (g_rules[i].used) {
            if (g_trace_open && has_dll_suffix(path, g_rules[i].dll))
                debug_log("skip already-consumed %s", path);
            continue;
        }
        if (!has_dll_suffix(path, g_rules[i].dll))
            continue;
        if (strcmp(path, g_rules[i].target) == 0)
            continue;

        const char *target = g_rules[i].target;
        debug_log("redirect candidate %s -> %s", path, target);
        pthread_mutex_unlock(&g_lock);
        return target;
    }
    pthread_mutex_unlock(&g_lock);

    return path;
}

static void consume_redirect(const char *target)
{
    if (!target || !g_one_shot)
        return;

    pthread_mutex_lock(&g_lock);
    for (size_t i = 0; i < g_rule_count; ++i) {
        if (!g_rules[i].used && strcmp(target, g_rules[i].target) == 0) {
            g_rules[i].used = true;
            debug_log("consumed %s", target);
            break;
        }
    }
    pthread_mutex_unlock(&g_lock);
}

static const char *redirect_at_path(int dirfd, const char *path, char *buffer, size_t size)
{
    if (!path || is_absolute_path(path) || dirfd == AT_FDCWD)
        return redirect_path(path);

    char fd_path[64];
    snprintf(fd_path, sizeof(fd_path), "/proc/self/fd/%d", dirfd);
    ssize_t len = readlink(fd_path, buffer, size);
    if (len <= 0 || (size_t)len >= size)
        return redirect_path(path);

    buffer[len] = '\0';
    strncat(buffer, "/", size - strlen(buffer) - 1);
    strncat(buffer, path, size - strlen(buffer) - 1);
    return redirect_path(buffer);
}

static int call_open_like(open_fn fn, const char *path, int flags, va_list ap)
{
    mode_t mode = 0;
    if (flags & O_CREAT)
        mode = (mode_t)va_arg(ap, int);

    trace_open_path("open", path);
    const char *actual = redirect_path(path);
    int ret;
    if (flags & O_CREAT)
        ret = fn(actual, flags, mode);
    else
        ret = fn(actual, flags);
    if (ret >= 0 && actual != path)
        consume_redirect(actual);
    return ret;
}

static int call_openat_like(openat_fn fn, int dirfd, const char *path, int flags, va_list ap)
{
    mode_t mode = 0;
    if (flags & O_CREAT)
        mode = (mode_t)va_arg(ap, int);

    trace_open_path("openat", path);
    char full_path[PATH_MAX];
    const char *actual = redirect_at_path(dirfd, path, full_path, sizeof(full_path));
    int ret;
    if (flags & O_CREAT)
        ret = fn(is_absolute_path(actual) ? AT_FDCWD : dirfd, actual, flags, mode);
    else
        ret = fn(is_absolute_path(actual) ? AT_FDCWD : dirfd, actual, flags);
    if (ret >= 0 && actual != path)
        consume_redirect(actual);
    return ret;
}

int open(const char *path, int flags, ...)
{
    init_once();
    va_list ap;
    va_start(ap, flags);
    int ret = call_open_like(real_open_fn, path, flags, ap);
    va_end(ap);
    return ret;
}

int open64(const char *path, int flags, ...)
{
    init_once();
    va_list ap;
    va_start(ap, flags);
    int ret = call_open_like(real_open64_fn ? real_open64_fn : real_open_fn, path, flags, ap);
    va_end(ap);
    return ret;
}

int __open_2(const char *path, int flags)
{
    init_once();
    trace_open_path("__open_2", path);
    const char *actual = redirect_path(path);
    int ret = real_open_fn(actual, flags);
    if (ret >= 0 && actual != path)
        consume_redirect(actual);
    return ret;
}

int __open64_2(const char *path, int flags)
{
    init_once();
    trace_open_path("__open64_2", path);
    const char *actual = redirect_path(path);
    int ret = (real_open64_fn ? real_open64_fn : real_open_fn)(actual, flags);
    if (ret >= 0 && actual != path)
        consume_redirect(actual);
    return ret;
}

int __libc_open(const char *path, int flags, ...)
{
    init_once();
    va_list ap;
    va_start(ap, flags);
    int ret = call_open_like(real_open_fn, path, flags, ap);
    va_end(ap);
    return ret;
}

int __libc_open64(const char *path, int flags, ...)
{
    init_once();
    va_list ap;
    va_start(ap, flags);
    int ret = call_open_like(real_open64_fn ? real_open64_fn : real_open_fn, path, flags, ap);
    va_end(ap);
    return ret;
}

int openat(int dirfd, const char *path, int flags, ...)
{
    init_once();
    va_list ap;
    va_start(ap, flags);
    int ret = call_openat_like(real_openat_fn, dirfd, path, flags, ap);
    va_end(ap);
    return ret;
}

int openat64(int dirfd, const char *path, int flags, ...)
{
    init_once();
    va_list ap;
    va_start(ap, flags);
    int ret = call_openat_like(real_openat64_fn ? real_openat64_fn : real_openat_fn, dirfd, path, flags, ap);
    va_end(ap);
    return ret;
}

FILE *fopen(const char *path, const char *mode)
{
    init_once();
    trace_open_path("fopen", path);
    const char *actual = redirect_path(path);
    FILE *ret = real_fopen_fn(actual, mode);
    if (ret && actual != path)
        consume_redirect(actual);
    return ret;
}

FILE *fopen64(const char *path, const char *mode)
{
    init_once();
    trace_open_path("fopen64", path);
    const char *actual = redirect_path(path);
    FILE *ret = (real_fopen64_fn ? real_fopen64_fn : real_fopen_fn)(actual, mode);
    if (ret && actual != path)
        consume_redirect(actual);
    return ret;
}

int access(const char *path, int mode)
{
    init_once();
    trace_open_path("access", path);
    return real_access_fn(redirect_path(path), mode);
}

int faccessat(int dirfd, const char *path, int mode, int flags)
{
    init_once();
    trace_open_path("faccessat", path);
    char full_path[PATH_MAX];
    const char *actual = redirect_at_path(dirfd, path, full_path, sizeof(full_path));
    return real_faccessat_fn(is_absolute_path(actual) ? AT_FDCWD : dirfd, actual, mode, flags);
}

int stat(const char *path, struct stat *buf)
{
    init_once();
    trace_open_path("stat", path);
    return real_stat_fn(redirect_path(path), buf);
}

int stat64(const char *path, struct stat64 *buf)
{
    init_once();
    trace_open_path("stat64", path);
    return real_stat64_fn(redirect_path(path), buf);
}

int lstat(const char *path, struct stat *buf)
{
    init_once();
    trace_open_path("lstat", path);
    return real_lstat_fn(redirect_path(path), buf);
}

int lstat64(const char *path, struct stat64 *buf)
{
    init_once();
    trace_open_path("lstat64", path);
    return real_lstat64_fn(redirect_path(path), buf);
}

int fstatat(int dirfd, const char *path, struct stat *buf, int flags)
{
    init_once();
    trace_open_path("fstatat", path);
    char full_path[PATH_MAX];
    const char *actual = redirect_at_path(dirfd, path, full_path, sizeof(full_path));
    fstatat_fn fn = real_fstatat_fn ? real_fstatat_fn : real_newfstatat_fn;
    return fn(is_absolute_path(actual) ? AT_FDCWD : dirfd, actual, buf, flags);
}

int newfstatat(int dirfd, const char *path, struct stat *buf, int flags)
{
    init_once();
    trace_open_path("newfstatat", path);
    char full_path[PATH_MAX];
    const char *actual = redirect_at_path(dirfd, path, full_path, sizeof(full_path));
    fstatat_fn fn = real_newfstatat_fn ? real_newfstatat_fn : real_fstatat_fn;
    return fn(is_absolute_path(actual) ? AT_FDCWD : dirfd, actual, buf, flags);
}

int __xstat(int ver, const char *path, struct stat *buf)
{
    init_once();
    trace_open_path("__xstat", path);
    return real_xstat_fn(ver, redirect_path(path), buf);
}

int __xstat64(int ver, const char *path, struct stat64 *buf)
{
    init_once();
    trace_open_path("__xstat64", path);
    return real_xstat64_fn(ver, redirect_path(path), buf);
}

int __lxstat(int ver, const char *path, struct stat *buf)
{
    init_once();
    trace_open_path("__lxstat", path);
    return real_lxstat_fn(ver, redirect_path(path), buf);
}

int __lxstat64(int ver, const char *path, struct stat64 *buf)
{
    init_once();
    trace_open_path("__lxstat64", path);
    return real_lxstat64_fn(ver, redirect_path(path), buf);
}

long syscall(long number, ...)
{
    init_once();

    va_list ap;
    va_start(ap, number);

    switch (number) {
#ifdef SYS_open
    case SYS_open: {
        const char *path = va_arg(ap, const char *);
        int flags = va_arg(ap, int);
        mode_t mode = 0;
        if (flags & O_CREAT)
            mode = (mode_t)va_arg(ap, int);
        va_end(ap);

        const char *actual = redirect_path(path);
        long ret = (flags & O_CREAT)
            ? real_syscall_fn(number, actual, flags, mode)
            : real_syscall_fn(number, actual, flags);
        if (ret >= 0 && actual != path)
            consume_redirect(actual);
        return ret;
    }
#endif
#ifdef SYS_openat
    case SYS_openat: {
        int dirfd = va_arg(ap, int);
        const char *path = va_arg(ap, const char *);
        int flags = va_arg(ap, int);
        mode_t mode = 0;
        if (flags & O_CREAT)
            mode = (mode_t)va_arg(ap, int);
        va_end(ap);

        char full_path[PATH_MAX];
        const char *actual = redirect_at_path(dirfd, path, full_path, sizeof(full_path));
        int actual_dirfd = is_absolute_path(actual) ? AT_FDCWD : dirfd;
        long ret = (flags & O_CREAT)
            ? real_syscall_fn(number, actual_dirfd, actual, flags, mode)
            : real_syscall_fn(number, actual_dirfd, actual, flags);
        if (ret >= 0 && actual != path)
            consume_redirect(actual);
        return ret;
    }
#endif
#ifdef SYS_openat2
    case SYS_openat2: {
        int dirfd = va_arg(ap, int);
        const char *path = va_arg(ap, const char *);
        struct open_how *how = va_arg(ap, struct open_how *);
        size_t size = va_arg(ap, size_t);
        va_end(ap);

        char full_path[PATH_MAX];
        const char *actual = redirect_at_path(dirfd, path, full_path, sizeof(full_path));
        int actual_dirfd = is_absolute_path(actual) ? AT_FDCWD : dirfd;
        long ret = real_syscall_fn(number, actual_dirfd, actual, how, size);
        if (ret >= 0 && actual != path)
            consume_redirect(actual);
        return ret;
    }
#endif
#ifdef SYS_access
    case SYS_access: {
        const char *path = va_arg(ap, const char *);
        int mode = va_arg(ap, int);
        va_end(ap);
        return real_syscall_fn(number, redirect_path(path), mode);
    }
#endif
#ifdef SYS_faccessat
    case SYS_faccessat: {
        int dirfd = va_arg(ap, int);
        const char *path = va_arg(ap, const char *);
        int mode = va_arg(ap, int);
        va_end(ap);

        char full_path[PATH_MAX];
        const char *actual = redirect_at_path(dirfd, path, full_path, sizeof(full_path));
        return real_syscall_fn(number, is_absolute_path(actual) ? AT_FDCWD : dirfd, actual, mode);
    }
#endif
#ifdef SYS_faccessat2
    case SYS_faccessat2: {
        int dirfd = va_arg(ap, int);
        const char *path = va_arg(ap, const char *);
        int mode = va_arg(ap, int);
        int flags = va_arg(ap, int);
        va_end(ap);

        char full_path[PATH_MAX];
        const char *actual = redirect_at_path(dirfd, path, full_path, sizeof(full_path));
        return real_syscall_fn(number, is_absolute_path(actual) ? AT_FDCWD : dirfd, actual, mode, flags);
    }
#endif
#ifdef SYS_newfstatat
    case SYS_newfstatat: {
        int dirfd = va_arg(ap, int);
        const char *path = va_arg(ap, const char *);
        struct stat *buf = va_arg(ap, struct stat *);
        int flags = va_arg(ap, int);
        va_end(ap);

        char full_path[PATH_MAX];
        const char *actual = redirect_at_path(dirfd, path, full_path, sizeof(full_path));
        return real_syscall_fn(number, is_absolute_path(actual) ? AT_FDCWD : dirfd, actual, buf, flags);
    }
#endif
#ifdef SYS_statx
    case SYS_statx: {
        int dirfd = va_arg(ap, int);
        const char *path = va_arg(ap, const char *);
        int flags = va_arg(ap, int);
        unsigned int mask = va_arg(ap, unsigned int);
        struct statx *buf = va_arg(ap, struct statx *);
        va_end(ap);

        char full_path[PATH_MAX];
        const char *actual = redirect_at_path(dirfd, path, full_path, sizeof(full_path));
        return real_syscall_fn(number, is_absolute_path(actual) ? AT_FDCWD : dirfd, actual, flags, mask, buf);
    }
#endif
    default: {
        long a1 = va_arg(ap, long);
        long a2 = va_arg(ap, long);
        long a3 = va_arg(ap, long);
        long a4 = va_arg(ap, long);
        long a5 = va_arg(ap, long);
        long a6 = va_arg(ap, long);
        va_end(ap);
        return real_syscall_fn(number, a1, a2, a3, a4, a5, a6);
    }
    }
}
