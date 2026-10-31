#!/bin/sh
set -eu
cd "$(dirname "$0")/.."
root=$(pwd)
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT HUP INT TERM
# Compile the actual production declarations and function bodies. Only the
# platform I/O, allocator and unused mutation dependencies are host stubs.
python3 - "$root/src/dart.c" "$tmp/dart_inherited.inc" <<'PY'
from pathlib import Path
import sys
s=Path(sys.argv[1]).read_text()
def fn(name):
 import re
 m=re.search(r'^.*\b'+name+r'\([^;]*?\n\{',s,re.M)
 assert m,name
 end=s.index('\n}',m.end())+2
 return s[m.start():end]+'\n'
a=s.index('#define DART_T8020_CONFIG');b=s.index('/* A retained DCP root')
out=s[a:b]
a=s.index('const struct dart_params dart_t8110');b=s.index('\n};',a)+3
out+=s[a:b]+'\n'
for name in ['dart_view_table_valid','dart_inherited_tcr_valid','dart_init_fdt_inherited','dart_table_in_dram','dart_collect_locked_tables','dart_setup_pt_region','dart_map_flags','dart_unmap','dart_free_l2','dart_translate_internal','dart_translate','dart_translate_silent','dart_search_range','dart_walk_inherited_range','dart_shutdown']:
 out+=fn(name)
s=Path(sys.argv[1]).with_name('kboot.c').read_text()
out+=fn('dt_init_dart_by_node')
out+=fn('dt_ensure_phandle')
for name in ['dt_get_iommu_node','dt_device_set_reserved_mem','dt_device_add_mem_region']:
 out+=fn(name)
a=s.index('struct t8142_display_reservation {');b=s.index('\n};',a)+3
out+=s[a:b]+'\n'
for name in ['dt_t8142_reserve_iova','dt_t8142_reserve_fragmented']:
 out+=fn(name)
Path(sys.argv[2]).write_text(out)
PY
${CC:-cc} -std=gnu11 -O1 -g -Wall -Wextra -Wno-sign-compare \
 -fsanitize=address,undefined -fno-sanitize-recover=undefined \
 -I"$tmp" -I"$root/tests/t8142_gpu_host" -I"$root/src" tests/dart_inherited_host.c \
 src/libfdt/fdt.c src/libfdt/fdt_ro.c src/libfdt/fdt_rw.c src/libfdt/fdt_wip.c \
 src/libfdt/fdt_sw.c src/libfdt/fdt_empty_tree.c src/libfdt/fdt_strerror.c \
 src/libfdt/fdt_addresses.c -o "$tmp/host"
"$tmp/host"
