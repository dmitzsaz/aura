# SPDX-License-Identifier: MIT AND GPL-2.0-only
"""Run the production ADT parser and RAM/DT setup against MTE carve-out fixtures."""

from pathlib import Path
import struct
import subprocess

import pytest

from test_siliconforest_handoff import definition

ROOT = Path(__file__).resolve().parents[2]
DRAM_BASE = 0x10000000000
DRAM_SIZE = 0x800000000
TAG_BASE = 0x1078E200000
TAG_SIZE = DRAM_SIZE // 32


def adt_node(name, props=(), children=()):
    props = [("name", name.encode() + b"\0"), *props]
    blob = struct.pack("<II", len(props), len(children))
    for key, value in props:
        blob += key.encode().ljust(32, b"\0") + struct.pack("<I", len(value))
        blob += value + bytes(-len(value) % 4)
    return blob + b"".join(children)


def fixture_adt(base=DRAM_BASE, size=DRAM_SIZE, region=(TAG_BASE, TAG_SIZE),
                region_bytes=None, chosen=True, carveouts=True, dram_bytes=None):
    props = [("dram-base", struct.pack("<Q", base)),
             ("dram-size", struct.pack("<Q", size) if dram_bytes is None else dram_bytes)]
    regions = [] if region is None else [
        ("region-id-218", struct.pack("<QQ", *region) if region_bytes is None else region_bytes)]
    children = [adt_node("carveout-memory-map", regions)] if carveouts else []
    return adt_node("device-tree", children=[adt_node("chosen", props, children)] if chosen else [])


def read_fdt(path):
    blob = path.read_bytes()
    off, strings = struct.unpack_from(">II", blob, 8)
    stack, nodes = [], {}
    while True:
        token, = struct.unpack_from(">I", blob, off)
        off += 4
        if token == 1:
            end = blob.index(0, off)
            stack.append(blob[off:end].decode())
            off = (end + 4) & ~3
            nodes["/".join(stack) or "/"] = {}
        elif token == 2:
            stack.pop()
        elif token == 3:
            length, nameoff = struct.unpack_from(">II", blob, off)
            off += 8
            end = blob.index(0, strings + nameoff)
            name = blob[strings + nameoff:end].decode()
            nodes["/".join(stack) or "/"][name] = blob[off:off + length]
            off = (off + length + 3) & ~3
        elif token == 9:
            return nodes
        else:
            assert token == 4


def build_harness(directory):
    rust = directory / "adt-host.rs"
    rust.write_text(
        '#![allow(non_camel_case_types, dead_code)]\n'
        'type c_size_t = usize;\n'
        f'#[path = "{ROOT / "rust/src/adt.rs"}"] mod parser;\n')
    library = directory / "libadt.a"
    subprocess.run(["rustc", "--edition=2021", "--crate-type=staticlib",
                    "-C", "panic=abort", str(rust), "-o", str(library)], check=True)
    memory = (ROOT / "src/memory.c").read_text()
    kboot = (ROOT / "src/kboot.c").read_text()
    # Extract void/bool definitions without changing their bodies.
    for name, signature in (("memory_get_mte_tag_storage", "bool "),
                            ("memory_reserve_mte_tag_storage", "void ")):
        start = memory.index(signature + name + "(")
        body = memory[start:memory.index("\n}", start) + 2]
        if name == "memory_get_mte_tag_storage":
            functions = body
        else:
            functions += "\n" + body
    functions += "\n" + definition(kboot, "dt_reserve_mte_tag_storage")
    functions += "\n" + definition(kboot, "dt_set_memory")
    utils = (ROOT / "src/utils.c").read_text()
    start = utils.index("u64 top_of_memory_alloc(")
    functions += "\n" + utils[start:utils.index("\n}", start) + 2]
    source = directory / "mte-memory.c"
    source.write_text(r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "libfdt.h"
typedef unsigned long u64;
typedef uint32_t u32;
_Static_assert(sizeof(u64) == 8, "LP64 required");
#define T8142 0x8142
#define SZ_16K 16384
#define MAX_MEM_REGIONS 32
#define ALIGN_UP(x, a) (((x) + (a) - 1) & ~((a) - 1))
#define bail(...) do { printf(__VA_ARGS__); return -1; } while (0)
#define panic(...) do { printf(__VA_ARGS__); exit(2); } while (0)
#define ADT_GETPROP(a, n, k, v) adt_getprop_copy(a, n, k, v, sizeof(*(v)))
#define ADT_GETPROP_ARRAY(a, n, k, v) adt_getprop_copy(a, n, k, v, sizeof(v))
void *adt;
static void *dt;
static u32 adt_size, chip_id;
static u64 mem_size_actual, heap_limit;
static struct { u64 phys_base, mem_size, top_of_kernel_data; } cur_boot_args;
u32 adt_get_size(void) { return adt_size; }
extern int adt_path_offset(const void *, const char *);
extern int adt_getprop_copy(const void *, int, const char *, void *, size_t);
static u64 get_page_size(void) { return SZ_16K; }
static u64 heapblock_high_water(void) { return cur_boot_args.top_of_kernel_data; }
static void heapblock_set_limit(void *p) { heap_limit = (u64)p; }
''' + functions + r'''
int main(int argc, char **argv)
{
    assert(argc == 8 || argc == 9);
    FILE *file = fopen(argv[1], "rb");
    assert(file && !fseek(file, 0, SEEK_END));
    adt_size = ftell(file);
    rewind(file);
    adt = malloc(adt_size);
    assert(adt && fread(adt, 1, adt_size, file) == adt_size);
    fclose(file);
    cur_boot_args.phys_base = strtoul(argv[3], NULL, 0);
    cur_boot_args.mem_size = strtoul(argv[4], NULL, 0);
    cur_boot_args.top_of_kernel_data = cur_boot_args.phys_base + SZ_16K;
    chip_id = strtoul(argv[5], NULL, 0);
    mem_size_actual = 0x800000000;
    int variant = atoi(argv[6]);
    unsigned char blob[32768];
    dt = blob;
    assert(!fdt_create_empty_tree(dt, sizeof(blob)));
    assert(!fdt_setprop_u32(dt, 0, "#address-cells", 2));
    assert(!fdt_setprop_u32(dt, 0, "#size-cells", 2));
    int parent = fdt_add_subnode(dt, 0, "reserved-memory");
    assert(parent >= 0);
    assert(!fdt_setprop_u32(dt, parent, "#address-cells", variant == 2 ? 1 : 2));
    assert(!fdt_setprop_u32(dt, parent, "#size-cells", 2));
    assert(!fdt_setprop_empty(dt, parent, "ranges"));
    if (variant == 1)
        assert(!fdt_del_node(dt, parent));
    if (variant == 3)
        assert(!fdt_setprop_u32(dt, parent, "ranges", 0));
    if (variant == 4) {
        for (int i = 0; i < MAX_MEM_REGIONS - 1; i++) {
            char name[40];
            snprintf(name, sizeof(name), "firmware@%x", 0x10000 + i * SZ_16K);
            int node = fdt_add_subnode(dt, parent, name);
            fdt64_t reg[2] = {cpu_to_fdt64(0x10000 + i * SZ_16K), cpu_to_fdt64(SZ_16K)};
            assert(node >= 0 && !fdt_setprop(dt, node, "reg", reg, sizeof(reg)));
        }
    }
    int node = fdt_add_subnode(dt, 0, "memory");
    assert(node >= 0 && !fdt_setprop_string(dt, node, "device_type", "memory"));
    if (argc == 9) {
        file = fopen(argv[8], "rb");
        assert(file && !fseek(file, 0, SEEK_END));
        long length = ftell(file);
        assert(length > 0);
        rewind(file);
        void *template = malloc(length);
        assert(template && fread(template, 1, length, file) == (size_t)length);
        fclose(file);
        dt = malloc(length + sizeof(blob));
        assert(dt && !fdt_open_into(template, dt, length + sizeof(blob)));
        free(template);
    }
    if (atoi(argv[7])) {
        memory_reserve_mte_tag_storage();
        if (heap_limit)
            assert(heap_limit == cur_boot_args.phys_base + cur_boot_args.mem_size);
    }
    if (variant == 5) {
        u64 first = top_of_memory_alloc(SZ_16K);
        u64 second = top_of_memory_alloc(2 * SZ_16K);
        assert(heap_limit && first + SZ_16K <= heap_limit);
        assert(second + 2 * SZ_16K == first);
    }
    if (dt_set_memory())
        return 1;
    assert(!fdt_pack(dt));
    file = fopen(argv[2], "wb");
    assert(file && fwrite(dt, 1, fdt_totalsize(dt), file) == fdt_totalsize(dt));
    assert(!fclose(file));
    free(adt);
    return 0;
}
''')
    lib = ROOT / "src/libfdt"
    binary = directory / "mte-memory"
    subprocess.run(["cc", "-std=gnu11", "-Wall", "-Wextra", "-Werror",
                    "-I", str(lib), str(source),
                    *map(str, (lib / f for f in ("fdt.c", "fdt_ro.c", "fdt_rw.c", "fdt_sw.c",
                                               "fdt_wip.c", "fdt_empty_tree.c", "fdt_strerror.c", "fdt_addresses.c"))),
                    str(library), "-lpthread", "-ldl", "-lm", "-o", str(binary)], check=True)
    return binary


@pytest.fixture(scope="module")
def harness(tmp_path_factory):
    return build_harness(tmp_path_factory.mktemp("mte-memory"))


def run(harness, tmp_path, blob=None, start=DRAM_BASE, size=DRAM_SIZE, chip=0x8142,
        variant=0, limit=False, success=True):
    source, output = tmp_path / "input.adt", tmp_path / "output.dtb"
    source.write_bytes(fixture_adt() if blob is None else blob)
    result = subprocess.run([str(harness), str(source), str(output), hex(start), hex(size),
                             hex(chip), str(variant), str(int(limit))], capture_output=True, text=True)
    assert (result.returncode == 0) == success, result.stdout + result.stderr
    return (read_fdt(output) if success else None), result.stdout


def memory_ranges(nodes):
    props = next(props for name, props in nodes.items()
                 if name == "/memory" or name.startswith("/memory@"))
    return list(struct.iter_unpack(">QQ", props["reg"]))


def test_tag_storage_is_nomap_and_not_ram(harness, tmp_path):
    nodes, _ = run(harness, tmp_path)
    props = nodes[f"/reserved-memory/mte-tag-storage@{TAG_BASE:x}"]
    assert props == {"reg": struct.pack(">QQ", TAG_BASE, TAG_SIZE), "no-map": b""}
    assert memory_ranges(nodes) == [(DRAM_BASE, TAG_BASE - DRAM_BASE),
                                    (TAG_BASE + TAG_SIZE, DRAM_BASE + DRAM_SIZE - TAG_BASE - TAG_SIZE)]


@pytest.mark.parametrize("start,size,expected", [
    (DRAM_BASE, TAG_BASE - DRAM_BASE, [(DRAM_BASE, TAG_BASE - DRAM_BASE)]),
    (DRAM_BASE, TAG_BASE - DRAM_BASE + 0x10000, [(DRAM_BASE, TAG_BASE - DRAM_BASE)]),
    (TAG_BASE, TAG_SIZE + 0x10000, [(TAG_BASE + TAG_SIZE, 0x10000)]),
    (TAG_BASE + TAG_SIZE, 0x10000, [(TAG_BASE + TAG_SIZE, 0x10000)]),
])
def test_ram_intersections(harness, tmp_path, start, size, expected):
    nodes, _ = run(harness, tmp_path, start=start, size=size)
    assert memory_ranges(nodes) == expected


def test_aura_allocations_are_below_tags(harness, tmp_path):
    nodes, log = run(harness, tmp_path, size=TAG_BASE + TAG_SIZE - DRAM_BASE,
                     limit=True, variant=5)
    assert memory_ranges(nodes) == [(DRAM_BASE, TAG_BASE - DRAM_BASE - 4 * 0x4000)]
    assert "limiting usable RAM" in log


@pytest.mark.parametrize("kwargs", [
    {"region": None}, {"carveouts": False},
    {"region_bytes": bytes(8)}, {"region_bytes": bytes(24)},
    {"dram_bytes": bytes(4)}, {"size": 0}, {"size": DRAM_SIZE + 1},
    {"region": (TAG_BASE, 0)}, {"region": (TAG_BASE, TAG_SIZE // 2)},
    {"region": (TAG_BASE + 1, TAG_SIZE)},
    {"region": (DRAM_BASE - TAG_SIZE, TAG_SIZE)},
    {"region": (DRAM_BASE + DRAM_SIZE - TAG_SIZE + 0x4000, TAG_SIZE)},
    {"region": ((1 << 64) - 0x4000, TAG_SIZE)},
    {"base": (1 << 64) - 0x4000},
])
def test_invalid_adt_warns_without_reserving(harness, tmp_path, kwargs):
    nodes, log = run(harness, tmp_path, blob=fixture_adt(**kwargs))
    assert not any("mte-tag-storage" in path for path in nodes)
    assert memory_ranges(nodes) == [(DRAM_BASE, DRAM_SIZE)]
    assert "WARNING:" in log


def test_other_socs_do_not_interpret_region_218(harness, tmp_path):
    nodes, log = run(harness, tmp_path, chip=0x8132)
    assert not any("mte-tag-storage" in path for path in nodes)
    assert memory_ranges(nodes) == [(DRAM_BASE, DRAM_SIZE)]
    assert "WARNING:" not in log


def test_range_comes_from_adt_on_another_memory_configuration(harness, tmp_path):
    base, size = DRAM_BASE * 2, DRAM_SIZE // 2
    tag_base, tag_size = base + size - size // 32, size // 32
    nodes, _ = run(harness, tmp_path, blob=fixture_adt(base, size, (tag_base, tag_size)),
                    start=base, size=size, limit=True)
    assert memory_ranges(nodes) == [(base, tag_base - base)]
    assert nodes[f"/reserved-memory/mte-tag-storage@{tag_base:x}"]["reg"] == struct.pack(
        ">QQ", tag_base, tag_size)


@pytest.mark.parametrize("variant", [1, 2, 3, 4])
def test_invalid_dt_or_full_memory_map_fails(harness, tmp_path, variant):
    run(harness, tmp_path, variant=variant, success=False)


def test_no_usable_ram_fails(harness, tmp_path):
    run(harness, tmp_path, start=TAG_BASE, size=TAG_SIZE, success=False)
