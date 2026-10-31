#!/bin/sh
set -eu
cd "$(dirname "$0")/.."
root=$(pwd)
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT HUP INT TERM
cp src/kboot_gpu_t8142.c src/kboot_gpu_t8142_fields.h tests/t8142_gpu_host.c "$tmp/"
cp tests/t8142_gpu_host/*.h "$tmp/"
${CC:-cc} -std=gnu11 -O1 -g -Wall -Wextra -Wno-sign-compare \
 -fsanitize=address,undefined -fno-sanitize-recover=undefined \
 -I"$tmp" -I"$root/src" "$tmp/t8142_gpu_host.c" "$tmp/kboot_gpu_t8142.c" \
 src/libfdt/fdt.c src/libfdt/fdt_ro.c src/libfdt/fdt_rw.c src/libfdt/fdt_wip.c \
 src/libfdt/fdt_sw.c src/libfdt/fdt_empty_tree.c src/libfdt/fdt_strerror.c \
 src/libfdt/fdt_addresses.c -o "$tmp/host"
"$tmp/host"
