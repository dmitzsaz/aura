/* SPDX-License-Identifier: MIT */
/* T8142 HAL300 handoff. No GPU power votes, firmware writes or CPU start. */
#include "adt.h"
#include "exception.h"
#include "firmware.h"
#include "kboot_gpu_t8142.h"
#include "malloc.h"
#include "soc.h"
#include "string.h"
#include "utils.h"

#include "libfdt/libfdt.h"

#define T8142_OPPS    14
#define T8142_REGIONS 8
#define T8142_RANGES  10 /* Also preserve both __DATA_SHARED_RO segments. */

struct t8142_range {
    u64 base, size;
};
struct t8142_pstate {
    u32 hz, mv;
};
struct t8142_handoff {
    struct t8142_range ranges[T8142_RANGES];
    struct t8142_pstate core[T8142_OPPS], sram[T8142_OPPS];
    u32 period;
};

static const char *const t8142_region_names[T8142_REGIONS] = {
    "ttbs", "pagetables", "handoff", "shared-l2", "gfx-data", "gfx1-data", "fw-text", "fw1-text",
};
/* The first six paths are the upstream T8140/PR #32 handoff nodes. */
static const char *const t8142_region_nodes[6] = {
    "uat-ttbs", "uat-pagetables", "uat-handoff", "uat-l2-pagetables", "gfx-data", "gfx1-data",
};
static const char *const t8142_adt_regions[6] = {
    "gpu-region",           "gfx-shared-region", "gfx-handoff",
    "gfx-shared-l2-region", "gfx-data",          "gfx1-data",
};

struct t8142_image {
    u32 text_size, data_size, entropy[2];
    u8 sha256[32];
};
/* Public identities shared with Linux hw/t8142.rs; no firmware bytes. */
static const struct t8142_image t8142_images[2] = {
    {0x50000, 0xc0000, {302352, 302579}, {0x85, 0x31, 0x4b, 0xcc, 0x08, 0x3a, 0xcc, 0x5a,
                                          0xaf, 0xfd, 0xb7, 0xcf, 0xc5, 0xb9, 0xb8, 0x31,
                                          0xbe, 0x69, 0xa0, 0x8a, 0xeb, 0x42, 0xd6, 0x75,
                                          0xe5, 0x9c, 0xcb, 0xad, 0x17, 0x0f, 0x6a, 0xdd}},
    {0x4c000, 0xb4000, {301008, 301235}, {0x08, 0xdc, 0x4b, 0xe7, 0xa2, 0xa1, 0x3f, 0xc1,
                                          0x60, 0xe3, 0x49, 0xdd, 0x93, 0x16, 0x23, 0xb9,
                                          0x97, 0x68, 0xbe, 0x3e, 0x61, 0xae, 0x6e, 0xad,
                                          0x6b, 0x94, 0x77, 0x96, 0x18, 0xf0, 0x04, 0xf2}},
};

extern void rust_gpu_text_sha256(const u8 *data, size_t len, u8 out[32]);

static int t8142_adt_exact(int node, const char *name, void *out, u32 size)
{
    u32 len;
    const void *p = adt_getprop(adt, node, name, &len);
    if (!p || len != size)
        return -1;
    memcpy(out, p, size);
    return 0;
}

static u32 t8142_le32(const u8 *p)
{
    return p[0] | (u32)p[1] << 8 | (u32)p[2] << 16 | (u32)p[3] << 24;
}

static int t8142_gpu_node(void *dt)
{
    int gpu = fdt_node_offset_by_compatible(dt, -1, "apple,agx-t8142");
    int alias = fdt_path_offset(dt, "gpu");
    if (gpu < 0 || (alias >= 0 && alias != gpu) ||
        fdt_node_offset_by_compatible(dt, gpu, "apple,agx-t8142") >= 0)
        return -1;
    return gpu;
}

static int t8142_read_handoff(struct t8142_handoff *h)
{
    int sgx = adt_path_offset(adt, "/arm-io/sgx");
    int chosen = adt_path_offset(adt, "/chosen");
    u32 count, valid, tables;
    u64 dram, size;
    if (sgx < 0 || chosen < 0 || !adt_is_compatible(adt, sgx, "gpu,t8142") ||
        t8142_adt_exact(chosen, "dram-base", &dram, 8) ||
        t8142_adt_exact(chosen, "dram-size", &size, 8) || !size || dram > UINT64_MAX - size ||
        t8142_adt_exact(sgx, "perf-state-count", &count, 4) || count != T8142_OPPS ||
        t8142_adt_exact(sgx, "gpu-num-perf-states", &valid, 4) || valid != count - 1 ||
        t8142_adt_exact(sgx, "perf-state-table-count", &tables, 4) || tables != 1 ||
        t8142_adt_exact(sgx, "gpu-power-sample-period", &h->period, 4) || !h->period ||
        h->period > 10000 || t8142_adt_exact(sgx, "perf-states", h->core, sizeof(h->core)) ||
        t8142_adt_exact(sgx, "perf-states-sram", h->sram, sizeof(h->sram)))
        return -1;
    for (u32 i = 0; i < count; i++) {
        u32 hz = h->core[i].hz, core = h->core[i].mv, sram = h->sram[i].mv;
        /* SRAM tables on this IP carry voltages; their frequency column is unused. */
        if (hz % 1000000 || hz > 4000000000U || (i ? hz <= h->core[i - 1].hz : hz != 0) ||
            core > 2000 || sram > 2000 || (hz && (core < 300 || sram < 300)))
            return -1;
    }
    for (u32 i = 0; i < 6; i++) {
        char key[64];
        snprintf(key, sizeof(key), "%s-base", t8142_adt_regions[i]);
        if (t8142_adt_exact(sgx, key, &h->ranges[i].base, 8))
            return -1;
        snprintf(key, sizeof(key), "%s-size", t8142_adt_regions[i]);
        if (t8142_adt_exact(sgx, key, &h->ranges[i].size, 8))
            return -1;
    }
    for (u32 role = 0; role < 2; role++) {
        const char *path = role ? "/arm-io/gfx1-asc" : "/arm-io/gfx-asc";
        const char *nubpath =
            role ? "/arm-io/gfx1-asc/iop-gfx1-nub" : "/arm-io/gfx-asc/iop-gfx-nub";
        int asc = adt_path_offset(adt, path), nub = adt_path_offset(adt, nubpath);
        u32 loaded, len;
        struct adt_segment_ranges seg[3];
        static const char names[] = "__TEXT;__DATA;__DATA_SHARED_RO";
        const char *n = adt_getprop(adt, asc, "segment-names", &len);
        if (asc < 0 || nub < 0 || !n || len != sizeof(names) || memcmp(n, names, sizeof(names)) ||
            t8142_adt_exact(nub, "pre-loaded", &loaded, 4) || loaded != 1 ||
            t8142_adt_exact(asc, "segment-ranges", seg, sizeof(seg)))
            return -1;
        for (u32 i = 0; i < 3; i++)
            if (seg[i].phys != seg[i].remap)
                return -1;
        if (seg[0].size != t8142_images[role].text_size ||
            seg[1].size != t8142_images[role].data_size ||
            seg[1].phys != h->ranges[4 + role].base || seg[1].size != h->ranges[4 + role].size)
            return -1;
        h->ranges[6 + role] = (struct t8142_range){seg[0].phys, seg[0].size};
        h->ranges[8 + role] = (struct t8142_range){seg[2].phys, seg[2].size};
    }
    for (u32 i = 0; i < T8142_RANGES; i++) {
        const struct t8142_range *r = &h->ranges[i];
        if (!r->size || (r->base | r->size) & (SZ_16K - 1) || r->base < dram ||
            r->base - dram >= size || r->size > size - (r->base - dram) || r->base >= (1UL << 42) ||
            r->size > (1UL << 42) - r->base)
            return -1;
        for (u32 j = 0; j < i; j++)
            if (r->base < h->ranges[j].base + h->ranges[j].size &&
                h->ranges[j].base < r->base + r->size)
                return -1;
    }
    return 0;
}

/* Measure twice, then normalize only private copies, exactly as Linux fwid.rs. */
static int t8142_identify(const struct t8142_handoff *h, u32 role, const struct t8142_image *image)
{
    const struct t8142_range *text = &h->ranges[6 + role];
    u8 *copy = malloc(text->size), *again = malloc(text->size);
    int ret = -1;
    if (!copy || !again)
        goto out;
    enum exc_guard_t previous = exc_guard;
    int faults = exc_count;
    exc_guard = GUARD_MARK | GUARD_SILENT;
    memcpy(copy, (const void *)text->base, text->size);
    memcpy(again, (const void *)text->base, text->size);
    exc_guard = previous;
    if (faults != exc_count || memcmp(copy, again, text->size))
        goto out;
    u64 data;
    memcpy(&data, copy + 0x23c, sizeof(data));
    if (data != h->ranges[4 + role].base)
        goto out;
    memset(copy + 0x23c, 0, sizeof(data));
    static const char tags[2][5] = {"GKTS", "ECAP"};
    for (u32 i = 0; i < 2; i++) {
        u32 off = image->entropy[i];
        if (memcmp(copy + off, tags[i], 4) || t8142_le32(copy + off + 4) != 8)
            goto out;
        memset(copy + off + 8, 0, 8);
    }
    u8 digest[32];
    rust_gpu_text_sha256(copy, text->size, digest);
    ret = memcmp(digest, image->sha256, sizeof(digest)) ? -1 : 0;
out:
    free(copy);
    free(again);
    return ret;
}

static int t8142_calibration(void *dt, const struct t8142_handoff *h)
{
    int gpu = t8142_gpu_node(dt), len;
    const u8 *p = gpu < 0 ? NULL : fdt_getprop(dt, gpu, "apple,t8142-gpu-calibration", &len);
    /* Opaque owner payload; only version, identity and OPP/period ties are inspected. */
    return !p || len != 512 || t8142_le32(p) != 1 || t8142_le32(p + 4) != T8142 ||
                   t8142_le32(p + 8) != 0x3ff || t8142_le32(p + 12) != h->period ||
                   t8142_le32(p + 0x14) != T8142_OPPS - 1 ||
                   t8142_le32(p + 0x104) != h->core[T8142_OPPS - 1].hz / 1000000
               ? -1
               : 0;
}

static void t8142_range_name(char *name, size_t size, u32 i, const struct t8142_range *r)
{
    if (i < 6)
        snprintf(name, size, "%s", t8142_region_nodes[i]);
    else
        snprintf(name, size, "asc-firmware@%lx", r->base);
}

static bool t8142_available(void *dt, int node)
{
    int len;
    const char *status = fdt_getprop(dt, node, "status", &len);
    return !status || (len == 5 && !memcmp(status, "okay", 5)) ||
           (len == 3 && !memcmp(status, "ok", 3));
}

/* Reject conflicting owners before replacing the upstream placeholder reservations. */
static int t8142_check_reservations(void *dt, const struct t8142_handoff *h)
{
    int resv = fdt_path_offset(dt, "/reserved-memory"), node, len;
    if (resv < 0 || fdt_address_cells(dt, resv) != 2 || fdt_size_cells(dt, resv) != 2 ||
        !fdt_getprop(dt, resv, "ranges", &len) || len)
        return -1;
    fdt_for_each_subnode(node, dt, resv)
    {
        const fdt64_t *reg = fdt_getprop(dt, node, "reg", &len);
        if (!reg)
            continue;
        const char *name = fdt_get_name(dt, node, NULL);
        bool ours = false;
        for (u32 i = 0; i < T8142_RANGES; i++) {
            char expected[64];
            t8142_range_name(expected, sizeof(expected), i, &h->ranges[i]);
            if (strcmp(name, expected))
                continue;
            /* A populated reservation may be reused only for the same range. */
            if (len != 16 ||
                ((fdt64_ld(reg) || fdt64_ld(reg + 1)) &&
                 (fdt64_ld(reg) != h->ranges[i].base || fdt64_ld(reg + 1) != h->ranges[i].size)))
                return -1;
            ours = true;
        }
        if (ours || !t8142_available(dt, node))
            continue;
        if (len % 16)
            return -1;
        for (int off = 0; off < len / 8; off += 2) {
            u64 base = fdt64_ld(reg + off), size = fdt64_ld(reg + off + 1);
            if (base > UINT64_MAX - size)
                return -1;
            for (u32 i = 0; i < T8142_RANGES; i++)
                if (base < h->ranges[i].base + h->ranges[i].size && h->ranges[i].base < base + size)
                    return -1;
        }
    }
    return 0;
}

static int t8142_publish(void *dt, const struct t8142_handoff *h)
{
    int gpu = t8142_gpu_node(dt), len;
    const fdt32_t *ref = fdt_getprop(dt, gpu, "operating-points-v2", &len);
    if (!ref || len != 4)
        return -1;
    u32 table_ph = fdt32_ld(ref);
    int table = fdt_node_offset_by_phandle(dt, table_ph), opp;
    if (table < 0 || fdt_node_check_compatible(dt, table, "operating-points-v2"))
        return -1;
    /* The loader owns this table; replacement makes repeated preparation idempotent. */
    while ((opp = fdt_first_subnode(dt, table)) >= 0)
        if (fdt_del_node(dt, opp))
            return -1;
    for (u32 i = 0; i < T8142_OPPS; i++) {
        char name[16];
        snprintf(name, sizeof(name), "opp%02u", i);
        table = fdt_node_offset_by_phandle(dt, table_ph);
        opp = fdt_add_subnode(dt, table, name);
        if (opp < 0 || fdt_setprop_u64(dt, opp, "opp-hz", h->core[i].hz) ||
            fdt_setprop_u32(dt, opp, "opp-microvolt", h->core[i].mv * 1000) ||
            fdt_setprop_u32(dt, opp, "apple,sram-microvolt", h->sram[i].mv * 1000))
            return -1;
    }
    fdt32_t handles[T8142_REGIONS];
    char names[128];
    size_t names_len = 0;
    for (u32 i = 0; i < T8142_RANGES; i++) {
        char name[64];
        t8142_range_name(name, sizeof(name), i, &h->ranges[i]);
        int resv = fdt_path_offset(dt, "/reserved-memory");
        int node = fdt_subnode_offset(dt, resv, name);
        if (node < 0)
            node = fdt_add_subnode(dt, resv, name);
        fdt64_t reg[] = {cpu_to_fdt64(h->ranges[i].base), cpu_to_fdt64(h->ranges[i].size)};
        if (node < 0 || fdt_setprop(dt, node, "reg", reg, sizeof(reg)) ||
            fdt_setprop_empty(dt, node, "no-map") ||
            fdt_setprop_string(dt, node, "status", "okay") ||
            (i >= 6 && fdt_setprop_string(dt, node, "compatible", "apple,asc-mem")))
            return -1;
        u32 ph = fdt_get_phandle(dt, node);
        if (!ph && (fdt_generate_phandle(dt, &ph) || fdt_setprop_u32(dt, node, "phandle", ph)))
            return -1;
        if (i < T8142_REGIONS) {
            handles[i] = cpu_to_fdt32(ph);
            size_t len = strlen(t8142_region_names[i]) + 1;
            if (len > sizeof(names) - names_len)
                return -1;
            memcpy(names + names_len, t8142_region_names[i], len);
            names_len += len;
        }
    }
    gpu = t8142_gpu_node(dt);
    if (fdt_setprop(dt, gpu, "memory-region", handles, sizeof(handles)) ||
        fdt_setprop(dt, gpu, "memory-region-names", names, names_len) ||
        fdt_setprop_u32(dt, gpu, "apple,power-sample-period", h->period) ||
        firmware_set_fdt(dt, gpu, "apple,firmware-version", &os_firmware))
        return -1;
    /* This hint is informational. Both loader and kernel measure the actual TEXT pair. */
    int chosen = fdt_path_offset(dt, "/chosen");
    if (chosen < 0 || fdt_setprop_string(dt, chosen, "aurora,m5-gpu-firmware-profile", "M5-26A434"))
        return -1;
    gpu = t8142_gpu_node(dt);
    return fdt_setprop_string(dt, gpu, "status", "okay");
}

static int t8142_set_gpu(void *dt, const struct t8142_image images[2])
{
    if (chip_id != T8142)
        return 0;
    /* Disable even a stale owner overlay before allocating the transaction copy. */
    int node = -1;
    while ((node = fdt_node_offset_by_compatible(dt, node, "apple,agx-t8142")) >= 0) {
        if (fdt_setprop_string(dt, node, "status", "disabled")) {
            /* No room to add status: removing the device still lets the OS boot. */
            fdt_nop_node(dt, node);
            node = -1;
        }
    }
    if (t8142_gpu_node(dt) < 0)
        return 0;
    size_t capacity = fdt_totalsize(dt);
    void *copy = malloc(capacity);
    struct t8142_handoff h;
    int ret = -1;
    if (copy && !fdt_open_into(dt, copy, capacity) && !t8142_read_handoff(&h) &&
        !t8142_calibration(copy, &h) && !t8142_check_reservations(copy, &h) &&
        !t8142_identify(&h, 0, &images[0]) && !t8142_identify(&h, 1, &images[1]))
        ret = t8142_publish(copy, &h);
    if (!ret)
        memcpy(dt, copy, capacity);
    free(copy);
    printf("FDT: GPU: T8142 HAL300 handoff %s\n",
           ret ? "incomplete or invalid, GPU disabled" : "26A434 validated, GPU enabled");
    return 0;
}

int dt_set_gpu_t8142_26(void *dt)
{
    return t8142_set_gpu(dt, t8142_images);
}
