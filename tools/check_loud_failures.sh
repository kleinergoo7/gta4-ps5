#!/usr/bin/env bash
# Fail loudly, never silently.
#
# Lists failure paths in port code that return a failure value without any log
# or error propagation in the preceding lines. New hits should either log the
# reason or hand back an error that the caller reports.
#
# Usage: tools/check_loud_failures.sh [--strict]
#   default  : report and exit 0 (advisory)
#   --strict  : exit 1 when hits exist (for CI)
set -uo pipefail
cd "$(dirname "$0")/.."

python3 - "$@" <<'PY'
import glob, re, sys

strict = "--strict" in sys.argv[1:]
reporting = ("LOG", "fprintf", "printf", "REXLOG", "journal.", "errorMessage",
             "throw", "std::cerr", "DIAG_EMIT")

roots = ["LibertyRecomp/install", "LibertyRecomp/kernel", "LibertyRecomp/os",
         "LibertyRecomp/gpu", "LibertyRecomp/ui", "LibertyRecomp/cpu"]

hits = []
for root in roots:
    for path in glob.glob(root + "/**/*.cpp", recursive=True):
        if path.endswith("installer_cli.cpp") or path.endswith("check_loud_failures.sh"):
            continue
        try:
            lines = open(path, errors="replace").read().splitlines()
        except OSError:
            continue
        for i, line in enumerate(lines):
            if not re.match(r"\s*return (false|nullptr|-1|nullopt);", line):
                continue
            window = " ".join(lines[max(0, i - 6):i + 1])
            if any(tok in window for tok in reporting):
                continue
            hits.append((path, i + 1, "silent failure: " + line.strip()))

        # Errors must propagate: flag discarded calls and log-only catches.
        for i, line in enumerate(lines):
            if re.search(r"\(\s*void\s*\)\s*\w+\s*\(", line):
                hits.append((path, i + 1, "discarded result via (void) cast: " + line.strip()))
            if re.match(r"\s*\}?\s*catch\s*\(", line):
                body = " ".join(lines[i:i + 6])
                if ("throw" not in body) and not re.search(r"return\s+(false|nullptr|-1|X_STATUS)", body):
                    if any(tok in body for tok in ("LOG", "fprintf", "REXLOG", "printf")):
                        hits.append((path, i + 1, "catch that only logs and continues: " + line.strip()))

for path, lineno, text in hits:
    print(f"{path}:{lineno}: silent failure: {text}")

print(f"\n{len(hits)} silent failure path(s)", file=sys.stderr)
sys.exit(1 if (strict and hits) else 0)
PY
