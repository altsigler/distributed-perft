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

# Determine if the test machine is AMD ZEN4 architecture.
# We provide an optimized binary for the AMD ZEN4.
#
cpu_family=$(grep -m 1 'cpu family' /proc/cpuinfo | awk '{print $4}')
cpu_model=$(grep -m 1 'model' /proc/cpuinfo | awk '{print $3}')
cpu_vendor=$(grep -m 1 'vendor_id' /proc/cpuinfo | awk '{print $3}')

AMD_ZEN4=0
if [ "$cpu_vendor" = "AuthenticAMD" ] && [ "$cpu_family" -eq 25 ]; then
    if [ "$cpu_model" -ge 96 ] && [ "$cpu_model" -le 175 ]; then
        AMD_ZEN4=1
    fi
fi

# --- Clone + build ---------------------------------------------------------
clone_or_keep "$ENGINE_DIR" "$REPO"

if [ $AMD_ZEN4 -eq 1 ]; then
  chmod u+x $BINARY-zen4
  ln -s scperft-zen4 $BINARY 
else
  chmod u+x $BINARY-gen
  ln -s scperft-gen $BINARY 
fi


[ -d "$SRC_DIR" ] || die "expected source dir $SRC_DIR — repo layout may have changed"

[ -x "$BINARY" ] || die "expected binary at $BINARY but it's missing"

log "done."
