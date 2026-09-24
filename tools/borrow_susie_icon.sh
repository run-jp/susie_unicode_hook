#!/bin/sh
# tools/borrow_susie_icon.sh
#
# Susie.exe からアイコンを抽出し、src/susie_icon.ico に上書きするツール
#
# デフォルトはグレースケール加工する (Susie本体と見た目で混同しないため)
# --color を付けると、加工せず Susie のアイコンをそのまま使う
#
# 必要なもの: icoutils (wrestool), Python3 + Pillow
#   Debian/Ubuntu/WSL: sudo apt install icoutils python3-pil
#   MSYS2:             pacman -S mingw-w64-x86_64-icoutils python3-pillow
#
# 使い方:
#   tools/borrow_susie_icon.sh [--color] /path/to/Susie.exe
#   make clean && make       (アイコンを差し替えて再ビルド)
#
#   --color を付けない場合(デフォルト): グレースケール加工する
#   --color を付けた場合:               Susieのアイコンをそのまま使う

set -e

MODE="gray"
if [ "$1" = "--color" ]; then
    MODE="color"
    shift
fi

SUSIE_EXE="$1"
if [ -z "$SUSIE_EXE" ] || [ ! -f "$SUSIE_EXE" ]; then
    echo "使い方: $0 [--color] /path/to/Susie.exe" >&2
    exit 1
fi

SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
OUT_ICO="$SCRIPT_DIR/../src/susie_icon.ico"
TMPDIR=$(mktemp -d)
trap 'rm -rf "$TMPDIR"' EXIT

echo "Susie.exeからアイコンを抽出しています..."
wrestool -x --type=14 --name=1 -o "$TMPDIR/extracted.ico" "$SUSIE_EXE"

if [ "$MODE" = "color" ]; then
    echo "抽出したアイコンをそのまま使います(加工なし)..."
    cp "$TMPDIR/extracted.ico" "$OUT_ICO"
else
    echo "グレースケールに変換しています..."
    python3 - "$TMPDIR/extracted.ico" "$OUT_ICO" << 'PYEOF'
import sys
from PIL import Image

src, dst = sys.argv[1], sys.argv[2]
img = Image.open(src)
sizes = sorted(img.info.get('sizes', [(32, 32)]), reverse=True)  # 大きい順

frames = []
for sz in sizes:
    frame = Image.open(src)
    frame.size = sz
    frame.load()  # 該当サイズのフレームを読み込む (PillowのICOハンドリング)
    frame = frame.convert('RGBA')
    r, g, b, a = frame.split()
    gray = Image.merge('RGB', (r, g, b)).convert('L')
    gray_rgb = Image.merge('RGB', (gray, gray, gray))
    frames.append(Image.merge('RGBA', (*gray_rgb.split(), a)))

frames[0].save(dst, sizes=[f.size for f in frames], append_images=frames[1:])
print(f"書き込み完了: {dst}")
PYEOF
fi

echo "完了。'make clean && make' で再ビルドしてください"
