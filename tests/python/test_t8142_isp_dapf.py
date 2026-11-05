# SPDX-License-Identifier: MIT
"""Power ordering, readback and fail-soft errors in the ISP DAPF handoff."""
from pathlib import Path
import subprocess

from test_j713_dapf_instance import block


def test_t8142_isp_dapf_power_and_faults(tmp_path):
    source = (Path(__file__).resolve().parents[2] / "src/dapf.c").read_text()
    production = (
        block(source, "struct dapf_t8110_config {") + ";\n"
        + block(source, "static int dapf_init_t8110a(") + "\n"
        + block(source, "static int dapf_init_t8142_isp(") + "\n"
        + block(source, "int dapf_init(") + "\n"
    )
    harness = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
typedef uint8_t u8;
typedef uint32_t u32;
typedef unsigned long u64;
#define PACKED __attribute__((packed))
#define T8132 0x8132
#define T8142 0x8142
#define SZ_16K 0x4000
static int chip_id = T8142;
static void *adt;
enum exc_guard_t { GUARD_OFF, GUARD_SKIP, GUARD_SILENT = 0x100 };
static enum exc_guard_t exc_guard;
static int exc_count;
static unsigned char bank[0x4000], instances[6 * 16];
static bool powered, compatible, power_error, power_fault, bad_actual, bad_readback;
static bool write_fault, read_fault, async_fault, absent, no_instance, malformed;
static unsigned writes, reads, powers, checks, barriers;
static u64 bank_base = 0x4a28d0000UL, bank_size;
static void *config_data;
static u32 config_length;
static void sync_mmio(void) {
    assert(exc_guard == (GUARD_SKIP | GUARD_SILENT));
    barriers++;
    if (async_fault && writes) { exc_count++; async_fault = false; }
}
#define sysop(op) sync_mmio()
static int pmgr_power_on(int die, const char *name) {
    assert(!die && !strcmp(name, "ISP_SYS"));
    assert(exc_guard == (GUARD_SKIP | GUARD_SILENT));
    powers++;
    if (power_fault) exc_count++;
    if (power_error) return -1;
    powered = true;
    return 0;
}
static int pmgr_power_is_on(int die, const char *name) {
    assert(!die && !strcmp(name, "ISP_SYS"));
    checks++;
    return bad_actual ? 0 : powered;
}
static const void *adt_getprop(const void *tree, int node, const char *name, u32 *len) {
    if (!strcmp(name, "clock-gates")) return NULL;
    if (!strcmp(name, "instance")) {
        *len = malformed ? sizeof(instances) - 1 : sizeof(instances);
        return no_instance ? NULL : instances;
    }
    assert(!strcmp(name, "dapf-instance-0"));
    *len = config_length;
    return absent ? NULL : config_data;
}
static bool adt_is_compatible(const void *tree, int node, const char *name) {
    return compatible && !strcmp(name, "dart,t8110");
}
static int adt_path_offset_trace(const void *tree, const char *path, int *trace) {
    trace[0] = 1; return 1;
}
static int adt_get_reg(const void *tree, int *trace, const char *name,
                       int index, u64 *base, u64 *size) {
    assert(index == 5); *base = bank_base; *size = bank_size; return 0;
}
static int pmgr_adt_power_enable(const char *path) { assert(0); return -1; }
static int pmgr_adt_power_disable(const char *path) { assert(0); return -1; }
static int dapf_select_t8132(const char *p, int i, int n, int *trace, u32 *instance)
{ assert(0); return -1; }
static int dapf_init_t8020(const char *p, u64 b, int n) { assert(0); return -1; }
static int dapf_init_t8110(const char *p, u64 b, int n) { assert(0); return -1; }
static size_t offset(u64 address, size_t width) {
    assert(powered && checks && !bad_actual);
    assert(exc_guard == (GUARD_SKIP | GUARD_SILENT));
    assert(address >= bank_base && address + width <= bank_base + sizeof(bank));
    return address - bank_base;
}
static void write32(u64 address, u32 value) {
    size_t off = offset(address, 4); writes++;
    if (write_fault) { exc_count++; return; }
    if (bad_readback && off == 0x20) return;
    memcpy(bank + off, &value, 4);
}
static void write64(u64 address, u64 value) {
    size_t off = offset(address, 8); writes++;
    if (write_fault) { exc_count++; return; }
    if (off % 0x40 == 0x10) value &= ~3UL;
    memcpy(bank + off, &value, 8);
}
static u32 read32(u64 address) {
    size_t off = offset(address, 4); u32 value; reads++;
    if (read_fault) exc_count++;
    memcpy(&value, bank + off, 4); return value;
}
static u64 read64(u64 address) {
    size_t off = offset(address, 8); u64 value; reads++;
    if (read_fault) exc_count++;
    memcpy(&value, bank + off, 8); return value;
}
''' + production + r'''
static struct dapf_t8110_config configs[2];
static void reset(void) {
    memset(bank, 0, sizeof(bank));
    memset(instances, 0, sizeof(instances));
    memcpy(instances + 5 * 16, "FPAD", 4);
    configs[0] = (struct dapf_t8110_config){.start=0x380704000UL,
        .end=0x380704063UL, .r20=0xcc3, .r0_hi=3, .r0_lo=1};
    configs[1] = (struct dapf_t8110_config){.start=0x220104000UL,
        .end=0x2201162fcUL, .r20=0xcc3, .r0_hi=3, .r0_lo=1};
    config_data = configs; config_length = sizeof(configs); bank_size = sizeof(bank);
    powered = power_error = power_fault = bad_actual = bad_readback = false;
    write_fault = read_fault = async_fault = absent = no_instance = malformed = false;
    compatible = true;
    writes = reads = powers = checks = barriers = exc_count = 0;
    exc_guard = GUARD_OFF;
}
static int run(void) {
    int ret = dapf_init("/arm-io/dart-isp", 5);
    assert(exc_guard == GUARD_OFF);
    return ret;
}
int main(void) {
    reset(); assert(!run());
    assert(powered && powers == 1 && checks == 1 && writes == 10 && reads == 10);
    assert(barriers >= 4);
    /* No enable or DAPF access on invalid metadata. */
    for (int i = 0; i < 9; i++) {
        reset();
        if (i == 0) absent = true;
        if (i == 1) config_length--;
        if (i == 2) config_length = 0;
        if (i == 3) bank_size = 0x40;
        if (i == 4) configs[0].start = configs[0].end + 1;
        if (i == 5) compatible = false;
        if (i == 6) no_instance = true;
        if (i == 7) malformed = true;
        if (i == 8) memset(instances, 0, sizeof(instances));
        assert(run() < 0 && !powers && !writes && !reads);
    }
    for (int i = 0; i < 3; i++) {
        reset();
        if (i == 0) power_error = true;
        if (i == 1) power_fault = true;
        if (i == 2) bad_actual = true;
        assert(run() < 0 && powers == 1 && !writes && !reads);
    }
    for (int i = 0; i < 4; i++) {
        reset();
        if (i == 0) bad_readback = true;
        if (i == 1) write_fault = true;
        if (i == 2) read_fault = true;
        if (i == 3) async_fault = true;
        assert(run() < 0 && powered);
    }
    reset(); exc_guard = GUARD_SILENT;
    assert(!dapf_init("/arm-io/dart-isp0", 5) && exc_guard == GUARD_SILENT);
    return 0;
}
'''
    binary = tmp_path / "isp-dapf"
    subprocess.run(["cc", "-std=gnu11", "-Wall", "-Wextra", "-Werror",
                    "-Wno-unused-parameter", "-fsanitize=address,undefined",
                    "-x", "c", "-", "-o", str(binary)],
                   input=harness, text=True, check=True)
    subprocess.run([str(binary)], check=True)
