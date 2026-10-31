#!/bin/sh
set -eu
cd "$(dirname "$0")/.."
root=$(pwd)
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT HUP INT TERM
python3 - "$root/src/kboot.c" "$tmp/j704_converter.inc" <<'PY'
import pathlib, sys
s=pathlib.Path(sys.argv[1]).read_text()
a=s.index('struct adt_tunable_info {'); b=s.index('\n};',a)+3
c=s.index('struct acio_tunable {'); d=s.index('\n#include "kboot_j704_platform.h"',c)
s=s[a:b]+'\n'+s[c:d]
s=s.replace('static_assert(', '_Static_assert(')
pathlib.Path(sys.argv[2]).write_text(s)
PY
${CC:-cc} -std=gnu11 -O1 -g -Wall -Wextra -Wno-sign-compare \
    -fsanitize=address,undefined -fno-sanitize-recover=undefined \
    -I"$tmp" -I"$root/src" tests/j704_platform_host.c \
    src/libfdt/fdt.c src/libfdt/fdt_ro.c src/libfdt/fdt_rw.c src/libfdt/fdt_wip.c \
    src/libfdt/fdt_sw.c src/libfdt/fdt_empty_tree.c src/libfdt/fdt_strerror.c \
    src/libfdt/fdt_addresses.c -o "$tmp/host"
"$tmp/host"
