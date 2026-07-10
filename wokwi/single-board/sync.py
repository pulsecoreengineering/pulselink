#!/usr/bin/env python3
"""Copies combined/combined.ino's transitive core/transport/gateway header
dependencies into combined/, flattening #include paths to same-directory
(wokwi.com needs this — see this project's README for why).

Unlike wokwi/gateway-node/'s old sync.py (removed — Wokwi doesn't support
multi-board projects), combined.ino itself is hand-written, not
mechanically derived from gateway.ino/node.ino: merging two setup()/loop()
functions and renaming colliding symbols (now_ticks(), service_*(), ...)
needs real judgment a regex script can't safely make. Only the *header*
files are mechanically copied+flattened here — combined.ino's own logic
should stay in sync with gateway.ino/node.ino by hand when either changes;
they're small enough (both under 450 lines) that a diff is quick to check.

Run from the repo root: python3 wokwi/single-board/sync.py
Then re-check combined/combined.ino still syntax-checks clean (see this
project's README for the exact command).
"""

import re
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[2]
OUT_DIR = Path(__file__).resolve().parent / "combined"

# Everything combined.ino transitively includes, except combined.ino
# itself (hand-written, lives in this directory already — see module
# docstring for why it isn't generated).
HEADER_FILES = [
    "core/pl_config.h",
    "core/pl_mac.h",
    "core/pl_registry.h",
    "core/pl_join.h",
    "core/pl_gateway_hsm.h",
    "core/pl_mqtt.h",
    "core/pl_spool.h",
    "core/pl_frame.h",
    "core/pl_fields.h",
    "core/pl_cmdtable.h",
    "core/pl_cmd_status.h",
    "core/pl_loss_tracker.h",
    "core/pl_dedupe.h",
    "core/pl_topics.h",
    "core/pl_ring.h",
    "transport/pl_transport.h",
    "transport/fake/pl_fake_transport.h",
    "gateway/pl_nvs_registry_storage.h",
    "gateway/pl_pubsubclient_mqtt.h",
]

_INCLUDE_RE = re.compile(r'#include\s+"(?:\.\./)+(?:[\w-]+/)*([\w.]+)"')


def flatten_includes(text: str) -> str:
    return _INCLUDE_RE.sub(r'#include "\1"', text)


def main():
    OUT_DIR.mkdir(parents=True, exist_ok=True)
    for rel_path in HEADER_FILES:
        src = REPO_ROOT / rel_path
        text = flatten_includes(src.read_text())
        dest = OUT_DIR / Path(rel_path).name
        dest.write_text(text)
    print(f"wrote {len(HEADER_FILES)} headers to {OUT_DIR}")


if __name__ == "__main__":
    main()
