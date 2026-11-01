/* ============================================================================
 * ftpd.c —— WinCE 端 FTP 服务器（手机经 Wi-Fi 访问 / 上传 / 修改车机文件）
 *
 * 目标平台：Windows CE 5.0 / 6.0 / 7.0（ARM / MIPS / x86 均可），
 *           也兼容 Windows Mobile 5/6。
 *
 * 只依赖系统自带的 coredll（文件、线程、编码转换）和 ws2（WinSock）。
 * 不依赖 MFC / ATL / .NET / STL，单文件即可编译。
 *
 * 支持的 FTP 命令：
 *   USER PASS ACCT QUIT NOOP SYST FEAT OPTS CLNT HELP STAT SITE
 *   PWD XPWD CWD XCWD CDUP XCUP TYPE MODE STRU ALLO
 *   PASV EPSV PORT EPRT
 *   LIST NLST MLSD MLST
 *   RETR STOR APPE REST SIZE MDTM
 *   DELE MKD XMKD RMD XRMD RNFR RNTO
 *
 * 设计要点（都是 WinCE 上真正会踩的坑）：
 *   1. 手机端（AndFTP / ES / FE / Solid / Cx 等）几乎都用【被动模式】，
 *      所以 PASV/EPSV 必须返回车机在局域网里的真实 IP，不能回 0.0.0.0
 *      或 127.0.0.1，否则手机连不上数据端口。
 *   2. WinCE 文件名是 UTF-16，手机发来的是 UTF-8。这里自己实现 UTF-8
 *      编解码，不依赖 CP_UTF8 的支持情况；老客户端发 GBK 时按 CP_ACP 兜底。
 *   3. 路径必须做规范化，防止 "..\.." 逃出配置的根目录。
 *   4. 每个连接一个线程，主线程只负责 accept；数据连接单独一条 TCP。
 *   5. 车机常常没有控制台，所以日志写文件，启动时把本机 IP 也写进去，
 *      方便直接在手机上填地址。
 *
 * 编译：见同目录 README.md（CeGCC 或 VS2005/2008 均可）。
 * ==========================================================================*/

#include <winsock2.h>
#include <windows.h>
#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

#ifndef MAX_PATH
#define MAX_PATH 260
#endif

#define FTPD_NAME     "WinCE-FTPD"
#define FTPD_VERSION  "1.0"

/* ── 格式化输出：三个平台三套名字 ──
 *   MSVC / eVC4     : _vsnprintf
 *   CeGCC + GNU ld  : vsnprintf（它自带的 mingwex 有）
 *   裸 coredll 链接 : 两者都没有，只能拿 coredll 的 wvsprintfW 包一层
 * 最后一种是用 LLVM lld-link 直接链 coredll 时才会遇到，
 * 但真实存在（CeGCC 的 GNU 导入库 lld 读不了）。
 */
#if defined(_MSC_VER)
#define FTP_VSNPRINTF _vsnprintf
#elif defined(FTPD_BARE_COREDLL)
static int ftpd_bare_vsnprintf(char *buf, int cap, const char *fmt, va_list ap);
#define FTP_VSNPRINTF ftpd_bare_vsnprintf
#else
#define FTP_VSNPRINTF vsnprintf
#endif

/* LIST 的三种形态 */
#define LST_LIST 0
#define LST_NLST 1
#define LST_MLSD 2

/* --------------------------------------------------------------------------
 * 配置
 * ------------------------------------------------------------------------*/
typedef struct {
    int   port;                 /* 监听端口，默认 21 */
    WCHAR root[MAX_PATH];       /* 对外暴露的根目录，L"\\" = 整个设备 */
    WCHAR user[64];             /* 空 = 允许匿名（任意用户名） */
    WCHAR pass[64];             /* 空 = 不需要密码 */
    BOOL  readOnly;             /* 只读模式 */
    int   maxClients;           /* 最大并发连接数 */
    WCHAR logFile[MAX_PATH];    /* 空 = 不写日志 */
    char  pasvIP[32];           /* 非空则 PASV 固定回报这个 IP */
    int   ansiCP;               /* 0 = UTF-8；936 = GBK 兼容老客户端 */
} FtpConfig;

static FtpConfig        g_cfg;
static SOCKET           g_listen = INVALID_SOCKET;
static volatile BOOL    g_run = TRUE;
static volatile LONG    g_clients = 0;
static volatile LONG    g_sessionId = 0;
static HANDLE           g_log = INVALID_HANDLE_VALUE;
static CRITICAL_SECTION g_logLock;

/* --------------------------------------------------------------------------
 * 会话
 * ------------------------------------------------------------------------*/
typedef struct {
    SOCKET  ctrl;               /* 控制连接 */
    SOCKET  pasv;               /* 被动模式监听套接字 */
    struct sockaddr_in portAddr;/* 主动模式 PORT 指定的目标 */
    BOOL    hasPort;
    WCHAR   cwd[MAX_PATH];      /* 虚拟当前目录，形如 L"\\Music" */
    BOOL    loggedIn;
    BOOL    needPass;
    int     type;               /* 0 = ASCII, 1 = IMAGE */
    DWORD   rest;               /* REST 断点偏移 */
    WCHAR   rnfr[MAX_PATH];
    BOOL    hasRnfr;
    int     id;
    char    peer[64];
    char    inbuf[1024];
    int     inlen;
} FtpSession;

/* ==========================================================================
 * 一、基础工具
 * ========================================================================*/

#if defined(FTPD_BARE_COREDLL)
/* 用 coredll 的 wvsprintfW 实现一个最小 vsnprintf。
 * 只支持本项目实际用到的格式：%s %d %u %04u %02u %3d %-8s %10u %c。
 * wvsprintfW 最多产出 1024 字节，正好够本文件所有输出（最大缓冲 1200，
 * 且每条消息实际远小于 1024）。 */
static int ftpd_bare_vsnprintf(char *buf, int cap, const char *fmt, va_list ap)
{
    WCHAR wbuf[1024];
    WCHAR wfmt[512];
    int i, n;

    if (cap <= 0) return 0;
    for (i = 0; fmt[i] && i < 511; i++) wfmt[i] = (WCHAR)(unsigned char)fmt[i];
    wfmt[i] = 0;

    if (wvsprintfW(wbuf, wfmt, ap) <= 0) {
        buf[0] = 0;
        return 0;
    }
    n = WideCharToMultiByte(CP_ACP, 0, wbuf, -1, buf, cap, NULL, NULL);
    if (n <= 0) { buf[0] = 0; return 0; }
    return n - 1;   /* WideCharToMultiByte 含结尾 0 */
}
#endif

static int FormatA(char *dst, int cap, const char *fmt, ...)
{
    va_list ap;
    int n;

    if (cap <= 0) return 0;
    va_start(ap, fmt);
    n = FTP_VSNPRINTF(dst, cap - 1, fmt, ap);
    va_end(ap);
    if (n < 0) n = 0;
    if (n > cap - 1) n = cap - 1;
    dst[n] = 0;
    return n;
}

static void LogA(const char *fmt, ...)
{
    char msg[768];
    char line[896];
    SYSTEMTIME st;
    DWORD written = 0;
    va_list ap;
    int n;

    if (g_log == INVALID_HANDLE_VALUE) return;

    va_start(ap, fmt);
    n = FTP_VSNPRINTF(msg, sizeof(msg) - 1, fmt, ap);
    va_end(ap);
    if (n < 0) n = 0;
    if (n > (int)sizeof(msg) - 1) n = (int)sizeof(msg) - 1;
    msg[n] = 0;

    GetLocalTime(&st);
    FormatA(line, sizeof(line), "%04u-%02u-%02u %02u:%02u:%02u  %s\r\n",
            (unsigned)st.wYear, (unsigned)st.wMonth, (unsigned)st.wDay,
            (unsigned)st.wHour, (unsigned)st.wMinute, (unsigned)st.wSecond, msg);

    EnterCriticalSection(&g_logLock);
    SetFilePointer(g_log, 0, NULL, FILE_END);
    WriteFile(g_log, line, (DWORD)strlen(line), &written, NULL);
    LeaveCriticalSection(&g_logLock);
}

/* 宽串拷贝：刻意不用 lstrcpyW / wcscpy，某些车机的 coredll 导出不全 */
static void WCopy(WCHAR *dst, int cap, const WCHAR *src)
{
    int i;

    if (cap <= 0) return;
    if (!src) { dst[0] = 0; return; }
    for (i = 0; i < cap - 1 && src[i]; i++) dst[i] = src[i];
    dst[i] = 0;
}

static void WCat(WCHAR *dst, int cap, const WCHAR *src)
{
    int n = (int)wcslen(dst);
    WCopy(dst + n, cap - n, src);
}

static WCHAR UpChar(WCHAR c)
{
    if (c >= L'a' && c <= L'z') return (WCHAR)(c - 32);
    return c;
}

static BOOL WEqualCI(const WCHAR *a, const WCHAR *b)
{
    int i = 0;
    for (;;) {
        WCHAR ca = UpChar(a[i]);
        WCHAR cb = UpChar(b[i]);
        if (ca != cb) return FALSE;
        if (!ca) return TRUE;
        i++;
    }
}

static int StriCmp(const char *a, const char *b)
{
    for (;;) {
        int ca = (unsigned char)*a;
        int cb = (unsigned char)*b;
        if (ca >= 'A' && ca <= 'Z') ca += 32;
        if (cb >= 'A' && cb <= 'Z') cb += 32;
        if (ca != cb) return ca - cb;
        if (!ca) return 0;
        a++; b++;
    }
}

static BOOL StriPrefix(const char *s, const char *prefix)
{
    int i;
    for (i = 0; prefix[i]; i++) {
        int ca = (unsigned char)s[i];
        int cb = (unsigned char)prefix[i];
        if (ca >= 'A' && ca <= 'Z') ca += 32;
        if (cb >= 'A' && cb <= 'Z') cb += 32;
        if (ca != cb) return FALSE;
    }
    return TRUE;
}

static BOOL WEqual(const WCHAR *a, const WCHAR *b)
{
    int i = 0;
    for (;;) {
        if (a[i] != b[i]) return FALSE;
        if (!a[i]) return TRUE;
        i++;
    }
}

/* UTF-16 -> UTF-8 */
static int WideToUtf8(const WCHAR *src, int srcLen, char *dst, int dstCap)
{
    int i, o = 0;

    if (dstCap <= 0) return 0;
    for (i = 0; i < srcLen; i++) {
        unsigned int c = (unsigned int)src[i];
        char tmp[4];
        int n;

        if (c >= 0xD800 && c <= 0xDBFF && i + 1 < srcLen) {
            unsigned int c2 = (unsigned int)src[i + 1];
            if (c2 >= 0xDC00 && c2 <= 0xDFFF) {
                c = 0x10000 + ((c - 0xD800) << 10) + (c2 - 0xDC00);
                i++;
            }
        }

        if (c < 0x80) {
            tmp[0] = (char)c; n = 1;
        } else if (c < 0x800) {
            tmp[0] = (char)(0xC0 | (c >> 6));
            tmp[1] = (char)(0x80 | (c & 0x3F));
            n = 2;
        } else if (c < 0x10000) {
            tmp[0] = (char)(0xE0 | (c >> 12));
            tmp[1] = (char)(0x80 | ((c >> 6) & 0x3F));
            tmp[2] = (char)(0x80 | (c & 0x3F));
            n = 3;
        } else {
            tmp[0] = (char)(0xF0 | (c >> 18));
            tmp[1] = (char)(0x80 | ((c >> 12) & 0x3F));
            tmp[2] = (char)(0x80 | ((c >> 6) & 0x3F));
            tmp[3] = (char)(0x80 | (c & 0x3F));
            n = 4;
        }
        if (o + n >= dstCap) break;
        memcpy(dst + o, tmp, n);
        o += n;
    }
    dst[o] = 0;
    return o;
}

/* UTF-8 -> UTF-16；遇到非法字节返回 -1（调用方再用 CP_ACP 兜底） */
static int Utf8ToWide(const char *src, int srcLen, WCHAR *dst, int dstCap)
{
    int i = 0, o = 0;

    if (dstCap <= 0) return -1;
    while (i < srcLen) {
        unsigned char b = (unsigned char)src[i];
        unsigned int c;
        int n, k;

        if (b < 0x80)          { c = b;        n = 1; }
        else if ((b & 0xE0) == 0xC0) { c = b & 0x1F; n = 2; }
        else if ((b & 0xF0) == 0xE0) { c = b & 0x0F; n = 3; }
        else if ((b & 0xF8) == 0xF0) { c = b & 0x07; n = 4; }
        else return -1;

        if (i + n > srcLen) return -1;
        for (k = 1; k < n; k++) {
            unsigned char cb = (unsigned char)src[i + k];
            if ((cb & 0xC0) != 0x80) return -1;
            c = (c << 6) | (cb & 0x3F);
        }
        i += n;

        if (c >= 0x10000) {
            if (o + 2 >= dstCap) break;
            c -= 0x10000;
            dst[o++] = (WCHAR)(0xD800 + (c >> 10));
            dst[o++] = (WCHAR)(0xDC00 + (c & 0x3FF));
        } else {
            if (o + 1 >= dstCap) break;
            dst[o++] = (WCHAR)c;
        }
    }
    dst[o] = 0;
    return o;
}

/* 命令参数（UTF-8 / GBK 自适应）-> 宽串 */
static void DecodeArg(const char *s, WCHAR *out, int cap)
{
    int len = (int)strlen(s);
    int n;

    if (cap <= 0) return;
    if (len <= 0) { out[0] = 0; return; }

    if (g_cfg.ansiCP != 0) {
        n = MultiByteToWideChar(g_cfg.ansiCP, 0, s, len, out, cap - 1);
    } else {
        n = Utf8ToWide(s, len, out, cap - 1);
        if (n < 0) {
            /* 手机会有老客户端直接发 GBK，按系统 ANSI 代码页兜底 */
            n = MultiByteToWideChar(CP_ACP, 0, s, len, out, cap - 1);
        }
    }
    if (n < 0) n = 0;
    if (n > cap - 1) n = cap - 1;
    out[n] = 0;
}

/* 宽串 -> 发送给客户端用的字节串 */
static void EncodeName(const WCHAR *w, char *out, int cap)
{
    int len = (int)wcslen(w);

    if (cap <= 0) return;
    if (g_cfg.ansiCP != 0) {
        int n = WideCharToMultiByte(g_cfg.ansiCP, 0, w, len, out, cap - 1, NULL, NULL);
        if (n < 0) n = 0;
        if (n > cap - 1) n = cap - 1;
        out[n] = 0;
    } else {
        WideToUtf8(w, len, out, cap);
    }
}

/* ==========================================================================
 * 二、虚拟路径
 * ========================================================================*/

/*
 * 把 base + arg 规范化成从根出发的虚拟路径：
 *   "\"      根
 *   "\a\b"   其余
 * 处理 "."、".."、"/" 与 "\" 混用，且保证不会越过根。
 */
static void NormalizeVirtual(const WCHAR *base, const WCHAR *arg, WCHAR *out, int cap)
{
    WCHAR tmp[MAX_PATH * 2];
    WCHAR res[MAX_PATH];
    int i = 0, o = 0;

    res[0] = 0;

    if (arg && (arg[0] == L'\\' || arg[0] == L'/')) {
        WCopy(tmp, MAX_PATH * 2, arg);
    } else {
        WCopy(tmp, MAX_PATH * 2, (base && base[0]) ? base : L"\\");
        if (arg && arg[0]) {
            WCat(tmp, MAX_PATH * 2, L"\\");
            WCat(tmp, MAX_PATH * 2, arg);
        }
    }

    while (tmp[i]) {
        int start, n;

        while (tmp[i] == L'\\' || tmp[i] == L'/') i++;
        start = i;
        while (tmp[i] && tmp[i] != L'\\' && tmp[i] != L'/') i++;
        n = i - start;
        if (n <= 0) continue;

        if (n == 1 && tmp[start] == L'.') continue;
        if (n == 2 && tmp[start] == L'.' && tmp[start + 1] == L'.') {
            if (o > 0) {
                int k = o - 1;
                while (k > 0 && res[k] != L'\\') k--;
                o = k;
                res[o] = 0;
            }
            continue;
        }

        if (n > MAX_PATH - 2) n = MAX_PATH - 2;
        if (o + n + 2 >= MAX_PATH) break;
        res[o++] = L'\\';
        {
            int k;
            for (k = 0; k < n; k++) res[o + k] = tmp[start + k];
        }
        o += n;
        res[o] = 0;
    }

    if (o == 0) { res[0] = L'\\'; res[1] = 0; }
    WCopy(out, cap, res);
}

/* 虚拟路径 -> 真实路径（拼上配置的根目录） */
static void MakeFull(const WCHAR *virt, WCHAR *full, int cap)
{
    WCHAR root[MAX_PATH];
    int n;

    WCopy(root, MAX_PATH, g_cfg.root);
    n = (int)wcslen(root);
    while (n > 1 && (root[n - 1] == L'\\' || root[n - 1] == L'/')) {
        root[--n] = 0;
    }
    if (root[0] == 0) { root[0] = L'\\'; root[1] = 0; n = 1; }

    if (virt[0] == L'\\' && virt[1] == 0) {
        WCopy(full, cap, root);
    } else if (n == 1 && root[0] == L'\\') {
        WCopy(full, cap, virt);
    } else {
        WCopy(full, cap, root);
        WCat(full, cap, virt);
    }
}

static const WCHAR *BaseName(const WCHAR *path)
{
    int i, last = 0;

    for (i = 0; path[i]; i++) {
        if (path[i] == L'\\' || path[i] == L'/') last = i + 1;
    }
    return path + last;
}

/* 虚拟路径 -> 显示用 "/a/b"（UTF-8） */
static void DisplayPath(const WCHAR *virt, char *out, int cap)
{
    WCHAR w[MAX_PATH];
    int i, o = 0;

    w[o++] = L'/';
    for (i = 1; virt[i] && o < MAX_PATH - 2; i++) {
        w[o++] = (virt[i] == L'\\') ? L'/' : virt[i];
    }
    w[o] = 0;
    EncodeName(w, out, cap);
}

/* ==========================================================================
 * 三、套接字
 * ========================================================================*/

static int SockWait(SOCKET s, BOOL forWrite, int timeoutMs)
{
    fd_set set;
    struct timeval tv;
    int r;

    FD_ZERO(&set);
    FD_SET(s, &set);
    tv.tv_sec = timeoutMs / 1000;
    tv.tv_usec = (timeoutMs % 1000) * 1000;

    if (forWrite) r = select((int)s + 1, NULL, &set, NULL, &tv);
    else          r = select((int)s + 1, &set, NULL, NULL, &tv);
    return r;   /* 1 就绪，0 超时，-1 出错 */
}

/* 返回 >0 读到的字节，0 对端关闭，-1 错误，-2 超时 */
static int SockRecv(SOCKET s, char *buf, int len, int timeoutMs)
{
    int r = SockWait(s, FALSE, timeoutMs);
    if (r == 0) return -2;
    if (r < 0) return -1;
    r = recv(s, buf, len, 0);
    if (r == 0) return 0;
    if (r < 0) return (WSAGetLastError() == WSAEWOULDBLOCK) ? -2 : -1;
    return r;
}

static BOOL SockSendAll(SOCKET s, const char *buf, int len, int timeoutMs)
{
    int sent = 0;

    while (sent < len) {
        int r = SockWait(s, TRUE, timeoutMs);
        if (r <= 0) return FALSE;
        r = send(s, buf + sent, len - sent, 0);
        if (r <= 0) {
            if (r < 0 && WSAGetLastError() == WSAEWOULDBLOCK) continue;
            return FALSE;
        }
        sent += r;
    }
    return TRUE;
}

static void Reply(FtpSession *s, const char *fmt, ...)
{
    char buf[1200];
    va_list ap;
    int n;

    va_start(ap, fmt);
    n = FTP_VSNPRINTF(buf, sizeof(buf) - 3, fmt, ap);
    va_end(ap);
    if (n < 0) n = 0;
    if (n > (int)sizeof(buf) - 3) n = (int)sizeof(buf) - 3;
    buf[n++] = '\r';
    buf[n++] = '\n';
    SockSendAll(s->ctrl, buf, n, 20000);
}

/* ==========================================================================
 * 四、数据连接
 * ========================================================================*/

/* 优雅地关掉数据连接。
 *
 * ⚠ 这里有个真实踩过的坑：直接 closesocket() 在还有未读数据时，
 *   WinSock 会发 RST 而不是 FIN。手机端多半是用 read/write 循环收目录，
 *   拿到 RST 会把整份列表当"传输中断"丢掉——表现就是
 *   「手机里删文件立刻生效，但车机新增的文件怎么刷新都不出现」。
 *   先 shutdown(SD_SEND) 把 FIN 发出去，再收掉对端可能残留的字节，
 *   最后才 close，对端就能干净地读到 EOF。 */
static void DataCloseGraceful(SOCKET d)
{
    char sink[512];
    int guard = 0;

    if (d == INVALID_SOCKET) return;

    shutdown(d, SD_SEND);

    /* 把对端可能还在发的剩余字节读掉，最多读 64KB / 32 次，
     * 避免对端因 unread data 触发 RST。 */
    while (guard++ < 32) {
        int r = SockRecv(d, sink, (int)sizeof(sink), 1000);
        if (r <= 0) break;
    }
    closesocket(d);
}

static void CloseData(FtpSession *s)
{
    if (s->pasv != INVALID_SOCKET) {
        closesocket(s->pasv);
        s->pasv = INVALID_SOCKET;
    }
    s->hasPort = FALSE;
}

static SOCKET DataOpen(FtpSession *s)
{
    SOCKET d = INVALID_SOCKET;

    /* 被动模式：接受客户端连过来的数据连接 */
    if (s->pasv != INVALID_SOCKET) {
        struct sockaddr_in a;
        int alen = (int)sizeof(a);

        if (SockWait(s->pasv, FALSE, 20000) > 0) {
            d = accept(s->pasv, (struct sockaddr *)&a, &alen);
        }
        closesocket(s->pasv);
        s->pasv = INVALID_SOCKET;
        if (d != INVALID_SOCKET) {
            int one = 1;
            setsockopt(d, IPPROTO_TCP, TCP_NODELAY, (const char *)&one, (int)sizeof(one));
        }
        return d;
    }

    /* 主动模式：连回 PORT/EPRT 指定的地址 */
    if (s->hasPort) {
        u_long nb = 1;
        int err = 0, elen = (int)sizeof(err);
        int r;
        fd_set wf, ef;
        struct timeval tv;

        s->hasPort = FALSE;
        d = socket(AF_INET, SOCK_STREAM, 0);
        if (d == INVALID_SOCKET) return INVALID_SOCKET;

        ioctlsocket(d, FIONBIO, &nb);
        r = connect(d, (struct sockaddr *)&s->portAddr, (int)sizeof(s->portAddr));
        if (r != 0 && WSAGetLastError() != WSAEWOULDBLOCK) {
            closesocket(d);
            return INVALID_SOCKET;
        }
        FD_ZERO(&wf); FD_SET(d, &wf);
        FD_ZERO(&ef); FD_SET(d, &ef);
        tv.tv_sec = 15; tv.tv_usec = 0;
        r = select((int)d + 1, NULL, &wf, &ef, &tv);
        if (r <= 0) { closesocket(d); return INVALID_SOCKET; }

        getsockopt(d, SOL_SOCKET, SO_ERROR, (char *)&err, &elen);
        if (err != 0) { closesocket(d); return INVALID_SOCKET; }

        nb = 0;
        ioctlsocket(d, FIONBIO, &nb);
        {
            int one = 1;
            setsockopt(d, IPPROTO_TCP, TCP_NODELAY, (const char *)&one, (int)sizeof(one));
        }
        return d;
    }

    return INVALID_SOCKET;
}

/* PASV / EPSV */
static void CmdPasv(FtpSession *s, BOOL extended)
{
    struct sockaddr_in a;
    struct sockaddr_in local;
    SOCKET ls;
    u_long one = 1;
    int alen, port;
    unsigned int ip = 0;

    CloseData(s);

    ls = socket(AF_INET, SOCK_STREAM, 0);
    if (ls == INVALID_SOCKET) {
        Reply(s, "425 Cannot create passive socket.");
        return;
    }
    setsockopt(ls, SOL_SOCKET, SO_REUSEADDR, (const char *)&one, (int)sizeof(one));

    memset(&a, 0, sizeof(a));
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = INADDR_ANY;
    a.sin_port = 0;
    if (bind(ls, (struct sockaddr *)&a, (int)sizeof(a)) != 0 ||
        listen(ls, 1) != 0) {
        closesocket(ls);
        Reply(s, "425 Cannot open passive port.");
        return;
    }

    alen = (int)sizeof(a);
    if (getsockname(ls, (struct sockaddr *)&a, &alen) != 0) {
        closesocket(ls);
        Reply(s, "425 Cannot query passive port.");
        return;
    }
    port = (int)ntohs(a.sin_port);

    /* 回报的 IP 必须是手机能路由到的那个，取控制连接的本端地址 */
    alen = (int)sizeof(local);
    if (getsockname(s->ctrl, (struct sockaddr *)&local, &alen) == 0) {
        ip = (unsigned int)local.sin_addr.s_addr;
    }
    if (g_cfg.pasvIP[0]) {
        ip = (unsigned int)inet_addr(g_cfg.pasvIP);
    }
    if (ip == 0 || ip == 0xFFFFFFFFU) ip = (unsigned int)inet_addr("127.0.0.1");

    if (extended) {
        Reply(s, "229 Entering Extended Passive Mode (|||%d|)", port);
    } else {
        unsigned char *b = (unsigned char *)&ip;
        Reply(s, "227 Entering Passive Mode (%u,%u,%u,%u,%d,%d)",
              (unsigned)b[0], (unsigned)b[1], (unsigned)b[2], (unsigned)b[3],
              port >> 8, port & 0xFF);
    }

    s->pasv = ls;
}

static void CmdPort(FtpSession *s, const char *arg)
{
    unsigned int h1, h2, h3, h4, p1, p2;
    struct sockaddr_in peer;
    int plen = (int)sizeof(peer);
    unsigned long ip;

    if (sscanf(arg, "%u,%u,%u,%u,%u,%u", &h1, &h2, &h3, &h4, &p1, &p2) != 6 ||
        h1 > 255 || h2 > 255 || h3 > 255 || h4 > 255 || p1 > 255 || p2 > 255) {
        Reply(s, "501 Bad PORT syntax.");
        return;
    }

    ip = htonl((h1 << 24) | (h2 << 16) | (h3 << 8) | h4);

    /* 防 FTP bounce：只接受控制连接对端自己声明的地址 */
    if (getpeername(s->ctrl, (struct sockaddr *)&peer, &plen) == 0) {
        if (peer.sin_addr.s_addr != ip &&
            peer.sin_addr.s_addr != htonl(0x7F000001)) {
            Reply(s, "501 PORT address does not match control connection.");
            return;
        }
    }

    CloseData(s);
    memset(&s->portAddr, 0, sizeof(s->portAddr));
    s->portAddr.sin_family = AF_INET;
    s->portAddr.sin_addr.s_addr = ip;
    s->portAddr.sin_port = htons((unsigned short)(p1 * 256 + p2));
    s->hasPort = TRUE;
    Reply(s, "200 PORT command successful.");
}

static void CmdEprt(FtpSession *s, char *arg)
{
    char *p1, *p2, *p3;
    char ipstr[64];
    int port;
    struct sockaddr_in peer;
    int plen = (int)sizeof(peer);

    /* 格式：|1|1.2.3.4|1234| */
    if (arg[0] != '|') { Reply(s, "501 Bad EPRT syntax."); return; }
    p1 = arg + 1;
    p2 = strchr(p1, '|');
    if (!p2) { Reply(s, "501 Bad EPRT syntax."); return; }
    *p2++ = 0;
    p3 = strchr(p2, '|');
    if (!p3) { Reply(s, "501 Bad EPRT syntax."); return; }
    *p3 = 0;

    if (p1[0] != '1') { Reply(s, "522 Network protocol not supported, use (1)."); return; }
    if ((int)strlen(p2) >= (int)sizeof(ipstr)) { Reply(s, "501 Bad EPRT syntax."); return; }
    strcpy(ipstr, p2);
    port = atoi(p3 + 1);
    if (port <= 0 || port > 65535) { Reply(s, "501 Bad EPRT port."); return; }

    CloseData(s);
    memset(&s->portAddr, 0, sizeof(s->portAddr));
    s->portAddr.sin_family = AF_INET;
    s->portAddr.sin_addr.s_addr = inet_addr(ipstr);
    s->portAddr.sin_port = htons((unsigned short)port);
    if (s->portAddr.sin_addr.s_addr == INADDR_NONE) {
        Reply(s, "501 Bad EPRT address.");
        return;
    }
    if (getpeername(s->ctrl, (struct sockaddr *)&peer, &plen) == 0) {
        if (peer.sin_addr.s_addr != s->portAddr.sin_addr.s_addr &&
            peer.sin_addr.s_addr != htonl(0x7F000001)) {
            Reply(s, "501 EPRT address does not match control connection.");
            return;
        }
    }
    s->hasPort = TRUE;
    Reply(s, "200 EPRT command successful.");
}

/* ==========================================================================
 * 五、目录列表
 * ========================================================================*/

static BOOL GetPathInfo(const WCHAR *full, DWORD *pattr, DWORD *psize, FILETIME *pft)
{
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW(full, &fd);

    if (h == INVALID_HANDLE_VALUE) return FALSE;
    FindClose(h);
    if (pattr) *pattr = fd.dwFileAttributes;
    if (psize) *psize = fd.nFileSizeLow;
    if (pft)   *pft = fd.ftLastWriteTime;
    return TRUE;
}

static void SanitizeName(char *s)
{
    int i;
    for (i = 0; s[i]; i++) {
        if (s[i] == '\r' || s[i] == '\n') s[i] = '_';
    }
}

static void SendOneEntry(FtpSession *s, SOCKET d, const WCHAR *name,
                         DWORD attr, DWORD size, const FILETIME *pft, int mode)
{
    static const char *mon[] = { "Jan", "Feb", "Mar", "Apr", "May", "Jun",
                                 "Jul", "Aug", "Sep", "Oct", "Nov", "Dec" };
    char nm[MAX_PATH * 3];
    char line[MAX_PATH * 3 + 160];
    BOOL isDir = (attr & FILE_ATTRIBUTE_DIRECTORY) ? TRUE : FALSE;

    (void)s;
    EncodeName(name, nm, sizeof(nm));
    SanitizeName(nm);

    if (mode == LST_NLST) {
        FormatA(line, sizeof(line), "%s\r\n", nm);
    } else if (mode == LST_MLSD) {
        SYSTEMTIME st;
        FileTimeToSystemTime(pft, &st);
        FormatA(line, sizeof(line),
                "type=%s;size=%u;modify=%04u%02u%02u%02u%02u%02u;perm=%s; %s\r\n",
                isDir ? "dir" : "file", (unsigned)size,
                (unsigned)st.wYear, (unsigned)st.wMonth, (unsigned)st.wDay,
                (unsigned)st.wHour, (unsigned)st.wMinute, (unsigned)st.wSecond,
                isDir ? "elcmp" : "adfrw", nm);
    } else {
        SYSTEMTIME st, now;
        FILETIME localFt;
        char dateBuf[32];
        const char *perm;

        if (isDir) perm = g_cfg.readOnly ? "dr-xr-xr-x" : "drwxr-xr-x";
        else       perm = g_cfg.readOnly ? "-r--r--r--" : "-rw-r--r--";

        FileTimeToLocalFileTime(pft, &localFt);
        FileTimeToSystemTime(&localFt, &st);
        GetLocalTime(&now);
        if (st.wYear == now.wYear) {
            FormatA(dateBuf, sizeof(dateBuf), "%s %02u %02u:%02u",
                    (st.wMonth >= 1 && st.wMonth <= 12) ? mon[st.wMonth - 1] : "Jan",
                    (unsigned)st.wDay, (unsigned)st.wHour, (unsigned)st.wMinute);
        } else {
            FormatA(dateBuf, sizeof(dateBuf), "%s %02u  %04u",
                    (st.wMonth >= 1 && st.wMonth <= 12) ? mon[st.wMonth - 1] : "Jan",
                    (unsigned)st.wDay, (unsigned)st.wYear);
        }
        FormatA(line, sizeof(line), "%s %3d %-8s %-8s %10u %s %s\r\n",
                perm, 1, "ftp", "ftp", (unsigned)size, dateBuf, nm);
    }
    SockSendAll(d, line, (int)strlen(line), 30000);
}

static void DoList(FtpSession *s, const WCHAR *arg, int mode)
{
    WCHAR virt[MAX_PATH], full[MAX_PATH], mask[MAX_PATH];
    DWORD attr = 0;
    FILETIME ft;
    BOOL hasWild, isDir = FALSE, known = FALSE;
    SOCKET d;
    HANDLE hFind;
    WIN32_FIND_DATAW fd;

    NormalizeVirtual(s->cwd, arg, virt, MAX_PATH);
    MakeFull(virt, full, MAX_PATH);

    hasWild = (wcschr(full, L'*') || wcschr(full, L'?')) ? TRUE : FALSE;
    if (!hasWild) {
        /* 目录存在性用 GetFileAttributesW 判断：根目录 "\\" 也可靠 */
        attr = GetFileAttributesW(full);
        if (attr == INVALID_FILE_ATTRIBUTES) {
            Reply(s, "550 No such file or directory.");
            return;
        }
        known = TRUE;
        isDir = (attr & FILE_ATTRIBUTE_DIRECTORY) ? TRUE : FALSE;
        WCopy(mask, MAX_PATH, full);
        if (isDir) {
            /* 关键：根目录已经以 \ 结尾，不能再补一个，否则变成 UNC 路径 "\\*" */
            int n = (int)wcslen(mask);
            if (n > 0 && mask[n - 1] != L'\\') WCat(mask, MAX_PATH, L"\\");
            WCat(mask, MAX_PATH, L"*");
        }
    } else {
        WCopy(mask, MAX_PATH, full);
    }

    Reply(s, "150 Opening data connection.");
    d = DataOpen(s);
    if (d == INVALID_SOCKET) {
        Reply(s, "425 Cannot open data connection.");
        return;
    }

    if (!hasWild && known && !isDir) {
        DWORD size = 0;
        GetPathInfo(full, NULL, &size, &ft);
        SendOneEntry(s, d, BaseName(full), attr, size, &ft, mode);
    } else {
        if (mode == LST_LIST && !hasWild && isDir) {
            FILETIME dirFt;
            memset(&dirFt, 0, sizeof(dirFt));
            if (GetPathInfo(full, NULL, NULL, &dirFt)) {
                SendOneEntry(s, d, L".", FILE_ATTRIBUTE_DIRECTORY, 0, &dirFt, mode);
                SendOneEntry(s, d, L"..", FILE_ATTRIBUTE_DIRECTORY, 0, &dirFt, mode);
            }
        }
        hFind = FindFirstFileW(mask, &fd);
        if (hFind != INVALID_HANDLE_VALUE) {
            do {
                if (fd.cFileName[0] == L'.' &&
                    (fd.cFileName[1] == 0 ||
                     (fd.cFileName[1] == L'.' && fd.cFileName[2] == 0))) {
                    continue;
                }
                SendOneEntry(s, d, fd.cFileName, fd.dwFileAttributes,
                             fd.nFileSizeLow, &fd.ftLastWriteTime, mode);
            } while (FindNextFileW(hFind, &fd));
            FindClose(hFind);
        }
    }

    /* 目录列表也必须优雅关闭：手机端拿 RST 会把整份列表当失败丢掉，
     * 表现就是"新文件怎么刷都不出来"。 */
    DataCloseGraceful(d);
    Reply(s, "226 Transfer complete.");
}

/* ==========================================================================
 * 六、文件传输
 * ========================================================================*/

static void DoRetr(FtpSession *s, const WCHAR *arg)
{
    WCHAR virt[MAX_PATH], full[MAX_PATH];
    HANDLE h;
    DWORD attr = 0, size = 0, got = 0;
    FILETIME ft;
    SOCKET d;
    char *buf;
    BOOL ok = TRUE;

    NormalizeVirtual(s->cwd, arg, virt, MAX_PATH);
    MakeFull(virt, full, MAX_PATH);

    if (!GetPathInfo(full, &attr, &size, &ft)) {
        Reply(s, "550 File not found.");
        return;
    }
    if (attr & FILE_ATTRIBUTE_DIRECTORY) {
        Reply(s, "550 Is a directory.");
        return;
    }

    h = CreateFileW(full, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                    NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) {
        Reply(s, "550 Cannot open file.");
        return;
    }

    Reply(s, "150 Opening data connection (%u bytes).", (unsigned)size);
    d = DataOpen(s);
    if (d == INVALID_SOCKET) {
        CloseHandle(h);
        Reply(s, "425 Cannot open data connection.");
        return;
    }

    if (s->rest > 0) {
        SetFilePointer(h, (LONG)s->rest, NULL, FILE_BEGIN);
        s->rest = 0;
    }

    buf = (char *)malloc(32768);
    if (!buf) {
        CloseHandle(h);
        DataCloseGraceful(d);
        Reply(s, "451 Out of memory.");
        return;
    }

    for (;;) {
        if (!ReadFile(h, buf, 32768, &got, NULL) || got == 0) break;
        if (!SockSendAll(d, buf, (int)got, 30000)) { ok = FALSE; break; }
    }

    free(buf);
    CloseHandle(h);
    /* 下载完同样要先把 FIN 发出去，否则手机可能把已收到的尾部当残缺 */
    DataCloseGraceful(d);
    Reply(s, ok ? "226 Transfer complete." : "426 Transfer aborted.");
}

static void DoStor(FtpSession *s, const WCHAR *arg, BOOL append)
{
    WCHAR virt[MAX_PATH], full[MAX_PATH];
    HANDLE h;
    DWORD attr = 0, wrote = 0;
    SOCKET d;
    char *buf;
    BOOL ok = TRUE, diskErr = FALSE;
    DWORD disp;

    if (g_cfg.readOnly) {
        Reply(s, "550 Permission denied (server is read-only).");
        return;
    }

    NormalizeVirtual(s->cwd, arg, virt, MAX_PATH);
    MakeFull(virt, full, MAX_PATH);

    attr = GetFileAttributesW(full);
    if (attr != INVALID_FILE_ATTRIBUTES && (attr & FILE_ATTRIBUTE_DIRECTORY)) {
        Reply(s, "550 Cannot overwrite a directory.");
        return;
    }

    /* REST 是基于实际文件长度的续传偏移；文件不存在时由内核当 0 处理 */
    if (!append && s->rest > 0 && attr == INVALID_FILE_ATTRIBUTES) s->rest = 0;

    if (append || s->rest > 0) disp = OPEN_ALWAYS;
    else                       disp = CREATE_ALWAYS;

    h = CreateFileW(full, GENERIC_WRITE, FILE_SHARE_READ,
                    NULL, disp, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) {
        Reply(s, "550 Cannot create file.");
        return;
    }

    Reply(s, "150 Opening data connection.");
    d = DataOpen(s);
    if (d == INVALID_SOCKET) {
        CloseHandle(h);
        Reply(s, "425 Cannot open data connection.");
        return;
    }

    if (append) {
        SetFilePointer(h, 0, NULL, FILE_END);
    } else if (s->rest > 0) {
        DWORD have = 0;
        if (!GetPathInfo(full, NULL, &have, NULL)) have = 0;
        if (s->rest > have) s->rest = have;
        SetFilePointer(h, (LONG)s->rest, NULL, FILE_BEGIN);
        s->rest = 0;
    }

    buf = (char *)malloc(32768);
    if (!buf) {
        CloseHandle(h);
        DataCloseGraceful(d);
        Reply(s, "451 Out of memory.");
        return;
    }

    for (;;) {
        int r = SockRecv(d, buf, 32768, 60000);
        if (r == 0) break;
        if (r < 0) { ok = FALSE; break; }
        if (!WriteFile(h, buf, (DWORD)r, &wrote, NULL) || (int)wrote != r) {
            diskErr = TRUE;
            break;
        }
    }

    if (ok && !diskErr && !append) SetEndOfFile(h);

    /* 上传完必须 FlushFileBuffers 再 CloseHandle。
     * WinCE 的 FAT 写入会先攒在缓存里，不等刷盘就回 226，手机会以为
     * "上传成功了但文件还没落地"，紧接着去列目录就可能看到旧状态。 */
    if (ok && !diskErr) FlushFileBuffers(h);

    free(buf);
    CloseHandle(h);

    /* 接收方向：对方发完就会关连接，这里先 shutdown 再把余量读掉 */
    DataCloseGraceful(d);

    if (diskErr) Reply(s, "452 Error writing file (disk full?).");
    else if (!ok) Reply(s, "426 Transfer aborted.");
    else          Reply(s, "226 Transfer complete.");
}

/* ==========================================================================
 * 七、其它文件命令
 * ========================================================================*/

static void DoCwd(FtpSession *s, const WCHAR *arg)
{
    WCHAR virt[MAX_PATH], full[MAX_PATH];
    DWORD attr = 0;

    if (!arg || !arg[0]) { Reply(s, "501 Missing directory name."); return; }
    NormalizeVirtual(s->cwd, arg, virt, MAX_PATH);
    MakeFull(virt, full, MAX_PATH);

    attr = GetFileAttributesW(full);
    if (attr == INVALID_FILE_ATTRIBUTES || !(attr & FILE_ATTRIBUTE_DIRECTORY)) {
        Reply(s, "550 Not a directory.");
        return;
    }
    WCopy(s->cwd, MAX_PATH, virt);
    Reply(s, "250 Directory changed.");
}

static void DoSize(FtpSession *s, const WCHAR *arg)
{
    WCHAR virt[MAX_PATH], full[MAX_PATH];
    DWORD attr = 0, size = 0;

    if (!arg || !arg[0]) { Reply(s, "501 Missing file name."); return; }
    NormalizeVirtual(s->cwd, arg, virt, MAX_PATH);
    MakeFull(virt, full, MAX_PATH);

    attr = GetFileAttributesW(full);
    if (attr == INVALID_FILE_ATTRIBUTES) { Reply(s, "550 File not found."); return; }
    if (attr & FILE_ATTRIBUTE_DIRECTORY) { Reply(s, "550 Not a regular file."); return; }
    if (!GetPathInfo(full, NULL, &size, NULL)) { Reply(s, "550 File not found."); return; }
    Reply(s, "213 %u", (unsigned)size);
}

static void DoMdtm(FtpSession *s, const WCHAR *arg)
{
    WCHAR virt[MAX_PATH], full[MAX_PATH];
    DWORD attr = 0;
    FILETIME ft;
    SYSTEMTIME st;

    if (!arg || !arg[0]) { Reply(s, "501 Missing file name."); return; }
    NormalizeVirtual(s->cwd, arg, virt, MAX_PATH);
    MakeFull(virt, full, MAX_PATH);

    attr = GetFileAttributesW(full);
    if (attr == INVALID_FILE_ATTRIBUTES) { Reply(s, "550 File not found."); return; }
    if (attr & FILE_ATTRIBUTE_DIRECTORY) { Reply(s, "550 Not a regular file."); return; }
    if (!GetPathInfo(full, NULL, NULL, &ft)) { Reply(s, "550 File not found."); return; }
    FileTimeToSystemTime(&ft, &st);
    Reply(s, "213 %04u%02u%02u%02u%02u%02u",
          (unsigned)st.wYear, (unsigned)st.wMonth, (unsigned)st.wDay,
          (unsigned)st.wHour, (unsigned)st.wMinute, (unsigned)st.wSecond);
}

static void DoMlst(FtpSession *s, const WCHAR *arg)
{
    WCHAR virt[MAX_PATH], full[MAX_PATH];
    DWORD attr = 0, size = 0;
    FILETIME ft;
    SYSTEMTIME st;
    char nm[MAX_PATH * 3];
    BOOL isDir;

    NormalizeVirtual(s->cwd, (arg && arg[0]) ? arg : L".", virt, MAX_PATH);
    MakeFull(virt, full, MAX_PATH);
    if (!GetPathInfo(full, &attr, &size, &ft)) {
        Reply(s, "550 File not found.");
        return;
    }
    isDir = (attr & FILE_ATTRIBUTE_DIRECTORY) ? TRUE : FALSE;
    FileTimeToSystemTime(&ft, &st);
    EncodeName(virt, nm, sizeof(nm));
    SanitizeName(nm);

    Reply(s, "250-Listing %s", nm);
    Reply(s, " type=%s;size=%u;modify=%04u%02u%02u%02u%02u%02u;perm=%s; %s",
          isDir ? "dir" : "file", (unsigned)size,
          (unsigned)st.wYear, (unsigned)st.wMonth, (unsigned)st.wDay,
          (unsigned)st.wHour, (unsigned)st.wMinute, (unsigned)st.wSecond,
          isDir ? "elcmp" : "adfrw", nm);
    Reply(s, "250 End");
}

/* ==========================================================================
 * 八、命令分发
 * ========================================================================*/

static BOOL NeedLogin(FtpSession *s)
{
    if (s->loggedIn) return FALSE;
    Reply(s, "530 Please login with USER and PASS.");
    return TRUE;
}

static BOOL HandleCommand(FtpSession *s, char *verb, char *arg)
{
    /* ---- 登录前就能用的命令 ---- */
    if (!strcmp(verb, "USER")) {
        WCHAR u[64];
        if (!arg[0]) { Reply(s, "501 Missing user name."); return TRUE; }
        DecodeArg(arg, u, 64);
        if (g_cfg.user[0] == 0 || WEqualCI(u, g_cfg.user)) {
            if (g_cfg.pass[0] == 0) {
                s->loggedIn = TRUE;
                s->needPass = FALSE;
                Reply(s, "230 Login successful.");
            } else {
                s->needPass = TRUE;
                s->loggedIn = FALSE;
                Reply(s, "331 Password required.");
            }
        } else {
            s->needPass = FALSE;
            s->loggedIn = FALSE;
            Reply(s, "530 Login incorrect.");
        }
        return TRUE;
    }
    if (!strcmp(verb, "PASS")) {
        WCHAR p[64];
        if (!s->needPass) { Reply(s, "503 Login with USER first."); return TRUE; }
        DecodeArg(arg, p, 64);
        if (WEqual(p, g_cfg.pass) || (g_cfg.pass[0] == 0)) {
            s->loggedIn = TRUE;
            s->needPass = FALSE;
            Reply(s, "230 Login successful.");
        } else {
            s->needPass = FALSE;
            Reply(s, "530 Login incorrect.");
        }
        return TRUE;
    }
    if (!strcmp(verb, "QUIT")) {
        Reply(s, "221 Goodbye.");
        return FALSE;
    }
    if (!strcmp(verb, "SYST")) {
        Reply(s, "215 Windows_NT");
        return TRUE;
    }
    if (!strcmp(verb, "FEAT")) {
        Reply(s, "211-Features:");
        if (g_cfg.ansiCP == 0) Reply(s, " UTF8");
        Reply(s, " SIZE");
        Reply(s, " MDTM");
        Reply(s, " REST STREAM");
        Reply(s, " MLST type*;size*;modify*;perm*;");
        Reply(s, " MLSD");
        Reply(s, " TVFS");
        Reply(s, " EPSV");
        Reply(s, " EPRT");
        Reply(s, "211 End");
        return TRUE;
    }
    if (!strcmp(verb, "OPTS")) {
        if (g_cfg.ansiCP == 0) Reply(s, "200 UTF8 mode enabled.");
        else Reply(s, "200 OK.");
        return TRUE;
    }
    if (!strcmp(verb, "NOOP")) { Reply(s, "200 OK."); return TRUE; }
    if (!strcmp(verb, "CLNT")) { Reply(s, "200 OK."); return TRUE; }
    if (!strcmp(verb, "ACCT")) { Reply(s, "202 ACCT not needed."); return TRUE; }
    if (!strcmp(verb, "AUTH")) { Reply(s, "502 TLS not supported."); return TRUE; }
    if (!strcmp(verb, "PBSZ") || !strcmp(verb, "PROT")) {
        Reply(s, "502 Not implemented.");
        return TRUE;
    }
    if (!strcmp(verb, "HELP")) {
        Reply(s, "214-Commands supported:");
        Reply(s, " USER PASS QUIT SYST FEAT OPTS NOOP PWD CWD CDUP TYPE");
        Reply(s, " PASV EPSV PORT EPRT LIST NLST MLSD MLST RETR STOR APPE");
        Reply(s, " REST SIZE MDTM DELE MKD RMD RNFR RNTO SITE STAT ABOR");
        Reply(s, "214 End");
        return TRUE;
    }

    if (NeedLogin(s)) return TRUE;

    /* ---- 需要登录 ---- */
    if (!strcmp(verb, "PWD") || !strcmp(verb, "XPWD")) {
        char disp[MAX_PATH * 3];
        DisplayPath(s->cwd, disp, sizeof(disp));
        Reply(s, "257 \"%s\" is current directory.", disp);
        return TRUE;
    }
    if (!strcmp(verb, "CWD") || !strcmp(verb, "XCWD")) {
        WCHAR a[MAX_PATH];
        DecodeArg(arg, a, MAX_PATH);
        DoCwd(s, a);
        return TRUE;
    }
    if (!strcmp(verb, "CDUP") || !strcmp(verb, "XCUP")) {
        DoCwd(s, L"..");
        return TRUE;
    }
    if (!strcmp(verb, "TYPE")) {
        if (arg[0] == 'I' || arg[0] == 'i') { s->type = 1; Reply(s, "200 Type set to I."); }
        else if (arg[0] == 'A' || arg[0] == 'a') { s->type = 0; Reply(s, "200 Type set to A."); }
        else if (arg[0] == 'L' || arg[0] == 'l') { s->type = 1; Reply(s, "200 Type set to L 8."); }
        else Reply(s, "504 Unsupported type.");
        return TRUE;
    }
    if (!strcmp(verb, "MODE")) {
        if (arg[0] == 'S' || arg[0] == 's') Reply(s, "200 Mode set to S.");
        else Reply(s, "504 Only stream mode is supported.");
        return TRUE;
    }
    if (!strcmp(verb, "STRU")) {
        if (arg[0] == 'F' || arg[0] == 'f') Reply(s, "200 Structure set to F.");
        else Reply(s, "504 Only file structure is supported.");
        return TRUE;
    }
    if (!strcmp(verb, "ALLO")) { Reply(s, "202 ALLO not needed."); return TRUE; }
    if (!strcmp(verb, "ABOR")) { Reply(s, "226 Abort successful."); return TRUE; }

    if (!strcmp(verb, "PASV")) { CmdPasv(s, FALSE); return TRUE; }
    if (!strcmp(verb, "EPSV")) {
        if (!StriCmp(arg, "ALL")) { Reply(s, "200 EPSV ALL ok."); return TRUE; }
        CmdPasv(s, TRUE);
        return TRUE;
    }
    if (!strcmp(verb, "PORT")) { CmdPort(s, arg); return TRUE; }
    if (!strcmp(verb, "EPRT")) { CmdEprt(s, arg); return TRUE; }

    if (!strcmp(verb, "LIST") || !strcmp(verb, "NLST") || !strcmp(verb, "MLSD")) {
        WCHAR a[MAX_PATH];
        int mode = (!strcmp(verb, "LIST")) ? LST_LIST :
                   (!strcmp(verb, "NLST")) ? LST_NLST : LST_MLSD;
        char *p = arg;

        if (mode != LST_MLSD) {
            while (*p == '-') {
                while (*p && *p != ' ') p++;
                while (*p == ' ') p++;
            }
        }
        DecodeArg(p, a, MAX_PATH);
        DoList(s, a, mode);
        return TRUE;
    }
    if (!strcmp(verb, "MLST")) {
        WCHAR a[MAX_PATH];
        DecodeArg(arg, a, MAX_PATH);
        DoMlst(s, a);
        return TRUE;
    }
    if (!strcmp(verb, "RETR")) {
        WCHAR a[MAX_PATH];
        if (!arg[0]) { Reply(s, "501 Missing file name."); return TRUE; }
        DecodeArg(arg, a, MAX_PATH);
        DoRetr(s, a);
        return TRUE;
    }
    if (!strcmp(verb, "STOR") || !strcmp(verb, "APPE")) {
        WCHAR a[MAX_PATH];
        if (!arg[0]) { Reply(s, "501 Missing file name."); return TRUE; }
        DecodeArg(arg, a, MAX_PATH);
        DoStor(s, a, !strcmp(verb, "APPE"));
        return TRUE;
    }
    if (!strcmp(verb, "REST")) {
        s->rest = (DWORD)strtoul(arg, NULL, 10);
        Reply(s, "350 Restarting at %u. Send STOR or RETR.", (unsigned)s->rest);
        return TRUE;
    }
    if (!strcmp(verb, "SIZE")) {
        WCHAR a[MAX_PATH];
        if (!arg[0]) { Reply(s, "501 Missing file name."); return TRUE; }
        DecodeArg(arg, a, MAX_PATH);
        DoSize(s, a);
        return TRUE;
    }
    if (!strcmp(verb, "MDTM")) {
        WCHAR a[MAX_PATH];
        if (!arg[0]) { Reply(s, "501 Missing file name."); return TRUE; }
        DecodeArg(arg, a, MAX_PATH);
        DoMdtm(s, a);
        return TRUE;
    }
    if (!strcmp(verb, "DELE")) {
        WCHAR a[MAX_PATH], virt[MAX_PATH], full[MAX_PATH];
        DWORD attr = 0;
        if (!arg[0]) { Reply(s, "501 Missing file name."); return TRUE; }
        if (g_cfg.readOnly) { Reply(s, "550 Permission denied (read-only)."); return TRUE; }
        DecodeArg(arg, a, MAX_PATH);
        NormalizeVirtual(s->cwd, a, virt, MAX_PATH);
        MakeFull(virt, full, MAX_PATH);
        attr = GetFileAttributesW(full);
        if (attr == INVALID_FILE_ATTRIBUTES) { Reply(s, "550 File not found."); return TRUE; }
        if (attr & FILE_ATTRIBUTE_DIRECTORY) { Reply(s, "550 Is a directory."); return TRUE; }
        if (!DeleteFileW(full)) {
            SetFileAttributesW(full, FILE_ATTRIBUTE_NORMAL);
            if (!DeleteFileW(full)) { Reply(s, "550 Delete failed."); return TRUE; }
        }
        Reply(s, "250 File deleted.");
        return TRUE;
    }
    if (!strcmp(verb, "MKD") || !strcmp(verb, "XMKD")) {
        WCHAR a[MAX_PATH], virt[MAX_PATH], full[MAX_PATH];
        char disp[MAX_PATH * 3];
        if (!arg[0]) { Reply(s, "501 Missing directory name."); return TRUE; }
        if (g_cfg.readOnly) { Reply(s, "550 Permission denied (read-only)."); return TRUE; }
        DecodeArg(arg, a, MAX_PATH);
        NormalizeVirtual(s->cwd, a, virt, MAX_PATH);
        MakeFull(virt, full, MAX_PATH);
        if (!CreateDirectoryW(full, NULL)) {
            Reply(s, "550 Create directory failed (already exists?).");
            return TRUE;
        }
        DisplayPath(virt, disp, sizeof(disp));
        Reply(s, "257 \"%s\" created.", disp);
        return TRUE;
    }
    if (!strcmp(verb, "RMD") || !strcmp(verb, "XRMD")) {
        WCHAR a[MAX_PATH], virt[MAX_PATH], full[MAX_PATH];
        if (!arg[0]) { Reply(s, "501 Missing directory name."); return TRUE; }
        if (g_cfg.readOnly) { Reply(s, "550 Permission denied (read-only)."); return TRUE; }
        DecodeArg(arg, a, MAX_PATH);
        NormalizeVirtual(s->cwd, a, virt, MAX_PATH);
        MakeFull(virt, full, MAX_PATH);
        if (!RemoveDirectoryW(full)) {
            Reply(s, "550 Remove directory failed (not empty?).");
            return TRUE;
        }
        Reply(s, "250 Directory removed.");
        return TRUE;
    }
    if (!strcmp(verb, "RNFR")) {
        WCHAR a[MAX_PATH], virt[MAX_PATH], full[MAX_PATH];
        if (!arg[0]) { Reply(s, "501 Missing file name."); return TRUE; }
        if (g_cfg.readOnly) { Reply(s, "550 Permission denied (read-only)."); return TRUE; }
        DecodeArg(arg, a, MAX_PATH);
        NormalizeVirtual(s->cwd, a, virt, MAX_PATH);
        MakeFull(virt, full, MAX_PATH);
        if (GetFileAttributesW(full) == INVALID_FILE_ATTRIBUTES) { Reply(s, "550 File not found."); return TRUE; }
        WCopy(s->rnfr, MAX_PATH, virt);
        s->hasRnfr = TRUE;
        Reply(s, "350 Ready for RNTO.");
        return TRUE;
    }
    if (!strcmp(verb, "RNTO")) {
        WCHAR a[MAX_PATH], virt[MAX_PATH], full[MAX_PATH], src[MAX_PATH];
        if (!s->hasRnfr) { Reply(s, "503 RNFR required first."); return TRUE; }
        if (!arg[0]) { Reply(s, "501 Missing file name."); return TRUE; }
        s->hasRnfr = FALSE;
        DecodeArg(arg, a, MAX_PATH);
        NormalizeVirtual(s->cwd, a, virt, MAX_PATH);
        MakeFull(virt, full, MAX_PATH);
        MakeFull(s->rnfr, src, MAX_PATH);
        if (!MoveFileW(src, full)) { Reply(s, "550 Rename failed."); return TRUE; }
        Reply(s, "250 Rename successful.");
        return TRUE;
    }
    if (!strcmp(verb, "STAT")) {
        if (arg[0]) { Reply(s, "211 Status ok."); return TRUE; }
        Reply(s, "211-FTP server status:");
        Reply(s, "     %s %s", FTPD_NAME, FTPD_VERSION);
        Reply(s, "     Connected from %s", s->peer);
        Reply(s, "     Session %d, type %s", s->id, s->type ? "I" : "A");
        Reply(s, "     %u client(s) online", (unsigned)g_clients);
        Reply(s, "211 End");
        return TRUE;
    }
    if (!strcmp(verb, "SITE")) {
        if (StriPrefix(arg, "CHMOD")) { Reply(s, "200 SITE CHMOD ok."); return TRUE; }
        if (!StriCmp(arg, "HELP")) {
            Reply(s, "214-SITE commands:");
            Reply(s, " CHMOD  HELP  SHUTDOWN");
            Reply(s, "214 End");
            return TRUE;
        }
        if (StriPrefix(arg, "DIAG")) {
            /* 现场诊断：SITE DIAG 会实时扫一遍当前目录并回报，
             * 用来区分「服务器没看到新文件」和「手机没刷新」。
             * 手机端多数客户端没有发自定义命令的入口，所以这里同时
             * 支持 SITE DIAG 和 STAT <path> 两种写法。 */
            WCHAR virt[MAX_PATH], full[MAX_PATH], mask[MAX_PATH];
            WIN32_FIND_DATAW fd;
            HANDLE hFind;
            int n = 0;

            NormalizeVirtual(s->cwd, L"", virt, MAX_PATH);
            MakeFull(virt, full, MAX_PATH);
            WCopy(mask, MAX_PATH, full);
            {
                int len = (int)wcslen(mask);
                if (len > 0 && mask[len - 1] != L'\\') WCat(mask, MAX_PATH, L"\\");
                WCat(mask, MAX_PATH, L"*");
            }

            Reply(s, "211-DIAG: live scan of current directory");
            hFind = FindFirstFileW(mask, &fd);
            if (hFind != INVALID_HANDLE_VALUE) {
                do {
                    char nm[MAX_PATH * 3];
                    if (fd.cFileName[0] == L'.' &&
                        (fd.cFileName[1] == 0 ||
                         (fd.cFileName[1] == L'.' && fd.cFileName[2] == 0))) {
                        continue;
                    }
                    EncodeName(fd.cFileName, nm, sizeof(nm));
                    SanitizeName(nm);
                    Reply(s, " %c %10u  %s",
                          (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) ? 'D' : 'F',
                          (unsigned)fd.nFileSizeLow, nm);
                    n++;
                } while (FindNextFileW(hFind, &fd));
                FindClose(hFind);
            }
            Reply(s, "211-DIAG: %d entries seen by server RIGHT NOW", n);
            Reply(s, "211-DIAG: if a new file appears above but not on the phone,");
            Reply(s, "211-DIAG: the delay is the phone client's cache, not this server.");
            Reply(s, "211 End");   /* 结尾必须是 "211 " + 空格，客户端靠它判断结束 */
            return TRUE;
        }
        if (StriPrefix(arg, "SHUTDOWN")) {
            LogA("session %d requested shutdown", s->id);
            Reply(s, "221 Server shutting down.");
            g_run = FALSE;
            return FALSE;
        }
        Reply(s, "504 Unknown SITE command.");
        return TRUE;
    }

    Reply(s, "502 Command not implemented.");
    return TRUE;
}

/* ==========================================================================
 * 九、会话线程
 * ========================================================================*/

static BOOL ReadLine(FtpSession *s, char *out, int cap)
{
    for (;;) {
        int i;
        for (i = 0; i < s->inlen; i++) {
            if (s->inbuf[i] == '\n') {
                int n = i;
                if (n > 0 && s->inbuf[n - 1] == '\r') n--;
                if (n > cap - 1) n = cap - 1;
                memcpy(out, s->inbuf, n);
                out[n] = 0;
                memmove(s->inbuf, s->inbuf + i + 1, s->inlen - i - 1);
                s->inlen -= i + 1;
                return TRUE;
            }
        }
        if (s->inlen >= (int)sizeof(s->inbuf) - 1) {
            s->inlen = 0;
            Reply(s, "500 Command line too long.");
        }
        {
            int r = SockRecv(s->ctrl, s->inbuf + s->inlen,
                             (int)sizeof(s->inbuf) - 1 - s->inlen, 600000);
            if (r == -2) { Reply(s, "421 Idle timeout."); return FALSE; }
            if (r <= 0) return FALSE;
            s->inlen += r;
        }
    }
}

static BOOL SplitLine(char *line, char **verb, char **arg)
{
    char *p = line;

    while (*p == ' ') p++;
    if (!*p) return FALSE;
    *verb = p;
    while (*p && *p != ' ') p++;
    if (*p) {
        *p++ = 0;
        while (*p == ' ') p++;
    }
    *arg = p;
    {
        int n = (int)strlen(*arg);
        while (n > 0 && ((*arg)[n - 1] == ' ' || (*arg)[n - 1] == '\r' ||
                         (*arg)[n - 1] == '\t')) {
            (*arg)[--n] = 0;
        }
    }
    return TRUE;
}

static void UpperA(char *s)
{
    int i;
    for (i = 0; s[i]; i++) {
        if (s[i] >= 'a' && s[i] <= 'z') s[i] = (char)(s[i] - 32);
    }
}

static DWORD WINAPI ClientThread(LPVOID param)
{
    FtpSession *s = (FtpSession *)param;
    char line[1024];

    LogA("session %d: connected from %s", s->id, s->peer);
    Reply(s, "220 %s %s ready.", FTPD_NAME, FTPD_VERSION);

    while (g_run) {
        char *verb, *arg;
        if (!ReadLine(s, line, sizeof(line))) break;
        if (!SplitLine(line, &verb, &arg)) continue;
        UpperA(verb);
        LogA("session %d: %s %s", s->id, verb, arg);
        if (!HandleCommand(s, verb, arg)) break;
    }

    CloseData(s);
    if (s->ctrl != INVALID_SOCKET) {
        closesocket(s->ctrl);
        s->ctrl = INVALID_SOCKET;
    }
    LogA("session %d: closed", s->id);
    InterlockedDecrement(&g_clients);
    free(s);
    return 0;
}

/* ==========================================================================
 * 十、配置与日志
 * ========================================================================*/

static BOOL ReadAllFile(const WCHAR *path, char **out, DWORD *outSize)
{
    HANDLE h;
    DWORD size, got = 0;
    char *buf;

    h = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, NULL,
                    OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) return FALSE;

    size = GetFileSize(h, NULL);
    if (size == 0 || size > 65536) {
        CloseHandle(h);
        return FALSE;
    }
    buf = (char *)malloc(size + 1);
    if (!buf) {
        CloseHandle(h);
        return FALSE;
    }
    if (!ReadFile(h, buf, size, &got, NULL) || got != size) {
        free(buf);
        CloseHandle(h);
        return FALSE;
    }
    buf[size] = 0;
    CloseHandle(h);
    *out = buf;
    *outSize = size;
    return TRUE;
}

static void TrimA(char *s)
{
    int n = (int)strlen(s);
    int i = 0;

    while (n > 0 && (s[n - 1] == '\r' || s[n - 1] == '\n' ||
                     s[n - 1] == ' ' || s[n - 1] == '\t')) {
        s[--n] = 0;
    }
    while (s[i] == ' ' || s[i] == '\t') i++;
    if (i > 0) memmove(s, s + i, strlen(s + i) + 1);
}

static void MakeDefaultIni(const WCHAR *iniPath)
{
    static const char *tpl =
        "; WinCE FTP server config (generated automatically)\r\n"
        ";\r\n"
        "; Port       listen port, 21 by default; use 2121 if 21 is occupied\r\n"
        "; Root       directory exposed to clients; \"\\\" = whole device\r\n"
        "; User       login name; empty = anonymous (any name, no password)\r\n"
        "; Password   login password; empty = no password\r\n"
        "; ReadOnly   1 = clients can download only\r\n"
        "; MaxClients max simultaneous sessions\r\n"
        "; LogFile    log path; empty = ftpd.log next to the exe; \"none\" = off\r\n"
        "; PasvIP     optional IP advertised in PASV replies (multi-NIC devices)\r\n"
        "; Encoding   UTF8 or GBK (GBK is for old clients that ignore UTF8)\r\n"
        "Port=21\r\n"
        "Root=\\\r\n"
        "User=ftp\r\n"
        "Password=ftp\r\n"
        "ReadOnly=0\r\n"
        "MaxClients=4\r\n"
        "LogFile=\r\n"
        "PasvIP=\r\n"
        "Encoding=UTF8\r\n";
    HANDLE h;
    DWORD written = 0;

    h = CreateFileW(iniPath, GENERIC_WRITE, 0, NULL,
                    CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) return;
    WriteFile(h, tpl, (DWORD)strlen(tpl), &written, NULL);
    CloseHandle(h);
}

static void LoadConfig(void)
{
    WCHAR mod[MAX_PATH], dir[MAX_PATH], ini[MAX_PATH];
    char *data = NULL;
    DWORD size = 0;
    char rootA[MAX_PATH * 3] = "";
    char logA[MAX_PATH * 3] = "";
    char userA[128] = "";
    char passA[128] = "";
    char pasvA[32] = "";
    char encA[16] = "";
    char portA[16] = "";
    char maxA[16] = "";
    char roA[16] = "";

    /* 默认值 */
    g_cfg.port = 21;
    WCopy(g_cfg.root, MAX_PATH, L"\\");
    g_cfg.user[0] = 0;
    g_cfg.pass[0] = 0;
    g_cfg.readOnly = FALSE;
    g_cfg.maxClients = 4;
    g_cfg.logFile[0] = 0;
    g_cfg.pasvIP[0] = 0;
    g_cfg.ansiCP = 0;

    mod[0] = 0;
    GetModuleFileNameW(NULL, mod, MAX_PATH);
    WCopy(dir, MAX_PATH, mod);
    {
        int i, last = 0;
        for (i = 0; dir[i]; i++) if (dir[i] == L'\\' || dir[i] == L'/') last = i;
        dir[last] = 0;
        if (dir[0] == 0) WCopy(dir, MAX_PATH, L"\\");
    }

    WCopy(ini, MAX_PATH, dir);
    WCat(ini, MAX_PATH, L"\\ftpd.ini");

    if (ReadAllFile(ini, &data, &size)) {
        char *p = data;
        if (size > 3 && (unsigned char)p[0] == 0xEF &&
            (unsigned char)p[1] == 0xBB && (unsigned char)p[2] == 0xBF) {
            p += 3;
        }
        while (*p) {
            char *line = p;
            char *eq;
            char *nl = strchr(p, '\n');
            if (nl) {
                *nl = 0;
                p = nl + 1;
            } else {
                p = p + strlen(p);
            }
            TrimA(line);
            if (!line[0] || line[0] == ';' || line[0] == '#') continue;
            eq = strchr(line, '=');
            if (!eq) continue;
            *eq = 0;
            TrimA(line);
            TrimA(eq + 1);
            if      (!StriCmp(line, "Port"))       FormatA(portA, sizeof(portA), "%s", eq + 1);
            else if (!StriCmp(line, "Root"))       FormatA(rootA, sizeof(rootA), "%s", eq + 1);
            else if (!StriCmp(line, "User"))       FormatA(userA, sizeof(userA), "%s", eq + 1);
            else if (!StriCmp(line, "Password"))   FormatA(passA, sizeof(passA), "%s", eq + 1);
            else if (!StriCmp(line, "ReadOnly"))   FormatA(roA, sizeof(roA), "%s", eq + 1);
            else if (!StriCmp(line, "MaxClients")) FormatA(maxA, sizeof(maxA), "%s", eq + 1);
            else if (!StriCmp(line, "LogFile"))    FormatA(logA, sizeof(logA), "%s", eq + 1);
            else if (!StriCmp(line, "PasvIP"))     FormatA(pasvA, sizeof(pasvA), "%s", eq + 1);
            else if (!StriCmp(line, "Encoding"))   FormatA(encA, sizeof(encA), "%s", eq + 1);
        }
        free(data);
    } else {
        MakeDefaultIni(ini);
    }

    /* Encoding 先定，后面的路径/账号按它解码 */
    if (encA[0]) {
        if (!StriCmp(encA, "UTF8") || !StriCmp(encA, "UTF-8")) g_cfg.ansiCP = 0;
        else if (!StriCmp(encA, "GBK") || !StriCmp(encA, "936") ||
                 !StriCmp(encA, "CP936")) g_cfg.ansiCP = 936;
        else g_cfg.ansiCP = 0;
    }

    if (portA[0]) {
        int v = atoi(portA);
        if (v > 0 && v < 65536) g_cfg.port = v;
    }
    if (maxA[0]) {
        int v = atoi(maxA);
        if (v > 0 && v <= 32) g_cfg.maxClients = v;
    }
    if (roA[0]) g_cfg.readOnly = (atoi(roA) != 0) ? TRUE : FALSE;

    if (rootA[0]) {
        DecodeArg(rootA, g_cfg.root, MAX_PATH);
        if (g_cfg.root[0] == 0) WCopy(g_cfg.root, MAX_PATH, L"\\");
        if (g_cfg.root[0] != L'\\' && g_cfg.root[0] != L'/') {
            WCHAR t[MAX_PATH];
            WCopy(t, MAX_PATH, L"\\");
            WCat(t, MAX_PATH, g_cfg.root);
            WCopy(g_cfg.root, MAX_PATH, t);
        }
    }
    if (userA[0]) DecodeArg(userA, g_cfg.user, 64);
    if (passA[0]) DecodeArg(passA, g_cfg.pass, 64);
    if (pasvA[0]) FormatA(g_cfg.pasvIP, sizeof(g_cfg.pasvIP), "%s", pasvA);

    if (logA[0] && StriCmp(logA, "none") != 0) {
        DecodeArg(logA, g_cfg.logFile, MAX_PATH);
    } else if (logA[0] && StriCmp(logA, "none") == 0) {
        g_cfg.logFile[0] = 0;
    } else {
        WCopy(g_cfg.logFile, MAX_PATH, dir);
        WCat(g_cfg.logFile, MAX_PATH, L"\\ftpd.log");
    }
}

static void InitLog(void)
{
    DWORD written = 0;

    if (g_cfg.logFile[0] == 0) return;
    g_log = CreateFileW(g_cfg.logFile, GENERIC_WRITE, FILE_SHARE_READ, NULL,
                        OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (g_log == INVALID_HANDLE_VALUE) return;
    if (GetFileSize(g_log, NULL) > 256 * 1024) {
        SetFilePointer(g_log, 0, NULL, FILE_BEGIN);
        SetEndOfFile(g_log);
    }
    SetFilePointer(g_log, 0, NULL, FILE_END);
    WriteFile(g_log, "\r\n", 2, &written, NULL);
}

static void LogLocalIPs(void)
{
    char host[128];
    struct hostent *he;
    int i;

    if (gethostname(host, sizeof(host)) != 0) return;
    he = gethostbyname(host);
    if (!he || !he->h_addr_list) return;
    for (i = 0; he->h_addr_list[i]; i++) {
        struct in_addr in;
        memcpy(&in, he->h_addr_list[i], 4);
        LogA("local address: %s", inet_ntoa(in));
    }
}

/* ==========================================================================
 * 十一、服务器主循环
 * ========================================================================*/

static BOOL InitListen(void)
{
    struct sockaddr_in a;
    u_long one = 1;

    g_listen = socket(AF_INET, SOCK_STREAM, 0);
    if (g_listen == INVALID_SOCKET) {
        LogA("socket() failed: %d", WSAGetLastError());
        return FALSE;
    }
    setsockopt(g_listen, SOL_SOCKET, SO_REUSEADDR, (const char *)&one, (int)sizeof(one));

    memset(&a, 0, sizeof(a));
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = INADDR_ANY;
    a.sin_port = htons((unsigned short)g_cfg.port);

    if (bind(g_listen, (struct sockaddr *)&a, (int)sizeof(a)) != 0) {
        LogA("bind() port %d failed: %d", g_cfg.port, WSAGetLastError());
        closesocket(g_listen);
        g_listen = INVALID_SOCKET;
        return FALSE;
    }
    if (listen(g_listen, 4) != 0) {
        LogA("listen() failed: %d", WSAGetLastError());
        closesocket(g_listen);
        g_listen = INVALID_SOCKET;
        return FALSE;
    }
    return TRUE;
}

static void Serve(void)
{
    while (g_run) {
        fd_set rf;
        struct timeval tv;
        int r;

        FD_ZERO(&rf);
        FD_SET(g_listen, &rf);
        tv.tv_sec = 1;
        tv.tv_usec = 0;

        r = select((int)g_listen + 1, &rf, NULL, NULL, &tv);
        if (r <= 0) continue;

        {
            struct sockaddr_in peer;
            int plen = (int)sizeof(peer);
            SOCKET c = accept(g_listen, (struct sockaddr *)&peer, &plen);
            FtpSession *s;
            HANDLE th;
            DWORD tid = 0;
            int one = 1;

            if (c == INVALID_SOCKET) continue;

            if (InterlockedIncrement(&g_clients) > g_cfg.maxClients) {
                InterlockedDecrement(&g_clients);
                {
                    static const char busy[] = "421 Too many users, try again later.\r\n";
                    SockSendAll(c, busy, (int)sizeof(busy) - 1, 5000);
                }
                closesocket(c);
                continue;
            }

            s = (FtpSession *)malloc(sizeof(FtpSession));
            if (!s) {
                InterlockedDecrement(&g_clients);
                closesocket(c);
                continue;
            }
            memset(s, 0, sizeof(*s));
            s->ctrl = c;
            s->pasv = INVALID_SOCKET;
            s->hasPort = FALSE;
            s->id = (int)InterlockedIncrement(&g_sessionId);
            s->type = 1;
            WCopy(s->cwd, MAX_PATH, L"\\");
            FormatA(s->peer, sizeof(s->peer), "%s", inet_ntoa(peer.sin_addr));
            setsockopt(c, IPPROTO_TCP, TCP_NODELAY, (const char *)&one, (int)sizeof(one));

            th = CreateThread(NULL, 0, ClientThread, s, 0, &tid);
            if (!th) {
                closesocket(c);
                free(s);
                InterlockedDecrement(&g_clients);
                continue;
            }
            CloseHandle(th);
        }
    }
}

static int FtpRun(void)
{
    WSADATA wsa;
    int rc;

    InitializeCriticalSection(&g_logLock);
    LoadConfig();
    InitLog();

    LogA("---- %s %s starting ----", FTPD_NAME, FTPD_VERSION);
    LogA("port=%d maxClients=%d readOnly=%d encoding=%s",
         g_cfg.port, g_cfg.maxClients, g_cfg.readOnly,
         g_cfg.ansiCP ? "GBK" : "UTF8");
    {
        char rootU[MAX_PATH * 3];
        EncodeName(g_cfg.root, rootU, sizeof(rootU));
        LogA("root=%s", rootU);
    }

    rc = WSAStartup(MAKEWORD(2, 2), &wsa);
    (void)wsa;
    if (rc != 0) {
        LogA("WSAStartup failed: %d", rc);
        return 1;
    }

    if (!InitListen()) {
        LogA("server not started");
        WSACleanup();
        return 1;
    }

    LogLocalIPs();
    LogA("listening on port %d", g_cfg.port);

    Serve();

    if (g_listen != INVALID_SOCKET) closesocket(g_listen);
    WSACleanup();
    LogA("---- stopped ----");
    if (g_log != INVALID_HANDLE_VALUE) {
        CloseHandle(g_log);
        g_log = INVALID_HANDLE_VALUE;
    }
    DeleteCriticalSection(&g_logLock);
    return 0;
}

/* ==========================================================================
 * 十二、入口
 * ========================================================================*/

#ifndef FTPD_NO_WINMAIN
int WINAPI WinMain(HINSTANCE hInst, HINSTANCE hPrev, LPWSTR cmdLine, int show)
{
    (void)hInst; (void)hPrev; (void)cmdLine; (void)show;
    return FtpRun();
}
#endif

#ifdef FTPD_CONSOLE
int main(int argc, char **argv)
{
    (void)argc; (void)argv;
    return FtpRun();
}
#endif
