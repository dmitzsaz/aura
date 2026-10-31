/* SPDX-License-Identifier: MIT */
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "libfdt/libfdt.h"
typedef uint32_t u32;
typedef uint64_t u64;
#define PACKED __attribute__((packed))
#define T8142 0x8142
static unsigned chip_id = T8142;
static char tree[262144];
static void *dt = tree, *adt;
static int missing_node = -1, bad_prop = -1;
static size_t length;
static u32 cpu_rows[4] = {1000000,800,2000000,900};
static u32 cpu_bytes = sizeof(cpu_rows);
static const char *paths[] = {"/arm-io/apcie", "/arm-io/apcie/pci-bridge0", "/arm-io/apcie/pci-bridge2"};
static unsigned char record[24 * 4097];
static int adt_path_offset(void *unused, const char *path)
{
    (void)unused;
    for (int i = 0; i < 3; i++)
        if (!strcmp(path, paths[i])) return i == missing_node ? -1 : i;
    if (!strcmp(path,"/arm-io/pmgr")) return 3;
    return -1;
}
static const void *adt_getprop(void *unused, int node, const char *name, u32 *size)
{
    (void)unused; (void)name;
    if (node == 3) { *size = cpu_bytes; return cpu_rows; }
    *size = length;
    return node == bad_prop ? NULL : record;
}
/* The runner extracts the exact production converter, not a copy. */
#include "j704_converter.inc"
#include "kboot_j704_platform.h"
static void reset(void)
{
    assert(!fdt_create_empty_tree(dt, sizeof(tree)));
    assert(!fdt_setprop_string(dt, 0, "compatible", "apple,j704"));
    int n = fdt_add_subnode(dt, 0, "j704-platform-data");
    assert(n >= 0);
    assert(!fdt_setprop_string(dt, n, "compatible", "apple,j704-platform-data"));
    assert(fdt_add_subnode(dt, n, "shared") >= 0);
    n = fdt_path_offset(dt, "/j704-platform-data");
    assert(fdt_add_subnode(dt, n, "port0") >= 0);
    n = fdt_path_offset(dt, "/j704-platform-data");
    assert(fdt_add_subnode(dt, n, "port2") >= 0);
    length = 24; missing_node = bad_prop = -1; chip_id = T8142;
    struct acio_tunable v = {4, 4, 0xff, 0x42};
    memcpy(record, &v, sizeof(v));
}
static bool ready(void)
{
    int n = fdt_path_offset(dt, "/j704-platform-data");
    return n >= 0 && fdt_getprop(dt, n, "apple,pcie-tunables-ready", NULL);
}
static void bad(unsigned off, unsigned size, u64 mask, u64 value, size_t len)
{
    reset();
    /* Refusing invalid input must also revoke an earlier successful receipt. */
    assert(!dt_set_j704_platform_data()); assert(ready());
    struct acio_tunable v = {off, size, mask, value};
    memcpy(record, &v, sizeof(v)); length = len;
    assert(dt_set_j704_platform_data() < 0); assert(!ready());
}
int main(void)
{
    reset(); assert(!dt_set_j704_platform_data()); assert(ready());
    for (unsigned j = 0; j < 12; j++) {
        char path[80]; int len;
        snprintf(path, sizeof(path), "/j704-platform-data/%s", j704_pcie_tables[j].role);
        int n = fdt_path_offset(dt, path);
        const fdt32_t *p = fdt_getprop(dt, n, j704_pcie_tables[j].info.fdt_name, &len);
        assert(p && len == 12 && fdt32_to_cpu(p[0]) == 4 &&
               fdt32_to_cpu(p[1]) == 0xff && fdt32_to_cpu(p[2]) == 0x42);
    }
    assert(!dt_set_j704_platform_data()); /* replaces, does not append */
    int n = fdt_path_offset(dt, "/j704-platform-data/shared"), len;
    assert(fdt_getprop(dt, n, "apple,tunable-phy", &len) && len == 12);
    bad(1,4,0,0,24); bad(0,8,0,0,24); bad(0,4,1ULL<<32,0,24);
    bad(0,4,0,1ULL<<32,24); bad(0x1000000,4,0,0,24);
    bad(0,4,0,0,0); bad(0,4,0,0,23); bad(0,4,0,0,24*4097);
    for (int i = 0; i < 3; i++) {
        reset(); missing_node = i; assert(dt_set_j704_platform_data() < 0); assert(!ready());
        reset(); bad_prop = i; assert(dt_set_j704_platform_data() < 0); assert(!ready());
    }
    reset(); n = fdt_path_offset(dt, "/j704-platform-data/port2");
    assert(!fdt_del_node(dt, n)); assert(dt_set_j704_platform_data() < 0); assert(!ready());
    reset(); chip_id = 0x8140; assert(!dt_set_j704_platform_data()); assert(!ready());
    reset(); assert(!fdt_setprop_string(dt,0,"compatible","apple,j813"));
    assert(!dt_set_j704_platform_data()); assert(!ready());
    reset();
    assert(fdt_add_subnode(dt,0,"opp-table-ecluster")>=0);
    assert(fdt_add_subnode(dt,0,"opp-table-pcluster")>=0);
    assert(!dt_set_j704_platform_data()); assert(ready());
    int table=fdt_path_offset(dt,"/opp-table-pcluster"), row, rows=0;
    fdt_for_each_subnode(row,dt,table) rows++;
    assert(rows==2);
    row=fdt_path_offset(dt,"/opp-table-pcluster/opp03");
    const fdt64_t *hz=fdt_getprop(dt,row,"opp-hz",NULL);
    assert(hz && fdt64_to_cpu(*hz)==2000000000ULL);
    assert(!dt_set_j704_platform_data()); assert(ready());
    cpu_bytes--;assert(dt_set_j704_platform_data()<0);assert(!ready());cpu_bytes++;
    cpu_rows[2]=cpu_rows[0];assert(dt_set_j704_platform_data()<0);assert(!ready());
    cpu_rows[2]=2000000;
    puts("J704 platform handoff: valid/repeat, malformed/bounds, missing inputs, other boards PASS");
}
