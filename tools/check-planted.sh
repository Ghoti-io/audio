#!/bin/sh
# SPDX-License-Identifier: LGPL-3.0-only
#
# Copyright (C) 2026 Corey Pennycuff
#
# This file is part of Ghoti.io Audio.
#
# Ghoti.io Audio is free software: you can redistribute it and/or modify
# it under the terms of the GNU Lesser General Public License version 3 as
# published by the Free Software Foundation.
#
# Prove that the MP3 encoder's tests fail on the defects they exist to catch.
#
# A test that has never been seen to fail may be measuring nothing, and an
# encoder is the case where that is easy to believe: its output is valid
# whatever it decides, so a wrong decision usually decodes. Each patch in
# tests/planted/ plants one defect in the encoder (a changed condition, an
# ignored term, a constant where a measurement belongs), and the test named in
# its `# caught by:` header must notice.
#
# This builds a throwaway copy of the library under build/planted/, applies
# ONE patch at a time, builds testMp3_encode, and requires the named tests to
# FAIL; then takes the patch out, rebuilds, and requires the same tests to
# PASS (the control: a test that fails for any reason at all - a build that
# broke, a missing fixture - would otherwise count as having caught the
# defect). The working tree is not touched, which matters because
# bootstrap.sh installs it.
#
# A patch header is comment lines before the first `---`:
#
#   # 12 attack
#   # defect: a rise in energy is never recognised as an onset
#   # caught by: Mp3EncodeBlocks.ARiseOfTwelveDecibelsAmongSteadyNoiseIsAnOnset
#
# `caught by` is a gtest filter (a test name, or names joined with `:`). `*`
# means the whole binary and is used only where a defect crashes it, so no
# single test is the one that fails.
#
# Each patch is applied with `patch --fuzz=0` and the script checks that it
# matched and that a file changed, so a patch that applies to nothing fails
# the script rather than reading as a test that held. A control that runs no
# tests (a filter that matches nothing) fails too.
#
# Usage: PLANTED_PREFIX=<prefix> PLANTED_LIBDIR=<dir> tools/check-planted.sh [--selftest] [case...]
#   --selftest  prove the script itself: a patch that applies to nothing
#               fails it, and a patch that breaks nothing is reported as not
#               caught
#   case        run only the named patches (NN-name, without .patch)
# PLANTED_PREFIX is the PREFIX the dependencies were installed with and
# PLANTED_LIBDIR the directory their shared libraries are in (make
# check-planted sets both); the pkg-config path is the caller's, as for every
# make in this library. PLANTED_KEEP=1 keeps the copy.

set -u

HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(dirname "$HERE")"
PATCHES="$ROOT/tests/planted"
WORK="$ROOT/build/planted/copy"
LOG="$ROOT/build/planted/build.log"
JOBS="${PLANTED_JOBS:-8}"
PREFIX="${PLANTED_PREFIX:-}"
LIBDIR="${PLANTED_LIBDIR:-}"
TARGET="build/linux/release/apps/testMp3_encode"

if [ -z "$LIBDIR" ]; then
  printf 'check-planted: set PLANTED_LIBDIR to the directory the dependencies'"'"' shared libraries are in, and PLANTED_PREFIX to the PREFIX they were installed with (make check-planted does both)\n' >&2
  exit 2
fi
PREFIX_ARG=""
if [ -n "$PREFIX" ]; then
  PREFIX_ARG="PREFIX=$PREFIX"
fi

SELFTEST=0
NAMED=""
for arg in "$@"; do
  case "$arg" in
    --selftest) SELFTEST=1 ;;
    -*) printf 'check-planted: unknown option %s\n' "$arg" >&2; exit 2 ;;
    *) NAMED="$NAMED $arg" ;;
  esac
done

cleanup() {
  if [ "${PLANTED_KEEP:-0}" != 1 ]; then
    rm -rf "$ROOT/build/planted"
  fi
}
trap cleanup EXIT INT TERM

APPS="$WORK/build/linux/release/apps"
LDPATH="$APPS:$LIBDIR"

# The patch's own header, one field.
header() {
  sed -n "s/^# $2: *//p" "$1" | head -1
}

# The files a patch names, from its +++ lines.
files_of() {
  sed -n 's|^+++ b/\([^[:space:]]*\).*|\1|p' "$1"
}

build() {
  # Incremental: only what the patch touched is rebuilt. The environment's make
  # flags are not this make's.
  # shellcheck disable=SC2086
  (cd "$WORK" && env -u MAKEFLAGS -u MFLAGS -u MAKELEVEL make -j"$JOBS" $PREFIX_ARG "$TARGET" >>"$LOG" 2>&1)
}

# run_tests <filter>; prints the output, returns the test's status.
run_tests() {
  filter="$1"
  if [ "$filter" = '*' ]; then
    filter='*'
  fi
  (cd "$WORK" && env LD_LIBRARY_PATH="$LDPATH" timeout 170 "./$TARGET" --gtest_filter="$filter" 2>&1)
}

# The line that shows why the test failed: the first assertion, else the last
# line it printed.
failing_line() {
  out="$1"
  line="$(printf '%s\n' "$out" | grep -m1 -A2 -E 'Failure|FAILED  \]|Assertion|runtime error|Segmentation|Abort' | grep -v -E '^(--|[[:space:]]*)$' | head -2 | tr '\n' ' ' | cut -c1-200)"
  if [ -z "$line" ]; then
    line="$(printf '%s\n' "$out" | grep -v '^[[:space:]]*$' | tail -1 | cut -c1-200)"
  fi
  printf '%s' "$line"
}

# The number of tests a run executed, from gtest's summary, or 0.
ran_of() {
  printf '%s\n' "$1" | sed -n 's/^\[==========\] \([0-9]*\) tests\? from .* ran\..*/\1/p' | tail -1
}

# run_case <case> <patch file>; 0 caught with a passing control, 1 not caught or
# the control failed, 2 the patch did not apply.
run_case() {
  case_name="$1"
  patch_file="$2"
  printf 'planted %s\n' "$case_name"
  filter="$(header "$patch_file" 'caught by')"
  if [ -z "$filter" ]; then
    printf '  FAIL: the patch has no "# caught by:" header\n' >&2
    return 2
  fi
  files="$(files_of "$patch_file")"
  if [ -z "$files" ]; then
    printf '  FAIL: the patch names no file\n' >&2
    return 2
  fi
  for f in $files; do
    if [ ! -f "$WORK/$f" ]; then
      printf '  FAIL: the patch names %s, which the copy does not have\n' "$f" >&2
      return 2
    fi
  done
  if ! patch -p1 --forward --fuzz=0 -s -d "$WORK" -i "$patch_file" >/dev/null 2>&1; then
    printf '  FAIL: the patch did not apply (it matched nothing, or only part of it): %s\n' "$patch_file" >&2
    # Whatever it did apply is taken back out.
    for f in $files; do cp "$ROOT/$f" "$WORK/$f"; touch "$WORK/$f"; done
    return 2
  fi
  changed=0
  for f in $files; do
    if ! cmp -s "$ROOT/$f" "$WORK/$f"; then
      changed=1
    fi
  done
  if [ "$changed" = 0 ]; then
    printf '  FAIL: the patch applied and changed nothing\n' >&2
    return 2
  fi
  rc=0
  # The patched copy: build, run, and the run must fail.
  if ! build; then
    printf '  FAIL: the patched copy did not build (see %s); a defect that does not compile is no proof\n' "$LOG" >&2
    rc=1
  else
    out="$(run_tests "$filter")"
    status=$?
    if [ "$status" -eq 0 ]; then
      printf '  NOT CAUGHT: %s passed with the defect in place\n' "$filter" >&2
      rc=1
    elif [ "$status" -eq 124 ]; then
      printf '  NOT CAUGHT: %s timed out (exit 124); a hang is not a failure of the instrument\n' "$filter" >&2
      rc=1
    else
      printf '  caught by %s (exit %s): %s\n' "$filter" "$status" "$(failing_line "$out")"
    fi
  fi
  # The control: the same copy with the patch taken out. touch, because the
  # restored file's modification time is what makes make rebuild it.
  for f in $files; do cp "$ROOT/$f" "$WORK/$f"; touch "$WORK/$f"; done
  if ! build; then
    printf '  FAIL: the control did not build (see %s)\n' "$LOG" >&2
    return 1
  fi
  out="$(run_tests "$filter")"
  status=$?
  ran="$(ran_of "$out")"
  if [ "$status" -ne 0 ]; then
    printf '  CONTROL FAILED: %s fails without the defect (exit %s): %s\n' "$filter" "$status" "$(failing_line "$out")" >&2
    rc=1
  elif [ -z "$ran" ] || [ "$ran" -eq 0 ]; then
    printf '  CONTROL RAN NOTHING: %s matches no test, so a pass proves nothing\n' "$filter" >&2
    rc=1
  else
    printf '  control passes (%s tests)\n' "$ran"
  fi
  return $rc
}

make_copy() {
  rm -rf "$ROOT/build/planted"
  mkdir -p "$WORK"
  : >"$LOG"
  (cd "$ROOT" && tar --exclude=./build --exclude=./docs --exclude=./tests/fuzz -cf - \
    Makefile include src pkgconfig documentation examples tests tools 2>/dev/null) | tar -C "$WORK" -xf -
  if [ ! -f "$WORK/Makefile" ] || [ ! -d "$WORK/src" ]; then
    printf 'check-planted: could not make the copy under %s\n' "$WORK" >&2
    exit 2
  fi
}

failures=0

if [ "$SELFTEST" = 1 ]; then
  make_copy
  printf 'check-planted --selftest\n'
  # The unpatched copy must build for the rest to mean anything.
  if ! build; then
    printf '  FAIL: the copy did not build (see %s)\n' "$LOG" >&2
    exit 1
  fi
  run_case selftest "$PATCHES/selftest/matches-nothing.patch" >/dev/null 2>&1
  rc=$?
  if [ "$rc" -eq 2 ]; then
    printf '  ok   a patch that applies to nothing fails the script\n'
  else
    printf '  FAIL: a patch that applies to nothing did not fail the script (rc %s)\n' "$rc" >&2
    failures=$((failures + 1))
  fi
  run_case selftest "$PATCHES/selftest/harmless.patch" >/dev/null 2>&1
  rc=$?
  if [ "$rc" -eq 1 ]; then
    printf '  ok   a patch that breaks nothing is reported as not caught\n'
  else
    printf '  FAIL: a harmless patch was not reported as not caught (rc %s)\n' "$rc" >&2
    failures=$((failures + 1))
  fi
  if [ "$failures" -ne 0 ]; then
    exit 1
  fi
  printf 'check-planted --selftest: the script fails on a patch that matches nothing and on one that breaks nothing\n'
  exit 0
fi

if [ -n "$NAMED" ]; then
  CASES="$NAMED"
else
  CASES="$(cd "$PATCHES" && ls ./*.patch 2>/dev/null | sed 's|^\./||; s|\.patch$||')"
fi
if [ -z "$CASES" ]; then
  printf 'check-planted: no patches in %s, so this measures nothing\n' "$PATCHES" >&2
  exit 1
fi

make_copy
count=0
for c in $CASES; do
  patch_file="$PATCHES/$c.patch"
  if [ ! -f "$patch_file" ]; then
    printf 'check-planted: no patch %s\n' "$patch_file" >&2
    exit 2
  fi
  count=$((count + 1))
  if ! run_case "$c" "$patch_file"; then
    failures=$((failures + 1))
  fi
done

if [ "$failures" -ne 0 ]; then
  printf 'check-planted: %s of %s planted defects were not caught by the test named for them, or their control failed\n' "$failures" "$count" >&2
  exit 1
fi
printf 'check-planted: all %s planted defects were caught by the test named for them, and each control passed\n' "$count"
