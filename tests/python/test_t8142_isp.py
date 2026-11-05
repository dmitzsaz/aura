# SPDX-License-Identifier: MIT
"""Resident ISP metadata follows the current ADT without starting the ISP."""
from pathlib import Path
import subprocess

from test_isp_heap import HARNESS, TESTS
from test_siliconforest_handoff import run_c

ROOT = Path(__file__).resolve().parents[2]


def definition(source, name):
    signature = "static int " + name + "("
    start = source.index(signature)
    while source.index(";", start) < source.index("{", start):
        start = source.index(signature, start + len(signature))
    return source[start:source.index("\n}", start) + 2] + "\n"


def test_t8142_never_powers_or_allocates_the_resident_isp(tmp_path):
    source = (ROOT / "src/isp.c").read_text()
    source = source[source.index("#define ISP_ASC_VERSION"):]
    reset = TESTS[:TESTS.index("static void success(void)")]
    binary = tmp_path / "isp-resident"
    subprocess.run(["cc", "-std=gnu11", "-Wall", "-Wextra", "-Werror",
                    "-Wno-format", "-Wno-unused-function", "-fsanitize=address,undefined",
                    "-x", "c", "-", "-o", str(binary)],
                   input=HARNESS + source + reset + r'''
int main(void)
{
    reset(); chip_id = T8142;
    /* Repeated DT preparation must not reuse a heap from another profile. */
    isp_initialized = true;
    assert(isp_init() == 0 && !isp_initialized);
    assert(!enabled && !disabled && !active && !gated && !allocations);
    assert(!read_address && !dapf_calls && !mode_calls);
    return 0;
}
''', text=True, check=True)
    subprocess.run([str(binary)], check=True)


def test_t8142_reserves_named_current_segments_and_disables_bad_metadata(tmp_path):
    source = (ROOT / "src/kboot.c").read_text()
    header = (ROOT / "src/adt.h").read_text()
    start = header.index("struct adt_segment_ranges {")
    layout = header[start:header.index("} PACKED;", start) + len("} PACKED;")]
    run_c(tmp_path, "t8142-isp-metadata", r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "libfdt.h"
typedef unsigned long u64;
typedef uint32_t u32;
#define PACKED __attribute__((packed))
#define T6030 0x6030
#define T6040 0x6040
#define T8132 0x8132
#define ADT_GETPROP(a,n,k,v) ((size_t)0)
#define T8140 0x8140
#define T8142 0x8142
#define SZ_16K 0x4000
#define ALIGN_UP(x,a) (((x)+(a)-1)&~((a)-1))
#define V13_6_1 1
#define V13_5 2
#define bail(...) do { return -1; } while (0)
static void *dt, *adt;
static u32 chip_id = T8142;
static u32 board_id;
static int dt_bufsize = 32768;
static bool adt_is_compatible(const void *tree, int node, const char *compat) { return false; }
static unsigned char blob[32768];
static int fault;
static unsigned int metadata_calls;
struct fw_version_info { int version; };
static struct fw_version_info os_firmware, fw_versions[4];
''' + layout + r'''
static struct adt_segment_ranges segments[2];
static int adt_path_offset(const void *tree, const char *path)
{ return fault == 1 || (fault == 21 && !strcmp(path, "/arm-io/isp")) ? -1 : 1; }
static const void *adt_getprop(const void *tree, int node, const char *name, u32 *len)
{
    *len = fault == 2 ? sizeof(segments)-1 : sizeof(segments);
    return fault == 3 ? NULL : segments;
}
static u64 isp_iova_base(void) { return 0; }
static int isp_get_heap(u64 *p, u64 *v, u64 *s) { assert(0); return -1; }
static int firmware_set_fdt(void *tree, int node, const char *name,
                            const struct fw_version_info *ver)
{ metadata_calls++; return fault == 14 ? -1 : 0; }
''' + definition(source, "dt_get_or_add_reserved_mem") +
        definition(source, "dt_device_set_reserved_mem") +
        definition(source, "dt_device_add_mem_region") +
        definition(source, "dt_reserve_asc_segments") +
        definition(source, "dt_reserve_asc_firmware") +
        definition(source, "dt_set_isp_t8142") +
        definition(source, "dt_finish_isp_t8142") +
        definition(source, "dt_set_isp_fwdata") +
        definition(source, "dt_set_isp") + r'''
static void world(int which, u64 physical)
{
    fault = which;
    metadata_calls = 0;
    memset(blob,0,sizeof(blob)); dt=blob;
    assert(!fdt_create_empty_tree(dt,sizeof(blob)));
    int rm=fdt_add_subnode(dt,0,"reserved-memory"); assert(rm>=0);
    int heap=fdt_add_subnode(dt,rm,"isp-heap"); assert(heap>=0);
    assert(!fdt_setprop_u32(dt,heap,"phandle",1));
    int isp=fdt_add_subnode(dt,0,"isp"); assert(isp>=0);
    assert(!fdt_setprop_u32(dt,isp,"phandle",2));
    assert(!fdt_setprop_u32(dt,isp,"memory-region",1));
    assert(!fdt_setprop_string(dt,isp,"memory-region-names","heap"));
    assert(!fdt_setprop_string(dt,isp,"compatible","apple,t8142-isp"));
    fdt32_t mapping[] = {cpu_to_fdt32(2),0,0,0,cpu_to_fdt32(0x1700000)};
    heap=fdt_node_offset_by_phandle(dt,1);
    assert(!fdt_setprop(dt,heap,"iommu-addresses",mapping,sizeof(mapping)));
    int alias=fdt_add_subnode(dt,0,"aliases"); assert(alias>=0);
    assert(!fdt_setprop_string(dt,alias,"isp","/isp"));
    segments[0]=(struct adt_segment_ranges){.phys=physical,.iova=0,.size=0xd68000};
    segments[1]=(struct adt_segment_ranges){.phys=physical+0x2000000,.iova=0xd68000,.size=0x984000};
}
static void good(u64 physical)
{
    world(0,physical);
    assert(!dt_set_isp());
    assert(metadata_calls == 2);
    int isp=fdt_path_offset(dt,"isp"),len;
    const char *names=fdt_getprop(dt,isp,"memory-region-names",&len);
    assert(len==15 && !memcmp(names,"heap\0text\0data\0",15));
    const fdt32_t *handles=fdt_getprop(dt,isp,"memory-region",&len);
    assert(len==12 && fdt32_ld(handles)==1);
    for (int i=0;i<2;i++) {
        int node=fdt_node_offset_by_phandle(dt,fdt32_ld(handles+i+1));
        const fdt64_t *reg=fdt_getprop(dt,node,"reg",&len);
        assert(len==16 && fdt64_ld(reg)==segments[i].phys && fdt64_ld(reg+1)==segments[i].size);
        const fdt32_t *map=fdt_getprop(dt,node,"iommu-addresses",&len);
        assert(len==20 && fdt32_ld(map)==2);
        assert(fdt64_ld((const void *)(map+1))==segments[i].iova);
        assert(fdt64_ld((const void *)(map+3))==segments[i].size);
        assert(fdt_getprop(dt,node,"no-map",&len));
    }
    assert(!fdt_getprop(dt,isp,"status",&len));
    assert(!fdt_getprop(dt,fdt_path_offset(dt,"/reserved-memory/isp-heap"),"reg",&len));
}
int main(void)
{
    good(0x12000000000UL);
    good(0x13004000000UL); /* no previous boot's physical addresses survive */
    for(int i=1;i<=20;i++) {
        world(i,0x12000000000UL);
        if(i==4) segments[0].iova=0x4000;
        if(i==5) segments[1].size=0x1000000;
        if(i==6) segments[1].phys++;
        if(i==7) segments[0].size=0;
        if(i==8) segments[1].phys=UINT64_MAX-0x3fff;
        int heap = fdt_node_offset_by_phandle(dt, 1);
        if(i==9) assert(!fdt_setprop_u64(dt,heap,"reg",0));
        if(i==10) assert(!fdt_delprop(dt,heap,"iommu-addresses"));
        if(i==11) segments[1].iova++;
        if(i==12) segments[1].phys=segments[0].phys;
        if(i==13) segments[1].size++;
        if(i==15) {
            fdt32_t map[] = {cpu_to_fdt32(2),0,0,0,cpu_to_fdt32(0x1800000)};
            assert(!fdt_setprop(dt,heap,"iommu-addresses",map,sizeof(map)));
        }
        if(i==16) {
            fdt32_t map[] = {cpu_to_fdt32(99),0,0,0,cpu_to_fdt32(0x1700000)};
            assert(!fdt_setprop(dt,heap,"iommu-addresses",map,sizeof(map)));
        }
        if(i==17) {
            fdt32_t map[] = {cpu_to_fdt32(2),0,cpu_to_fdt32(0x4000),0,cpu_to_fdt32(0x1700000)};
            assert(!fdt_setprop(dt,heap,"iommu-addresses",map,sizeof(map)));
        }
        int isp = fdt_path_offset(dt,"isp");
        if(i==18) assert(!fdt_setprop_string(dt,isp,"memory-region-names","data"));
        if(i==19) assert(!fdt_setprop_string(dt,isp,"compatible","apple,t8140-isp"));
        if(i==20) segments[0].phys=UINT64_MAX-0x3fff;
        assert(!dt_set_isp());
        assert(!strcmp(fdt_getprop(dt,fdt_path_offset(dt,"isp"),"status",NULL),"disabled"));
        int len;
        assert(fdt_getprop(dt,fdt_path_offset(dt,"isp"),"memory-region",&len) && (i==14 ? len==12 : len==4));
    }
    world(21,0x12000000000UL);
    assert(!dt_set_isp());
    assert(metadata_calls==2);
    assert(!fdt_pack(dt));
    assert(!dt_finish_isp_t8142(1));
    assert(!fdt_getprop(dt,fdt_path_offset(dt,"isp"),"status",NULL));
    assert(!dt_finish_isp_t8142(-1));
    assert(!strcmp(fdt_getprop(dt,fdt_path_offset(dt,"isp"),"status",NULL),"disabled"));
    return 0;
}
''')
