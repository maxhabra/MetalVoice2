#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
duration="${1:-20}"
level="${2:-moderate}"
if (( $# > 0 )); then shift; fi
if (( $# > 0 )); then shift; fi
case "$level" in off|low|moderate|high|very-high) ;; *) echo "Invalid level: $level" >&2; exit 2 ;; esac
bundle="$PWD/build/MetalVoice2.app"
if [[ ! -d "$bundle" ]]; then echo "Build the app first with ./scripts/build.sh" >&2; exit 2; fi
mkdir -p build/benchmarks
run_dir="$(mktemp -d "$PWD/build/benchmarks/${level}.XXXXXX")"
# LaunchServices preserves the app's TCC identity; direct terminal execution can
# be attributed to a different responsible process and cannot use that grant.
/usr/bin/open -W -n -g -a "$bundle" --stdout "$run_dir/report.json" --stderr "$run_dir/errors.txt" \
    --args --benchmark "$duration" --ns "$level" "$@" || true
if [[ ! -s "$run_dir/report.json" ]]; then
    if [[ -s "$run_dir/errors.txt" ]]; then cat "$run_dir/errors.txt" >&2
    else echo "The benchmark app did not produce a report." >&2; fi
    exit 1
fi
python3 - "$run_dir/report.json" <<'PY'
import json
import sys
from pathlib import Path
path = Path(sys.argv[1])
report = json.loads(path.read_text())
print(json.dumps(report, indent=2, sort_keys=True))
print(f"Saved counters: {path}")
if not report["latencyValid"] or not report["inputFrames"] or not report["outputFrames"]:
    raise SystemExit("No valid live audio/latency result")
if report.get("echoCancellation") and (report.get("echoReferenceState") != 2 or not report.get("echoReferenceFrames") or report.get("echoReferenceDrops")):
    raise SystemExit("Echo reference was not synchronized; inspect the reference counters")
if any(report[name] for name in ("underruns", "overruns", "resyncs")):
    raise SystemExit("Audio discontinuities occurred; inspect the counters above")
PY
