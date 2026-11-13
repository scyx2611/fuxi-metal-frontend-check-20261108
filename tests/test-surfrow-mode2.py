#!/usr/bin/env python3
"""Check that sweetsurfrow defaults to mode 2 (round only rows that are not 64-byte aligned)."""
from pathlib import Path
import sys

if len(sys.argv) < 2:
    print(f"Usage: {sys.argv[0]} <source-dir>", file=sys.stderr)
    sys.exit(1)

root = Path(sys.argv[1]).resolve()
sites = {
    root / 'xnu/fuxi-scanout/SweetSurface.cpp': '',
    root / 'xnu/patches/0108-fuxi-gpu-scanout.patch': '+',
}
for path, prefix in sites.items():
    text = path.read_text(encoding='utf-8', errors='replace')
    wanted = [
        prefix + 'static int\n' + prefix + 'sweet_surface_gpu_rows(void)',
        prefix + '\t\tuint32_t v = 2;\n' + prefix + '\t\t(void)PE_parse_boot_argn("sweetsurfrow"',
        prefix + '\t\tenabled = v <= 2 ? (int)v : 2;\n',
        prefix + '\treturn enabled;\n',
        '(sweet_surface_gpu_rows() != 2 || (s->bytesPerRow & 63) != 0)) {\n',
    ]
    for needle in wanted:
        assert text.count(needle) == 1, f"{path}: expected exactly one {needle!r}, found {text.count(needle)}"
    for stale in ('uint32_t v = 1;\n' + prefix + '\t\t(void)PE_parse_boot_argn("sweetsurfrow"', 'enabled = v != 0;'):
        assert stale not in text, f"{path}: stale {stale!r}"
print("sweetsurfrow mode 2 default present at both sites")
