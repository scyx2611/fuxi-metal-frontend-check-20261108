"""Restore the WMI scan priority field in the pinned ath12k build."""

import argparse
from hashlib import sha256
from pathlib import Path


SOURCE = "out/ath12k-src/wmi.c"
SOURCE_SHA256 = "aaa6b296361c48b7407150cc79ec825bf4943f02038b022370d35975c411ca31"
OLD = (
    b"\t\targ->scan_priority = WMI_SCAN_PRIORITY_LOW;\n"
    b"\tcmd->notify_scan_events = cpu_to_le32(arg->notify_scan_events);\n"
)
NEW = OLD.replace(
    b"\tcmd->notify_scan_events",
    b"\tcmd->scan_priority = cpu_to_le32(arg->scan_priority);\n"
    b"\tcmd->notify_scan_events",
)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("module", type=Path)
    source = parser.parse_args().module / SOURCE
    current = source.read_bytes()
    original = current.replace(NEW, OLD, 1)
    if sha256(original).hexdigest() != SOURCE_SHA256 or original.count(OLD) != 1:
        raise ValueError("The pinned ath12k wmi.c differs")

    # The 11d offload change selects the priority but dropped the TLV assignment.
    # https://lists.infradead.org/pipermail/ath12k/2025-March/006346.html
    patched = original.replace(OLD, NEW, 1)
    if patched != current:
        output = source.with_suffix(".patched")
        output.write_bytes(patched)
        output.replace(source)
    print(f"PASS ath12k encodes WMI scan priority ({sha256(patched).hexdigest()})")


if __name__ == "__main__":
    main()
