# WinCE FTP 服务器（ftpd）

把 WinCE 车机 / 手持机 / 老设备变成一台 FTP 服务器，让**同一局域网里的手机**用任意文件管理器
（AndFTP、ES、Solid、FE、Cx、MiXplorer…）直接浏览、下载、上传、改名、删除设备里的文件——
不需要数据线，不需要装驱动，也不需要 root。

- 单文件 C 源码：`ftpd.c`，只依赖系统自带的 `coredll` + `ws2`
- 纯被动 / 主动模式都支持，手机端默认的 PASV 可用
- UTF-8 文件名（中文名不乱码），也能切 GBK 兼容老客户端
- 路径规范化，`..\..` 逃不出配置的根目录
- 日志写文件（车机通常没有控制台），启动时把本机 IP 一起写进去，方便手机填地址

---

## 〇、现成的 exe

直接下载：<https://github.com/xianying520/wince-ftpd/releases/download/v1.0/ftpd.exe>
（38,912 字节，直接拷到车机就能用，不用自己编）：

```
Machine=0x01c0 (ARM)   Subsystem=9 (WinCE GUI)   只导入 COREDLL.dll / WS2.dll
```

校验过：纯 ARM 指令（不是 Thumb-2），开头机器码与已在真机跑通的 `WinCE-CarLifeHU.exe`
完全一致，所以 ARMv4T 及以上都能跑。

需要重新编译时按下面任一方式。

---

## 一、编译

### 方式 A：CeGCC（命令行，推荐）

和本仓库其它 WinCE 项目用同一套 `arm-mingw32ce` 工具链：

```bash
make                 # 输出 build/ftpd.exe
```

等价的裸命令：

```bash
arm-mingw32ce-gcc -Os -DUNICODE -D_UNICODE -c ftpd.c -o ftpd.o
arm-mingw32ce-gcc -s -o ftpd.exe ftpd.o -lcoredll -lws2
```

两个坑（都踩过）：

- **必须链 `-lws2`，不能链 `-lws2_32`**。`ws2_32` 是桌面 Windows 的导入库，车机上没有
  `WS2_32.dll`，链错了程序一点就退。
- **不要加 `-mwindows` / `--subsystem`**。CeGCC 的 `arm-mingw32ce` 目标默认就产出 WinCE
  GUI 子系统程序，加了反而报错。

### 方式 B：VS2005 / VS2008 + WinCE 6.0 SDK

1. 新建 **WCE Application**（空项目）或 **Win32 智能设备项目**，把 `ftpd.c` 加进去。
2. 项目属性里改三处：
   - C/C++ → 预处理器定义：`UNICODE;_UNICODE`
   - 链接器 → 输入 → 附加依赖项：`ws2.lib`（就是 WinCE 的 WinSock2，**不是** `ws2_32.lib`）
   - 链接器 → 系统 → 子系统：`Windows (/SUBSYSTEM:WINDOWS)`
3. 字符集选 **Unicode**，编译。

> 源码是 UTF-8 编码。若你的 VS 是中文版且注释显示乱码，不影响编译；
> 介意的话把文件另存为 ANSI(GB2312) 再编。

### 方式 C：eVC4 + 老 SDK

同上，链接 `ws2.lib`，入口 `WinMain`，Unicode。eVC4 的 CRT 没有 `vsnprintf`，
代码里已经用宏区分（MSVC 走 `_vsnprintf`），不用改。

---

## 二、部署

1. 把 `ftpd.exe` 拷到设备可写目录，例如：

   ```
   \Storage Card\ftpd\ftpd.exe
   ```

2. 先运行一次，它会在**同目录**生成 `ftpd.ini`，然后按需修改：

   ```ini
   Port=21
   Root=\
   User=ftp
   Password=ftp
   ReadOnly=0
   MaxClients=4
   LogFile=
   PasvIP=
   Encoding=UTF8
   ```

3. 改完重新运行（或重启设备）生效。

### 配置项

| 键 | 说明 | 默认 |
|---|---|---|
| `Port` | 监听端口。21 被占用/被系统服务占用时改 `2121`，手机端端口同步改 | `21` |
| `Root` | 暴露给手机的根目录。`\` = 整个设备；建议收窄成 `\Storage Card` | `\` |
| `User` | 登录用户名。**留空 = 匿名**（任意用户名、无密码） | `ftp` |
| `Password` | 登录密码。留空 = 不需要密码 | `ftp` |
| `ReadOnly` | `1` = 只允许下载/浏览，禁止上传、改名、删除、建目录 | `0` |
| `MaxClients` | 最大并发连接数（1–32）。每个连接一个线程，别开太大 | `4` |
| `LogFile` | 日志路径。留空 = exe 同目录 `ftpd.log`；填 `none` = 关闭日志 | 空 |
| `PasvIP` | 多网卡设备上强制 PASV 回报的 IP（一般不用填，见排错） | 空 |
| `Encoding` | `UTF8`（默认）或 `GBK`。老客户端不认 UTF8 时用 GBK | `UTF8` |

> `ftpd.ini` 用 UTF-8 保存即可；`Root` 里写中文路径（如 `\存储卡`）也能识别。

### 开机自启（可选）

在 `\Windows\Startup\` 下放一个指向 `ftpd.exe` 的快捷方式即可。
车机没有资源管理器时，可以在 PC 上建好 `.lnk` 再拷进去，或改注册表 `HKLM\init` 启动项
（不熟注册表的话建议只用 Startup 文件夹）。

---

## 三、手机怎么连

### 1. 先拿到车机的 IP

- 打开 `ftpd.log`，启动那几行里有：

  ```
  local address: 192.168.1.23
  ```

- 或在车机「控制面板 → 网络和拨号连接」里看 Wi-Fi 的 IP。

**手机和车机必须在同一网段**（同一个路由器 / 同一个手机热点）。

### 2. 客户端填写

| 项 | 值 |
|---|---|
| 主机 / 地址 | 车机 IP，例如 `192.168.1.23` |
| 端口 | `21`（改过就填改后的） |
| 用户名 / 密码 | `ftp` / `ftp`（按你的 ini） |
| 模式 | **被动模式 PASV**（手机端一般都默认被动，务必别关） |
| 编码 | UTF-8 |

常见 App 的路径：

- **AndFTP**：主机名 + 端口 + 用户/密码 → 类型选 FTP → 保存 → 连接
- **ES 文件浏览器**：网络 → FTP → 新建 → 填地址端口账号
- **Solid Explorer**：+ → FTP/SFTP → 主机/端口/账号
- **FE File Explorer**：+ → FTP → 填地址
- **Cx / MiXplorer**：添加存储 → FTP

连上以后就是普通的文件管理：长按文件可改名/删除/移动，粘贴即可上传。
上传大文件时别让手机锁屏断 Wi-Fi。

### 3. Windows 资源管理器（PC 也可以连）

地址栏输入：

```
ftp://ftp:ftp@192.168.1.23:21/
```

（资源管理器只支持被动模式，本服务器默认就支持。）

---

## 三点五、"车机有新文件，手机要刷很多次才看到"

这是 FTP 客户端缓存，不是服务器慢。先按下面的办法确认，再对症下药。

### 先分清是谁的问题

在任意能发自定义命令的 FTP 客户端里执行：

```
SITE DIAG
```

服务器会**当场重新扫一遍当前目录**并逐个列出来。判读只有两种：

- **新文件出现在 SITE DIAG 列表里，但手机上不显示** → 文件确实已经在车机磁盘上，
  服务器也看到了，**是手机 App 的目录缓存**。见下面「客户端侧」。
- **SITE DIAG 里也没有** → 才轮到查车机这边（见「车机侧」）。

（`SITE DIAG` 需要客户端支持自定义命令。手机 App 大多没有这个入口，
可以改用 PC 上的 FileZilla / 命令行 `ftp` 连同一个地址测。）

### 客户端侧（最常见）

- 下拉刷新只触发「重新列表」，很多 App 还有独立的**缓存层**。找设置里的
  「缓存」「缩略图缓存」「清除缓存」，关掉或清一次。
- **ES 文件浏览器 / MiXplorer** 这类默认开图片缩略图，大目录下新文件要等
  缩略图队列跑完才显示，体感就是"刷很多次才出来"。
- 部分 App 的刷新有节流（比如 5 秒内只真刷新一次），连续快速下拉其实是无效的，
  **等 5–10 秒再刷一次**比连刷十次有用。
- 换 Solid Explorer / AndFTP / FE 对比一下，如果换一个就正常，就锁定是 App 问题。

### 车机侧

本版本已经做了两件针对这件事的改进：

1. **数据连接优雅关闭**：以前列完目录直接 `closesocket`，WinSock 在还有未读数据时
   会发 RST 而不是 FIN，手机端可能把整份目录当"传输中断"丢掉，于是继续显示旧缓存。
   现在先 `shutdown(SD_SEND)` 把 FIN 发出去，再收掉残留字节，最后才 close。
2. **上传后 `FlushFileBuffers`**：以前上传完直接关句柄就回 226，WinCE 的 FAT
   可能还攒在缓存里没落盘，手机紧接着列目录看到的可能是旧状态。

另外注意：如果你的 `Root` 指向的是 **U 盘 / SD 卡**，车机上的 FAT 驱动自身有
目录项缓存，新文件在 PC 上也许要几秒才看得到——这属于存储驱动，程序改不了，
但 `SITE DIAG` 能帮你判断是不是这一层。

---

## 四、支持的 FTP 命令

```
USER PASS ACCT QUIT NOOP SYST FEAT OPTS CLNT HELP STAT SITE
PWD XPWD CWD XCWD CDUP XCUP TYPE MODE STRU ALLO
PASV EPSV PORT EPRT
LIST NLST MLSD MLST
RETR STOR APPE REST SIZE MDTM
DELE MKD XMKD RMD XRMD RNFR RNTO
```

意思是手机文件管理器需要的操作都齐了：列目录（含 MLSD 机器可读列表）、下载、上传、续传
（`REST`）、改名、删除、建/删目录、查大小和修改时间。

`SITE SHUTDOWN` 可以在登录后远程停掉服务器（无窗口设备的应急手段），
不需要的话别在公网暴露端口。

---

## 五、排错

| 现象 | 原因 / 处理 |
|---|---|
| 手机提示「连接被拒绝 / 无法连接」 | ① exe 没在跑；② 端口不对；③ IP 填错或不在同一网段；④ 车机 Wi-Fi 已休眠。先 `ping` 车机 IP |
| 能登录，但列目录一直转圈 | 数据连接没建起来。确认手机端是**被动模式**；车机有多网卡时在 ini 里设 `PasvIP=手机能访问的那个IP` |
| 能看目录，下载/上传失败 | 同上，九成是 PASV 数据端口被挡（路由器 AP 隔离、访客网络）。换手机热点直连试 |
| 中文文件名乱码 | `Encoding=GBK` 试一下；多数现代 App 用默认 `UTF8` 就正常 |
| 上传提示 550 / 无法创建文件 | `ReadOnly=1`；或目标文件是只读属性；或存储卡写保护/满了 |
| 登录 530 | 用户名密码不对；`User` 留空表示匿名，任意用户名都能进 |
| 21 端口起不来 | 端口被占用，改 `Port=2121`，手机端端口同步改 |
| 传大文件中途断 | 车机 Wi-Fi 省电策略/信号弱。关掉设备休眠，靠近路由；单文件别超过 4GB |
| 日志里有 `bind() failed` | 端口被占或被系统保留，换端口 |

日志在 `ftpd.log`（或 `LogFile` 指定位置），每次连接、每条命令都有记录，
排错时先看它。

---

## 六、安全提醒

- 这套东西是给**局域网**用的。不要把 21 端口映射到公网。
- 默认账号 `ftp/ftp` 只是方便首次调试，**部署后请改密码**，并把 `Root` 收窄到真正需要共享的目录。
- 服务器是明文 FTP（不支持 FTPS/TLS）。同网段里有人抓包就能看到密码，介意的话用 `ReadOnly=1` 或只在可信网络开。

---

## 七、限制

- 无 TLS/FTPS（WinCE 上做 TLS 成本太高，不在本工具范围）。
- 单文件最大 4GB（32 位 `DWORD` 偏移）。
- 传输过程中不处理 `ABOR`（客户端直接断开数据连接即可，服务器会回 426 并收尾）。
- `TYPE A` 不做换行转换，一律按二进制传输（手机端实际都用 `TYPE I`）。
- 每个连接一个线程；并发数由 `MaxClients` 控制。

---

## 八、PC 端自测（可选，给维护者）

`host/` 里是一层把 Win32 API 映射到 POSIX 的垫片，**编译的是同一份 `ftpd.c`**，
所以在电脑上跑的就是将来烧进车机的那套逻辑。

```bash
mkdir -p .build
clang -std=gnu99 -I host -DFTPD_CONSOLE -DFTPD_NO_WINMAIN \
      ftpd.c -o .build/ftpd_host -lpthread -liconv
python3 host/test_ftpd.py .build/ftpd_host
```

27 项端到端用例：登录/拒绝、目录增删、上传下载、1MiB 完整性、APPE、REST 续传
（下载与上传两个方向、以及偏移超过文件尾的情况）、SIZE/MDTM、LIST/NLST/MLSD/MLST、
改名删除、UTF-8 文件名与目录、GBK 编码模式、路径逃逸、主动模式、EPSV、只读模式、
并发上限、多会话并行、命令集。CI 里还会再跑一遍 ASan（地址/越界检查）版本。

> Android 上 `/sdcard` 是 `noexec`，测试脚本会自动把可执行文件复制到应用私有目录再跑。

另外用 CeGCC 的头文件做过一次纯语法检查（`--target=armv7-windows-gnu -DUNDER_CE -D_M_ARM -DARM`），
并核对过 `libcoredll.a` / `libws2.a` 里用到的每个导入符号都存在。

---

## 九、实现要点（改代码前先看）

1. **PASV 回报的 IP** 取控制连接的**本端地址**，不是 `0.0.0.0` 也不是 `127.0.0.1`；
   多网卡时可用 `PasvIP` 覆盖。手机拿这个 IP 才能连上数据端口。
2. **文件名编码**：WinCE 内部是 UTF-16，线上是 UTF-8。自己实现了 UTF-8 编解码，
   不依赖系统 `CP_UTF8` 支持；解不出来时按 `CP_ACP`（中文系统 = 936）兜底。
3. **路径**：`NormalizeVirtual()` 把 `.` `..` 和 `/` `\` 混用统一成从 `\` 出发的虚拟路径，
   再拼 `Root`，所以 `STOR ..\..\x` 只会落在根目录里。
4. **线程模型**：主线程 `select` + `accept`，每个连接一个线程；控制连接 10 分钟空闲超时。
5. **不做 `lstrcpyW` / `wcscpy`**：某些车机的 coredll 导出不全，宽串操作用自己的循环，
   少一个真机闪退的理由。
