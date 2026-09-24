# Susie for Win32 Unicodeパス名対応フック - ビルド用Makefile
#
# 使い方:
#   make          susie_unicode_hook.dll と SusieUnicode.exe を作る
#   make clean    生成物を削除する

CC      := i686-w64-mingw32-gcc
WINDRES := i686-w64-mingw32-windres
CFLAGS  := -O2 -Wall

DLL      := susie_unicode_hook.dll
EXE      := SusieUnicode.exe
DLL_RES_OBJ := susie_unicode_hook_res.o
EXE_RES_OBJ := susie_unicode_launcher_res.o

.PHONY: all clean

all: $(DLL) $(EXE)

$(DLL_RES_OBJ): src/susie_unicode_hook.rc src/version.h
	$(WINDRES) src/susie_unicode_hook.rc -O coff -o $@

$(DLL): src/susie_unicode_hook.c src/version.h $(DLL_RES_OBJ)
	$(CC) -shared $(CFLAGS) -o $@ src/susie_unicode_hook.c $(DLL_RES_OBJ) -Wl,--kill-at -lshell32 -luser32

$(EXE_RES_OBJ): src/susie_unicode_launcher.rc src/susie_icon.ico src/version.h
	$(WINDRES) src/susie_unicode_launcher.rc -O coff -o $@

$(EXE): src/susie_unicode_launcher.c src/version.h $(EXE_RES_OBJ)
	$(CC) $(CFLAGS) -mwindows -o $@ src/susie_unicode_launcher.c $(EXE_RES_OBJ)

clean:
	rm -f $(DLL) $(EXE) $(DLL_RES_OBJ) $(EXE_RES_OBJ)
