/* ============================================================================
 * wince_host.h —— 让 ftpd.c（WinCE 代码）能在电脑上原样编译、原样跑起来
 *
 * 只做「类型 + 函数名」的等价映射，不碰一行 FTP 业务逻辑。
 * 这样测试验证的就是将来烧进车机的那份代码，而不是另写一个模拟器。
 *
 * 用法：
 *   clang -I host -DFTPD_CONSOLE -DFTPD_NO_WINMAIN ftpd.c -o ftpd_host -lpthread
 *
 * 环境变量：
 *   FTPD_SHIMROOT  车机根目录 "\" 映射到哪个本地目录（默认 /tmp/ftpd_root）
 *   FTPD_SHIMHOME  程序所在目录 "\" （默认 /tmp/ftpd_home）
 * ==========================================================================*/
#ifndef WINCE_HOST_H
#define WINCE_HOST_H

#include <arpa/inet.h>
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <pthread.h>
#include <signal.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>
#include <wchar.h>
#include <iconv.h>

/* ---------------------------------------------------------------- 基本类型 */
typedef int            BOOL;
typedef unsigned int   DWORD;
typedef long           LONG;
typedef unsigned long  ULONG;
typedef unsigned short WORD;
typedef unsigned char  BYTE;
typedef void          *LPVOID;
typedef const void    *LPCVOID;
typedef char          *LPSTR;
typedef const char    *LPCSTR;
typedef wchar_t        WCHAR;
typedef WCHAR         *LPWSTR;
typedef const WCHAR   *LPCWSTR;
typedef void          *HANDLE;
typedef void          *HWND;
typedef void          *HINSTANCE;

#define TRUE  1
#define FALSE 0
#define WINAPI
#define CALLBACK

#ifndef MAX_PATH
#define MAX_PATH 260
#endif

#define INVALID_HANDLE_VALUE    ((HANDLE)(intptr_t)-1)
#define INVALID_FILE_ATTRIBUTES ((DWORD)0xFFFFFFFFUL)

#define FILE_ATTRIBUTE_READONLY   0x00000001
#define FILE_ATTRIBUTE_DIRECTORY  0x00000010
#define FILE_ATTRIBUTE_NORMAL     0x00000080

#define GENERIC_READ    0x80000000UL
#define GENERIC_WRITE   0x40000000UL
#define FILE_SHARE_READ  0x00000001
#define FILE_SHARE_WRITE 0x00000002

#define CREATE_NEW        1
#define CREATE_ALWAYS     2
#define OPEN_EXISTING     3
#define OPEN_ALWAYS       4
#define TRUNCATE_EXISTING 5

#define FILE_BEGIN   0
#define FILE_CURRENT 1
#define FILE_END     2

typedef struct { DWORD dwLowDateTime, dwHighDateTime; } FILETIME;
typedef struct {
    WORD wYear, wMonth, wDayOfWeek, wDay;
    WORD wHour, wMinute, wSecond, wMilliseconds;
} SYSTEMTIME;

typedef struct {
    DWORD    dwFileAttributes;
    FILETIME ftCreationTime, ftLastAccessTime, ftLastWriteTime;
    DWORD    nFileSizeHigh, nFileSizeLow;
    DWORD    dwReserved0, dwReserved1;
    WCHAR    cFileName[MAX_PATH];
    WCHAR    cAlternateFileName[14];
} WIN32_FIND_DATAW;

/* ------------------------------------------------------------------ 套接字 */
typedef int SOCKET;
#define INVALID_SOCKET (-1)
#define SOCKET_ERROR   (-1)

typedef struct { unsigned short wVersion; unsigned short wHighVersion; } WSADATA;

#define MAKEWORD(a, b) ((WORD)(((BYTE)(a)) | ((WORD)((BYTE)(b))) << 8))
#define WSAStartup(a, b) (0)
#define WSACleanup()     (0)

#define WSAEWOULDBLOCK 10035

static inline int shim_wsa_error(void)
{
    int e = errno;
    if (e == EAGAIN || e == EWOULDBLOCK || e == EINPROGRESS || e == EALREADY)
        return WSAEWOULDBLOCK;
    return e;
}
#define WSAGetLastError() shim_wsa_error()
#define GetLastError()    (errno)

static inline int shim_accept(int s, struct sockaddr *a, int *len)
{
    socklen_t l = (socklen_t)*len;
    int r = accept(s, a, &l);
    *len = (int)l;
    return r;
}
static inline int shim_getsockname(int s, struct sockaddr *a, int *len)
{
    socklen_t l = (socklen_t)*len;
    int r = getsockname(s, a, &l);
    *len = (int)l;
    return r;
}
static inline int shim_getpeername(int s, struct sockaddr *a, int *len)
{
    socklen_t l = (socklen_t)*len;
    int r = getpeername(s, a, &l);
    *len = (int)l;
    return r;
}
static inline int shim_setsockopt(int s, int lvl, int opt, const char *v, int len)
{
    return setsockopt(s, lvl, opt, v, (socklen_t)len);
}
static inline int shim_getsockopt(int s, int lvl, int opt, char *v, int *len)
{
    socklen_t l = (socklen_t)*len;
    int r = getsockopt(s, lvl, opt, v, &l);
    *len = (int)l;
    return r;
}
static inline int shim_send(int s, const char *b, int n, int f)
{
    return (int)send(s, b, (size_t)n, f | MSG_NOSIGNAL);
}
static inline int shim_ioctlsocket(int s, long cmd, u_long *arg)
{
    int fl = fcntl(s, F_GETFL, 0);
    (void)cmd;
    if (fl < 0) return -1;
    if (arg && *arg) fl |= O_NONBLOCK;
    else             fl &= ~O_NONBLOCK;
    return fcntl(s, F_SETFL, fl);
}

#define FIONBIO 1L
#define closesocket(s)          close(s)
#define accept(s, a, l)         shim_accept((s), (a), (l))
#define getsockname(s, a, l)    shim_getsockname((s), (a), (l))
#define getpeername(s, a, l)    shim_getpeername((s), (a), (l))
#define setsockopt(s, ...)      shim_setsockopt((s), __VA_ARGS__)
#define getsockopt(s, ...)      shim_getsockopt((s), __VA_ARGS__)
#define send(s, b, n, f)        shim_send((s), (const char *)(b), (n), (f))
#define ioctlsocket(s, c, a)    shim_ioctlsocket((s), (c), (a))

/* ---------------------------------------------------------------- 文件 API */
typedef struct ShimHandle {
    int   kind;      /* 0 = 文件，1 = 线程，2 = 目录查找 */
    int   fd;
    void *ptr;
    int   count;
    int   index;
} ShimHandle;

static inline void shim_init_dirs(void)
{
    static int done = 0;
    static char root[768];
    static char home[768];

    if (done) return;
    {
        const char *r = getenv("FTPD_SHIMROOT");
        const char *h = getenv("FTPD_SHIMHOME");
        snprintf(root, sizeof(root), "%s", (r && *r) ? r : "/tmp/ftpd_root");
        snprintf(home, sizeof(home), "%s", (h && *h) ? h : "/tmp/ftpd_home");
    }
    setenv("FTPD_SHIMROOT", root, 1);
    setenv("FTPD_SHIMHOME", home, 1);
    done = 1;
}

static inline const char *shim_root(void)
{
    shim_init_dirs();
    return getenv("FTPD_SHIMROOT");
}

static inline const char *shim_home(void)
{
    shim_init_dirs();
    return getenv("FTPD_SHIMHOME");
}

static inline int shim_utf8_put(unsigned int c, char *out)
{
    if (c < 0x80) { out[0] = (char)c; return 1; }
    if (c < 0x800) {
        out[0] = (char)(0xC0 | (c >> 6));
        out[1] = (char)(0x80 | (c & 0x3F));
        return 2;
    }
    if (c < 0x10000) {
        out[0] = (char)(0xE0 | (c >> 12));
        out[1] = (char)(0x80 | ((c >> 6) & 0x3F));
        out[2] = (char)(0x80 | (c & 0x3F));
        return 3;
    }
    out[0] = (char)(0xF0 | (c >> 18));
    out[1] = (char)(0x80 | ((c >> 12) & 0x3F));
    out[2] = (char)(0x80 | ((c >> 6) & 0x3F));
    out[3] = (char)(0x80 | (c & 0x3F));
    return 4;
}

/* WinCE 路径 "\a\b" -> 本地 "<root>/a/b" */
static inline void shim_path(const WCHAR *w, char *out, int cap)
{
    char tmp[1024];
    int i, o = 0;

    for (i = 0; w[i] && o < (int)sizeof(tmp) - 8; i++) {
        unsigned int c = (unsigned int)w[i];
        if (c == '\\' || c == '/') tmp[o++] = '/';
        else if (c < 0x80)        tmp[o++] = (char)c;
        else                      o += shim_utf8_put(c, tmp + o);
    }
    tmp[o] = 0;

    /* 程序目录用伪设备 \FTPD_HOME 表示，其余路径都挂在 \ 根下 */
    if (!strncmp(tmp, "/FTPD_HOME", 10))
        snprintf(out, cap, "%s%s", shim_home(), tmp + 10);
    else if (tmp[0] == '/')
        snprintf(out, cap, "%s%s", shim_root(), tmp);
    else
        snprintf(out, cap, "%s/%s", shim_root(), tmp);
}

static inline void shim_time_ft(time_t t, FILETIME *ft)
{
    unsigned long long v = ((unsigned long long)t + 11644473600ULL) * 10000000ULL;
    ft->dwLowDateTime  = (DWORD)(v & 0xFFFFFFFFULL);
    ft->dwHighDateTime = (DWORD)(v >> 32);
}

static inline time_t shim_ft_time(const FILETIME *ft)
{
    unsigned long long v = ((unsigned long long)ft->dwHighDateTime << 32) | ft->dwLowDateTime;
    return (time_t)(v / 10000000ULL) - 11644473600LL;
}

static inline HANDLE shim_new_handle(int kind)
{
    ShimHandle *h = (ShimHandle *)calloc(1, sizeof(ShimHandle));
    if (!h) return INVALID_HANDLE_VALUE;
    h->kind = kind;
    h->fd = -1;
    return (HANDLE)h;
}

static inline int shim_u8_to_w(const char *s, int len, WCHAR *out, int cap);

/* 目录项是 UTF-8 字节，必须真正解码成 UTF-16，不能一字节一字符 */
static inline void shim_wcopy(WCHAR *dst, const char *src)
{
    if (shim_u8_to_w(src, (int)strlen(src), dst, MAX_PATH) < 0) dst[0] = 0;
}

static inline HANDLE CreateFileW(const WCHAR *name, DWORD access, DWORD share,
                                 void *sa, DWORD disp, DWORD flags, HANDLE tmpl)
{
    char path[1024];
    int flags2 = 0, fd;
    ShimHandle *h;

    (void)share; (void)sa; (void)flags; (void)tmpl;
    shim_path(name, path, sizeof(path));

    if (access & GENERIC_READ)  flags2 |= O_RDONLY;
    if (access & GENERIC_WRITE) flags2 = (flags2 ? (O_RDWR) : O_WRONLY);

    switch (disp) {
    case CREATE_ALWAYS:     flags2 |= O_CREAT | O_TRUNC; break;
    case OPEN_ALWAYS:       flags2 |= O_CREAT; break;
    case OPEN_EXISTING:     break;
    case CREATE_NEW:        flags2 |= O_CREAT | O_EXCL; break;
    case TRUNCATE_EXISTING: flags2 |= O_TRUNC; break;
    default: break;
    }

    fd = open(path, flags2, 0666);
    if (fd < 0) return INVALID_HANDLE_VALUE;

    h = (ShimHandle *)shim_new_handle(0);
    if (h == INVALID_HANDLE_VALUE) { close(fd); return INVALID_HANDLE_VALUE; }
    h->fd = fd;
    return (HANDLE)h;
}

static inline BOOL ReadFile(HANDLE h, void *buf, DWORD n, DWORD *got, void *ov)
{
    ShimHandle *sh = (ShimHandle *)h;
    ssize_t r;

    (void)ov;
    if (!got) return FALSE;
    *got = 0;
    if (!sh || sh->kind != 0) return FALSE;
    r = read(sh->fd, buf, (size_t)n);
    if (r < 0) return FALSE;
    *got = (DWORD)r;
    return TRUE;
}

static inline BOOL WriteFile(HANDLE h, const void *buf, DWORD n, DWORD *written, void *ov)
{
    ShimHandle *sh = (ShimHandle *)h;
    DWORD total = 0;

    (void)ov;
    if (!written) return FALSE;
    *written = 0;
    if (!sh || sh->kind != 0) return FALSE;

    while (total < n) {
        ssize_t r = write(sh->fd, (const char *)buf + total, (size_t)(n - total));
        if (r <= 0) return FALSE;
        total += (DWORD)r;
    }
    *written = total;
    return TRUE;
}

static inline DWORD SetFilePointer(HANDLE h, LONG low, LONG *high, DWORD method)
{
    ShimHandle *sh = (ShimHandle *)h;
    int whence = (method == FILE_END) ? SEEK_END :
                 (method == FILE_CURRENT) ? SEEK_CUR : SEEK_SET;
    off_t r;

    (void)high;
    if (!sh || sh->kind != 0) return (DWORD)-1;
    r = lseek(sh->fd, (off_t)low, whence);
    if (r < 0) return (DWORD)-1;
    return (DWORD)r;
}

static inline BOOL SetEndOfFile(HANDLE h)
{
    ShimHandle *sh = (ShimHandle *)h;
    off_t pos;

    if (!sh || sh->kind != 0) return FALSE;
    pos = lseek(sh->fd, 0, SEEK_CUR);
    if (pos < 0) return FALSE;
    return ftruncate(sh->fd, pos) == 0;
}

static inline DWORD GetFileSize(HANDLE h, DWORD *high)
{
    ShimHandle *sh = (ShimHandle *)h;
    struct stat st;

    if (high) *high = 0;
    if (!sh || sh->kind != 0) return 0xFFFFFFFFUL;
    if (fstat(sh->fd, &st) != 0) return 0xFFFFFFFFUL;
    return (DWORD)st.st_size;
}

static inline BOOL CloseHandle(HANDLE h)
{
    ShimHandle *sh = (ShimHandle *)h;

    if (!sh || h == INVALID_HANDLE_VALUE) return FALSE;
    if (sh->kind == 0 && sh->fd >= 0) close(sh->fd);
    if (sh->kind == 2 && sh->ptr) free(sh->ptr);
    free(sh);
    return TRUE;
}

static inline BOOL GetFileTime(HANDLE h, FILETIME *c, FILETIME *a, FILETIME *w)
{
    ShimHandle *sh = (ShimHandle *)h;
    struct stat st;

    if (!sh || sh->kind != 0) return FALSE;
    if (fstat(sh->fd, &st) != 0) return FALSE;
    if (c) shim_time_ft(st.st_ctime, c);
    if (a) shim_time_ft(st.st_atime, a);
    if (w) shim_time_ft(st.st_mtime, w);
    return TRUE;
}

static inline DWORD GetFileAttributesW(const WCHAR *name)
{
    char path[1024];
    struct stat st;
    DWORD attr = 0;

    shim_path(name, path, sizeof(path));
    if (stat(path, &st) != 0) return INVALID_FILE_ATTRIBUTES;
    if (S_ISDIR(st.st_mode)) attr |= FILE_ATTRIBUTE_DIRECTORY;
    if (!(st.st_mode & S_IWUSR)) attr |= FILE_ATTRIBUTE_READONLY;
    if (attr == 0) attr = FILE_ATTRIBUTE_NORMAL;
    return attr;
}

static inline BOOL SetFileAttributesW(const WCHAR *name, DWORD attr)
{
    char path[1024];

    (void)attr;
    shim_path(name, path, sizeof(path));
    return chmod(path, 0666) == 0;
}

static inline BOOL DeleteFileW(const WCHAR *name)
{
    char path[1024];
    struct stat st;

    shim_path(name, path, sizeof(path));
    if (stat(path, &st) != 0) return FALSE;
    if (S_ISDIR(st.st_mode)) return FALSE;
    return unlink(path) == 0;
}

static inline BOOL CreateDirectoryW(const WCHAR *name, void *sa)
{
    char path[1024];
    (void)sa;
    shim_path(name, path, sizeof(path));
    return mkdir(path, 0777) == 0;
}

static inline BOOL RemoveDirectoryW(const WCHAR *name)
{
    char path[1024];
    shim_path(name, path, sizeof(path));
    return rmdir(path) == 0;
}

static inline BOOL MoveFileW(const WCHAR *from, const WCHAR *to)
{
    char a[1024], b[1024];
    shim_path(from, a, sizeof(a));
    shim_path(to, b, sizeof(b));
    return rename(a, b) == 0;
}

/* ------------------------------------------------------------ 目录查找 */
static inline int shim_wild(const char *pat, const char *name)
{
    const char *p = pat, *n = name;
    const char *star = NULL, *sname = NULL;

    while (*n) {
        if (*p == '?' ||
            (*p && tolower((unsigned char)*p) == tolower((unsigned char)*n))) {
            p++; n++;
        } else if (*p == '*') {
            star = p++;
            sname = n;
        } else if (star) {
            p = star + 1;
            n = ++sname;
        } else {
            return 0;
        }
    }
    while (*p == '*') p++;
    return *p == 0;
}

static inline void shim_fill_find(WIN32_FIND_DATAW *fd, const char *dir,
                                  const char *name)
{
    char path[1024];
    struct stat st;

    memset(fd, 0, sizeof(*fd));
    snprintf(path, sizeof(path), "%s/%s", dir, name);
    if (stat(path, &st) != 0) return;

    shim_wcopy(fd->cFileName, name);
    fd->dwFileAttributes = S_ISDIR(st.st_mode) ? FILE_ATTRIBUTE_DIRECTORY
                                               : FILE_ATTRIBUTE_NORMAL;
    fd->nFileSizeLow  = (DWORD)st.st_size;
    fd->nFileSizeHigh = 0;
    shim_time_ft(st.st_mtime, &fd->ftLastWriteTime);
    shim_time_ft(st.st_ctime, &fd->ftCreationTime);
    shim_time_ft(st.st_atime, &fd->ftLastAccessTime);
}

static inline HANDLE FindFirstFileW(const WCHAR *pattern, WIN32_FIND_DATAW *fd)
{
    char path[1024], dir[1024];
    const char *base;
    WIN32_FIND_DATAW *arr = NULL;
    int count = 0, cap = 0;
    BOOL wild;
    ShimHandle *h;

    shim_path(pattern, path, sizeof(path));

    base = strrchr(path, '/');
    if (base) {
        size_t dlen = (size_t)(base - path);
        if (dlen >= sizeof(dir)) return INVALID_HANDLE_VALUE;
        memcpy(dir, path, dlen);
        dir[dlen] = 0;
        if (dlen == 0) { dir[0] = '/'; dir[1] = 0; }
        base++;
    } else {
        snprintf(dir, sizeof(dir), ".");
        base = path;
    }

    wild = (strchr(base, '*') || strchr(base, '?')) ? TRUE : FALSE;

    if (!wild) {
        struct stat st;
        if (stat(path, &st) != 0) return INVALID_HANDLE_VALUE;
        arr = (WIN32_FIND_DATAW *)calloc(1, sizeof(WIN32_FIND_DATAW));
        if (!arr) return INVALID_HANDLE_VALUE;
        shim_fill_find(&arr[0], dir, base);
        count = 1;
    } else {
        DIR *dp = opendir(dir);
        struct dirent *de;
        if (!dp) return INVALID_HANDLE_VALUE;
        while ((de = readdir(dp)) != NULL) {
            if (!shim_wild(base, de->d_name)) continue;
            if (count >= cap) {
                int nc = cap ? cap * 2 : 32;
                WIN32_FIND_DATAW *na = (WIN32_FIND_DATAW *)
                    realloc(arr, (size_t)nc * sizeof(WIN32_FIND_DATAW));
                if (!na) break;
                arr = na;
                cap = nc;
            }
            shim_fill_find(&arr[count], dir, de->d_name);
            count++;
        }
        closedir(dp);
        if (count == 0) { free(arr); return INVALID_HANDLE_VALUE; }
    }

    h = (ShimHandle *)shim_new_handle(2);
    if (h == INVALID_HANDLE_VALUE) { free(arr); return INVALID_HANDLE_VALUE; }
    h->ptr = arr;
    h->count = count;
    h->index = 0;
    *fd = arr[0];
    return (HANDLE)h;
}

static inline BOOL FindNextFileW(HANDLE h, WIN32_FIND_DATAW *fd)
{
    ShimHandle *sh = (ShimHandle *)h;
    WIN32_FIND_DATAW *arr;

    if (!sh || sh->kind != 2) return FALSE;
    sh->index++;
    if (sh->index >= sh->count) return FALSE;
    arr = (WIN32_FIND_DATAW *)sh->ptr;
    *fd = arr[sh->index];
    return TRUE;
}

static inline BOOL FindClose(HANDLE h)
{
    return CloseHandle(h);
}

/* ------------------------------------------------------------ 时间 / 线程 */
static inline void shim_fill_systime(SYSTEMTIME *st, const struct tm *tm)
{
    st->wYear = (WORD)(tm->tm_year + 1900);
    st->wMonth = (WORD)(tm->tm_mon + 1);
    st->wDayOfWeek = (WORD)tm->tm_wday;
    st->wDay = (WORD)tm->tm_mday;
    st->wHour = (WORD)tm->tm_hour;
    st->wMinute = (WORD)tm->tm_min;
    st->wSecond = (WORD)tm->tm_sec;
    st->wMilliseconds = 0;
}

static inline void GetLocalTime(SYSTEMTIME *st)
{
    time_t t = time(NULL);
    struct tm tm;
    localtime_r(&t, &tm);
    shim_fill_systime(st, &tm);
}

static inline void GetSystemTime(SYSTEMTIME *st)
{
    time_t t = time(NULL);
    struct tm tm;
    gmtime_r(&t, &tm);
    shim_fill_systime(st, &tm);
}

static inline BOOL FileTimeToSystemTime(const FILETIME *ft, SYSTEMTIME *st)
{
    time_t t = shim_ft_time(ft);
    struct tm tm;
    gmtime_r(&t, &tm);
    shim_fill_systime(st, &tm);
    return TRUE;
}

static inline BOOL FileTimeToLocalFileTime(const FILETIME *ft, FILETIME *out)
{
    time_t t = shim_ft_time(ft);
    struct tm lt, gt;
    time_t off;

    localtime_r(&t, &lt);
    gt = lt;
    off = mktime(&lt) - mktime(&gt);
    shim_time_ft(t + off, out);
    return TRUE;
}

static inline DWORD GetTickCount(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (DWORD)((unsigned long long)ts.tv_sec * 1000ULL + ts.tv_nsec / 1000000ULL);
}

static inline void Sleep(DWORD ms)
{
    usleep((useconds_t)ms * 1000);
}

typedef struct { pthread_mutex_t m; } CRITICAL_SECTION;

static inline void InitializeCriticalSection(CRITICAL_SECTION *cs)
{
    pthread_mutex_init(&cs->m, NULL);
}
static inline void EnterCriticalSection(CRITICAL_SECTION *cs) { pthread_mutex_lock(&cs->m); }
static inline void LeaveCriticalSection(CRITICAL_SECTION *cs) { pthread_mutex_unlock(&cs->m); }
static inline void DeleteCriticalSection(CRITICAL_SECTION *cs) { pthread_mutex_destroy(&cs->m); }

static inline LONG InterlockedIncrement(volatile LONG *v)
{
    return (LONG)__sync_add_and_fetch(v, 1);
}
static inline LONG InterlockedDecrement(volatile LONG *v)
{
    return (LONG)__sync_sub_and_fetch(v, 1);
}

static inline void *ShimThreadThunk(void *arg)
{
    DWORD (WINAPI *fn)(LPVOID) = (DWORD (WINAPI *)(LPVOID))((void **)arg)[0];
    LPVOID param = ((void **)arg)[1];
    free(arg);
    fn(param);
    return NULL;
}

static inline HANDLE CreateThread(void *sa, size_t stack, DWORD (WINAPI *fn)(LPVOID),
                                 LPVOID param, DWORD flags, DWORD *tid)
{
    pthread_t th;
    pthread_attr_t attr;
    void **box;

    (void)sa; (void)stack; (void)flags;
    if (tid) *tid = 0;

    box = (void **)malloc(2 * sizeof(void *));
    if (!box) return INVALID_HANDLE_VALUE;
    box[0] = (void *)fn;
    box[1] = param;

    pthread_attr_init(&attr);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    if (pthread_create(&th, &attr, ShimThreadThunk, box) != 0) {
        pthread_attr_destroy(&attr);
        free(box);
        return INVALID_HANDLE_VALUE;
    }
    pthread_attr_destroy(&attr);
    return (HANDLE)shim_new_handle(1);
}

/* ------------------------------------------------------------ 编码 / 路径 */
static inline int shim_u8_to_w(const char *s, int len, WCHAR *out, int cap)
{
    int i = 0, o = 0;

    while (i < len) {
        unsigned char b = (unsigned char)s[i];
        unsigned int c;
        int n, k;

        if (b < 0x80)                { c = b; n = 1; }
        else if ((b & 0xE0) == 0xC0) { c = b & 0x1F; n = 2; }
        else if ((b & 0xF0) == 0xE0) { c = b & 0x0F; n = 3; }
        else if ((b & 0xF8) == 0xF0) { c = b & 0x07; n = 4; }
        else return -1;

        if (i + n > len) return -1;
        for (k = 1; k < n; k++) {
            unsigned char cb = (unsigned char)s[i + k];
            if ((cb & 0xC0) != 0x80) return -1;
            c = (c << 6) | (cb & 0x3F);
        }
        i += n;
        if (o + 2 >= cap) break;
        if (c >= 0x10000) {
            c -= 0x10000;
            out[o++] = (WCHAR)(0xD800 + (c >> 10));
            out[o++] = (WCHAR)(0xDC00 + (c & 0x3FF));
        } else {
            out[o++] = (WCHAR)c;
        }
    }
    out[o] = 0;
    return o;
}

static inline int shim_w_to_u8(const WCHAR *s, int len, char *out, int cap)
{
    int i, o = 0;

    for (i = 0; i < len && o < cap - 5; i++) {
        unsigned int c = (unsigned int)s[i];
        if (c >= 0xD800 && c <= 0xDBFF && i + 1 < len) {
            unsigned int c2 = (unsigned int)s[i + 1];
            if (c2 >= 0xDC00 && c2 <= 0xDFFF) {
                c = 0x10000 + ((c - 0xD800) << 10) + (c2 - 0xDC00);
                i++;
            }
        }
        o += shim_utf8_put(c, out + o);
    }
    out[o] = 0;
    return o;
}

/* GBK = 936：垫片用 iconv 真做转换，否则 GBK 模式测不出来 */
static inline int shim_iconv_to_w(const char *code, const char *s, int len,
                                  WCHAR *out, int cap)
{
    iconv_t cd = iconv_open("UTF-32LE", code);
    char *ip = (char *)s, *op = (char *)out;
    size_t il = (size_t)len, ol = (size_t)(cap - 1) * 4;
    size_t r;

    if (cd == (iconv_t)-1) return -1;
    r = iconv(cd, &ip, &il, &op, &ol);
    iconv_close(cd);
    if (r == (size_t)-1) return -1;
    {
        int n = (int)(((char *)op - (char *)out) / 4);
        out[n] = 0;
        return n;
    }
}

static inline int shim_iconv_from_w(const char *code, const WCHAR *s, int len,
                                    char *out, int cap)
{
    iconv_t cd = iconv_open(code, "UTF-32LE");
    char *ip = (char *)s, *op = out;
    size_t il = (size_t)len * 4, ol = (size_t)cap - 1;
    size_t r;

    if (cd == (iconv_t)-1) return -1;
    r = iconv(cd, &ip, &il, &op, &ol);
    iconv_close(cd);
    if (r == (size_t)-1) return -1;
    {
        int n = (int)(op - out);
        out[n] = 0;
        return n;
    }
}

static inline int MultiByteToWideChar(unsigned int cp, DWORD flags, const char *s,
                                      int len, WCHAR *out, int cap)
{
    (void)flags;
    if (len < 0) len = (int)strlen(s);
    if (cap <= 0) return 0;
    if (cp == 936) {
        int r = shim_iconv_to_w("GBK", s, len, out, cap);
        if (r < 0) { out[0] = 0; return 0; }
        return r;
    }
    return shim_u8_to_w(s, len, out, cap);
}

static inline int WideCharToMultiByte(unsigned int cp, DWORD flags, const WCHAR *s,
                                      int len, char *out, int cap,
                                      const char *def, BOOL *used)
{
    (void)flags; (void)def; (void)used;
    if (len < 0) len = (int)wcslen(s);
    if (cap <= 0) return 0;
    if (cp == 936) {
        int n = shim_iconv_from_w("GBK", s, len, out, cap);
        if (n < 0) { out[0] = 0; return 0; }
        if (n > cap - 1) n = cap - 1;
        out[n] = 0;
        return n;
    }
    return shim_w_to_u8(s, len, out, cap);
}

static inline DWORD GetModuleFileNameW(HINSTANCE inst, WCHAR *out, DWORD cap)
{
    static const WCHAR fake[] = L"\\FTPD_HOME\\ftpd.exe";
    DWORD i;

    (void)inst;
    for (i = 0; fake[i] && i < cap - 1; i++) out[i] = fake[i];
    out[i] = 0;
    return i;
}

#define CP_ACP  0
#define CP_UTF8 65001

#define OutputDebugStringW(s) ((void)(s))

#endif /* WINCE_HOST_H */
