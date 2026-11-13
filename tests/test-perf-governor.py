#!/usr/bin/env python3
"""Check the opt-in governor diagnostics (fuxiperfbase, fuxiperfmax, duty 1000) at both sites.

The defaults must stay the legacy policy: cool caps, 62/68 C thresholds and the 200 ms duty are unchanged.
"""
from pathlib import Path
import sys

if len(sys.argv) < 2:
    print(f"Usage: {sys.argv[0]} <source-dir>", file=sys.stderr)
    sys.exit(1)

root = Path(sys.argv[1]).resolve()
wanted = [
    ' fp_arg("fuxiperfbase",&fp_base_mhz,1402,2400);\n',
    ' fp_arg("fuxiperfmax",&fp_max,620,800);\n',
    ' fp_arg("fuxiperflimit",&fp_limit,450,800);\n',
    ' fp_arg("fuxiperfbudget",&fp_budget_ms,20,1000);\n',
    ' fp_run_max = fp_limit > 680 ? fp_limit : 680;\n',
    '       state == FP_IDLE ? fp_idle[d] : failed || fp_blocked ? fp_cool[d] : fp_base[d];\n',
    ' int duty_ok = (fp_budget_ms >= 1000 || used < fp_budget) && (fp_boosted || fp_duty_count < 16);\n',
    ' if (fp_boosted && fp_budget_ms < 1000 && fp_budget-used < deadline-now) deadline = now+fp_budget-used;\n',
    'static unsigned fp_run_max = 680;\n',
    ' if (!valid || temp > (int)fp_run_max || (!boosted && temp > (int)fp_start)) return 0;\n',
    # 預設值不得改變
    'static const unsigned fp_cool_cap[3] = {1228800,1401600,1363200};\n',
    'static unsigned fp_hot = 620, fp_limit = 680, fp_resume = 580;\n',
    'static unsigned fp_rise = 100, fp_fast_ms = 20, fp_budget_ms = 200;\n',
]
stale = ['fp_arg("fuxiperflimit",&fp_limit,450,680)', 'fp_arg("fuxiperfbudget",&fp_budget_ms,20,500)', 'temp > 680 ||']
mirror = (root / 'xnu/fuxi-perf/fuxi_perf.c').read_text(encoding='utf-8') + \
    (root / 'xnu/fuxi-perf/fuxi_perf_logic.h').read_text(encoding='utf-8')
patch = (root / 'xnu/patches/0097-fuxi-touch-boost.patch').read_text(encoding='utf-8')
for needle in wanted:
    assert mirror.count(needle) == 1, f"mirror: expected exactly one {needle!r}, found {mirror.count(needle)}"
    assert patch.count('+' + needle) == 1, f"0097: expected exactly one {needle!r}, found {patch.count('+' + needle)}"
for needle in stale:
    assert needle not in mirror and needle not in patch, f"stale {needle!r}"
print("governor diagnostics present at both sites; defaults unchanged")
