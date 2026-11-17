#!/bin/bash
# Build the pinned public Ath12kWifi module (Linux ath12k/mac80211/cfg80211 for XNU) as arm64 kernel objects
# and combine them into one relocatable object for the Fuxi kernel link.
# Usage: build-ath12k-objects.sh WORKDIR OUTDIR
set -euo pipefail
work=$(mkdir -p "$1" && cd "$1" && pwd)
out=$(mkdir -p "$2" && cd "$2" && pwd)
ATH12K_SHA=5f7f5a8a9f02f7ab45ed10a2f226e348c519a838
SHENG_SHA=68800685763d755c368775b848dd703fb3f8a3ce
UNIXNU_SHA=4a654d1f31662f18ab94391a4f0297b17b9bb2fe
pinned() {
    local name=$1 url=$2 sha=$3
    if [[ ! -d $work/$name/.git ]]; then
        git init -q "$work/$name"
        git -C "$work/$name" remote add origin "$url"
        git -C "$work/$name" fetch -q --no-tags --depth=1 origin "$sha"
        git -C "$work/$name" checkout -q --detach FETCH_HEAD
    fi
    test "$(git -C "$work/$name" rev-parse HEAD)" = "$sha"
}
pinned Ath12kWifi https://github.com/UniXNU/Ath12kWifi.git "$ATH12K_SHA"
pinned UniXNU-sheng https://github.com/UniXNU/UniXNU-sheng.git "$SHENG_SHA"
pinned UniXNU https://github.com/UniXNU/UniXNU.git "$UNIXNU_SHA"
# The module's build script asks Homebrew for "llvm" (llvm-nm, llvm-objcopy); hosted runners carry a versioned formula.
prefix=$(brew --prefix)
if [[ ! -x $prefix/opt/llvm/bin/llvm-objcopy ]]; then
    for candidate in "$prefix"/opt/llvm@*; do
        [[ -x $candidate/bin/llvm-objcopy ]] && { ln -sfn "$candidate" "$prefix/opt/llvm"; break; }
    done
fi
[[ -x $prefix/opt/llvm/bin/llvm-objcopy ]] || { echo 'no Homebrew llvm with llvm-objcopy' >&2; exit 1; }
export UNIXNU_XNU_ARCH=arm64 UNIXNU_IOS_VERSION=16.0
export PYTHONPATH=$work/UniXNU/scripts${PYTHONPATH:+:$PYTHONPATH}
python3 - "$work/Ath12kWifi/tools/fetch-sources.py" \
    "$work/UniXNU-sheng/scripts/build-ath12k-wifi.py" \
    "$(cd "$(dirname "$0")" && pwd)/ath12k-scan-dwell.py" \
    "$(cd "$(dirname "$0")/../tests" && pwd)/test-ath12k-scan-dwell.py" <<'PY'
from pathlib import Path
import sys
path = Path(sys.argv[1])
text = path.read_text()
old = '"sparse-checkout", "add", *source["paths"]'
new = '"sparse-checkout", "add", "--skip-checks", *paths'
if text.count(old) != 1:
    raise SystemExit("pinned sparse-checkout fetcher changed")
path.write_text(text.replace(old, new))
path = Path(sys.argv[2])
text = path.read_text()
old = 'from kernel_target import TRIPLE'
new = 'TRIPLE = "arm64-apple-macos15.5"'
if text.count(old) != 1:
    raise SystemExit("pinned ath12k target import changed")
path.write_text(text.replace(old, new))
text = path.read_text()
old = "clang = subprocess.run(['xcrun', '-f', 'clang']"
new = ("subprocess.run([sys.executable, " + repr(str(Path(sys.argv[3]).resolve())) +
       ", str(module)], check=True)\n"
       "subprocess.run([sys.executable, " + repr(str(Path(sys.argv[4]).resolve())) +
       ", str(module)], check=True)\n" + old)
if text.count(old) != 1:
    raise SystemExit("pinned ath12k compiler setup changed")
path.write_text(text.replace(old, new))
PY
python3 "$work/UniXNU-sheng/scripts/build-ath12k-wifi.py" "$work/Ath12kWifi" "$work/objects" > "$work/objects.txt"
sed -n 's/^OBJ //p' "$work/objects.txt" > "$work/objects.list"
test "$(wc -l < "$work/objects.list")" -gt 50
xcrun ld -r -arch arm64 -platform_version macos 15.5 15.5 \
    -filelist "$work/objects.list" -o "$out/fuxi_ath12k.o"
xcrun nm -u "$out/fuxi_ath12k.o" | sort -u > "$out/fuxi_ath12k.undefined.txt"
xcrun nm -g --defined-only "$out/fuxi_ath12k.o" | awk '{print $3}' | grep -v '^_nwk_' | sort -u > "$out/fuxi_ath12k.exports.txt"
cp "$work/objects/build-report.json" "$out/fuxi_ath12k.build-report.json"
cp "$work/Ath12kWifi/out/fuxi-scan-dwell.json" "$out/fuxi_ath12k.scan-dwell.json"
xcrun size -m "$out/fuxi_ath12k.o" > "$out/fuxi_ath12k.size.txt"
xcrun vtool -show-build "$out/fuxi_ath12k.o" > "$out/fuxi_ath12k.platform.txt"
shasum -a 256 "$out/fuxi_ath12k.o"
wc -l "$out/fuxi_ath12k.undefined.txt" "$out/fuxi_ath12k.exports.txt"
