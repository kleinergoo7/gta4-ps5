#!/usr/bin/env bash
# Fail loudly, never silently — with a ratchet so debt can only go down.
#
# Lists failure paths in port code that return a failure value without any log
# or error propagation in the preceding lines, plus discarded results and
# log-only catches.
#
# Usage: tools/check_loud_failures.sh [options]
#   (default)          report and exit 0 (advisory)
#   --strict           exit 1 when hits exist (for CI)
#   --ratchet          exit 1 if any file exceeds its baseline count
#   --update-baseline  rewrite tools/loud_failures_baseline.txt from the current tree
#
# Policy (AGENTS.md): touching a file means draining its debt, so the baseline
# for that file must drop, never rise.
set -uo pipefail
cd "$(dirname "$0")/.."

python3 - "$@" <<'PY'
import glob, os, re, sys

args = sys.argv[1:]
strict = "--strict" in args
ratchet = "--ratchet" in args
update = "--update-baseline" in args

baseline_path = "tools/loud_failures_baseline.txt"

reporting = ("LOG", "fprintf", "printf", "REXLOG", "journal.", "errorMessage",
             "throw", "std::cerr", "DIAG_EMIT")

roots = ["LibertyRecomp/install", "LibertyRecomp/kernel", "LibertyRecomp/os",
         "LibertyRecomp/gpu", "LibertyRecomp/ui", "LibertyRecomp/cpu"]

hits = []
for root in roots:
    for path in glob.glob(root + "/**/*.cpp", recursive=True):
        if path.endswith("installer_cli.cpp"):
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

        for i, line in enumerate(lines):
            if re.search(r"\(\s*void\s*\)\s*\w+\s*\(", line):
                hits.append((path, i + 1, "discarded result via (void) cast: " + line.strip()))
            if re.match(r"\s*\}?\s*catch\s*\(", line):
                body = " ".join(lines[i:i + 6])
                if ("throw" not in body) and not re.search(
                        r"return\s+(false|nullptr|-1|X_STATUS)", body):
                    if any(tok in body for tok in ("LOG", "fprintf", "REXLOG", "printf")):
                        hits.append((path, i + 1,
                                     "catch that only logs and continues: " + line.strip()))

counts = {}
for path, _lineno, _text in hits:
    counts[path] = counts.get(path, 0) + 1

if update:
    with open(baseline_path, "w") as f:
        f.write("# Silent-failure baseline: per-file count. May only decrease.\n")
        f.write(f"# total {len(hits)}\n")
        for path in sorted(counts):
            f.write(f"{counts[path]} {path}\n")
    print(f"baseline updated: {len(hits)} hit(s) across {len(counts)} file(s)",
          file=sys.stderr)
    sys.exit(0)

baseline = {}
if os.path.exists(baseline_path):
    for line in open(baseline_path):
        line = line.strip()
        if not line or line.startswith("#"):
            continue
        parts = line.split(None, 1)
        if len(parts) == 2 and parts[0].isdigit():
            baseline[parts[1]] = int(parts[0])

for path, lineno, text in hits:
    print(f"{path}:{lineno}: {text}")

regressions = []
if ratchet and baseline:
    for path, count in sorted(counts.items()):
        allowed = baseline.get(path, 0)
        if count > allowed:
            regressions.append(f"{path}: {count} silent failure(s), baseline {allowed}")

improved = []
for path, allowed in sorted(baseline.items()):
    current = counts.get(path, 0)
    if current < allowed:
        improved.append(f"{path}: {allowed} -> {current}")

print(f"\n{len(hits)} silent failure path(s)", file=sys.stderr)
for line in regressions:
    print(f"REGRESSION {line}", file=sys.stderr)
for line in improved:
    print(f"improved (update baseline) {line}", file=sys.stderr)

if regressions or (strict and hits):
    sys.exit(1)
sys.exit(0)
PY
