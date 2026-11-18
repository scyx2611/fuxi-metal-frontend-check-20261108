"""Apply Linux's peer ID 0 fix to the prepared, pinned ath12k source."""

import argparse
import hashlib
import json
from pathlib import Path
import re
import subprocess
import tempfile


UPSTREAM_COMMIT = "51d510589f5cb54b9251f4ecf4c13f219dead928"
SOURCE = "out/ath12k-src/dp_peer.c"
SOURCE_SHA256 = "32d0021252a5360a9e4ab97cd14f0de6c98e9a6ac69556379bf5e021f6bec35b"
OLD_GUARD = b"if (!peer_id || peer_id >= ATH12K_DP_PEER_ID_INVALID)"
NEW_GUARD = b"if (peer_id >= ATH12K_DP_PEER_ID_INVALID)"

TEST_PREFIX = r"""
#include <assert.h>
#include <stddef.h>
#include <stdint.h>

typedef uint16_t u16;
enum { ATH12K_DP_PEER_ID_INVALID = 3 };
struct ath12k_dp_peer { int value; };
struct ath12k_dp_hw { struct ath12k_dp_peer *dp_peers[ATH12K_DP_PEER_ID_INVALID]; };
struct ath12k_dp { int unused; };
struct ath12k_pdev_dp { struct ath12k_dp *dp; struct ath12k_dp_hw *dp_hw; };
#define RCU_LOCKDEP_WARN(condition, message) ((void)(condition))
#define rcu_read_lock_held() 1
#define rcu_dereference(pointer) (pointer)
static u16 ath12k_dp_peer_get_peerid_index(struct ath12k_dp *dp, u16 peer_id)
{
    assert(dp && peer_id < ATH12K_DP_PEER_ID_INVALID);
    return peer_id;
}
"""

TEST_MAIN = r"""
int main(void)
{
    struct ath12k_dp_peer peer = { 1 };
    struct ath12k_dp_hw hw = { { &peer, &peer, NULL } };
    struct ath12k_dp dp = { 0 };
    struct ath12k_pdev_dp pdev = { &dp, &hw };
    assert(ath12k_dp_peer_find_by_peerid(&pdev, 0) == (ACCEPT_ZERO ? &peer : NULL));
    assert(ath12k_dp_peer_find_by_peerid(&pdev, 1) == &peer);
    assert(!ath12k_dp_peer_find_by_peerid(&pdev, 2));
    assert(!ath12k_dp_peer_find_by_peerid(&pdev, ATH12K_DP_PEER_ID_INVALID));
    assert(!ath12k_dp_peer_find_by_peerid(&pdev, UINT16_MAX));
}
"""


def sha256(data):
    return hashlib.sha256(data).hexdigest()


def verify_lookup(original, patched):
    with tempfile.TemporaryDirectory(prefix="fuxi-ath12k-peer-") as temporary:
        source = Path(temporary) / "lookup.c"
        binary = source.with_suffix("")
        for accepts_zero, contents in enumerate((original, patched)):
            lookup = re.search(rb"^struct ath12k_dp_peer \*ath12k_dp_peer_find_by_peerid\(.*?^}\n",
                               contents, re.MULTILINE | re.DOTALL)
            if lookup is None:
                raise ValueError("Expected peer lookup is missing")
            source.write_text(TEST_PREFIX + lookup[0].decode() + TEST_MAIN, encoding="utf-8")
            subprocess.run(["clang", "-std=c11", "-Wall", "-Wextra", "-Werror",
                            "-fsanitize=address,undefined", f"-DACCEPT_ZERO={accepts_zero}",
                            str(source), "-o", str(binary)], check=True)
            subprocess.run([str(binary)], check=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("module", type=Path)
    args = parser.parse_args()
    source = args.module / SOURCE
    current = source.read_bytes()
    original = current.replace(NEW_GUARD, OLD_GUARD, 1) if NEW_GUARD in current else current
    if sha256(original) != SOURCE_SHA256 or original.count(OLD_GUARD) != 1:
        raise ValueError("The pinned ath12k dp_peer.c differs")
    patched = original.replace(OLD_GUARD, NEW_GUARD, 1)
    verify_lookup(original, patched)
    if current != patched:
        # Prepared sources can be symlinks. Replace the prepared copy and preserve the dependency checkout.
        output = source.with_suffix(".patched")
        output.write_bytes(patched)
        output.replace(source)
    provenance = {
        "schema": "fuxi-ath12k-peer-zero-v1",
        "upstream_commit": UPSTREAM_COMMIT,
        "source": SOURCE,
        "source_before_sha256": SOURCE_SHA256,
        "source_after_sha256": sha256(patched),
        "tests": "ASan/UBSan pinned lookup, old guard and valid/invalid peer IDs",
    }
    (args.module / "out/fuxi-peer-zero.json").write_text(
        json.dumps(provenance, indent=2) + "\n", encoding="utf-8", newline="\n")
    print("PASS ath12k peer lookup accepts mapped ID 0 and rejects invalid IDs")


if __name__ == "__main__":
    main()
