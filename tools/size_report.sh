#!/usr/bin/env bash
# Cross-compile size report: .text/.data/.bss of the LYNX objects (src/*.c, summed)
# and sizeof(lx_t) per target.
#
#   tools/size_report.sh [extra cflags...]        e.g. tools/size_report.sh -DLX_BUS=1
#
# Env:
#   BAVA_DIR=/path/to/bavaprotocol   also build BAVA (black box: src/*.c + include/)
#   LX_BUDGET_CM3=<bytes>            fail if Cortex-M3 .text exceeds this (CI guard)
#
# Targets whose compiler is not on PATH are reported as "n/a".
set -euo pipefail
ROOT=$(cd "$(dirname "$0")/.." && pwd)
OUT=$(mktemp -d); trap 'rm -rf "$OUT"' EXIT
EXTRA=("$@")
COMMON="-Os -ffunction-sections -fdata-sections"

#        label                     | compiler               | target flags
TARGETS=(
  "x86-64 host                     |gcc                    |"
  "Cortex-M3                       |arm-none-eabi-gcc      |-mcpu=cortex-m3 -mthumb"
  "Cortex-M0+                      |arm-none-eabi-gcc      |-mcpu=cortex-m0plus -mthumb"
  "ESP32 (Xtensa LX6)              |xtensa-esp32-elf-gcc   |-mlongcalls"
  "ESP32-S3 (Xtensa LX7)           |xtensa-esp32s3-elf-gcc |-mlongcalls"
  "ESP32-C3 (RV32IMC)              |riscv32-esp-elf-gcc    |-march=rv32imc_zicsr_zifencei -mabi=ilp32"
)

trim() { local s=$1; s=${s#"${s%%[![:space:]]*}"}; printf '%s' "${s%"${s##*[![:space:]]}"}"; }

# sum_size <size-tool> <objs...>  ->  "text data bss"
sum_size() { local sz=$1; shift; "$sz" -t "$@" | tail -1 | awk '{print $1, $2, $3}'; }

# sizeof_sym <nm-tool> <obj> <sym>
sizeof_sym() { printf '%d' "0x$("$1" -S "$2" | awk -v s="$3" '$4==s{print $2}')"; }

hdr="| Target | LYNX .text | .data | .bss | sizeof(lx_t) |"
sep="|---|---:|---:|---:|---:|"
[ -n "${BAVA_DIR:-}" ] && { hdr+=" BAVA .text | .data | .bss | sizeof(bava_handle_t) |"; sep+="---:|---:|---:|---:|"; }
echo "Flags: $COMMON ${EXTRA[*]:-}"
echo; echo "$hdr"; echo "$sep"

status=0
for t in "${TARGETS[@]}"; do
  IFS='|' read -r label cc tflags <<<"$t"
  label=$(trim "$label"); cc=$(trim "$cc"); tflags=$(trim "$tflags")
  if ! command -v "$cc" >/dev/null; then
    row="| $label | n/a | | | |"; [ -n "${BAVA_DIR:-}" ] && row+=" n/a | | | |"
    echo "$row"; continue
  fi
  pre=${cc%gcc}; d="$OUT/$pre${label//[^A-Za-z0-9]/_}"; mkdir -p "$d"
  lx_objs=()
  for s in "$ROOT"/src/*.c; do
    o="$d/lynx_$(basename "$s" .c).o"
    # shellcheck disable=SC2086
    "$cc" -std=c99 -Wall -Wextra -Werror -Wconversion $COMMON $tflags "${EXTRA[@]}" \
          -I"$ROOT/src" -c "$s" -o "$o"; lx_objs+=("$o")
  done
  printf '#include "lynx.h"\nlx_t lx_probe;\n' > "$d/p.c"
  # shellcheck disable=SC2086
  "$cc" -std=c99 -fno-common $COMMON $tflags "${EXTRA[@]}" -I"$ROOT/src" -c "$d/p.c" -o "$d/p.o"
  read -r tx da bs <<<"$(sum_size "${pre}size" "${lx_objs[@]}")"
  row="| $label | $tx | $da | $bs | $(sizeof_sym "${pre}nm" "$d/p.o" lx_probe) |"

  if [ "$label" = "Cortex-M3" ] && [ -n "${LX_BUDGET_CM3:-}" ] && [ "$tx" -gt "$LX_BUDGET_CM3" ]; then
    echo "::error::Cortex-M3 .text $tx B exceeds budget $LX_BUDGET_CM3 B" >&2; status=1
  fi

  if [ -n "${BAVA_DIR:-}" ]; then
    objs=()
    for s in "$BAVA_DIR"/src/*.c; do
      o="$d/bava_$(basename "$s" .c).o"
      # shellcheck disable=SC2086
      "$cc" -std=gnu11 -w $COMMON $tflags -I"$BAVA_DIR/include" -c "$s" -o "$o"; objs+=("$o")
    done
    printf '#include "bava.h"\nbava_handle_t bava_probe;\n' > "$d/q.c"
    # shellcheck disable=SC2086
    "$cc" -std=gnu11 -fno-common $COMMON $tflags -I"$BAVA_DIR/include" -c "$d/q.c" -o "$d/q.o"
    read -r tx da bs <<<"$(sum_size "${pre}size" "${objs[@]}")"
    row+=" $tx | $da | $bs | $(sizeof_sym "${pre}nm" "$d/q.o" bava_probe) |"
  fi
  echo "$row"
done
exit $status
