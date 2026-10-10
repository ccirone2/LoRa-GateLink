#!/bin/sh
# check_sections.sh <objdump> <firmware objects...>
# The libFuzzer build restores the firmware's RAM before every input from the gl_bss/gl_data sections (sections.h).
# A firmware variable anywhere else (.bss, .data) would keep its value from one input to the next, so list any the
# objects still define there, other than the sanitizers' own bookkeeping (ASan ODR indicators and registration
# flags, UBSan and coverage data, compiler-local labels, the exception personality reference), and fail if there
# are some.
objdump="$1"
shift
bad=$(for o in "$@"; do
  "$objdump" -t "$o" | grep -E '[[:space:]]O[[:space:]]+\.(bss|data)([.][^[:space:]]*)?[[:space:]]' \
    | grep -vE '[[:space:]]\.data\.rel\.ro' \
    | awk '{ print $NF }' \
    | grep -vE '^(__odr_asan_gen_|___asan_globals_registered|\.L|__sancov_|__ubsan_|DW\.ref\.)' \
    | sed "s|^|  $o: |"
done)
if [ -n "$bad" ]; then
  echo "firmware variables outside gl_bss/gl_data (their state would leak between fuzz inputs):"
  echo "$bad"
  exit 1
fi
echo "check_sections: every firmware variable is in gl_bss/gl_data ($# objects)"
