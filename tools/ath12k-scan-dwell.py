import argparse
import hashlib
import json
from pathlib import Path


PINS = {
    'include/nw_control.h': 'c895836cd8792a6844227da30c9eb91e598dd2fe8b865e53c69b9740bb68dab1',
    'src/nw_cfg80211_control.c': 'db58651619b8d41b5020f92e809f87923783c75c040713128ea12a72495ee9be',
}
LINUX_MAC_SHA256 = '6a5846b4b3bcf718cdf87226b99f90649169a3892cea2c88f677e32c02020b33'
DWELL_FUNCTION = '''static void ath12k_mac_scan_dwell(struct ath12k_wmi_scan_req_arg *arg,
                                   const struct cfg80211_scan_request *req)
{
\tif (!req->duration)
\t\treturn;
\tu32 dwell_ms = DIV_ROUND_UP((u32)req->duration * 1024U, 1000U);
\targ->dwell_time_active = dwell_ms;
\targ->dwell_time_active_2g = dwell_ms;
\targ->dwell_time_passive = dwell_ms;
\targ->dwell_time_active_6g = dwell_ms;
\targ->dwell_time_passive_6g = dwell_ms;
\targ->burst_duration = dwell_ms;
\tif (req->duration_mandatory)
\t\targ->adaptive_dwell_time_mode = SCAN_DWELL_MODE_STATIC;
}

'''


def sha256(data):
    return hashlib.sha256(data).hexdigest()


def edit(path, replacements, original_sha256=None):
    original = path.read_text()
    restored = original
    for before, after in reversed(replacements):
        if restored.count(after) == 1:
            restored = restored.replace(after, before, 1)
    if original_sha256 and sha256(restored.encode()) != original_sha256:
        raise ValueError(f'Pinned scan input changed: {path}')
    result = restored
    for before, after in replacements:
        if result.count(before) != 1:
            raise ValueError(f'Scan patch marker differs: {path}: {before[:80]!r}')
        result = result.replace(before, after, 1)
    if original != result:
        if path.is_symlink():
            path.unlink()
        path.write_text(result, newline='\n')
    return {'before_sha256': sha256(restored.encode()), 'after_sha256': sha256(result.encode())}


def apply(module):
    linux_mac = module / 'deps/linux/drivers/net/wireless/ath/ath12k/mac.c'
    if sha256(linux_mac.read_bytes()) != LINUX_MAC_SHA256:
        raise ValueError('Pinned Linux scan implementation changed')
    records = {}
    header = 'include/nw_control.h'
    records[header] = edit(module / header, [
        ('#define NW_CONTROL_GET_SESSION 1\n',
         '#define NW_CONTROL_GET_SESSION 1\n#define NW_CONTROL_FEATURE_SCAN_DWELL_MS (1U << 31)\n'),
        ('  NW_CONTROL_ATTR_COUNT\n',
         '  NW_CONTROL_ATTR_SCAN_DWELL_MS,\n  NW_CONTROL_ATTR_COUNT\n'),
    ], PINS[header])
    control = 'src/nw_cfg80211_control.c'
    records[control] = edit(module / control, [
        ('  if (!rdev->ops->scan) return -EOPNOTSUPP;\n',
         '''  uint32_t dwell_ms = 0;
  size_t dwell_bytes;
  const u8 *dwell = nw_control_get(view, NW_CONTROL_ATTR_SCAN_DWELL_MS, &dwell_bytes);
  if (dwell && (dwell_bytes != 4 ||
      nw_control_get_u32(view, NW_CONTROL_ATTR_SCAN_DWELL_MS, &dwell_ms) ||
      !dwell_ms || dwell_ms > 1000)) return -EINVAL;
  if (!rdev->ops->scan) return -EOPNOTSUPP;
'''),
        ('  if (!request->req.n_channels) { kfree(request); return -EINVAL; }\n',
         '''  if (!request->req.n_channels) { kfree(request); return -EINVAL; }
  if (dwell_ms) {
    request->req.duration = DIV_ROUND_UP(dwell_ms * 1000U, 1024U);
    request->req.duration_mandatory = true;
    uint32_t rounded_ms = DIV_ROUND_UP((uint32_t)request->req.duration * 1024U, 1000U);
    if ((rounded_ms + 500U) * request->req.n_channels > 19000U) {
      kfree(request); return -EINVAL;
    }
  }
'''),
        ('        nw_control_put_u32(reply, NW_CONTROL_ATTR_OPERSTATE, radio_enabled)) return NW_CONTROL_NO_MEMORY;\n',
         '''        nw_control_put_u32(reply, NW_CONTROL_ATTR_OPERSTATE, radio_enabled) ||
        nw_control_put_u32(reply, NW_CONTROL_ATTR_FEATURES, NW_CONTROL_FEATURE_SCAN_DWELL_MS)) return NW_CONTROL_NO_MEMORY;
'''),
    ], PINS[control])
    mac = 'out/ath12k-src/mac.c'
    records[mac] = edit(module / mac, [
        ('static int ath12k_mac_initiate_hw_scan(struct ieee80211_hw *hw,\n',
         DWELL_FUNCTION + 'static int ath12k_mac_initiate_hw_scan(struct ieee80211_hw *hw,\n'),
        ('\targ->scan_id = ATH12K_SCAN_ID;\n\n\tif (req->ie_len) {\n',
         '''\targ->scan_id = ATH12K_SCAN_ID;
\tath12k_mac_scan_dwell(arg, req);
\tif (req->duration)
\t\tath12k_dbg(ar->ab, ATH12K_DBG_MAC,
\t\t\t   "scan dwell %u TU -> %u ms mandatory %u\\n",
\t\t\t   req->duration, arg->dwell_time_active,
\t\t\t   req->duration_mandatory);

\tif (req->ie_len) {
'''),
    ])
    record = {'schema': 'fuxi-ath12k-scan-dwell-v1',
              'patcher_sha256': sha256(Path(__file__).read_bytes()),
              'linux_mac_sha256': LINUX_MAC_SHA256, 'feature_bit': 1 << 31,
              'attribute': 46, 'maximum_requested_ms': 1000,
              'maximum_scan_budget_ms': 19000, 'hardware_verified': False,
              'files': records}
    (module / 'out/fuxi-scan-dwell.json').write_text(json.dumps(record, indent=2) + '\n')
    print('PASS pinned scan dwell extension: control milliseconds -> cfg80211 TU -> WMI milliseconds')


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('module', type=Path)
    arguments = parser.parse_args()
    apply(arguments.module.resolve())


if __name__ == '__main__':
    main()
