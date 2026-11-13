#!/usr/bin/env python3
"""Check the per-IB GPU profile (fuxiearly bit 16): the backend in 0098 is the mirror byte for byte, and every hook is in place."""
from pathlib import Path
import sys

if len(sys.argv) < 2:
    raise SystemExit(f"Usage: {sys.argv[0]} <source-dir>")
xnu = Path(sys.argv[1]).resolve() / 'xnu'
mirror = (xnu / 'fuxi-adreno/fuxi_gpu_abi_backend.inc').read_bytes()
patch = (xnu / 'patches/0098-fuxi-native-adreno-abi.patch').read_bytes().split(b'\n')

head = patch.index(b'+++ b/bsd/dev/fuxi_gpu_abi_backend.inc')
lines = mirror[:-1].split(b'\n')
assert mirror.endswith(b'\n') and b'\r' not in mirror, 'mirror must be LF-terminated'
assert patch[head + 1] == b'@@ -0,0 +1,%d @@' % len(lines), (patch[head + 1], len(lines))
body = [l[1:] for l in patch[head + 2:head + 2 + len(lines)]]
assert all(l.startswith(b'+') for l in patch[head + 2:head + 2 + len(lines)]), 'section lines must all be additions'
assert body == lines, 'backend section of 0098 differs from xnu/fuxi-adreno/fuxi_gpu_abi_backend.inc'
assert not patch[head + 2 + len(lines)].startswith(b'+'), 'section runs past its hunk'

text = mirror.decode()
for needle in (
    'const int ibp=fg_ibp_want();',
    '(ibp ? FG_IBP_STAMP_WORDS*(count+1) : 0)+64>8191',
    'if (ibp) { const unsigned m2=fg_ibp_insert(words,n,iovas,sizes,count);',
    'if (ibp_on) { fg_ibp_active=1; fg_ibp_seq=fg_abi_seq; fg_ibp_count=count; }',
    'if (fg_ibp_on) fg_ibp_retire(fg_abi_completed, fg_abi_seq);',
    'if (fg_ibp_on) fg_ibp_retire(fg_abi_completed, f);',
    'fg_ibp_on=(early&16u)!=0; fg_ibp_reset();',
    '_Static_assert(FG_IBP_OFF+16u*(NativeAdrenoAbiMaxBuffers+1u) <= 4096u',
):
    assert text.count(needle) == 1, needle
# Off, nothing changes: every new path is behind fg_ibp_on or the per-submit decision it gates.
assert 'if (!fg_ibp_on || fg_abi_pk) return 0;' in text
print('ib profile patch checks passed')
