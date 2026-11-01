/* ============================================================================
 * ce/cestart.c —— WinCE 程序的最小 CRT 启动桩（给 lld-link 用）
 *
 * 为什么需要它：
 *   正常 CeGCC 工具链会链它自己的 crt3.o，但那是老式 GNU COFF 目标文件，
 *   只有 GNU ld 认。用 LLVM 的 lld-link 链接时没有 CRT，就得自己提供一个
 *   入口，负责：
 *     1) 把命令行从 Unicode 转成本程序用不到的形态（这里直接忽略）；
 *     2) 调 WinMain；
 *     3) 退出进程。
 *
 *   WinCE 没有 ExitProcess，退出用 ExitThread(0) 或者直接返回。
 *   实测真机上主线程 return 之后进程即结束，所以这里 return 即可。
 * ==========================================================================*/

#include <windows.h>

/* 本文件不需要 UNICODE 宏也能编译 */
#ifndef _WIN32_WCE
#define _WIN32_WCE 0x600
#endif

int WINAPI WinMain(HINSTANCE, HINSTANCE, LPWSTR, int);

/* 让链接器把入口点解析到这里。
 * WinCE 的 PE 入口在真机上由 coredll 的 loader 调用，
 * 用 MSVC 风格名字最省事（等价于 CRT 的 WinMainCRTStartup）。 */
void WINAPI WinMainCRTStartup(void)
{
    int rc = WinMain(GetModuleHandleW(NULL), NULL, GetCommandLineW(), SW_SHOW);
    ExitThread((DWORD)rc);
}
