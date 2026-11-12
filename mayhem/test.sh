#!/usr/bin/env bash
#
# mayhem/test.sh — RUN gloo's own gtest suite (already built by mayhem/build.sh).
# exit 0 = pass. EDIT per repo. PATCH-grade oracle: after an agent patches the source, the grader
# rebuilds (build.sh) then runs this. DELETE this file if the repo has no meaningful tests.
#
# IMPORTANT:
#  * Must assert BEHAVIOR/OUTPUT, not just exit status. The oracle has to check asserted values /
#    golden-output diffs / known-answer results — so a PATCH that "fixes" a bug by making the program
#    exit(0) (or any no-op) FAILS here. Running inputs and checking only "exit 0 / didn't crash" is
#    NOT a functional test (it's trivially reward-hackable) — use the project's real assertion suite.
#  * Do NOT build here — mayhem/build.sh already compiled the test suite (with the project's normal
#    flags). This script only RUNS the pre-built tests and reports counts. If the test runner is
#    missing, that's a build.sh bug — fail loudly rather than silently rebuilding.
#  * REQUIRED OUTPUT — a CTRF (https://ctrf.io) summary so Mayhem/the PATCH grader reads the counts:
#      - writes a CTRF JSON report to ${CTRF_REPORT:-$SRC/ctrf-report.json}, and
#      - prints a one-line `CTRF {...}` marker to stdout (same JSON, compact).
#    Only `results.summary` (with tests/passed/failed/pending/skipped/other) is required.
#    Use the emit_ctrf helper below; it computes tests = passed+failed+skipped and sets the exit
#    code (0 iff failed==0). Map your framework's output to passed/failed/skipped.
set -uo pipefail
[ -n "${SOURCE_DATE_EPOCH:-}" ] || unset SOURCE_DATE_EPOCH
: "${MAYHEM_JOBS:=$(nproc)}"   # build parallelism; env-overridable, falls back to nproc (use -j"$MAYHEM_JOBS")
cd "$SRC"

# emit_ctrf <tool> <passed> <failed> [skipped] [pending] [other]
# Writes a CTRF report (file + stdout `CTRF {...}` marker) and returns non-zero iff failed>0.
emit_ctrf() {
  local tool="$1" passed="$2" failed="$3" skipped="${4:-0}" pending="${5:-0}" other="${6:-0}"
  local tests=$(( passed + failed + skipped + pending + other ))
  cat > "${CTRF_REPORT:-$SRC/ctrf-report.json}" <<JSON
{
  "results": {
    "tool": { "name": "$tool" },
    "summary": {
      "tests": $tests,
      "passed": $passed,
      "failed": $failed,
      "pending": $pending,
      "skipped": $skipped,
      "other": $other
    }
  }
}
JSON
  printf 'CTRF {"results":{"tool":{"name":"%s"},"summary":{"tests":%d,"passed":%d,"failed":%d,"pending":%d,"skipped":%d,"other":%d}}}\n' \
    "$tool" "$tests" "$passed" "$failed" "$pending" "$skipped" "$other"
  [ "$failed" -eq 0 ]
}

GLOO_TEST=build-tests/gloo/test/gloo_test
if [ ! -x "$GLOO_TEST" ]; then
  echo "missing $GLOO_TEST: mayhem/build.sh did not build the test suite" >&2
  emit_ctrf "gtest" 0 1 0
  exit 1
fi

# Everything but the multi-process transport matrix (~10 min) and the three slowest collective
# suites. Both are exercised by the same TCP transport the remaining collectives run over.
FILTER='-*MultiProc*:AllreduceNewRing*:AllreduceNewBcube*:LargeBroadcast*'
out="$(mktemp)"
trap 'rm -f "$out"' EXIT
"$GLOO_TEST" --gtest_filter="$FILTER" >"$out" 2>&1
rc=$?
tail -n 8 "$out"

# The oracle is the values gtest asserted, read from its own summary: the run must report a
# plausible number of tests as PASSED, the same number as were run, and none FAILED.
ran=$(sed -n 's/^\[==========\] \([0-9][0-9]*\) tests\? from .* ran\..*/\1/p' "$out" | tail -n 1)
passed=$(sed -n 's/^\[  PASSED  \] \([0-9][0-9]*\) tests\?\..*/\1/p' "$out" | tail -n 1)
failed=$(sed -n 's/^\[  FAILED  \] \([0-9][0-9]*\) tests\?, listed below:.*/\1/p' "$out" | tail -n 1)
skipped=$(sed -n 's/^\[  SKIPPED \] \([0-9][0-9]*\) tests\?, listed below:.*/\1/p' "$out" | tail -n 1)
ran=${ran:-0}; passed=${passed:-0}; failed=${failed:-0}; skipped=${skipped:-0}

# Known-answer probes: specific tests whose pass line must appear (a binary that exits 0 without
# running the suite prints none of them).
probes_failed=0
for probe in \
    'PairTests/PairTest.RecvBufferHonorsValidRoffset/0' \
    'PairTests/PairTest.RecvBufferRejectsRoffsetLengthOverflow/0' \
    'SendRecvDefault/SendRecvTest.' \
    'OneToAllBroadcast/BroadcastTest.'; do
  if ! grep -q -F "[       OK ] $probe" "$out"; then
    echo "known-answer probe missing from gtest output: $probe" >&2
    probes_failed=$((probes_failed + 1))
  fi
done

if [ "$ran" -lt 1000 ] || [ "$passed" -ne $((ran - skipped)) ] || [ "$rc" -ne 0 ]; then
  echo "gtest summary inconsistent: rc=$rc ran=$ran passed=$passed failed=$failed skipped=$skipped" >&2
  failed=$((failed + 1))
fi
failed=$((failed + probes_failed))
emit_ctrf "gtest" "$passed" "$failed" "$skipped"
