# WinCE FTP 服务器 —— 用 CeGCC (arm-mingw32ce) 交叉编译
#
#   make          -> build/ftpd.exe
#   make check    -> 打印实际用的编译器
#   make clean
#
# ⚠ 两个已经踩过的坑（和 wince-carlife 项目一致）：
#   · 必须链 -lws2（WinCE 的 ws2.dll），绝不能写 -lws2_32（桌面 WS2_32.dll）
#   · -liphlpapi 用于 GetAdaptersInfo：车机不能上网时也能读到网卡 IP，
#     不依赖 DNS。真机上没有这个 DLL 的话程序起不来，所以 CI 会校验导入表。
#   · 不要加 -mwindows / --subsystem，arm-mingw32ce 默认就产出 WinCE GUI 程序

CROSS   ?= arm-mingw32ce-
CC      := $(CROSS)gcc

BUILD   := build
CFLAGS  := -Os -Wall -DUNICODE -D_UNICODE
LDFLAGS ?= -s
CORELIB ?= -lcoredll
NETLIB  ?= -lws2 -liphlpapi

EXE := $(BUILD)/ftpd.exe

.PHONY: all check clean

all: $(EXE)

$(BUILD):
	mkdir -p $(BUILD)

$(BUILD)/ftpd.o: ftpd.c | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@

$(EXE): $(BUILD)/ftpd.o
	$(CC) $(LDFLAGS) -o $@ $^ $(CORELIB) $(NETLIB)
	@ls -l $@

check:
	@$(CC) --version | head -1

clean:
	rm -rf $(BUILD)
