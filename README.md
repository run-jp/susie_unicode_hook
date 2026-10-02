# Susie for Win32 Unicodeパス名 対応APIフック/ランチャー

[Susie for Win32 v0.47b](https://www.digitalpad.co.jp/~takechin/download.html) が
- Unicode文字のファイル名、フォルダ名を含む画像を開けない問題

を解決する、非公式の Unicodeパス名への対応APIフック兼ランチャー です

※ Susie 本体とSusieプラグインのファイルは、一切書き換えません\
※ Susie for Win32 の作者 (たけちん氏) とは無関係の、非公式ツールです

## 必要なもの

- Windows
- [Susie for Win32 v0.47b](https://www.digitalpad.co.jp/~takechin/download.html)
- 32bit版の Susieプラグイン
- `SusieUnicode.exe` と `susie_unicode_hook.dll` (本配布物。[Releases](../../releases) からダウンロード、またはソースからビルド)

## インストール方法

1. `SusieUnicode.exe` と `susie_unicode_hook.dll` を、`Susie.exe` と
   同じフォルダにコピーする
2. 以後は `Susie.exe` ではなく `SusieUnicode.exe` から起動する
   (ショートカットやファイル関連付けのリンク先を変更する)

## アンインストール方法

1. `SusieUnicode.exe` へのショートカットがあれば削除、ファイル関連付けがあれば手動で元に戻す
2. `SusieUnicode.exe`, `susie_unicode_hook.dll`, `SusieUnicode.ini` を削除する

## 設定

基本的には設定不要

- SusieUnicode.ini

ファイルが無ければ、初回起動時に自動生成する

```ini
[Settings]
; %TEMP%\susie_unicode_hook.log にデバッグログを出力する
; (0 = 無効, 1 = 有効)
EnableLog=0

; 書庫(.zip等)のカタログファイル(.sue)の最終更新日時を、
; 対応する書庫ファイル自身の最終更新日時に合わせる
; (0 = 無効, 1 = 有効)
SyncCatalogFileTimestamp=0
```

## 実現方法

### なぜ Susie は、Unicode ファイル名/フォルダ名を含む画像ファイルを開けないのか?

**(a) Susie が呼び出す Win32 API が Unicode に対応していない**\
`Susie.exe` と Susie プラグインが画像ファイルアクセス時に呼び出している Win32 API が、
Unicode に対応していない、古い ANSI API ( [...]A系 ) である\
  \
**(b) Susie 内部で画像パス名が ANSI 文字列として扱われている**\
Susie 内部で、画像ファイルのパス名が ANSI 文字列として扱われているため、
Unicode を含む画像パス名を Susie の内部メモリ領域にそのまま格納できない\
  \
つまり Susie は、呼び出す
**(a) Win32 API**
も、内部メモリ領域に格納する
**(b) 画像パス名**
も、Unicode を扱えない\
(「呼び出す Win32 API を書き換えればOK!」の話ではない)

### Susie.exe と Susie プラグインを書き換えることなく、どうやって Unicode パス名に対応させるか?

**(a) Win32 API の ANSI～Unicode 橋渡し**\
**方法 : DLL Injection を使った、動的な呼び出し先 API 書き換え**\
Susie本体、およびロードされる Susie プラグイン (*.spi) の IAT
(Import Address Table) を動的に書き換え、Susie が呼び出す ANSI API
(`FindFirstFileA`, `CreateFileA`, `_lopen`, `GetCommandLineA` 等) を
フックし、それぞれ対応する Unicode API に「橋渡し」する\
`GetCommandLineA`もフックしているため、Susie 自身のファイルアクセスだけでなく、
関連付け起動やドラッグ&ドロップで渡されるパスにも対応する
また、Susie から受け取ったパス名を自前で Unicode に変換して W系API (`CreateFileW`, `FindFirstFileW` 等) で
開くプラグイン (例: `ax7z_s.spi`) にも対応するため、W系API もフックし、仮想ANSIパス名を実Unicodeパス名に戻して渡す

```
SusieUnicode.exe
      │ CREATE_SUSPENDED で `Susie.exe` を起動
      ▼
   Susie.exe
      │ DLL injection
      ▼
susie_unicode_hook.dll
      │ IAT patch (ResumeThreadの前に完了させる)
      ▼
  Susie / SPI
```

**(b) 画像パス名の Unicode～ANSI 橋渡し**\
**方法 : 画像パス名の「実Unicodeパス名」「仮想ANSIパス名」相互変換**\
Susie 内部は「Unicodeパス名」を扱えないので、`susie_unicode_hook.dll` がUnicodeパス名に1対1で対応する
「仮想ANSIパス名」 (実際には存在しないパス名) を生成し Susie に渡す。
Susie 内部では、「仮想ANSIパス名」の画像ファイルがあたかも存在しアクセスできるよう、
`susie_unicode_hook.dll` が Win32 API のパラメタも含めパス名を相互変換し「橋渡し」する

```
<実在するUnicode画像パス名>
C:\～\<Unicodeフォルダ名>\<Unicodeファイル名>.jpg
↑
susie_unicode_hook.dll が相互変換
↓
<実在しない仮想的なANSI画像パス名> (susie_unicode_hook.dll が生成)
C:\～\SUDxxxxxxxxxxxx\SUFxxxxxxxxxxxx.jpg
```
※ xxxxxxxxxxxx (12桁)は、実UnicodeパスのFNV-1aハッシュ値の下位48bit(16進数12桁)から生成

この2つの仕組みにより、Windowsの「8.3形式」を一切使うことなく、
また Susie 本体とSusieプラグインのファイルを一切書き換えることなく、Unicode対応が可能となる

## 制限事項

- Win32 ANSI API([...]A系)を使う Susie 本体・Susie プラグインが対象\
  Unicode API([...]W系)に対応済みの [Susie ver0.50 beta](https://www.digitalpad.co.jp/~takechin/betasue.html) と
  Susie プラグインには元から必要ない
- ウイルス誤検知の可能性\
  `SusieUnicode.exe` は、`Susie.exe` に外部からDLLを注入する方式(DLL injection)を
  使うため、環境によっては Windows セキュリティ機能、セキュリティソフトの
  ウイルス検知・警告対象になり、動作しない場合がある
- Susie ファイル選択ダイアログには未対応\
  Susie のファイル選択ダイアログを開いて、直接 Unicode 文字入りファイルを選ぶ場合は未対応
- ハッシュ値衝突の可能性\
  `susie_unicode_hook.dll` が生成する「仮想ANSIファイル名」「仮想ANSIフォルダ名」は
  FNV-1aハッシュ値の下位48bitから生成しているため、理論上はハッシュ衝突が発生し
  正常に動作しない可能性がある (100万回発行で、衝突確率0.2%未満)
- パス長制限\
  パス長は`MAX_PATH`(260文字)を前提としており、それを超える長いパス
  (`\\?\`プレフィックス等)には対応していない
- Susie プラグインが静的リンクする DLL には未対応\
  Susie プラグインが静的リンクする DLL (Susie プラグインと一緒に自動でロードされる DLL)内の
  ファイルアクセスはフックしていないため、その DLL が Unicode を含むパス名を開く場合は未対応
- IAT を経由しない API 呼び出しには未対応\
  Susie 本体・Susie プラグインが `GetProcAddress` で直接取得した API を呼ぶ場合、
  その呼び出しはフックされないため、Unicode を含むパス名を開く場合は未対応

## ビルド方法

MinGW-w64 (i686-w64-mingw32-gcc / windres) が必要\
32bit (i686) ターゲットでビルドする

```
make
```

makeを使わず、手動でビルドする場合:

```
i686-w64-mingw32-windres src/susie_unicode_hook.rc -O coff -o susie_unicode_hook_res.o
i686-w64-mingw32-gcc -shared -O2 -o susie_unicode_hook.dll \
    src/susie_unicode_hook.c susie_unicode_hook_res.o -Wl,--kill-at -lshell32 -luser32

i686-w64-mingw32-windres src/susie_unicode_launcher.rc -O coff -o susie_unicode_launcher_res.o
i686-w64-mingw32-gcc -O2 -mwindows -o SusieUnicode.exe \
    src/susie_unicode_launcher.c susie_unicode_launcher_res.o
```

`SusieUnicode.exe` のアプリケーションアイコンを、Susie本体のアイコンから流用する場合:

`tools/borrow_susie_icon.sh` を使って、Susie.exeからアイコンを抽出した上で、自前でビルドする

※ 抽出したSusieのアイコンを含む成果物は、再配布しないでください

```
tools/borrow_susie_icon.sh /path/to/Susie.exe            # グレースケール加工する(デフォルト)
tools/borrow_susie_icon.sh --color /path/to/Susie.exe    # 加工せずそのまま使う
make clean && make
```

## 免責

個人で作成したもので、動作は保証しません\
不具合報告や改善提案は Issue で受け付けますが、対応は保証できません

## ライセンス

[MIT License](LICENSE)
