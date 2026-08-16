#!/usr/bin/env bash

ENGINE="distributed-perft"
REPO="https://github.com/altsigler/distributed-perft"
ENGINE_DIR="bin/distributed-perft"
SRC_DIR=$ENGINE_DIR   # source files live in repo/

BINARY="$SRC_DIR/bin/scperft" # Use pre-built binary. No need to compile anything.

# shellcheck source=_common.sh
source "$(cd "$(dirname "$0")" && pwd)/_common.sh"

# --- Preflight -------------------------------------------------------------
HOST=$(detect_host)
case "$HOST" in
  *-x86_64) ;;
  *) die "distributed-perft uses BMI/BMI2/AVX2 — x86 only. Detected: $HOST" ;;
esac

# --- Clone + build ---------------------------------------------------------
clone_or_keep "$ENGINE_DIR" "$REPO"
chmod u+x $BINARY

[ -d "$SRC_DIR" ] || die "expected source dir $SRC_DIR — repo layout may have changed"

[ -x "$BINARY" ] || die "expected binary at $BINARY but it's missing"

log "done."
