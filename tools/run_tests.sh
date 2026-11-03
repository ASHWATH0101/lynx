#!/usr/bin/env bash
# Build lynx.c warning-clean and run the host test suite and the CRC-valid
# frame fuzzer (test/fuzz_frames.c) under ASan+UBSan, once per compile-time
# configuration.
#
#   tools/run_tests.sh            all configs
#   CC=clang tools/run_tests.sh   another compiler
set -euo pipefail
ROOT=$(cd "$(dirname "$0")/.." && pwd)
OUT=$(mktemp -d); trap 'rm -rf "$OUT"' EXIT
CC=${CC:-gcc}
STRICT="-std=c99 -Wall -Wextra -Werror -Wconversion -Wshadow -Wcast-align -pedantic"
SAN="-O2 -g -fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer"

CONFIGS=(
  ""
  "-DLX_LEAD_DELIM=1"
  "-DLX_CTZ_BUILTIN=0"
  "-DLX_BUS=1"
  "-DLX_BUS=1 -DLX_LEAD_DELIM=1"
  "-DLX_PORT -I$ROOT/test/port"               # critical-section contract checker + race tests
  "-DLX_PORT -I$ROOT/test/port -DLX_BUS=1"
  "-DLX_ISR_SAFE=1"
  "-DLX_ISR_SAFE=1 -DLX_RXQ_SIZE=16"
  "-DLX_ISR_SAFE=1 -DLX_RXQ_SIZE=256 -DLX_BUS=1"
  "-DLX_PORT -I$ROOT/test/port -DLX_ISR_SAFE=1"
  "-DLX_MAX_VARS=8u -DLX_MAX_PAYLOAD=250u"    # non-default limits; largest frame = one full COBS block
  "-DLX_BUS=1 -DLX_MAX_PAYLOAD=248u -DLX_ISR_SAFE=1"
)

for cfg in "${CONFIGS[@]}"; do
  echo "=== config: ${cfg:-default}"
  # shellcheck disable=SC2086
  {
    "$CC" $STRICT -Os $cfg -I"$ROOT" -c "$ROOT/lynx.c" -o "$OUT/strict.o"
    "$CC" $STRICT $SAN $cfg -I"$ROOT" -c "$ROOT/lynx.c" -o "$OUT/lynx.o"
    "$CC" -std=c99 -Wall -Wextra -Werror $SAN $cfg -I"$ROOT" "$OUT/lynx.o" "$ROOT/test/test_lynx.c" -o "$OUT/t"
    "$CC" -std=c99 -Wall -Wextra -Werror $SAN $cfg -I"$ROOT" "$OUT/lynx.o" "$ROOT/test/fuzz_frames.c" -o "$OUT/fz"
  }
  if "$OUT/t" > "$OUT/log" 2>&1 && "$OUT/fz" >> "$OUT/log" 2>&1; then
    grep -E "delivered|critical sections|CRC-valid|PASSED" "$OUT/log"
  else
    cat "$OUT/log"; echo "FAILED: ${cfg:-default}"; exit 1
  fi
done
echo "ALL CONFIGS PASSED"

# Configurations lynx.h must reject with an #error instead of building something broken.
BAD=(
  "-DLX_MAX_VARS=33u"
  "-DLX_MAX_PAYLOAD=251u"
  "-DLX_BUS=1 -DLX_MAX_PAYLOAD=249u"
  "-DLX_ISR_SAFE=1 -DLX_RXQ_SIZE=100u"
  "-DLX_ISR_SAFE=1 -DLX_RXQ_SIZE=512u"
  "-DLX_RETRIES=256u"                         # p_tries is 8 bits: would never time out
)
for cfg in "${BAD[@]}"; do
  # shellcheck disable=SC2086
  if "$CC" -std=c99 $cfg -I"$ROOT" -c "$ROOT/lynx.c" -o "$OUT/bad.o" 2> "$OUT/err"; then
    echo "FAILED: $cfg compiled"; exit 1
  fi
  grep -q "must be" "$OUT/err" || { cat "$OUT/err"; echo "FAILED: $cfg not rejected by its #error"; exit 1; }
done
echo "invalid configs rejected: ${#BAD[@]}"

# Real threads under ThreadSanitizer: RX path and app path of one node in
# different threads, lossy link, short timeouts (test/test_threads.c).
TSAN="-std=c99 -O1 -g -Wall -Wextra -Werror -fsanitize=thread -pthread -DLX_TIMEOUT_MS=2u"
TCONFIGS=(
  "-DLX_PORT -I$ROOT/test/port_mutex"         # RX thread decodes, critical sections = mutex
  "-DLX_PORT -I$ROOT/test/port_mutex -DLX_BUS=1"
  "-DLX_ISR_SAFE=1"                           # RX thread only queues, app thread polls
  "-DLX_ISR_SAFE=1 -DLX_RXQ_SIZE=16 -DLX_BUS=1"
)
for cfg in "${TCONFIGS[@]}"; do
  echo "=== threads (TSan): $cfg"
  # shellcheck disable=SC2086
  "$CC" $TSAN $cfg -I"$ROOT" "$ROOT/lynx.c" "$ROOT/test/test_threads.c" -o "$OUT/threads"
  TSAN_OPTIONS="halt_on_error=1" "$OUT/threads" || { echo "FAILED: threads $cfg"; exit 1; }
done

# Benchmark harness over a simulated UART (extras/bench/host/sim.c); with BAVA if BAVA_DIR is set.
echo "=== benchmark harness simulation${BAVA_DIR:+ (with BAVA)}"
B="$ROOT/extras/bench"
SIMSRC=("$B/host/sim.c" "$B/bench_core.c" "$ROOT/lynx.c")
SIMDEF=()
if [ -n "${BAVA_DIR:-}" ]; then
  SIMDEF=(-DBENCH_WITH_BAVA)
  for s in "$BAVA_DIR"/src/*.c; do      # black box: compiled, never modified
    "$CC" -std=gnu11 -O2 -w $SAN -I"$BAVA_DIR/include" -c "$s" -o "$OUT/bava_$(basename "$s" .c).o"
  done
  "$CC" -std=c99 -O2 -w $SAN -I"$BAVA_DIR/include" -I"$B" -c "$B/bench_bava.c" -o "$OUT/bava_adapter.o"
  SIMSRC+=("$OUT"/bava_*.o)
fi
# shellcheck disable=SC2086
"$CC" -std=c99 -Wall -Wextra -Werror $SAN "${SIMDEF[@]}" -I"$ROOT" -I"$B" "${SIMSRC[@]}" -o "$OUT/sim"
"$OUT/sim" > "$OUT/simlog" || { cat "$OUT/simlog"; exit 1; }
tail -1 "$OUT/simlog"
"$OUT/sim" 0.001 > "$OUT/simlog" || { cat "$OUT/simlog"; echo "lossy simulation did not finish"; exit 1; }
echo "lossy line (0.1% bytes dropped): completed"
