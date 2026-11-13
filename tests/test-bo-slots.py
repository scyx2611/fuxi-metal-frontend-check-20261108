#!/usr/bin/env python3
"""Check that all FG_ABI_MAX_BOS definition sites are raised to 4096u and no 1024u remains."""
from pathlib import Path
import sys

if len(sys.argv) < 2:
    print(f"Usage: {sys.argv[0]} <source-dir>", file=sys.stderr)
    sys.exit(1)

root = Path(sys.argv[1]).resolve()
xnu_dir = root / 'xnu'
if not xnu_dir.is_dir():
    raise SystemExit(f"Missing xnu directory under {root}")

# 1. Verify specific definition sites hold 4096u
wire_h = xnu_dir / 'fuxi-adreno/fuxi_abi_wire.h'
patch_0098 = xnu_dir / 'patches/0098-fuxi-native-adreno-abi.patch'

wire_text = wire_h.read_text(encoding='utf-8', errors='replace')
assert '#define FG_ABI_MAX_BOS 4096u' in wire_text, (
    f"{wire_h} does not contain '#define FG_ABI_MAX_BOS 4096u'"
)

patch_text = patch_0098.read_text(encoding='utf-8', errors='replace')
assert patch_text.count('+#define FG_ABI_MAX_BOS 4096u') == 2, (
    f"{patch_0098} does not contain exactly two '+#define FG_ABI_MAX_BOS 4096u' lines "
    f"(found {patch_text.count('+#define FG_ABI_MAX_BOS 4096u')})"
)

# 2. Verify no '#define FG_ABI_MAX_BOS 1024u' remains anywhere under <source-dir>/xnu
stale_needle = '#define FG_ABI_MAX_BOS 1024u'
stale_found = []
for p in xnu_dir.rglob('*'):
    if p.is_file():
        try:
            content = p.read_text(encoding='utf-8', errors='ignore')
            if stale_needle in content:
                stale_found.append(str(p.relative_to(root)))
        except Exception:
            pass

assert not stale_found, f"Found stale '{stale_needle}' in files: {stale_found}"

print("PASS: all FG_ABI_MAX_BOS definition sites hold 4096u and no 1024u definitions remain under xnu")
