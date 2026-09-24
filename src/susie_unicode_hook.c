/*
 * susie_unicode_hook.c (susie_unicode_hook.dll)
 *
 * Unicode 非対応の Susie.exe 向け Unicodeパス名対応フック
 */

#include <windows.h>
#include <shellapi.h>
#include <shlobj.h>
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include "version.h"

/* ------------------------------------------------------------------ */
/*  設定 (DLLと同じフォルダの SusieUnicode.ini から読み込む)          */
/* ------------------------------------------------------------------ */

static HMODULE g_hSelfModule = NULL; /* DllMainで設定する、このDLL自身のハンドル */
static BOOL g_settingEnableLog          = FALSE; /* デフォルト: OFF */
static BOOL g_settingSyncCatalogFileTimestamp   = FALSE; /* デフォルト: OFF */

/* SusieUnicode.ini のパスを組み立てる (DLLと同じフォルダ) */
static void GetIniPath(WCHAR *outW, size_t outWSize)
{
    WCHAR dllPathW[MAX_PATH];
    GetModuleFileNameW(g_hSelfModule, dllPathW, MAX_PATH);
    WCHAR *lastBs = wcsrchr(dllPathW, L'\\');
    if (lastBs) *(lastBs + 1) = 0;

    _snwprintf(outW, outWSize, L"%sSusieUnicode.ini", dllPathW);
    outW[outWSize - 1] = 0;
}

/* ini が無ければデフォルト設定で新規作成する */
static void CreateDefaultIniIfMissing(const WCHAR *iniPathW)
{
    if (GetFileAttributesW(iniPathW) != INVALID_FILE_ATTRIBUTES) return;

    FILE *f = _wfopen(iniPathW, L"wb");
    if (!f) return;
    {
        static const unsigned char bom[3] = { 0xEF, 0xBB, 0xBF };
        fwrite(bom, 1, 3, f);
    }
    fprintf(f,
        "[Settings]\r\n"
        "; %%TEMP%%\\susie_unicode_hook.log にデバッグログを出力する\r\n"
        "; (0 = 無効, 1 = 有効)\r\n"
        "EnableLog=0\r\n"
        "\r\n"
        "; 書庫(.zip等)のカタログファイル(.sue)の最終更新日時を、\r\n"
        "; 対応する書庫ファイル自身の最終更新日時に合わせる\r\n"
        "; (0 = 無効, 1 = 有効)\r\n"
        "SyncCatalogFileTimestamp=0\r\n");
    fclose(f);
}

/* GetPrivateProfileIntW ではなく、自前で ini ファイルを読み込む (iniキャッシュバグ回避) */
static int ReadIniIntSimple(const WCHAR *iniPathW, const char *key, int defaultValue)
{
    FILE *f = _wfopen(iniPathW, L"rb");
    if (!f) return defaultValue;

    char line[512];
    size_t keyLen = strlen(key);
    int result = defaultValue;

    while (fgets(line, sizeof(line), f)) {
        char *p = line;
        while (*p == ' ' || *p == '\t') p++;
        if (_strnicmp(p, key, keyLen) == 0) {
            char *q = p + keyLen;
            while (*q == ' ' || *q == '\t') q++;
            if (*q == '=') result = atoi(q + 1);
        }
    }
    fclose(f);
    return result;
}

static void LoadSettings(void)
{
    WCHAR iniPathW[MAX_PATH];
    GetIniPath(iniPathW, MAX_PATH);

    CreateDefaultIniIfMissing(iniPathW);

    g_settingEnableLog = ReadIniIntSimple(iniPathW, "EnableLog", 0) != 0;
    g_settingSyncCatalogFileTimestamp = ReadIniIntSimple(iniPathW, "SyncCatalogFileTimestamp", 0) != 0;
}

/* ------------------------------------------------------------------ */
/*  ログ (デバッグ用。%TEMP%\susie_unicode_hook.log に追記する)       */
/* ------------------------------------------------------------------ */

/* ANSI文字列をUTF-8に変換する (ログはUTF-8で統一するため)
   使い捨てバッファを使い回すだけの簡易ヘルパー */
static const char *A2U(const char *ansi)
{
    static char bufs[4][2048];
    static int  idx = 0;

    if (!ansi) return "(null)";

    char *out = bufs[idx];
    idx = (idx + 1) % 4;

    WCHAR wbuf[1024];
    int wlen = MultiByteToWideChar(CP_ACP, 0, ansi, -1, wbuf, 1024);
    if (wlen == 0) {
        strncpy(out, "(変換失敗)", sizeof(bufs[0]) - 1);
        out[sizeof(bufs[0]) - 1] = 0;
        return out;
    }
    WideCharToMultiByte(CP_UTF8, 0, wbuf, -1, out, (int)sizeof(bufs[0]), NULL, NULL);
    return out;
}

/* ワイド文字列版のA2U */
static const char *W2U(const WCHAR *wide)
{
    static char bufs[2][2048];
    static int  idx = 0;

    if (!wide) return "(null)";

    char *out = bufs[idx];
    idx = (idx + 1) % 2;
    WideCharToMultiByte(CP_UTF8, 0, wide, -1, out, (int)sizeof(bufs[0]), NULL, NULL);
    return out;
}

static void LogImpl(const char *fmt, ...)
{
    char msg[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof(msg), fmt, ap);
    va_end(ap);

    char path[MAX_PATH];
    GetTempPathA(MAX_PATH, path);
    strcat(path, "susie_unicode_hook.log");

    /* ファイルがまだ存在しない(=今回のプロセスで最初の書き込み)なら、
       UTF-8 BOM を先頭に書いておく
       (エディタ等がUTF-8だと自動認識でき、文字化けを防げる) */
    BOOL isNewFile = (GetFileAttributesA(path) == INVALID_FILE_ATTRIBUTES);

    FILE *f = fopen(path, "a");
    if (f) {
        if (isNewFile) {
            static const unsigned char bom[3] = { 0xEF, 0xBB, 0xBF };
            fwrite(bom, 1, 3, f);
        }
        SYSTEMTIME st;
        GetLocalTime(&st);
        fprintf(f, "[%02d:%02d:%02d.%03d] %s\n",
                st.wHour, st.wMinute, st.wSecond, st.wMilliseconds, msg);
        fclose(f);
    }
}

/* ログ無効時は引数 (A2U 等の変換) も評価しない */
#define Log(...) do { if (g_settingEnableLog) LogImpl(__VA_ARGS__); } while (0)

/* ------------------------------------------------------------------- */
/*  「実Unicodeパス名」～「仮想ANSIフォルダ名/ファイル名」変換テーブル */
/*    (名前付き共有メモリ)                                             */
/*  Susie.exe と SusieUnicode.exe 間でプロセス間共有                   */
/* ------------------------------------------------------------------- */

#define MAX_ALIASES 8192
#define SHM_MAPPING_NAME "SusieUnicodeHookAliasTable_v1"
#define SHM_MUTEX_NAME   "SusieUnicodeHookAliasMutex_v1"

/* 共有するデータ構造 */
typedef struct {
    char  alias[32];          /* Susie.exe に渡す、仮想ANSIフォルダ名("SUDxxxxxxxxxxxx") or ファイル名("SUFxxxxxxxxxxxx.ext") */
    WCHAR realPath[MAX_PATH]; /* 対応する実Unicodeフルパス名 */
    LONG  used;
} AliasEntry;

typedef struct {
    LONG       count;                  /* 現在登録されているエイリアス数 */
    AliasEntry entries[MAX_ALIASES];
} SharedAliasTable;

static HANDLE            g_hAliasMapping = NULL;
static SharedAliasTable *g_sharedAliases = NULL;
static HANDLE             g_hAliasMutex   = NULL;

/* 共有メモリ初期化 */
static void InitSharedAliasTable(void)
{
    g_hAliasMutex = CreateMutexA(NULL, FALSE, SHM_MUTEX_NAME);

    g_hAliasMapping = CreateFileMappingA(INVALID_HANDLE_VALUE, NULL, PAGE_READWRITE,
                                          0, (DWORD)sizeof(SharedAliasTable), SHM_MAPPING_NAME);
    BOOL isNewMapping = (GetLastError() != ERROR_ALREADY_EXISTS);

    if (g_hAliasMapping) {
        g_sharedAliases = (SharedAliasTable *)MapViewOfFile(
                g_hAliasMapping, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(SharedAliasTable));
    }
    Log("共有エイリアス対応表を%s (mapping=%p, view=%p)",
            isNewMapping ? "新規作成" : "既存のものに接続", (void*)g_hAliasMapping, (void*)g_sharedAliases);
}

/* 拡張子(ASCIIの範囲)を取り出す。無ければ空文字 */
static void GetAsciiExt(const WCHAR *wname, char *out, size_t outsize)
{
    out[0] = 0;
    const WCHAR *dot = wcsrchr(wname, L'.');
    if (!dot) return;
    dot++; /* '.' の次から */
    size_t i = 0;
    for (; dot[i] && i < outsize - 1; i++) {
        WCHAR c = dot[i];
        if (c > 0x7E) { out[0] = 0; return; } /* 拡張子自体が非ASCIIなら諦める */
        out[i] = (char)c;
    }
    out[i] = 0;
}

/* FNV-1a ハッシュ (64bit)。大文字小文字を無視するため事前に大文字化する */
static uint64_t Fnv1aHash64UpperW(const WCHAR *s)
{
    uint64_t h = 14695981039346656037ULL; /* FNV offset basis (64bit) */
    for (; *s; s++) {
        WCHAR c = *s;
        if (c >= L'a' && c <= L'z') c -= 32; /* 大文字化 (ASCII範囲のみ) */
        h ^= (uint64_t)c;
        h *= 1099511628211ULL; /* FNV prime (64bit) */
    }
    return h;
}

/* 実Unicodeパス名のFNV-1aハッシュ値から、
   仮想ANSIフォルダ名("SUDxxxxxxxxxxxx")
   あるいは
   ファイル名("SUFxxxxxxxxxxxx.ext")
   を生成する */
static void ComputeDeterministicAlias(const WCHAR *realFullPath, const WCHAR *realFileName,
                                       BOOL isDirectory, char *alias, size_t aliasSize)
{
    uint64_t hash = Fnv1aHash64UpperW(realFullPath) & 0xFFFFFFFFFFFFULL; /* 下位48bit (12桁の16進) */

    if (isDirectory) {
        _snprintf(alias, aliasSize, "SUD%012llX", (unsigned long long)hash);
    } else {
        char ext[16];
        GetAsciiExt(realFileName, ext, sizeof(ext));
        if (ext[0])
            _snprintf(alias, aliasSize, "SUF%012llX.%s", (unsigned long long)hash, ext);
        else
            _snprintf(alias, aliasSize, "SUF%012llX", (unsigned long long)hash);
    }
}

/* 実Unicodeパス名～仮想ANSIフォルダ名/ファイル名の対応関係を登録 */
static void RegisterAliasEx(const WCHAR *realFullPath, const WCHAR *realFileName,
                             BOOL isDirectory, char *alias, size_t aliasSize)
{
    ComputeDeterministicAlias(realFullPath, realFileName, isDirectory, alias, aliasSize);

    if (!g_sharedAliases) {
        /* 共有メモリが使えない場合、対応表への登録は諦めるが、
           エイリアス名自体はハッシュで決まるので呼び出し元には返せる */
        return;
    }

    WaitForSingleObject(g_hAliasMutex, INFINITE);

    /* 既に同じエイリアスが登録済みなら重複登録しない */
    LONG cnt = g_sharedAliases->count;
    for (LONG i = 0; i < cnt; i++) {
        if (g_sharedAliases->entries[i].used &&
            _stricmp(g_sharedAliases->entries[i].alias, alias) == 0) {
            WCHAR existing[MAX_PATH] = L"";
            if (_wcsicmp(g_sharedAliases->entries[i].realPath, realFullPath) != 0)
                wcscpy(existing, g_sharedAliases->entries[i].realPath);
            ReleaseMutex(g_hAliasMutex);
            if (existing[0])
                Log("ハッシュ衝突: %s は %s に登録済みのため、%s は解決できません",
                        alias, W2U(existing), W2U(realFullPath));
            return;
        }
    }

    LONG idx = g_sharedAliases->count;
    if (idx >= MAX_ALIASES) {
        ReleaseMutex(g_hAliasMutex);
        Log("エイリアス対応表が満杯です (%d件)", MAX_ALIASES);
        return;
    }

    strncpy(g_sharedAliases->entries[idx].alias, alias,
            sizeof(g_sharedAliases->entries[idx].alias) - 1);
    g_sharedAliases->entries[idx].alias[sizeof(g_sharedAliases->entries[idx].alias) - 1] = 0;
    wcsncpy(g_sharedAliases->entries[idx].realPath, realFullPath, MAX_PATH - 1);
    g_sharedAliases->entries[idx].realPath[MAX_PATH - 1] = 0;
    g_sharedAliases->entries[idx].used = 1;
    g_sharedAliases->count = idx + 1;

    ReleaseMutex(g_hAliasMutex);

    Log("ALIAS登録(共有,%s): %s -> %s", isDirectory ? "dir" : "file", alias, W2U(realFullPath));
}

/* パス1階層分の仮想ANSI名が変換テーブルにあれば実Unicodeフルパスを返す
   末尾が ".sue" (書庫のカタログファイル) の場合は、拡張子を除いた
   部分が一致するエイリアスを探して対応する                        */
static int ResolveAliasComponent(const char *component, WCHAR *outReal, size_t outSize)
{
    if (strncmp(component, "SUF", 3) != 0 && strncmp(component, "SUD", 3) != 0) return 0;
    if (!g_sharedAliases) return 0;

    int found = 0;
    WaitForSingleObject(g_hAliasMutex, INFINITE);
    LONG cnt = g_sharedAliases->count;
    for (LONG i = 0; i < cnt; i++) {
        if (g_sharedAliases->entries[i].used &&
            _stricmp(g_sharedAliases->entries[i].alias, component) == 0) {
            wcsncpy(outReal, g_sharedAliases->entries[i].realPath, outSize - 1);
            outReal[outSize - 1] = 0;
            found = 1;
            break;
        }
    }
    ReleaseMutex(g_hAliasMutex);
    if (found) return 1;

    /* Susieは書庫のカタログファイルを「拡張子を.sueに置き換えたファイル名」
       (例: "archive.zip" -> "archive.sue") で生成する。仮想ANSIファイル名
       だと "SUFxxxxxxxxxxxx.sue" になり元の "SUFxxxxxxxxxxxx.zip" と一致しないため、
       拡張子を除いた部分が一致する仮想ANSIファイル名を探す      */
    size_t len = strlen(component);
    if (len > 4 && _stricmp(component + len - 4, ".sue") == 0) {
        const char *dot = strchr(component, '.');
        size_t prefixLen = dot ? (size_t)(dot - component) : (len - 4);

        char prefix[40];
        if (prefixLen > 0 && prefixLen < sizeof(prefix)) {
            memcpy(prefix, component, prefixLen);
            prefix[prefixLen] = 0;

            WCHAR matchedReal[MAX_PATH] = L"";
            BOOL matched = FALSE;

            WaitForSingleObject(g_hAliasMutex, INFINITE);
            LONG cnt2 = g_sharedAliases->count;
            for (LONG i = 0; i < cnt2; i++) {
                if (!g_sharedAliases->entries[i].used) continue;
                const char *a = g_sharedAliases->entries[i].alias;
                /* ハッシュ部分が完全一致するエイリアスを探す */
                if (_strnicmp(a, prefix, prefixLen) == 0 &&
                    (a[prefixLen] == '.' || a[prefixLen] == '\0')) {
                    wcsncpy(matchedReal, g_sharedAliases->entries[i].realPath, MAX_PATH - 1);
                    matchedReal[MAX_PATH - 1] = 0;
                    matched = TRUE;
                    break;
                }
            }
            ReleaseMutex(g_hAliasMutex);

            if (matched) {
                /* 拡張子部分(最後の '\' より後ろの最後の '.') を .sue に置き換える */
                WCHAR *lastBs = wcsrchr(matchedReal, L'\\');
                WCHAR *searchFrom = lastBs ? lastBs + 1 : matchedReal;
                WCHAR *lastDot = wcsrchr(searchFrom, L'.');
                if (lastDot) *lastDot = 0;

                _snwprintf(outReal, outSize, L"%s.sue", matchedReal);
                outReal[outSize - 1] = 0;
                return 1;
            }
        }
    }

    return 0;
}

/* 変換テーブルに存在しない仮想ANSI名を、指定ディレクトリを実際にスキャンして
 * 各エントリの仮想ANSI名を計算し、答え合わせして解決する
 * (Susie再起動直後など、変換テーブルが空でも変換できるようにするため)   */
static BOOL TryResolveByLiveDirectoryScan(const WCHAR *realDirW, const char *aliasComponent,
                                           WCHAR *outReal, size_t outSize)
{
    WCHAR pattern[MAX_PATH];
    _snwprintf(pattern, MAX_PATH, L"%s\\*", realDirW);
    pattern[MAX_PATH - 1] = 0;

    WIN32_FIND_DATAW wfd;
    HANDLE h = FindFirstFileW(pattern, &wfd);
    if (h == INVALID_HANDLE_VALUE) return FALSE;

    BOOL found = FALSE;
    do {
        if (wcscmp(wfd.cFileName, L".") == 0 || wcscmp(wfd.cFileName, L"..") == 0) continue;

        WCHAR entryFull[MAX_PATH];
        _snwprintf(entryFull, MAX_PATH, L"%s\\%s", realDirW, wfd.cFileName);
        entryFull[MAX_PATH - 1] = 0;

        BOOL isDir = (wfd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
        char candidateAlias[32];
        ComputeDeterministicAlias(entryFull, wfd.cFileName, isDir, candidateAlias, sizeof(candidateAlias));

        if (_stricmp(candidateAlias, aliasComponent) == 0) {
            wcsncpy(outReal, entryFull, outSize - 1);
            outReal[outSize - 1] = 0;
            /* 次回から再スキャン不要になるよう対応表にも登録 */
            char dummyAlias[32];
            RegisterAliasEx(entryFull, wfd.cFileName, isDir, dummyAlias, sizeof(dummyAlias));
            found = TRUE;
            break;
        }
    } while (FindNextFileW(h, &wfd));
    FindClose(h);

    if (found) {
        Log("  ディレクトリ再スキャンで解決: \"%s\" -> \"%s\"", A2U(aliasComponent), W2U(outReal));
    }
    return found;
}

/* パスを '\' 区切りで1階層ずつ調べ、エイリアスに一致した階層を実パスに
   差し替えながら組み立て直す。"C:\SUDxxxxxxxxxxxx\SUFxxxxxxxxxxxx.jpg" のように
   Unicode名フォルダが何階層あっても解決できる。変換テーブルに無ければ
   TryResolveByLiveDirectoryScan で再解決を試みる。
   戻り値: 1箇所でもエイリアス置換が発生したら TRUE                */
static BOOL ResolveFullPathAliases(const char *ansiPath, WCHAR *outRealW, size_t outSizeChars)
{
    /* 先にWideへ変換してから区切る (Shift_JISの2バイト目の 0x5C を '\' と誤認しないため) */
    WCHAR work[2048];
    if (!MultiByteToWideChar(CP_ACP, 0, ansiPath, -1, work, 2048)) work[0] = 0;
    work[2047] = 0;

    WCHAR realAccum[MAX_PATH];
    realAccum[0] = 0;
    BOOL anyResolved = FALSE;
    BOOL first = TRUE;

    WCHAR *p = work;

    /* UNCパス("\\server\share\...")は単純に区切ると先頭の
       "\\" が失われるため、"\\server\share" を1つの根っことして
       特別扱いする                                                */
    if (work[0] == L'\\' && work[1] == L'\\') {
        WCHAR *serverEnd = wcschr(work + 2, L'\\');
        if (serverEnd) {
            WCHAR *shareEnd = wcschr(serverEnd + 1, L'\\');
            size_t rootLen = shareEnd ? (size_t)(shareEnd - work) : wcslen(work);
            if (rootLen > MAX_PATH - 1) rootLen = MAX_PATH - 1;

            wcsncpy(realAccum, work, rootLen);
            realAccum[rootLen] = 0;
            first = FALSE;

            p = shareEnd ? shareEnd + 1 : (work + wcslen(work));
        }
    }

    WCHAR *saveptr = NULL;
    WCHAR *tok = wcstok_s(p, L"\\", &saveptr);
    while (tok) {
        char tokA[MAX_PATH];
        if (!WideCharToMultiByte(CP_ACP, 0, tok, -1, tokA, sizeof(tokA), NULL, NULL)) tokA[0] = 0;

        WCHAR componentReal[MAX_PATH];
        if (ResolveAliasComponent(tokA, componentReal, MAX_PATH)) {
            /* エイリアス -> 実パスへ差し替え */
            wcsncpy(realAccum, componentReal, MAX_PATH - 1);
            realAccum[MAX_PATH - 1] = 0;
            anyResolved = TRUE;
        } else if (!first && (strncmp(tokA, "SUF", 3) == 0 || strncmp(tokA, "SUD", 3) == 0) &&
                   realAccum[0] != 0 &&
                   TryResolveByLiveDirectoryScan(realAccum, tokA, componentReal, MAX_PATH)) {
            /* 対応表には無いが、それらしい名前なので再スキャンで解決 */
            wcsncpy(realAccum, componentReal, MAX_PATH - 1);
            realAccum[MAX_PATH - 1] = 0;
            anyResolved = TRUE;
        } else {
            /* エイリアスでない -> そのまま連結 */
            if (first) {
                wcsncpy(realAccum, tok, MAX_PATH - 1);
                realAccum[MAX_PATH - 1] = 0;
            } else {
                size_t len = wcslen(realAccum);
                if (len + 1 + wcslen(tok) < MAX_PATH) {
                    wcscat(realAccum, L"\\");
                    wcscat(realAccum, tok);
                }
            }
        }
        first = FALSE;
        tok = wcstok_s(NULL, L"\\", &saveptr);
    }

    wcsncpy(outRealW, realAccum, outSizeChars - 1);
    outRealW[outSizeChars - 1] = 0;
    return anyResolved;
}

/* ANSIパス中の最後の '\' を探す (2バイト文字の2バイト目の 0x5C を誤認しない) */
static const char *FindLastBackslashA(const char *s)
{
    const char *last = NULL;
    for (; *s; s++) {
        if (IsDBCSLeadByte((BYTE)*s) && s[1]) { s++; continue; }
        if (*s == '\\') last = s;
    }
    return last;
}

/* ワイド文字列が現在の ANSI コードページで情報を失わずに表現できるか判定 */
static BOOL IsAnsiSafe(const WCHAR *wname, char *outAnsi, size_t outAnsiSize)
{
    /* ANSIがUTF-8 (Windowsの「ベータ: UTF-8を使用」設定) ならすべての文字を表現できる。
       CP_UTF8 では WC_NO_BEST_FIT_CHARS と usedDefault を指定すると失敗するため渡さない */
    BOOL isUtf8 = (GetACP() == CP_UTF8);
    BOOL usedDefault = FALSE;
    int len = WideCharToMultiByte(CP_ACP, isUtf8 ? 0 : WC_NO_BEST_FIT_CHARS, wname, -1,
                                   outAnsi, (int)outAnsiSize, NULL, isUtf8 ? NULL : &usedDefault);
    if (len == 0) return FALSE;      /* 変換自体に失敗 (バッファ不足等) */
    if (usedDefault) return FALSE;   /* 変換不可能な文字があった          */
    return TRUE;
}

/* ------------------------------------------------------------------ */
/*  検索セッション管理 (FindFirstFileA/FindNextFileA 用)              */
/* ------------------------------------------------------------------ */

#define MAX_SESSIONS 64

typedef struct {
    HANDLE hFindW;
    int    inUse;
    WCHAR  dir[MAX_PATH];   /* 検索対象ディレクトリ (末尾 '\\' 付き) */
} FindSession;

static FindSession g_sessions[MAX_SESSIONS];
static CRITICAL_SECTION g_sessLock;

static FindSession *AllocSession(void)
{
    EnterCriticalSection(&g_sessLock);
    for (int i = 0; i < MAX_SESSIONS; i++) {
        if (!g_sessions[i].inUse) {
            g_sessions[i].inUse = 1;
            LeaveCriticalSection(&g_sessLock);
            return &g_sessions[i];
        }
    }
    LeaveCriticalSection(&g_sessLock);
    return NULL;
}

static FindSession *FindSessionByHandle(HANDLE h)
{
    EnterCriticalSection(&g_sessLock);
    for (int i = 0; i < MAX_SESSIONS; i++) {
        if (g_sessions[i].inUse && (HANDLE)&g_sessions[i] == h) {
            LeaveCriticalSection(&g_sessLock);
            return &g_sessions[i];
        }
    }
    LeaveCriticalSection(&g_sessLock);
    return NULL;
}

static void FreeSession(FindSession *s)
{
    EnterCriticalSection(&g_sessLock);
    s->inUse = 0;
    LeaveCriticalSection(&g_sessLock);
}

/* WIN32_FIND_DATAW を、必要なら仮想ANSI名化して WIN32_FIND_DATAA に
   変換する。
   優先順位:
     1. 長い名前がそのままANSI safeならそれを使う
     2. ダメなら、仮想ANSI名を発行して対応表に記録する
        -> LoadLibraryA フック経由でプラグインのIATも、また
           GetCommandLineA フック経由でコマンドライン起動も、すべて
           同じ対応表を共有して解決するので、これで一貫して対応できる */
static void ConvertFindDataAndAlias(const FindSession *s,
                                     const WIN32_FIND_DATAW *wfd,
                                     WIN32_FIND_DATAA *out)
{
    memset(out, 0, sizeof(*out));
    out->dwFileAttributes = wfd->dwFileAttributes;
    out->ftCreationTime   = wfd->ftCreationTime;
    out->ftLastAccessTime = wfd->ftLastAccessTime;
    out->ftLastWriteTime  = wfd->ftLastWriteTime;
    out->nFileSizeHigh    = wfd->nFileSizeHigh;
    out->nFileSizeLow     = wfd->nFileSizeLow;

    char ansiName[MAX_PATH];
    if (IsAnsiSafe(wfd->cFileName, ansiName, sizeof(ansiName))) {
        /* ケース1: 長い名前がそのままANSIセーフ */
        strncpy(out->cFileName, ansiName, MAX_PATH - 1);
        if (!IsAnsiSafe(wfd->cAlternateFileName, out->cAlternateFileName, sizeof(out->cAlternateFileName)))
            out->cAlternateFileName[0] = 0;
        Log("  長い名前をそのまま使用: %s", A2U(ansiName));
    } else {
        /* ケース2: 仮想ANSI名を発行して対応表に記録する
           (ファイル名だけでなく、ディレクトリ名の場合も同様に扱う。
            ディレクトリかどうかは dwFileAttributes で判定) */
        WCHAR fullPath[MAX_PATH];
        _snwprintf(fullPath, MAX_PATH, L"%s%s", s->dir, wfd->cFileName);
        fullPath[MAX_PATH - 1] = 0;

        BOOL isDir = (wfd->dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
        char alias[32];
        RegisterAliasEx(fullPath, wfd->cFileName, isDir, alias, sizeof(alias));
        strncpy(out->cFileName, alias, MAX_PATH - 1);
        Log("  エイリアス発行(%s): %s", isDir ? "dir" : "file", alias);
    }
}

/* ------------------------------------------------------------------ */
/*  元の API へのポインタ                                             */
/* ------------------------------------------------------------------ */

typedef HANDLE (WINAPI *FindFirstFileA_t)(LPCSTR, LPWIN32_FIND_DATAA);
typedef BOOL   (WINAPI *FindNextFileA_t)(HANDLE, LPWIN32_FIND_DATAA);
typedef BOOL   (WINAPI *FindClose_t)(HANDLE);
typedef HANDLE (WINAPI *CreateFileA_t)(LPCSTR, DWORD, DWORD,
                                        LPSECURITY_ATTRIBUTES, DWORD, DWORD, HANDLE);
typedef DWORD  (WINAPI *GetFileAttributesA_t)(LPCSTR);
typedef DWORD  (WINAPI *GetFullPathNameA_t)(LPCSTR, DWORD, LPSTR, LPSTR *);
typedef HFILE  (WINAPI *_lopen_t)(LPCSTR, int);
typedef HFILE  (WINAPI *_lclose_t)(HFILE);
typedef BOOL   (WINAPI *CloseHandle_t)(HANDLE);
typedef HFILE  (WINAPI *_lcreat_t)(LPCSTR, int);
typedef HFILE  (WINAPI *OpenFile_t)(LPCSTR, LPOFSTRUCT, UINT);
typedef DWORD  (WINAPI *GetShortPathNameA_t)(LPCSTR, LPSTR, DWORD);
typedef BOOL   (WINAPI *SetCurrentDirectoryA_t)(LPCSTR);
typedef DWORD  (WINAPI *GetCurrentDirectoryA_t)(DWORD, LPSTR);
typedef BOOL   (WINAPI *CreateDirectoryA_t)(LPCSTR, LPSECURITY_ATTRIBUTES);
typedef BOOL   (WINAPI *MoveFileA_t)(LPCSTR, LPCSTR);
typedef UINT   (WINAPI *GetDriveTypeA_t)(LPCSTR);
typedef BOOL   (WINAPI *GetVolumeInformationA_t)(LPCSTR, LPSTR, DWORD, LPDWORD, LPDWORD, LPDWORD, LPSTR, DWORD);
typedef DWORD  (WINAPI *GetPrivateProfileStringA_t)(LPCSTR, LPCSTR, LPCSTR, LPSTR, DWORD, LPCSTR);
typedef BOOL   (WINAPI *WritePrivateProfileStringA_t)(LPCSTR, LPCSTR, LPCSTR, LPCSTR);
typedef UINT   (WINAPI *GetPrivateProfileIntA_t)(LPCSTR, LPCSTR, INT, LPCSTR);
typedef DWORD_PTR (WINAPI *SHGetFileInfoA_t)(LPCSTR, DWORD, SHFILEINFOA *, UINT, UINT);
typedef HMODULE (WINAPI *LoadLibraryA_t)(LPCSTR);
typedef HMODULE (WINAPI *LoadLibraryExA_t)(LPCSTR, HANDLE, DWORD);
typedef LPSTR  (WINAPI *GetCommandLineA_t)(void);

static FindFirstFileA_t     Real_FindFirstFileA;
static FindNextFileA_t      Real_FindNextFileA;
static FindClose_t          Real_FindClose;
static CreateFileA_t        Real_CreateFileA;
static GetFileAttributesA_t Real_GetFileAttributesA;
static GetFullPathNameA_t   Real_GetFullPathNameA;
static _lopen_t             Real__lopen;
static _lclose_t            Real__lclose;
static CloseHandle_t        Real_CloseHandle;
static _lcreat_t            Real__lcreat;
static OpenFile_t           Real_OpenFile;
static GetShortPathNameA_t  Real_GetShortPathNameA;
static SetCurrentDirectoryA_t       Real_SetCurrentDirectoryA;
static GetCurrentDirectoryA_t       Real_GetCurrentDirectoryA;
static CreateDirectoryA_t           Real_CreateDirectoryA;
static MoveFileA_t                  Real_MoveFileA;
static GetDriveTypeA_t              Real_GetDriveTypeA;
static GetVolumeInformationA_t      Real_GetVolumeInformationA;
static GetPrivateProfileStringA_t   Real_GetPrivateProfileStringA;
static WritePrivateProfileStringA_t Real_WritePrivateProfileStringA;
static GetPrivateProfileIntA_t      Real_GetPrivateProfileIntA;
static SHGetFileInfoA_t             Real_SHGetFileInfoA;
static LoadLibraryA_t       Real_LoadLibraryA;
static LoadLibraryExA_t     Real_LoadLibraryExA;
static GetCommandLineA_t    Real_GetCommandLineA;

static void PatchIat(HMODULE hModule);
static HMODULE WINAPI Hook_LoadLibraryA(LPCSTR lpLibFileName);
static HMODULE WINAPI Hook_LoadLibraryExA(LPCSTR lpLibFileName, HANDLE hFile, DWORD dwFlags);
static void TrackSueHandleIfApplicable(const char *ansiFileName, const WCHAR *resolvedSuePathW, HANDLE h);
static void ApplySueTimestampIfTracked(HANDLE h);

/* ------------------------------------------------------------------ */
/*  フック本体                                                        */
/* ------------------------------------------------------------------ */

static HANDLE WINAPI Hook_FindFirstFileA(LPCSTR lpFileName, LPWIN32_FIND_DATAA lpFindData)
{
    Log("FindFirstFileA(\"%s\")", A2U(lpFileName));

    if (!lpFileName) {
        return Real_FindFirstFileA(lpFileName, lpFindData);
    }

    /* ディレクトリ部分・末尾のファイル名部分、両方のエイリアスを扱う */
    char dirA[MAX_PATH] = {0};
    char wildcardA[MAX_PATH] = {0}; /* 末尾のファイル名 or ワイルドカード部分 */
    const char *lastSlash = FindLastBackslashA(lpFileName);
    if (lastSlash) {
        size_t n = (size_t)(lastSlash - lpFileName) + 1;
        if (n >= MAX_PATH) n = MAX_PATH - 1;
        memcpy(dirA, lpFileName, n);
        dirA[n] = 0;
        strncpy(wildcardA, lastSlash + 1, sizeof(wildcardA) - 1);
    } else {
        strncpy(wildcardA, lpFileName, sizeof(wildcardA) - 1);
    }

    /* 末尾がワイルドカードを含まない既知のエイリアス名なら、
       1ファイルの直接指定として扱う */
    WCHAR aliasRealPath[MAX_PATH];
    if (!strchr(wildcardA, '*') && !strchr(wildcardA, '?') &&
        ResolveFullPathAliases(lpFileName, aliasRealPath, MAX_PATH)) {

        Log("  エイリアス解決 (FindFirstFileA): \"%s\" -> \"%s\"",
                A2U(lpFileName), W2U(aliasRealPath));

        WIN32_FIND_DATAW wfdReal;
        HANDLE hReal = FindFirstFileW(aliasRealPath, &wfdReal);
        if (hReal == INVALID_HANDLE_VALUE) {
            SetLastError(GetLastError());
            return INVALID_HANDLE_VALUE;
        }

        FindSession *s = AllocSession();
        if (!s) {
            FindClose(hReal);
            SetLastError(ERROR_TOO_MANY_OPEN_FILES);
            return INVALID_HANDLE_VALUE;
        }
        s->hFindW = hReal;
        /* 実パスの親ディレクトリを覚えておく (念のため) */
        WCHAR *lastBs = wcsrchr(aliasRealPath, L'\\');
        s->dir[0] = 0;
        if (lastBs) {
            size_t n = (size_t)(lastBs - aliasRealPath) + 1;
            if (n > MAX_PATH - 1) n = MAX_PATH - 1;
            wcsncpy(s->dir, aliasRealPath, n);
            s->dir[n] = 0;
        }

        /* cFileName はエイリアス名のまま返す */
        memset(lpFindData, 0, sizeof(*lpFindData));
        lpFindData->dwFileAttributes = wfdReal.dwFileAttributes;
        lpFindData->ftCreationTime   = wfdReal.ftCreationTime;
        lpFindData->ftLastAccessTime = wfdReal.ftLastAccessTime;
        lpFindData->ftLastWriteTime  = wfdReal.ftLastWriteTime;
        lpFindData->nFileSizeHigh    = wfdReal.nFileSizeHigh;
        lpFindData->nFileSizeLow     = wfdReal.nFileSizeLow;

        strncpy(lpFindData->cFileName, wildcardA, MAX_PATH - 1);
        char realNameA[MAX_PATH];
        if (!IsAnsiSafe(wfdReal.cFileName, realNameA, sizeof(realNameA)) ||
            !IsAnsiSafe(wfdReal.cAlternateFileName, lpFindData->cAlternateFileName,
                        sizeof(lpFindData->cAlternateFileName)))
            lpFindData->cAlternateFileName[0] = 0;

        Log("  -> 疑似ハンドル %p (エイリアス経由)", (void*)s);
        return (HANDLE)s;
    }

    /* ワイルドカード列挙のケース。ディレクトリ部分を先に解決する */
    WCHAR dirW[MAX_PATH];
    BOOL dirWasAlias = ResolveFullPathAliases(dirA, dirW, MAX_PATH);
    if (dirWasAlias) {
        Log("  ディレクトリのエイリアス解決: \"%s\" -> \"%s\"", A2U(dirA), W2U(dirW));
    }
    /* 末尾に必ず '\' を付加する */
    {
        size_t len = wcslen(dirW);
        if (dirA[0] != 0 && (len == 0 || dirW[len - 1] != L'\\')) {
            if (len + 1 < MAX_PATH) { dirW[len] = L'\\'; dirW[len + 1] = 0; }
        }
    }

    /* 検索パターンを組み立てる。拡張子フィルタ(例 "*.jpg")はASCIIのみなので連結 */
    WCHAR wildcardW[MAX_PATH];
    MultiByteToWideChar(CP_ACP, 0, wildcardA, -1, wildcardW, MAX_PATH);

    WCHAR patternW[MAX_PATH];
    _snwprintf(patternW, MAX_PATH, L"%s%s", dirW, wildcardW);
    patternW[MAX_PATH - 1] = 0;

    WIN32_FIND_DATAW wfd;
    HANDLE hReal = FindFirstFileW(patternW, &wfd);
    if (hReal == INVALID_HANDLE_VALUE) {
        SetLastError(GetLastError());
        return INVALID_HANDLE_VALUE;
    }

    FindSession *s = AllocSession();
    if (!s) {
        FindClose(hReal);
        SetLastError(ERROR_TOO_MANY_OPEN_FILES);
        return INVALID_HANDLE_VALUE;
    }
    s->hFindW = hReal;
    wcsncpy(s->dir, dirW, MAX_PATH - 1);

    ConvertFindDataAndAlias(s, &wfd, lpFindData);

    Log("  -> 疑似ハンドル %p, 最初のファイル: %s", (void*)s, A2U(lpFindData->cFileName));
    return (HANDLE)s;
}

static BOOL WINAPI Hook_FindNextFileA(HANDLE hFindFile, LPWIN32_FIND_DATAA lpFindData)
{
    FindSession *s = FindSessionByHandle(hFindFile);
    if (!s) {
        /* 疑似ハンドルでなければ素通し (念のため) */
        return Real_FindNextFileA(hFindFile, lpFindData);
    }

    WIN32_FIND_DATAW wfd;
    if (!FindNextFileW(s->hFindW, &wfd)) {
        return FALSE;
    }
    ConvertFindDataAndAlias(s, &wfd, lpFindData);
    Log("FindNextFileA(疑似ハンドル %p) -> %s", hFindFile, A2U(lpFindData->cFileName));
    return TRUE;
}

static BOOL WINAPI Hook_FindClose(HANDLE hFindFile)
{
    FindSession *s = FindSessionByHandle(hFindFile);
    if (!s) {
        return Real_FindClose(hFindFile);
    }
    BOOL ok = FindClose(s->hFindW);
    FreeSession(s);
    return ok;
}

/* ANSIパスを実際に開くべきWideパスに変換 (CreateFileA等の共通処理) */
static void ResolvePathForOpen(const char *ansiPath, WCHAR *outW, size_t outWSize)
{
    if (ResolveFullPathAliases(ansiPath, outW, outWSize)) {
        Log("  エイリアス解決: \"%s\" -> \"%s\"", A2U(ansiPath), W2U(outW));
        return;
    }
    MultiByteToWideChar(CP_ACP, 0, ansiPath, -1, outW, (int)outWSize);
    Log("  通常変換: \"%s\" -> \"%s\"", A2U(ansiPath), W2U(outW));
}

static HANDLE WINAPI Hook_CreateFileA(LPCSTR lpFileName, DWORD dwDesiredAccess,
                                       DWORD dwShareMode, LPSECURITY_ATTRIBUTES lpSA,
                                       DWORD dwCreationDisposition, DWORD dwFlags,
                                       HANDLE hTemplate)
{
    Log("CreateFileA(\"%s\")", A2U(lpFileName));

    if (!lpFileName)
        return Real_CreateFileA(lpFileName, dwDesiredAccess, dwShareMode, lpSA,
                                 dwCreationDisposition, dwFlags, hTemplate);

    WCHAR wpath[MAX_PATH];
    ResolvePathForOpen(lpFileName, wpath, MAX_PATH);

    HANDLE h = CreateFileW(wpath, dwDesiredAccess, dwShareMode, lpSA,
                            dwCreationDisposition, dwFlags, hTemplate);
    if (h == INVALID_HANDLE_VALUE) {
        Log("  -> 失敗 (エラーコード %lu)", GetLastError());
    } else {
        Log("  -> 成功 (ハンドル %p)", h);
        TrackSueHandleIfApplicable(lpFileName, wpath, h);
    }
    return h;
}

static DWORD WINAPI Hook_GetFileAttributesA(LPCSTR lpFileName)
{
    if (!lpFileName)
        return Real_GetFileAttributesA(lpFileName);

    Log("GetFileAttributesA(\"%s\")", A2U(lpFileName));

    WCHAR wpath[MAX_PATH];
    ResolvePathForOpen(lpFileName, wpath, MAX_PATH);
    DWORD result = GetFileAttributesW(wpath);

    if (result == INVALID_FILE_ATTRIBUTES) {
        Log("  -> 失敗 (INVALID_FILE_ATTRIBUTES, エラーコード %lu)", GetLastError());
    } else {
        Log("  -> 成功 (属性 = 0x%08lX)", result);
    }
    return result;
}

/* CloseHandle/_lclose:
 * .sueハンドルとして追跡中なら、閉じる直前に書庫の最終更新日時に合わせる
 * _lcloseはkernel32内部でCloseHandle を直接呼ぶため、IAT経由では捕まらず
 * 両方を個別にフックする必要がある */

static BOOL WINAPI Hook_CloseHandle(HANDLE hObject)
{
    ApplySueTimestampIfTracked(hObject);
    return Real_CloseHandle(hObject);
}

static HFILE WINAPI Hook__lclose(HFILE hFile)
{
    ApplySueTimestampIfTracked((HANDLE)(INT_PTR)hFile);
    return Real__lclose(hFile);
}

/* _lopen/_lcreat/OpenFile:
 * CreateFileA より古いレガシーAPIで、Unicode版が存在しない
 * 仮想ANSI名を含むパスの場合だけ CreateFileW で同等のハンドルを作り、
 * それ以外は元の動作を変えないよう元の API (Real_Xxx) に任せる */

static HFILE WINAPI Hook__lopen(LPCSTR lpPathName, int iReadWrite)
{
    Log("_lopen(\"%s\", iReadWrite=0x%X)", A2U(lpPathName ? lpPathName : "(null)"), iReadWrite);

    WCHAR wpath[MAX_PATH];
    if (!lpPathName || !ResolveFullPathAliases(lpPathName, wpath, MAX_PATH)) {
        HFILE hf = Real__lopen(lpPathName, iReadWrite);
        Log("  -> %s (ハンドル %p)", hf == HFILE_ERROR ? "失敗" : "成功", (void*)(INT_PTR)hf);
        return hf;
    }
    Log("  エイリアス解決: \"%s\" -> \"%s\"", A2U(lpPathName), W2U(wpath));

    DWORD access;
    switch (iReadWrite & 0x3) {
        case OF_WRITE:     access = GENERIC_WRITE; break;
        case OF_READWRITE: access = GENERIC_READ | GENERIC_WRITE; break;
        default:           access = GENERIC_READ; break; /* OF_READ */
    }

    HANDLE h = CreateFileW(wpath, access, FILE_SHARE_READ | FILE_SHARE_WRITE,
                            NULL, OPEN_EXISTING, 0, NULL);
    if (h == INVALID_HANDLE_VALUE) {
        Log("  -> 失敗 (エラーコード %lu)", GetLastError());
        return (HFILE)HFILE_ERROR;
    }
    Log("  -> 成功 (ハンドル %p)", h);
    return (HFILE)(INT_PTR)h;
}

static HFILE WINAPI Hook__lcreat(LPCSTR lpPathName, int iAttribute)
{
    Log("_lcreat(\"%s\")", A2U(lpPathName ? lpPathName : "(null)"));

    WCHAR wpath[MAX_PATH];
    if (!lpPathName || !ResolveFullPathAliases(lpPathName, wpath, MAX_PATH)) {
        HFILE hf = Real__lcreat(lpPathName, iAttribute);
        Log("  -> %s (ハンドル %p)", hf == HFILE_ERROR ? "失敗" : "成功", (void*)(INT_PTR)hf);
        if (lpPathName && hf != HFILE_ERROR) {
            MultiByteToWideChar(CP_ACP, 0, lpPathName, -1, wpath, MAX_PATH);
            TrackSueHandleIfApplicable(lpPathName, wpath, (HANDLE)(INT_PTR)hf);
        }
        return hf;
    }
    Log("  エイリアス解決: \"%s\" -> \"%s\"", A2U(lpPathName), W2U(wpath));

    HANDLE h = CreateFileW(wpath, GENERIC_READ | GENERIC_WRITE,
                            FILE_SHARE_READ | FILE_SHARE_WRITE, NULL,
                            CREATE_ALWAYS, iAttribute ? (DWORD)iAttribute : FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) {
        Log("  -> 失敗 (エラーコード %lu)", GetLastError());
        return (HFILE)HFILE_ERROR;
    }
    Log("  -> 成功 (ハンドル %p)", h);
    TrackSueHandleIfApplicable(lpPathName, wpath, h);
    return (HFILE)(INT_PTR)h;
}

static HFILE WINAPI Hook_OpenFile(LPCSTR lpFileName, LPOFSTRUCT lpReOpenBuff, UINT uStyle)
{
    Log("OpenFile(\"%s\", uStyle=0x%X)", A2U(lpFileName ? lpFileName : "(null)"), uStyle);

    WCHAR wpath[MAX_PATH];
    if (!lpFileName || !ResolveFullPathAliases(lpFileName, wpath, MAX_PATH)) {
        HFILE hf = Real_OpenFile(lpFileName, lpReOpenBuff, uStyle);
        Log("  -> %s (戻り値 %p)", hf == HFILE_ERROR ? "失敗" : "成功", (void*)(INT_PTR)hf);
        if (lpFileName && hf != HFILE_ERROR && !(uStyle & (OF_EXIST | OF_PARSE | OF_DELETE))) {
            MultiByteToWideChar(CP_ACP, 0, lpFileName, -1, wpath, MAX_PATH);
            TrackSueHandleIfApplicable(lpFileName, wpath, (HANDLE)(INT_PTR)hf);
        }
        return hf;
    }
    Log("  エイリアス解決: \"%s\" -> \"%s\"", A2U(lpFileName), W2U(wpath));

    if (lpReOpenBuff) {
        memset(lpReOpenBuff, 0, sizeof(OFSTRUCT));
        lpReOpenBuff->cBytes = sizeof(OFSTRUCT);
        strncpy(lpReOpenBuff->szPathName, lpFileName, OFS_MAXPATHNAME - 1);
    }

    if (uStyle & OF_PARSE) return 0;

    if (uStyle & OF_DELETE) {
        if (!DeleteFileW(wpath)) {
            if (lpReOpenBuff) lpReOpenBuff->nErrCode = (WORD)GetLastError();
            Log("  -> OF_DELETE: 失敗 (エラーコード %lu)", GetLastError());
            return (HFILE)HFILE_ERROR;
        }
        Log("  -> OF_DELETE: 成功");
        return 1;
    }

    if (uStyle & OF_EXIST) {
        DWORD attrs = GetFileAttributesW(wpath);
        if (attrs == INVALID_FILE_ATTRIBUTES) {
            if (lpReOpenBuff) lpReOpenBuff->nErrCode = (WORD)GetLastError();
            Log("  -> OF_EXIST: 失敗 (エラーコード %lu)", GetLastError());
            return (HFILE)HFILE_ERROR;
        }
        Log("  -> OF_EXIST: 成功");
        return 1;
    }

    DWORD access;
    switch (uStyle & 0x3) {
        case OF_WRITE:     access = GENERIC_WRITE; break;
        case OF_READWRITE: access = GENERIC_READ | GENERIC_WRITE; break;
        default:           access = GENERIC_READ; break;
    }
    DWORD disposition = (uStyle & OF_CREATE) ? CREATE_ALWAYS : OPEN_EXISTING;

    HANDLE h = CreateFileW(wpath, access, FILE_SHARE_READ | FILE_SHARE_WRITE,
                            NULL, disposition, 0, NULL);
    if (h == INVALID_HANDLE_VALUE) {
        if (lpReOpenBuff) lpReOpenBuff->nErrCode = (WORD)GetLastError();
        Log("  -> 失敗 (エラーコード %lu)", GetLastError());
        return (HFILE)HFILE_ERROR;
    }
    Log("  -> 成功 (ハンドル %p)", h);
    TrackSueHandleIfApplicable(lpFileName, wpath, h);
    return (HFILE)(INT_PTR)h;
}

/* GetShortPathNameA は実在するパスでないと失敗するため、
 * 仮想ANSI名をそのまま渡すと失敗する。仮想ANSI名なら実Unicodeパスで
 * GetShortPathNameWを試し、駄目なら仮想ANSI名を無変換で返す */
static DWORD WINAPI Hook_GetShortPathNameA(LPCSTR lpszLongPath, LPSTR lpszShortPath, DWORD cchBuffer)
{
    Log("GetShortPathNameA(\"%s\")", A2U(lpszLongPath ? lpszLongPath : "(null)"));

    if (!lpszLongPath) return Real_GetShortPathNameA(lpszLongPath, lpszShortPath, cchBuffer);

    WCHAR realPath[MAX_PATH];
    if (ResolveFullPathAliases(lpszLongPath, realPath, MAX_PATH)) {
        WCHAR shortW[MAX_PATH];
        DWORD r = GetShortPathNameW(realPath, shortW, MAX_PATH);
        char shortAnsi[MAX_PATH];
        if (r > 0 && r < MAX_PATH && IsAnsiSafe(shortW, shortAnsi, sizeof(shortAnsi))) {
            DWORD len = (DWORD)strlen(shortAnsi);
            if (len < cchBuffer) {
                strcpy(lpszShortPath, shortAnsi);
                Log("  -> エイリアス経由で実在する短縮名を取得: \"%s\"", A2U(shortAnsi));
                return len;
            }
            return len + 1;
        }
        DWORD len = (DWORD)strlen(lpszLongPath);
        if (len < cchBuffer) {
            strcpy(lpszShortPath, lpszLongPath);
            Log("  -> 実在する短縮名なし。エイリアスをそのまま返す: \"%s\"", A2U(lpszLongPath));
            return len;
        }
        return len + 1;
    }

    DWORD result = Real_GetShortPathNameA(lpszLongPath, lpszShortPath, cchBuffer);
    if (result == 0) {
        Log("  -> 失敗 (エラーコード %lu)", GetLastError());
    } else {
        Log("  -> \"%s\"", A2U(lpszShortPath));
    }
    return result;
}

/* GetCommandLineA のフック
 * 関連付けやドラッグ&ドロップ経由で画像ファイルを開く場合、SusieはこのAPI経由でファイル名を得る
 * GetCommandLineW() から実パス名を取得し、Unicodeのファイル名/フォルダ名だけエイリアス化する */
static void BuildAliasedAnsiPath(const WCHAR *fullPathW, char *outAnsi, size_t outSize)
{
    WCHAR work[2048];
    wcsncpy(work, fullPathW, 2047);
    work[2047] = 0;

    /* エイリアス化が必要な相対パスは、カレントディレクトリが変わっても
       解決できるようフルパスにしておく */
    char tmpAnsi[2048];
    if (!IsAnsiSafe(work, tmpAnsi, sizeof(tmpAnsi))) {
        WCHAR fullW[2048];
        DWORD n = GetFullPathNameW(work, 2048, fullW, NULL);
        if (n > 0 && n < 2048) wcscpy(work, fullW);
    }

    WCHAR realAccum[MAX_PATH];
    realAccum[0] = 0;
    char ansiAccum[2048];
    ansiAccum[0] = 0;
    BOOL first = TRUE;

    WCHAR *p = work;

    /* UNCパス ("\\server\share\...") の場合、先頭の "\\server\share" を
       1つの根っことして特別扱いする (ResolveFullPathAliases と同じ理由) */
    if (work[0] == L'\\' && work[1] == L'\\') {
        WCHAR *afterSlashes = work + 2;
        WCHAR *serverEnd = wcschr(afterSlashes, L'\\');
        if (serverEnd) {
            WCHAR *shareStart = serverEnd + 1;
            WCHAR *shareEnd = wcschr(shareStart, L'\\');
            size_t rootLen = shareEnd ? (size_t)(shareEnd - work) : wcslen(work);

            size_t copyLen = rootLen < MAX_PATH - 1 ? rootLen : MAX_PATH - 1;
            wcsncpy(realAccum, work, copyLen);
            realAccum[copyLen] = 0;

            char rootAnsi[MAX_PATH];
            WideCharToMultiByte(CP_ACP, 0, realAccum, -1, rootAnsi, sizeof(rootAnsi), NULL, NULL);
            strncpy(ansiAccum, rootAnsi, sizeof(ansiAccum) - 1);

            first = FALSE;
            p = shareEnd ? shareEnd + 1 : (work + wcslen(work));
        }
    }

    WCHAR *saveptr = NULL;
    WCHAR *tok = wcstok_s(p, L"\\", &saveptr);
    while (tok) {
        WCHAR *next = wcstok_s(NULL, L"\\", &saveptr);
        BOOL isLast = (next == NULL);

        /* エイリアス登録用に実パスの累積を更新 */
        if (first) {
            wcsncpy(realAccum, tok, MAX_PATH - 1);
            realAccum[MAX_PATH - 1] = 0;
        } else {
            size_t len = wcslen(realAccum);
            if (len + 1 + wcslen(tok) < MAX_PATH) {
                wcscat(realAccum, L"\\");
                wcscat(realAccum, tok);
            }
        }

        char compAnsi[MAX_PATH];
        char aliasBuf[32];
        const char *toAppend;
        if (IsAnsiSafe(tok, compAnsi, sizeof(compAnsi))) {
            toAppend = compAnsi;
        } else {
            /* 実在すれば属性で、無ければ「最後の階層以外はディレクトリ」とみなす */
            DWORD attrs = GetFileAttributesW(realAccum);
            BOOL isDir = (attrs != INVALID_FILE_ATTRIBUTES)
                         ? (attrs & FILE_ATTRIBUTE_DIRECTORY) != 0 : !isLast;
            RegisterAliasEx(realAccum, tok, isDir, aliasBuf, sizeof(aliasBuf));
            toAppend = aliasBuf;
        }

        if (first) {
            strncat(ansiAccum, toAppend, sizeof(ansiAccum) - strlen(ansiAccum) - 1);
        } else {
            strncat(ansiAccum, "\\", sizeof(ansiAccum) - strlen(ansiAccum) - 1);
            strncat(ansiAccum, toAppend, sizeof(ansiAccum) - strlen(ansiAccum) - 1);
        }

        first = FALSE;
        tok = next;
    }

    strncpy(outAnsi, ansiAccum, outSize - 1);
    outAnsi[outSize - 1] = 0;
}

static LPSTR WINAPI Hook_GetCommandLineA(void)
{
    static char s_fixedCmdLineA[8192];

    LPWSTR cmdW = GetCommandLineW();
    int argc = 0;
    LPWSTR *argv = CommandLineToArgvW(cmdW, &argc);
    if (!argv) {
        return Real_GetCommandLineA();
    }

    char rebuilt[8192];
    rebuilt[0] = 0;

    for (int i = 0; i < argc; i++) {
        char argAnsi[MAX_PATH];
        /* 必要な階層だけエイリアス化してANSIセーフなパスに変換 */
        BuildAliasedAnsiPath(argv[i], argAnsi, sizeof(argAnsi));

        if (strlen(rebuilt) + strlen(argAnsi) + 4 < sizeof(rebuilt)) {
            strcat(rebuilt, "\"");
            strcat(rebuilt, argAnsi);
            strcat(rebuilt, "\" ");
        }
    }
    LocalFree(argv);

    strncpy(s_fixedCmdLineA, rebuilt, sizeof(s_fixedCmdLineA) - 1);
    s_fixedCmdLineA[sizeof(s_fixedCmdLineA) - 1] = 0;

    Log("GetCommandLineA() -> \"%s\"", A2U(s_fixedCmdLineA));
    return s_fixedCmdLineA;
}

static BOOL WINAPI Hook_SetCurrentDirectoryA(LPCSTR lpPathName)
{
    Log("SetCurrentDirectoryA(\"%s\")", A2U(lpPathName ? lpPathName : "(null)"));

    if (!lpPathName) return Real_SetCurrentDirectoryA(lpPathName);

    WCHAR wpath[MAX_PATH];
    BOOL r;
    if (ResolveFullPathAliases(lpPathName, wpath, MAX_PATH)) {
        Log("  エイリアス解決: \"%s\" -> \"%s\"", A2U(lpPathName), W2U(wpath));
        r = SetCurrentDirectoryW(wpath);
    } else {
        r = Real_SetCurrentDirectoryA(lpPathName);
    }

    Log("  -> %s", r ? "成功" : "失敗");
    return r;
}

/* 実際のカレントディレクトリを、Unicode部分を仮想ANSI名にしたパスで返す
   (Susie自身は未使用だが、プラグインが使う可能性があるためフック)   */
static DWORD WINAPI Hook_GetCurrentDirectoryA(DWORD nBufferLength, LPSTR lpBuffer)
{
    WCHAR cwdW[MAX_PATH];
    DWORD n = GetCurrentDirectoryW(MAX_PATH, cwdW);
    if (n == 0 || n >= MAX_PATH) return Real_GetCurrentDirectoryA(nBufferLength, lpBuffer);

    char cwdA[MAX_PATH];
    BuildAliasedAnsiPath(cwdW, cwdA, sizeof(cwdA));
    DWORD len = (DWORD)strlen(cwdA);
    if (cwdW[n - 1] == L'\\' && len > 0 && cwdA[len - 1] != '\\' && len + 1 < sizeof(cwdA)) {
        cwdA[len++] = '\\';
        cwdA[len] = 0;
    }
    if (len < nBufferLength && lpBuffer) {
        strcpy(lpBuffer, cwdA);
        return len;
    }
    return len + 1;
}

/* SHGetFileInfoAのフック (シェルからファイル種別/アイコンを取得するAPI)
 * SHGetFileInfoW に委譲し、結果をANSIに変換して返す */
static DWORD_PTR WINAPI Hook_SHGetFileInfoA(LPCSTR pszPath, DWORD dwFileAttributes,
        SHFILEINFOA *psfi, UINT cbFileInfo, UINT uFlags)
{
    if (!pszPath || (uFlags & SHGFI_PIDL))
        return Real_SHGetFileInfoA(pszPath, dwFileAttributes, psfi, cbFileInfo, uFlags);

    Log("SHGetFileInfoA(\"%s\", uFlags=0x%X)", A2U(pszPath), uFlags);

    WCHAR realPath[MAX_PATH];
    if (ResolveFullPathAliases(pszPath, realPath, MAX_PATH)) {
        SHFILEINFOW sfiW;
        memset(&sfiW, 0, sizeof(sfiW));
        DWORD_PTR result = SHGetFileInfoW(realPath, dwFileAttributes, &sfiW, sizeof(sfiW), uFlags);

        if (psfi) {
            memset(psfi, 0, sizeof(*psfi));
            psfi->hIcon = sfiW.hIcon;
            psfi->iIcon = sfiW.iIcon;
            psfi->dwAttributes = sfiW.dwAttributes;
            WideCharToMultiByte(CP_ACP, 0, sfiW.szDisplayName, -1,
                    psfi->szDisplayName, sizeof(psfi->szDisplayName), NULL, NULL);
            WideCharToMultiByte(CP_ACP, 0, sfiW.szTypeName, -1,
                    psfi->szTypeName, sizeof(psfi->szTypeName), NULL, NULL);
        }
        Log("  -> エイリアス経由 (Wide版委譲): %s (種類=\"%s\")",
                result ? "成功" : "失敗", A2U(psfi ? psfi->szTypeName : ""));
        return result;
    }

    DWORD_PTR result = Real_SHGetFileInfoA(pszPath, dwFileAttributes, psfi, cbFileInfo, uFlags);
    Log("  -> %s (種類=\"%s\")", result ? "成功" : "失敗", A2U(psfi ? psfi->szTypeName : ""));
    return result;
}

/* 以下は可視化目的でログ出力するだけのフック群 (実処理は本物に委譲) */

static DWORD WINAPI Hook_GetFullPathNameA(LPCSTR lpFileName, DWORD nBufferLength,
                                           LPSTR lpBuffer, LPSTR *lpFilePart)
{
    Log("GetFullPathNameA(\"%s\")", A2U(lpFileName ? lpFileName : "(null)"));
    DWORD result = Real_GetFullPathNameA(lpFileName, nBufferLength, lpBuffer, lpFilePart);
    if (result == 0) {
        Log("  -> 失敗 (エラーコード %lu)", GetLastError());
    } else {
        Log("  -> \"%s\"", A2U(lpBuffer));
    }
    return result;
}

static BOOL WINAPI Hook_CreateDirectoryA(LPCSTR lpPathName, LPSECURITY_ATTRIBUTES lpSA)
{
    Log("CreateDirectoryA(\"%s\")", A2U(lpPathName ? lpPathName : "(null)"));
    BOOL r = Real_CreateDirectoryA(lpPathName, lpSA);
    Log("  -> %s (エラーコード %lu)", r ? "成功" : "失敗", GetLastError());
    return r;
}

static BOOL WINAPI Hook_MoveFileA(LPCSTR lpExisting, LPCSTR lpNew)
{
    Log("MoveFileA(\"%s\" -> \"%s\")", A2U(lpExisting ? lpExisting : "(null)"),
            A2U(lpNew ? lpNew : "(null)"));
    BOOL r = Real_MoveFileA(lpExisting, lpNew);
    Log("  -> %s (エラーコード %lu)", r ? "成功" : "失敗", GetLastError());
    return r;
}

static UINT WINAPI Hook_GetDriveTypeA(LPCSTR lpRootPathName)
{
    Log("GetDriveTypeA(\"%s\")", A2U(lpRootPathName ? lpRootPathName : "(null)"));
    UINT r = Real_GetDriveTypeA(lpRootPathName);
    Log("  -> %u", r);
    return r;
}

static BOOL WINAPI Hook_GetVolumeInformationA(LPCSTR lpRootPathName, LPSTR lpVolNameBuf,
        DWORD nVolNameSize, LPDWORD lpSerial, LPDWORD lpMaxComp, LPDWORD lpFlags,
        LPSTR lpFsNameBuf, DWORD nFsNameSize)
{
    Log("GetVolumeInformationA(\"%s\")", A2U(lpRootPathName ? lpRootPathName : "(null)"));
    BOOL r = Real_GetVolumeInformationA(lpRootPathName, lpVolNameBuf, nVolNameSize,
            lpSerial, lpMaxComp, lpFlags, lpFsNameBuf, nFsNameSize);
    Log("  -> %s (エラーコード %lu)", r ? "成功" : "失敗", GetLastError());
    return r;
}

static DWORD WINAPI Hook_GetPrivateProfileStringA(LPCSTR lpAppName, LPCSTR lpKeyName,
        LPCSTR lpDefault, LPSTR lpReturnedString, DWORD nSize, LPCSTR lpFileName)
{
    Log("GetPrivateProfileStringA(section=\"%s\", key=\"%s\", ini=\"%s\")",
            A2U(lpAppName ? lpAppName : "(null)"), A2U(lpKeyName ? lpKeyName : "(null)"),
            A2U(lpFileName ? lpFileName : "(null)"));
    DWORD r = Real_GetPrivateProfileStringA(lpAppName, lpKeyName, lpDefault,
            lpReturnedString, nSize, lpFileName);
    Log("  -> \"%s\"", A2U(lpReturnedString));
    return r;
}

static BOOL WINAPI Hook_WritePrivateProfileStringA(LPCSTR lpAppName, LPCSTR lpKeyName,
        LPCSTR lpString, LPCSTR lpFileName)
{
    Log("WritePrivateProfileStringA(section=\"%s\", key=\"%s\", value=\"%s\", ini=\"%s\")",
            A2U(lpAppName ? lpAppName : "(null)"), A2U(lpKeyName ? lpKeyName : "(null)"),
            A2U(lpString ? lpString : "(null)"), A2U(lpFileName ? lpFileName : "(null)"));
    BOOL r = Real_WritePrivateProfileStringA(lpAppName, lpKeyName, lpString, lpFileName);
    Log("  -> %s", r ? "成功" : "失敗");
    return r;
}

static UINT WINAPI Hook_GetPrivateProfileIntA(LPCSTR lpAppName, LPCSTR lpKeyName,
        INT nDefault, LPCSTR lpFileName)
{
    Log("GetPrivateProfileIntA(section=\"%s\", key=\"%s\", ini=\"%s\")",
            A2U(lpAppName ? lpAppName : "(null)"), A2U(lpKeyName ? lpKeyName : "(null)"),
            A2U(lpFileName ? lpFileName : "(null)"));
    UINT r = Real_GetPrivateProfileIntA(lpAppName, lpKeyName, nDefault, lpFileName);
    Log("  -> %u", r);
    return r;
}

/* ------------------------------------------------------------------ */
/*  Appendix : .sueカタログファイルの最終更新日時を書庫に合わせる機能 */
/* ------------------------------------------------------------------ */

/* .sueファイル名から、対応する書庫の本物のパスを見つける
   対応表 -> ディレクトリ再スキャンの順に探す */
static BOOL FindArchiveRealPathForSue(const char *sueComponent, const WCHAR *containingDirW,
                                       WCHAR *outArchiveReal, size_t outSize)
{
    size_t len = strlen(sueComponent);
    if (len <= 4 || _stricmp(sueComponent + len - 4, ".sue") != 0) return FALSE;

    /* stem: 末尾の ".sue" だけを取り除いた全体 */
    size_t stemLen = len - 4;
    if (stemLen == 0 || stemLen >= 200) return FALSE;
    char stem[200];
    memcpy(stem, sueComponent, stemLen);
    stem[stemLen] = 0;

    /* 1. まず変換テーブルを探す */
    if (g_sharedAliases) {
        WaitForSingleObject(g_hAliasMutex, INFINITE);
        LONG cnt = g_sharedAliases->count;
        for (LONG i = 0; i < cnt; i++) {
            if (!g_sharedAliases->entries[i].used) continue;
            const char *a = g_sharedAliases->entries[i].alias;
            if (_strnicmp(a, stem, stemLen) == 0 &&
                (a[stemLen] == '.' || a[stemLen] == '\0')) {
                wcsncpy(outArchiveReal, g_sharedAliases->entries[i].realPath, outSize - 1);
                outArchiveReal[outSize - 1] = 0;
                ReleaseMutex(g_hAliasMutex);
                return TRUE;
            }
        }
        ReleaseMutex(g_hAliasMutex);
    }

    /* 2. 同じディレクトリを再スキャンして探す
          (a) エイリアス化されていた書庫: ハッシュ部分が一致するか
          (b) ANSIで安全な書庫: 実ファイル名全体が一致するか        */
    if (containingDirW && containingDirW[0]) {
        WCHAR stemW[200];
        MultiByteToWideChar(CP_ACP, 0, stem, -1, stemW, 200);

        WCHAR pattern[MAX_PATH];
        _snwprintf(pattern, MAX_PATH, L"%s\\*", containingDirW);
        pattern[MAX_PATH - 1] = 0;

        WIN32_FIND_DATAW wfd;
        HANDLE h = FindFirstFileW(pattern, &wfd);
        if (h != INVALID_HANDLE_VALUE) {
            do {
                if (wfd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;

                /* 作成済みの.sue自身を候補として誤って拾わないよう除外 */
                size_t wlen = wcslen(wfd.cFileName);
                if (wlen > 4 && _wcsicmp(wfd.cFileName + wlen - 4, L".sue") == 0) continue;

                WCHAR entryFull[MAX_PATH];
                _snwprintf(entryFull, MAX_PATH, L"%s\\%s", containingDirW, wfd.cFileName);
                entryFull[MAX_PATH - 1] = 0;

                /* (b) 実ファイル名(拡張子を除いた部分)が stem と
                       そのまま一致するか (実ファイル名自体に複数の
                       ピリオドが含まれていても、最後のピリオドだけを
                       拡張子境界とみなす)                            */
                WCHAR nameOnly[MAX_PATH];
                wcsncpy(nameOnly, wfd.cFileName, MAX_PATH - 1);
                nameOnly[MAX_PATH - 1] = 0;
                WCHAR *wdot = wcsrchr(nameOnly, L'.');
                if (wdot) *wdot = 0;

                if (_wcsicmp(nameOnly, stemW) == 0) {
                    wcsncpy(outArchiveReal, entryFull, outSize - 1);
                    outArchiveReal[outSize - 1] = 0;
                    FindClose(h);
                    return TRUE;
                }

                /* (a) エイリアス化されていた場合、ハッシュ部分が一致するか */
                char candidateAlias[32];
                ComputeDeterministicAlias(entryFull, wfd.cFileName, FALSE,
                                           candidateAlias, sizeof(candidateAlias));

                const char *cdot = strchr(candidateAlias, '.');
                size_t clen = cdot ? (size_t)(cdot - candidateAlias) : strlen(candidateAlias);
                if (clen == stemLen && _strnicmp(candidateAlias, stem, stemLen) == 0) {
                    wcsncpy(outArchiveReal, entryFull, outSize - 1);
                    outArchiveReal[outSize - 1] = 0;
                    FindClose(h);
                    return TRUE;
                }
            } while (FindNextFileW(h, &wfd));
            FindClose(h);
        }
    }

    return FALSE;
}

/* .sueのハンドルと対応する書庫の実パスを、閉じるときの上書き用に一時的に覚えておく */
#define MAX_SUE_HANDLES 64
typedef struct {
    HANDLE handle;
    WCHAR  archiveRealPath[MAX_PATH];
    BOOL   used;
} SueHandleEntry;

static SueHandleEntry     g_sueHandles[MAX_SUE_HANDLES];
static CRITICAL_SECTION   g_sueHandleLock;

static void RegisterSueHandle(HANDLE h, const WCHAR *archiveRealPath)
{
    EnterCriticalSection(&g_sueHandleLock);
    for (int i = 0; i < MAX_SUE_HANDLES; i++) {
        if (!g_sueHandles[i].used) {
            g_sueHandles[i].handle = h;
            wcsncpy(g_sueHandles[i].archiveRealPath, archiveRealPath, MAX_PATH - 1);
            g_sueHandles[i].archiveRealPath[MAX_PATH - 1] = 0;
            g_sueHandles[i].used = TRUE;
            break;
        }
    }
    LeaveCriticalSection(&g_sueHandleLock);
}

/* 見つかれば表から取り除いて TRUE を返す (1回使ったら消費) */
static BOOL PopSueHandle(HANDLE h, WCHAR *outArchiveRealPath, size_t outSize)
{
    BOOL found = FALSE;
    EnterCriticalSection(&g_sueHandleLock);
    for (int i = 0; i < MAX_SUE_HANDLES; i++) {
        if (g_sueHandles[i].used && g_sueHandles[i].handle == h) {
            wcsncpy(outArchiveRealPath, g_sueHandles[i].archiveRealPath, outSize - 1);
            outArchiveRealPath[outSize - 1] = 0;
            g_sueHandles[i].used = FALSE;
            found = TRUE;
            break;
        }
    }
    LeaveCriticalSection(&g_sueHandleLock);
    return found;
}

/* .sueファイルを開いた直後に呼ぶ。対応する書庫が見つかれば
   ハンドルと一緒に記録しておく                                    */
static void TrackSueHandleIfApplicable(const char *ansiFileName, const WCHAR *resolvedSuePathW, HANDLE h)
{
    if (!g_settingSyncCatalogFileTimestamp) return;
    if (h == INVALID_HANDLE_VALUE || !h) return;

    const char *base = FindLastBackslashA(ansiFileName);
    base = base ? base + 1 : ansiFileName;

    size_t len = strlen(base);
    if (len <= 4 || _stricmp(base + len - 4, ".sue") != 0) return;

    WCHAR dirW[MAX_PATH];
    wcsncpy(dirW, resolvedSuePathW, MAX_PATH - 1);
    dirW[MAX_PATH - 1] = 0;
    WCHAR *lastBs = wcsrchr(dirW, L'\\');
    if (lastBs) {
        *lastBs = 0;
    } else {
        /* ディレクトリ部分が無い(=相対パスで渡された)場合、プロセスの
           現在の作業ディレクトリを使う。Susieが「カレントディレクトリを
           書庫のあるフォルダに変更してから、ファイル名だけ相対パスで
           .sueを作成する」という挙動をすることがあり、これに対応する
           ため(非Unicode書庫でSyncCatalogFileTimestampが効かない不具合の原因) */
        if (!GetCurrentDirectoryW(MAX_PATH, dirW)) dirW[0] = 0;
    }

    WCHAR archiveReal[MAX_PATH];
    if (FindArchiveRealPathForSue(base, dirW, archiveReal, MAX_PATH)) {
        RegisterSueHandle(h, archiveReal);
        Log("  .sueハンドルを追跡開始 (対応する書庫: %s)", W2U(archiveReal));
    }
}

/* ハンドルを閉じる直前に呼ぶ。追跡対象なら書庫の最終更新日時を
   .sue側に上書きする                                              */
static void ApplySueTimestampIfTracked(HANDLE h)
{
    WCHAR archiveReal[MAX_PATH];
    if (!PopSueHandle(h, archiveReal, MAX_PATH)) return;

    WIN32_FILE_ATTRIBUTE_DATA fad;
    if (GetFileAttributesExW(archiveReal, GetFileExInfoStandard, &fad)) {
        if (SetFileTime(h, NULL, NULL, &fad.ftLastWriteTime)) {
            Log("  .sueの最終更新日時を書庫に合わせました: %s", W2U(archiveReal));
        } else {
            Log("  .sueの最終更新日時の設定に失敗 (エラーコード %lu)", GetLastError());
        }
    } else {
        Log("  書庫の属性取得に失敗、最終更新日時の同期をスキップ (エラーコード %lu)", GetLastError());
    }
}

/* ------------------------------------------------------------------ */
/*  IAT パッチ処理                                                    */
/* ------------------------------------------------------------------ */

typedef struct {
    const char *dllName;     /* DLL名 (GetModuleHandleA 用) */
    const char *funcName;    /* API名 (GetProcAddress 用) */
    void       *realAddr;    /* GetProcAddressで取得したAPIの実アドレス。IAT書き換え時に使用 */
    void       *hookFunc;    /* フック先アドレス (本DLL内) */
    void      **realFuncOut; /* 実アドレスの書き込み先 (Real_XxxA等へのポインタ) */
} PatchTarget;

/* PEヘッダを辿ってIMPORTディレクトリを取得 */
static PIMAGE_IMPORT_DESCRIPTOR GetImportDescriptor(HMODULE hModule)
{
    BYTE *base = (BYTE *)hModule;
    PIMAGE_DOS_HEADER pDos = (PIMAGE_DOS_HEADER)base;
    if (pDos->e_magic != IMAGE_DOS_SIGNATURE) return NULL;

    PIMAGE_NT_HEADERS pNt = (PIMAGE_NT_HEADERS)(base + pDos->e_lfanew);
    if (pNt->Signature != IMAGE_NT_SIGNATURE) return NULL;

    IMAGE_DATA_DIRECTORY dir =
        pNt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (dir.VirtualAddress == 0 || dir.Size == 0) return NULL;

    return (PIMAGE_IMPORT_DESCRIPTOR)(base + dir.VirtualAddress);
}

/* フック対象 API
   dllName は各APIが実際にエクスポートされているDLL名
   realAddrは初期値 NULL。初回呼び出し時に GetProcAddress でアドレス解決する */
static PatchTarget g_targets[] = {
    { "KERNEL32.dll", "FindFirstFileA",     NULL, (void*)Hook_FindFirstFileA,     (void**)&Real_FindFirstFileA },
    { "KERNEL32.dll", "FindNextFileA",      NULL, (void*)Hook_FindNextFileA,      (void**)&Real_FindNextFileA },
    { "KERNEL32.dll", "FindClose",          NULL, (void*)Hook_FindClose,          (void**)&Real_FindClose },
    { "KERNEL32.dll", "CreateFileA",        NULL, (void*)Hook_CreateFileA,        (void**)&Real_CreateFileA },
    { "KERNEL32.dll", "GetFileAttributesA", NULL, (void*)Hook_GetFileAttributesA, (void**)&Real_GetFileAttributesA },
    { "KERNEL32.dll", "GetFullPathNameA",   NULL, (void*)Hook_GetFullPathNameA,   (void**)&Real_GetFullPathNameA },
    { "KERNEL32.dll", "_lopen",             NULL, (void*)Hook__lopen,             (void**)&Real__lopen },
    { "KERNEL32.dll", "_lclose",            NULL, (void*)Hook__lclose,            (void**)&Real__lclose },
    { "KERNEL32.dll", "CloseHandle",        NULL, (void*)Hook_CloseHandle,        (void**)&Real_CloseHandle },
    { "KERNEL32.dll", "_lcreat",            NULL, (void*)Hook__lcreat,            (void**)&Real__lcreat },
    { "KERNEL32.dll", "OpenFile",           NULL, (void*)Hook_OpenFile,           (void**)&Real_OpenFile },
    { "KERNEL32.dll", "GetShortPathNameA",  NULL, (void*)Hook_GetShortPathNameA,  (void**)&Real_GetShortPathNameA },
    { "KERNEL32.dll", "SetCurrentDirectoryA",       NULL, (void*)Hook_SetCurrentDirectoryA,       (void**)&Real_SetCurrentDirectoryA },
    { "KERNEL32.dll", "GetCurrentDirectoryA",       NULL, (void*)Hook_GetCurrentDirectoryA,       (void**)&Real_GetCurrentDirectoryA },
    { "KERNEL32.dll", "CreateDirectoryA",           NULL, (void*)Hook_CreateDirectoryA,           (void**)&Real_CreateDirectoryA },
    { "KERNEL32.dll", "MoveFileA",                  NULL, (void*)Hook_MoveFileA,                  (void**)&Real_MoveFileA },
    { "KERNEL32.dll", "GetDriveTypeA",              NULL, (void*)Hook_GetDriveTypeA,              (void**)&Real_GetDriveTypeA },
    { "KERNEL32.dll", "GetVolumeInformationA",      NULL, (void*)Hook_GetVolumeInformationA,      (void**)&Real_GetVolumeInformationA },
    { "KERNEL32.dll", "GetPrivateProfileStringA",   NULL, (void*)Hook_GetPrivateProfileStringA,   (void**)&Real_GetPrivateProfileStringA },
    { "KERNEL32.dll", "WritePrivateProfileStringA", NULL, (void*)Hook_WritePrivateProfileStringA, (void**)&Real_WritePrivateProfileStringA },
    { "KERNEL32.dll", "GetPrivateProfileIntA",      NULL, (void*)Hook_GetPrivateProfileIntA,      (void**)&Real_GetPrivateProfileIntA },
    { "SHELL32.dll",  "SHGetFileInfoA",             NULL, (void*)Hook_SHGetFileInfoA,             (void**)&Real_SHGetFileInfoA },
    { "KERNEL32.dll", "LoadLibraryA",       NULL, (void*)Hook_LoadLibraryA,       (void**)&Real_LoadLibraryA },
    { "KERNEL32.dll", "LoadLibraryExA",     NULL, (void*)Hook_LoadLibraryExA,     (void**)&Real_LoadLibraryExA },
    { "KERNEL32.dll", "GetCommandLineA",    NULL, (void*)Hook_GetCommandLineA,    (void**)&Real_GetCommandLineA },
};
static const int g_nTargets = sizeof(g_targets) / sizeof(g_targets[0]);
static BOOL g_targetAddrsResolved = FALSE;

static void ResolveTargetAddrsOnce(void)
{
    if (g_targetAddrsResolved) return;
    for (int i = 0; i < g_nTargets; i++) {
        HMODULE hMod = GetModuleHandleA(g_targets[i].dllName);
        if (!hMod) {
            hMod = LoadLibraryA(g_targets[i].dllName); /* 未ロードならロード */
        }
        g_targets[i].realAddr = hMod ? (void *)GetProcAddress(hMod, g_targets[i].funcName) : NULL;
        Log("ターゲット解決: %s!%s = %p", g_targets[i].dllName, g_targets[i].funcName, g_targets[i].realAddr);
    }
    g_targetAddrsResolved = TRUE;
}

/* IATを書き換え */
static void PatchIat(HMODULE hModule)
{
    ResolveTargetAddrsOnce();

    PIMAGE_IMPORT_DESCRIPTOR pImportDesc = GetImportDescriptor(hModule);
    if (!pImportDesc) {
        Log("IMPORTディレクトリが見つかりません (module=%p)", (void*)hModule);
        return;
    }

    BYTE *base = (BYTE *)hModule;

    for (; pImportDesc->Name || pImportDesc->FirstThunk; pImportDesc++) {
        if (!pImportDesc->FirstThunk) continue;

        PIMAGE_THUNK_DATA pThunkIat =
            (PIMAGE_THUNK_DATA)(base + pImportDesc->FirstThunk);

        for (; pThunkIat->u1.Function; pThunkIat++) {
            void *currentAddr = (void *)(UINT_PTR)pThunkIat->u1.Function;

            for (int i = 0; i < g_nTargets; i++) {
                if (g_targets[i].realAddr && currentAddr == g_targets[i].realAddr) {

                    void **iatSlot = (void **)&pThunkIat->u1.Function;

                    *g_targets[i].realFuncOut = currentAddr;

                    DWORD oldProtect;
                    if (VirtualProtect(iatSlot, sizeof(void *), PAGE_READWRITE, &oldProtect)) {
                        *iatSlot = g_targets[i].hookFunc;
                        VirtualProtect(iatSlot, sizeof(void *), oldProtect, &oldProtect);
                        Log("フック設置: %s (module=%p)", g_targets[i].funcName, (void*)hModule);
                    } else {
                        Log("VirtualProtect失敗: %s (module=%p, err=%lu)",
                                g_targets[i].funcName, (void*)hModule, GetLastError());
                    }
                    break;
                }
            }
        }
    }
}

/* Susieプラグイン(.spi=実体はDLL)のロード直後に、プラグインのIATもパッチする
   (パッチしないとプラグイン内部の CreateFileA 等が素通しになる) */
static HMODULE WINAPI Hook_LoadLibraryA(LPCSTR lpLibFileName)
{
    HMODULE h = Real_LoadLibraryA(lpLibFileName);
    if (h) {
        Log("LoadLibraryA(\"%s\") -> パッチ", A2U(lpLibFileName));
        PatchIat(h);
    }
    return h;
}

static HMODULE WINAPI Hook_LoadLibraryExA(LPCSTR lpLibFileName, HANDLE hFile, DWORD dwFlags)
{
    HMODULE h = Real_LoadLibraryExA(lpLibFileName, hFile, dwFlags);
    /* リソース読み込み用のロード(DATAFILE等)は実行イメージではないのでパッチしない */
    if (h && !((UINT_PTR)h & 3)) {
        Log("LoadLibraryExA(\"%s\") -> パッチ", A2U(lpLibFileName));
        PatchIat(h);
    }
    return h;
}

/* ------------------------------------------------------------------ */
/*  DllMain                                                           */
/* ------------------------------------------------------------------ */

BOOL WINAPI DllMain(HINSTANCE hinstDLL, DWORD fdwReason, LPVOID lpvReserved)
{
    (void)lpvReserved;
    switch (fdwReason) {
    case DLL_PROCESS_ATTACH:
        DisableThreadLibraryCalls(hinstDLL);
        g_hSelfModule = hinstDLL;
        InitializeCriticalSection(&g_sessLock);
        InitializeCriticalSection(&g_sueHandleLock);
        memset(g_sueHandles, 0, sizeof(g_sueHandles));
        memset(g_sessions, 0, sizeof(g_sessions));

        LoadSettings(); /* ログを使う前に読み込む (EnableLog の判定のため) */

        Log("==== susie_unicode_hook v" SUH_VERSION_STR " ロード完了。IATパッチ開始 ====");
        Log("設定: EnableLog=%d, SyncCatalogFileTimestamp=%d",
                g_settingEnableLog, g_settingSyncCatalogFileTimestamp);
        InitSharedAliasTable();
        PatchIat(GetModuleHandle(NULL)); /* Susie.exe 自身のIATを書き換える */
        Log("==== IATパッチ完了 ====");
        break;

    case DLL_PROCESS_DETACH:
        if (g_sharedAliases) UnmapViewOfFile(g_sharedAliases);
        if (g_hAliasMapping) CloseHandle(g_hAliasMapping);
        if (g_hAliasMutex) CloseHandle(g_hAliasMutex);
        DeleteCriticalSection(&g_sessLock);
        DeleteCriticalSection(&g_sueHandleLock);
        break;
    }
    return TRUE;
}
