/*
 * susie_unicode_launcher.c (SusieUnicode.exe)
 *
 * Susie.exeをCREATE_SUSPENDEDで起動し、susie_unicode_hook.dllを注入して
 * IATパッチ完了後にメインスレッドを再開するランチャー
 *
 * 使い方: SusieUnicode.exe [Susieに渡す引数... (ファイルパス等)]
 */

#include <windows.h>
#include <stdio.h>
#include <wchar.h>

static void FatalErr(const wchar_t *msg, DWORD err)
{
    wchar_t buf[512];
    _snwprintf(buf, 512, L"%s\n(エラーコード: %lu)", msg, err);
    buf[511] = 0;
    MessageBoxW(NULL, buf, L"SusieUnicode", MB_OK | MB_ICONERROR);
    ExitProcess(1);
}

/* コマンドライン文字列の先頭から、argv[0]相当(実行ファイル部分)の
   直後の位置を返す。Windowsの慣例に従い、二重引用符で始まる場合は
   次の二重引用符まで、それ以外は最初の空白までを1トークンとみなす */
static const wchar_t *SkipArgv0(const wchar_t *cmd)
{
    const wchar_t *p = cmd;
    while (*p == L' ' || *p == L'\t') p++;
    if (*p == L'"') {
        p++;
        while (*p && *p != L'"') p++;
        if (*p == L'"') p++;
    } else {
        while (*p && *p != L' ' && *p != L'\t') p++;
    }
    return p;
}

int main(void)
{
    WCHAR exeDirW[MAX_PATH];
    GetModuleFileNameW(NULL, exeDirW, MAX_PATH);
    WCHAR *lastSlashW = wcsrchr(exeDirW, L'\\');
    if (lastSlashW) *(lastSlashW + 1) = 0;

    WCHAR susiePathW[MAX_PATH];
    _snwprintf(susiePathW, MAX_PATH, L"%sSusie.exe", exeDirW);
    susiePathW[MAX_PATH - 1] = 0;

    WCHAR dllPathW[MAX_PATH];
    _snwprintf(dllPathW, MAX_PATH, L"%ssusie_unicode_hook.dll", exeDirW);
    dllPathW[MAX_PATH - 1] = 0;

    if (GetFileAttributesW(susiePathW) == INVALID_FILE_ATTRIBUTES) {
        MessageBoxW(NULL, L"同じフォルダに Susie.exe が見つかりません",
                    L"SusieUnicode", MB_OK | MB_ICONERROR);
        return 1;
    }
    if (GetFileAttributesW(dllPathW) == INVALID_FILE_ATTRIBUTES) {
        MessageBoxW(NULL, L"同じフォルダに susie_unicode_hook.dll が見つかりません",
                    L"SusieUnicode", MB_OK | MB_ICONERROR);
        return 1;
    }

    /* GetCommandLineW() の生文字列から実行ファイル部分だけを置き換え、
       残り(ユーザーが渡した引数)は無加工でそのまま使う */
    const wchar_t *rest = SkipArgv0(GetCommandLineW());

    WCHAR cmdLineW[8192];
    _snwprintf(cmdLineW, 8192, L"\"%s\"%s", susiePathW, rest);
    cmdLineW[8191] = 0;

    STARTUPINFOW si;
    PROCESS_INFORMATION pi;
    memset(&si, 0, sizeof(si));
    si.cb = sizeof(si);
    memset(&pi, 0, sizeof(pi));

    if (!CreateProcessW(susiePathW, cmdLineW, NULL, NULL, FALSE,
                         CREATE_SUSPENDED, NULL, NULL, &si, &pi)) {
        FatalErr(L"Susie.exe の起動 (CreateProcess) に失敗しました", GetLastError());
    }

    /* --- DLLパス文字列(Wide)をターゲットプロセスに書き込む --- */
    SIZE_T pathBytes = (wcslen(dllPathW) + 1) * sizeof(WCHAR);
    LPVOID remoteMem = VirtualAllocEx(pi.hProcess, NULL, pathBytes,
                                       MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!remoteMem) {
        DWORD err = GetLastError();
        TerminateProcess(pi.hProcess, 1);
        FatalErr(L"ターゲットプロセスへのメモリ確保に失敗しました", err);
    }

    if (!WriteProcessMemory(pi.hProcess, remoteMem, dllPathW, pathBytes, NULL)) {
        DWORD err = GetLastError();
        TerminateProcess(pi.hProcess, 1);
        FatalErr(L"ターゲットプロセスへの書き込みに失敗しました", err);
    }

    /* --- LoadLibraryW をリモートスレッドとして実行 --- */
    LPTHREAD_START_ROUTINE pLoadLibraryW =
        (LPTHREAD_START_ROUTINE)GetProcAddress(GetModuleHandleW(L"kernel32.dll"),
                                                "LoadLibraryW");

    HANDLE hInjectThread = CreateRemoteThread(pi.hProcess, NULL, 0, pLoadLibraryW,
                                               remoteMem, 0, NULL);
    if (!hInjectThread) {
        DWORD err = GetLastError();
        TerminateProcess(pi.hProcess, 1);
        FatalErr(L"リモートスレッドの作成 (DLL注入) に失敗しました", err);
    }

    /* DLL のロード (DllMain 内での IAT パッチ含む) が終わるまで待つ */
    WaitForSingleObject(hInjectThread, INFINITE);

    DWORD exitCode = 0;
    if (!GetExitCodeThread(hInjectThread, &exitCode)) {
        exitCode = 0; /* 取得自体に失敗した場合もロード失敗として扱う */
    }
    CloseHandle(hInjectThread);
    VirtualFreeEx(pi.hProcess, remoteMem, 0, MEM_RELEASE);

    if (exitCode == 0) {
        TerminateProcess(pi.hProcess, 1);
        MessageBoxW(NULL,
            L"susie_unicode_hook.dll のロードに失敗しました\n"
            L"32bit/64bitの不一致、または依存DLL不足の可能性があります",
            L"SusieUnicode", MB_OK | MB_ICONERROR);
        return 1;
    }

    /* --- Susie 本来のコードを開始 --- */
    if (ResumeThread(pi.hThread) == (DWORD)-1) {
        DWORD err = GetLastError();
        TerminateProcess(pi.hProcess, 1);
        FatalErr(L"Susie.exe の実行再開 (ResumeThread) に失敗しました", err);
    }

    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return 0;
}
