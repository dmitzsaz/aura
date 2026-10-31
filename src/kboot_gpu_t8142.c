/* SPDX-License-Identifier: GPL-2.0-only OR MIT */
/* The owner supplies HAL300 coefficients in a private DT overlay. Everything
 * describing live firmware memory and performance states comes from the ADT.
 * This producer reads metadata only; it neither powers nor starts the GPU. */
#include "adt.h"
#include "malloc.h"
#include "soc.h"
#include "utils.h"
#include "libfdt/libfdt.h"
#include "kboot_gpu_t8142_fields.h"

struct t8142_pstate { u32 hz, mv; };
struct t8142_region { const char *name; u64 base, size; };

static int t8142_adt_exact(int node, const char *key, void *out, u32 size)
{
    u32 len = 0;
    const void *value = adt_getprop(adt, node, key, &len);
    if (!value || len != size) return -1;
    memcpy(out, value, size);
    return 0;
}

static int t8142_gpu_prepare(void *tree)
{
    int gpu = fdt_path_offset(tree, "gpu"), len;
    const fdt32_t *manual = gpu < 0 ? NULL : fdt_getprop(tree, gpu, "apple,hal300-derived", &len);
    if (!manual || len != sizeof(t8142_hal300_fields) / sizeof(*t8142_hal300_fields) * 5 * sizeof(fdt32_t)) return -1;
    for (size_t i = 0; i < sizeof(t8142_hal300_fields)/sizeof(*t8142_hal300_fields); i++) {
        const fdt32_t *r = manual + 5*i;
        u32 width = t8142_hal300_fields[i].width;
        u64 value = fdt32_ld(r+3) | ((u64)fdt32_ld(r+4) << 32);
        if (fdt32_ld(r) != t8142_hal300_fields[i].object ||
            fdt32_ld(r+1) != t8142_hal300_fields[i].offset || fdt32_ld(r+2) != width ||
            (width < 8 && value >> (8*width))) return -1;
    }
    int sgx = adt_path_offset(adt, "/arm-io/sgx");
    int chosen = adt_path_offset(adt, "/chosen");
    u64 dram, dram_size;
    u32 count, banks, valid, period;
    if (sgx < 0 || chosen < 0 ||
        t8142_adt_exact(chosen,"dram-base",&dram,8) ||
        t8142_adt_exact(chosen,"dram-size",&dram_size,8) || dram > UINT64_MAX-dram_size ||
        t8142_adt_exact(sgx,"gpu-num-perf-states",&valid,4) || valid >= 16 ||
        t8142_adt_exact(sgx,"perf-state-count",&count,4) || count < valid+1 || count > 16 ||
        t8142_adt_exact(sgx,"perf-state-table-count",&banks,4) || banks != 1 ||
        t8142_adt_exact(sgx,"gpu-power-sample-period",&period,4) || !period || period > 10000)
        return -1;
    u32 bytes;
    const struct t8142_pstate *core = adt_getprop(adt,sgx,"perf-states",&bytes);
    if (!core || bytes != count*sizeof(*core)) return -1;
    const struct t8142_pstate *sram = adt_getprop(adt,sgx,"perf-states-sram",&bytes);
    if (!sram || bytes != count*sizeof(*sram) || core[0].hz) return -1;
    for (u32 i=0;i<=valid;i++) {
        if ((i && core[i].hz <= core[i-1].hz) || core[i].mv > UINT32_MAX/1000 ||
            sram[i].mv > UINT32_MAX/1000) return -1;
    }
    /* A profile is explicitly bound to its source performance table. These
     * profile properties let repeated preparation revalidate the same input. */
    const fdt32_t *source = fdt_getprop(tree,gpu,"apple,hal300-source-pstates",&len);
    if (!source || len != (int)((valid+1)*12)) return -1;
    for (u32 i=0;i<=valid;i++)
        if (fdt32_ld(source+3*i)!=core[i].hz || fdt32_ld(source+3*i+1)!=core[i].mv ||
            fdt32_ld(source+3*i+2)!=sram[i].mv) return -1;
    const fdt32_t *source_period = fdt_getprop(tree,gpu,"apple,hal300-source-period",&len);
    if (!source_period || len != 4 || fdt32_ld(source_period) != period) return -1;

    struct t8142_region r[] = {
        {"ttbs",0,0},{"pagetables",0,0},{"handoff",0,0},{"l2-pagetables",0,0},
        {"fw-text0",0,0},{"fw-data0",0,0},{"crashlog0",0,0},
        {"fw-text1",0,0},{"fw-data1",0,0},{"crashlog1",0,0},
    };
    const char *properties[] = {"gpu-region","gfx-shared-region","gfx-handoff","gfx-shared-l2-region"};
    char key[64];
    for (u32 i=0;i<4;i++) {
        snprintf(key,sizeof(key),"%s-base",properties[i]);
        if (t8142_adt_exact(sgx,key,&r[i].base,8)) return -1;
        snprintf(key,sizeof(key),"%s-size",properties[i]);
        if (t8142_adt_exact(sgx,key,&r[i].size,8)) return -1;
    }
    for (u32 role=0;role<2;role++) {
        const char *ascpath = role ? "/arm-io/gfx1-asc" : "/arm-io/gfx-asc";
        const char *nubpath = role ? "/arm-io/gfx1-asc/iop-gfx1-nub" : "/arm-io/gfx-asc/iop-gfx-nub";
        int asc = adt_path_offset(adt,ascpath), nub = adt_path_offset(adt,nubpath);
        u32 loaded;
        if (asc < 0 || nub < 0 || t8142_adt_exact(nub,"pre-loaded",&loaded,4) || loaded != 1)
            return -1;
        const char *names = adt_getprop(adt,asc,"segment-names",&bytes);
        static const char expected[] = "__TEXT;__DATA;__DATA_SHARED_RO";
        if (!names || bytes != sizeof(expected) || memcmp(names,expected,sizeof(expected))) return -1;
        const struct adt_segment_ranges *segments = adt_getprop(adt,asc,"segment-ranges",&bytes);
        if (!segments || bytes != 3*sizeof(*segments)) return -1;
        for (u32 j=0;j<3;j++) {
            if (segments[j].remap != segments[j].phys) return -1;
            r[4+3*role+j].base=segments[j].phys;
            r[4+3*role+j].size=segments[j].size;
        }
    }
    for (u32 i=0;i<10;i++) {
        if (!r[i].size || ((r[i].base|r[i].size)&0x3fff) || r[i].base < dram ||
            r[i].base-dram > dram_size || r[i].size > dram_size-(r[i].base-dram)) return -1;
        for (u32 j=0;j<i;j++)
            if (r[i].base < r[j].base+r[j].size && r[j].base < r[i].base+r[i].size) return -1;
    }
    const fdt32_t *opp = fdt_getprop(tree,gpu,"operating-points-v2",&len);
    if (!opp || len != 4) return -1;
    u32 opp_phandle=fdt32_ld(opp);
    int table=fdt_node_offset_by_phandle(tree,opp_phandle), child;
    if (table < 0) return -1;
    /* Repeated preparation replaces, never duplicates, live performance rows. */
    while ((child=fdt_first_subnode(tree,table))>=0)
        if (fdt_del_node(tree,child)) return -1;
    for (u32 i=0;i<=valid;i++) {
        snprintf(key,sizeof(key),"opp%02u",i);
        table=fdt_node_offset_by_phandle(tree,opp_phandle);
        child=fdt_add_subnode(tree,table,key);
        if (child<0 || fdt_setprop_u64(tree,child,"opp-hz",core[i].hz) ||
            fdt_setprop_u32(tree,child,"opp-microvolt",core[i].mv*1000) ||
            fdt_setprop_u32(tree,child,"apple,sram-microvolt",sram[i].mv*1000)) return -1;
    }
    fdt32_t handles[10];
    for (u32 i=0;i<10;i++) {
        int reserved=fdt_path_offset(tree,"/reserved-memory");
        snprintf(key,sizeof(key),"gpu-%s@%llx",r[i].name,(unsigned long long)r[i].base);
        child=fdt_subnode_offset(tree,reserved,key);
        if (child<0) child=fdt_add_subnode(tree,reserved,key);
        u32 handle;
        if (child<0 || fdt_generate_phandle(tree,&handle)) return -1;
        fdt64_t reg[]={cpu_to_fdt64(r[i].base),cpu_to_fdt64(r[i].size)};
        if (fdt_setprop(tree,child,"reg",reg,sizeof(reg)) ||
            fdt_setprop_empty(tree,child,"no-map") || fdt_setprop_u32(tree,child,"phandle",handle)) return -1;
        handles[i]=cpu_to_fdt32(handle);
    }
    gpu=fdt_path_offset(tree,"gpu");
    if (fdt_setprop(tree,gpu,"memory-region",handles,sizeof(handles))) return -1;
    fdt_delprop(tree,gpu,"memory-region-names");
    for (u32 i=0;i<10;i++)
        if (fdt_appendprop_string(tree,gpu,"memory-region-names",r[i].name)) return -1;
    if (fdt_setprop_u32(tree,gpu,"apple,power-sample-period",period) ||
        fdt_setprop_string(tree,gpu,"status","okay")) return -1;
    return 0;
}

int dt_set_gpu_t8142(void *tree)
{
    int gpu=fdt_path_offset(tree,"gpu");
    if (chip_id!=T8142 || gpu<0 || fdt_node_check_compatible(tree,0,"apple,j704") ||
        fdt_node_check_compatible(tree,gpu,"apple,agx-t8142")) return 0;
    size_t capacity=fdt_totalsize(tree);
    void *work=malloc(capacity);
    if (!work) return -1;
    int ret=fdt_open_into(tree,work,capacity);
    if (!ret) ret=t8142_gpu_prepare(work);
    if (!ret) {
        memcpy(tree,work,capacity);
        printf("FDT: T8142 GPU: live ADT reservations/OPPs with owner HAL300 profile\n");
    } else {
        gpu=fdt_path_offset(tree,"gpu");
        ret=fdt_setprop_string(tree,gpu,"status","disabled");
        printf("FDT: T8142 GPU: missing or invalid owner HAL300 profile/handoff; disabled\n");
    }
    free(work);
    return ret;
}
