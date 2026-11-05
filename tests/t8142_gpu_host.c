/* SPDX-License-Identifier: MIT */
#include <assert.h>
#include <stdlib.h>
#include <string.h>
static void *gpu_copy(void *dst, const void *src, size_t len);
static void *gpu_malloc(size_t size);
#define memcpy gpu_copy
#define malloc gpu_malloc
#include "kboot_gpu_t8142.c"
#undef memcpy
#undef malloc

void *adt;
u32 chip_id = T8142;
volatile enum exc_guard_t exc_guard;
volatile int exc_count;
struct fw_version_info os_firmware;
static void *ram;
#define RAM_BASE 0x200000000UL
#define RAM_SIZE 0x2000000UL
#define CAPACITY 65536
static int allocations, fail_alloc, unstable, trap_read;
static void *gpu_malloc(size_t size)
{
    return ++allocations == fail_alloc ? NULL : malloc(size);
}
static void *gpu_copy(void *dst, const void *src, size_t len)
{
    u64 base = (u64)src;
    if (base >= RAM_BASE && base < RAM_BASE + RAM_SIZE) {
        assert(len <= RAM_SIZE - (base - RAM_BASE));
        src = (const u8 *)ram + base - RAM_BASE;
        memcpy(dst, src, len);
        if (unstable)
            ((u8 *)src)[0] ^= 1;
        if (trap_read)
            exc_count++;
        return dst;
    }
    return memcpy(dst, src, len);
}

/* All addresses, power inputs and firmware bytes here are synthetic. */
int adt_path_offset(const void *tree, const char *path)
{
    return fdt_path_offset(tree, path);
}
const void *adt_getprop(const void *tree, int node, const char *name, u32 *len)
{
    int bytes;
    const void *p = fdt_getprop(tree, node, name, &bytes);
    if (len)
        *len = p ? bytes : 0;
    return p;
}
bool adt_is_compatible(const void *tree, int node, const char *compat)
{
    int len;
    const char *p = fdt_getprop(tree, node, "compatible", &len);
    return p && len == (int)strlen(compat) + 1 && !memcmp(p, compat, len);
}
int firmware_set_fdt(void *dt, int node, const char *prop, const struct fw_version_info *fw)
{
    (void)fw;
    fdt32_t version[] = {cpu_to_fdt32(27), 0, 0};
    return fdt_setprop(dt, node, prop, version, sizeof(version));
}

int adt_getprop_copy(const void *tree, int node, const char *name, void *out, size_t size)
{
    u32 len;
    const void *p = adt_getprop(tree, node, name, &len);
    if (!p || len != size)
        return -1;
    memcpy(out, p, size);
    return len;
}
u32 read32(u64 address)
{
    u32 value;
    memcpy(&value, (const void *)address, 4);
    return value;
}
int dt_set_gpu_m4(void *dt)
{
    (void)dt;
    abort();
}
#define bail(...)                                                                                  \
    do {                                                                                           \
        printf(__VA_ARGS__);                                                                       \
        return -1;                                                                                 \
    } while (0)
struct perf_state {
    u32 freq, volt;
};
/* PR #32 uses zero initialization for trailing fields and four-byte tags. */
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmissing-field-initializers"
#ifdef __clang__
#if __has_warning("-Wunterminated-string-initialization")
#pragma clang diagnostic ignored "-Wunterminated-string-initialization"
#endif
#endif
#include "legacy.inc"
#pragma GCC diagnostic pop

static void set_le32(void *dt, int node, const char *name, u32 value)
{
    assert(!fdt_setprop(dt, node, name, &value, 4));
}
static void set_le64(void *dt, int node, const char *name, u64 value)
{
    assert(!fdt_setprop(dt, node, name, &value, 8));
}
static int add(void *dt, const char *path, const char *name)
{
    int node = fdt_add_subnode(dt, fdt_path_offset(dt, path), name);
    assert(node >= 0);
    return node;
}
static int sgx(void)
{
    return fdt_path_offset(adt, "/arm-io/sgx");
}
static void firmware_image(u8 *p, u32 size, const struct t8142_image *image, u64 data, u32 role)
{
    for (u32 i = 0; i < size; i++)
        p[i] = (i * 7 + role * 11) & 0xff;
    memset(p + 0x23c, 0, 8);
    for (u32 i = 0; i < 2; i++) {
        memcpy(p + image->entropy[i], i ? "ECAP" : "GKTS", 4);
        p[image->entropy[i] + 4] = 8;
        memset(p + image->entropy[i] + 5, 0, 11);
    }
    (void)data;
}

static void fixture(void *dt, struct t8142_image images[2])
{
    assert(!fdt_create_empty_tree(adt, CAPACITY));
    add(adt, "/", "chosen");
    int chosen = fdt_path_offset(adt, "/chosen");
    set_le64(adt, chosen, "dram-base", RAM_BASE);
    set_le64(adt, chosen, "dram-size", RAM_SIZE);
    add(adt, "/", "arm-io");
    add(adt, "/arm-io", "sgx");
    assert(!fdt_setprop_string(adt, sgx(), "compatible", "gpu,t8142"));
    set_le32(adt, sgx(), "perf-state-count", T8142_OPPS);
    set_le32(adt, sgx(), "gpu-num-perf-states", T8142_OPPS - 1);
    set_le32(adt, sgx(), "perf-state-table-count", 1);
    set_le32(adt, sgx(), "gpu-power-sample-period", 10);
    struct t8142_pstate core[T8142_OPPS], sram[T8142_OPPS];
    for (u32 i = 0; i < T8142_OPPS; i++) {
        core[i] = (struct t8142_pstate){i * 100000000, 500 + i};
        sram[i] = (struct t8142_pstate){0, 600 + i};
    }
    assert(!fdt_setprop(adt, sgx(), "perf-states", core, sizeof(core)));
    assert(!fdt_setprop(adt, sgx(), "perf-states-sram", sram, sizeof(sram)));
    for (u32 i = 0; i < 6; i++) {
        char key[64];
        u64 size = i >= 4 ? images[i - 4].data_size : 0x4000;
        snprintf(key, sizeof(key), "%s-base", t8142_adt_regions[i]);
        set_le64(adt, sgx(), key, RAM_BASE + i * 0x200000);
        snprintf(key, sizeof(key), "%s-size", t8142_adt_regions[i]);
        set_le64(adt, sgx(), key, size);
    }
    for (u32 role = 0; role < 2; role++) {
        const char *path = role ? "/arm-io/gfx1-asc" : "/arm-io/gfx-asc";
        add(adt, "/arm-io", role ? "gfx1-asc" : "gfx-asc");
        int asc = fdt_path_offset(adt, path);
        assert(!fdt_setprop_string(adt, asc, "segment-names", "__TEXT;__DATA;__DATA_SHARED_RO"));
        struct adt_segment_ranges seg[3] = {0};
        seg[0].phys = seg[0].remap = RAM_BASE + (6 + role) * 0x200000;
        seg[0].size = images[role].text_size;
        seg[1].phys = seg[1].remap = RAM_BASE + (4 + role) * 0x200000;
        seg[1].size = images[role].data_size;
        seg[2].phys = seg[2].remap = RAM_BASE + (8 + role) * 0x200000;
        seg[2].size = 0x4000;
        assert(!fdt_setprop(adt, asc, "segment-ranges", seg, sizeof(seg)));
        int nub = add(adt, path, role ? "iop-gfx1-nub" : "iop-gfx-nub");
        set_le32(adt, nub, "pre-loaded", 1);
        u8 *text = (u8 *)ram + seg[0].phys - RAM_BASE;
        firmware_image(text, seg[0].size, &images[role], seg[1].phys, role);
        rust_gpu_text_sha256(text, seg[0].size, images[role].sha256);
        memcpy(text + 0x23c, &seg[1].phys, 8);
        for (u32 i = 0; i < 2; i++)
            memset(text + images[role].entropy[i] + 8, 0xa5, 8);
    }
    assert(!fdt_create_empty_tree(dt, CAPACITY));
    add(dt, "/", "chosen");
    int alias = add(dt, "/", "aliases");
    assert(!fdt_setprop_string(dt, alias, "gpu", "/soc/gpu"));
    int resv = add(dt, "/", "reserved-memory");
    assert(!fdt_setprop_u32(dt, resv, "#address-cells", 2));
    assert(!fdt_setprop_u32(dt, resv, "#size-cells", 2));
    assert(!fdt_setprop_empty(dt, resv, "ranges"));
    for (u32 i = 0; i < 6; i++) {
        int node = add(dt, "/reserved-memory", t8142_region_nodes[i]);
        u8 reg[16] = {0};
        assert(!fdt_setprop(dt, node, "reg", reg, sizeof(reg)));
        assert(!fdt_setprop_u32(dt, node, "phandle", 100 + i));
    }
    int table = add(dt, "/", "opp-table");
    assert(!fdt_setprop_u32(dt, table, "phandle", 10));
    assert(!fdt_setprop_string(dt, table, "compatible", "operating-points-v2"));
    int opp = add(dt, "/opp-table", "stale-opp");
    assert(!fdt_setprop_string(dt, opp, "status", "disabled"));
    add(dt, "/", "soc");
    int gpu = add(dt, "/soc", "gpu");
    assert(!fdt_setprop_string(dt, gpu, "compatible", "apple,agx-t8142"));
    assert(!fdt_setprop_string(dt, gpu, "status", "disabled"));
    assert(!fdt_setprop_u32(dt, gpu, "operating-points-v2", 10));
    u8 calibration[512] = {0};
    u32 header[] = {1, T8142, 0x3ff, 10};
    memcpy(calibration, header, sizeof(header));
    u32 maxstate = 13, mhz = 1300;
    memcpy(calibration + 0x14, &maxstate, 4);
    memcpy(calibration + 0x104, &mhz, 4);
    assert(!fdt_setprop(dt, gpu, "apple,t8142-gpu-calibration", calibration, sizeof(calibration)));
}

static void mutate_cal(void *dt, u32 offset, u8 value)
{
    int gpu = t8142_gpu_node(dt), len;
    u8 *p = (u8 *)fdt_getprop(dt, gpu, "apple,t8142-gpu-calibration", &len);
    assert(p && offset < (u32)len);
    p[offset] = value;
}
static void assert_enabled(void *dt)
{
    int gpu = t8142_gpu_node(dt), len;
    const char *status = fdt_getprop(dt, gpu, "status", NULL);
    assert(status && !strcmp(status, "okay"));
    const fdt32_t *handles = fdt_getprop(dt, gpu, "memory-region", &len);
    assert(handles && len == T8142_REGIONS * 4);
    for (u32 i = 0; i < T8142_REGIONS; i++) {
        int node = fdt_node_offset_by_phandle(dt, fdt32_ld(handles + i));
        assert(node >= 0 && fdt_getprop(dt, node, "no-map", NULL));
        assert(!strcmp(fdt_stringlist_get(dt, gpu, "memory-region-names", i, NULL),
                       t8142_region_names[i]));
    }
    int resv = fdt_path_offset(dt, "/reserved-memory"), count = 0, node;
    fdt_for_each_subnode(node, dt, resv)
    {
        assert(fdt_getprop(dt, node, "no-map", NULL));
        count++;
    }
    assert(count == T8142_RANGES);
    int table = fdt_node_offset_by_phandle(dt, 10);
    count = 0;
    fdt_for_each_subnode(node, dt, table)
    {
        const char *name = fdt_get_name(dt, node, NULL);
        unsigned i;
        assert(sscanf(name, "opp%u", &i) == 1 && i < T8142_OPPS);
        const fdt64_t *hz = fdt_getprop(dt, node, "opp-hz", NULL);
        const fdt32_t *core = fdt_getprop(dt, node, "opp-microvolt", NULL);
        const fdt32_t *sram = fdt_getprop(dt, node, "apple,sram-microvolt", NULL);
        assert(hz && core && sram && fdt64_ld(hz) == i * 100000000UL);
        assert(fdt32_ld(core) == (500 + i) * 1000 && fdt32_ld(sram) == (600 + i) * 1000);
        count++;
    }
    assert(count == T8142_OPPS);
}

static void test_legacy(void *dt, int scenario)
{
    bool t8140 = scenario == 46;
    u32 count = t8140 ? 16 : 13;
    chip_id = t8140 ? T8140 : T8142;
    set_le32(adt, sgx(), "perf-state-count", count);
    set_le32(adt, sgx(), "gpu-num-perf-states", count - 1);
    struct perf_state ps[16];
    for (u32 i = 0; i < count; i++)
        ps[i] = (struct perf_state){100000000 * i, 500 + i};
    assert(!fdt_setprop(adt, sgx(), "perf-states", ps, count * sizeof(*ps)));
    assert(!fdt_setprop(adt, sgx(), "perf-states-sram", ps, count * sizeof(*ps)));
    int table = fdt_node_offset_by_phandle(dt, 10), child;
    while ((child = fdt_first_subnode(dt, table)) >= 0)
        assert(!fdt_del_node(dt, child));
    for (u32 i = 0; i < count; i++) {
        char name[16];
        snprintf(name, sizeof(name), "opp%02u", i);
        child = add(dt, "/opp-table", name);
        assert(!fdt_setprop_u64(dt, child, "opp-hz", 0));
        assert(!fdt_setprop_u32(dt, child, "opp-microvolt", 0));
    }
    for (u32 role = 0; role < 2 && !t8140; role++) {
        u32 size = role ? 0x48000 : 0x4c000;
        u8 *text = (u8 *)ram + (6 + role) * 0x200000;
        memset(text, 0, size);
        memcpy(text, T8142_GPU_FIRMWARE_BUILD, sizeof(T8142_GPU_FIRMWARE_BUILD));
        u32 offsets[2] = {role ? 0x45841 : 0x45d09, role ? 0x45924 : 0x45dec};
        for (u32 i = 0; i < 2; i++) {
            memcpy(text + offsets[i], i ? "ECAP" : "GKTS", 4);
            text[offsets[i] + 4] = 8;
        }
        if (scenario == 45)
            text[0] = 0; /* An unknown image still follows PR #32's path. */
        struct adt_segment_ranges seg[2] = {{0}};
        seg[0].phys = (u64)text;
        seg[0].size = size;
        int asc = fdt_path_offset(adt, role ? "/arm-io/gfx1-asc" : "/arm-io/gfx-asc");
        assert(!fdt_setprop(adt, asc, "segment-ranges", seg, sizeof(seg)));
        assert(!fdt_setprop_string(adt, asc, "segment-names", "__TEXT;__DATA"));
    }
    int gpu = t8142_gpu_node(dt);
    assert(!fdt_delprop(dt, gpu, "apple,t8142-gpu-calibration"));
    assert(!fdt_setprop_string(dt, gpu, "status", "okay"));
    if (t8140)
        assert(!fdt_setprop_string(dt, gpu, "compatible", "apple,agx-t8140"));
    assert(!dt_set_gpu(dt));
    gpu = fdt_path_offset(dt, "gpu");
    assert(!strcmp(fdt_getprop(dt, gpu, "status", NULL), "okay"));
    const char *profile =
        fdt_getprop(dt, fdt_path_offset(dt, "/chosen"), "aurora,m5-gpu-firmware-profile", NULL);
    if (scenario == 44)
        assert(profile && !strcmp(profile, T8142_GPU_FIRMWARE_PROFILE));
    else
        assert(!profile);
    int rows = 0;
    table = fdt_node_offset_by_phandle(dt, 10);
    fdt_for_each_subnode(child, dt, table)
    {
        int len;
        const fdt32_t *mv = fdt_getprop(dt, child, "opp-microvolt", &len);
        assert(mv && len == 4 && fdt32_ld(mv) == (500 + rows) * 1000);
        assert(!fdt_getprop(dt, child, "apple,sram-microvolt", NULL));
        rows++;
    }
    assert(rows == (int)count);
}

int main(int argc, char **argv)
{
    assert(argc == 2);
    int scenario = atoi(argv[1]);
    ram = calloc(1, RAM_SIZE);
    assert(ram);
    adt = calloc(1, CAPACITY);
    void *dt = calloc(1, CAPACITY), *before = calloc(1, CAPACITY);
    struct t8142_image images[2];
    memcpy(images, t8142_images, sizeof(images));
    fixture(dt, images);
    if (scenario >= 44 && scenario <= 46) {
        test_legacy(dt, scenario);
        free(dt);
        free(before);
        free(adt);
        free(ram);
        puts("PASS legacy G17 handoff");
        return 0;
    }
    int gpu = t8142_gpu_node(dt), expect = 0, noop = 0;
    switch (scenario) {
        case 0:
            expect = 1;
            break;
        case 1:
            assert(!fdt_delprop(dt, gpu, "apple,t8142-gpu-calibration"));
            break;
        case 2:
            mutate_cal(dt, 0, 2);
            break;
        case 3:
            mutate_cal(dt, 4, 0);
            break;
        case 4:
            mutate_cal(dt, 8, 0);
            break;
        case 5:
            mutate_cal(dt, 12, 0);
            break;
        case 6:
            mutate_cal(dt, 0x14, 12);
            break;
        case 7:
            mutate_cal(dt, 0x104, 0);
            break;
        case 8:
            set_le32(adt, sgx(), "perf-state-count", 13);
            break;
        case 9:
            set_le32(adt, sgx(), "gpu-num-perf-states", 14);
            break;
        case 10:
            set_le32(adt, sgx(), "perf-state-table-count", 2);
            break;
        case 11:
            set_le32(adt, sgx(), "gpu-power-sample-period", 10001);
            break;
        case 12:
            set_le64(adt, sgx(), "gpu-region-base", RAM_BASE + 1);
            break;
        case 13:
            set_le64(adt, sgx(), "gfx-handoff-base", RAM_BASE);
            break;
        case 14:
            set_le64(adt, sgx(), "gpu-region-base", RAM_BASE - 0x4000);
            break;
        case 15:
            set_le64(adt, sgx(), "gpu-region-size", UINT64_MAX);
            break;
        case 16:
            set_le64(adt, sgx(), "gfx-data-base", RAM_BASE + 0x1f00000);
            break;
        case 17:
            ((u8 *)ram + 6 * 0x200000)[0] ^= 1;
            break;
        case 18:
            ((u8 *)ram + 7 * 0x200000)[0] ^= 1;
            break;
        case 19:
            memset((u8 *)ram + 6 * 0x200000 + 0x23c, 0, 8);
            break;
        case 20:
            memset((u8 *)ram + 7 * 0x200000 + images[1].entropy[0], 0, 4);
            break;
        case 21:
            set_le32(adt, fdt_path_offset(adt, "/arm-io/gfx-asc/iop-gfx-nub"), "pre-loaded", 0);
            break;
        case 22:
            assert(!fdt_delprop(adt, sgx(), "perf-states-sram"));
            break;
        case 23: {
            int node = add(dt, "/reserved-memory", "other-owner");
            fdt64_t reg[] = {cpu_to_fdt64(RAM_BASE), cpu_to_fdt64(0x4000)};
            assert(!fdt_setprop(dt, node, "reg", reg, sizeof(reg)));
            break;
        }
        case 24:
            assert(!fdt_setprop_u32(dt, gpu, "operating-points-v2", 9999));
            break;
        case 25:
            assert(!fdt_pack(dt));
            break;
        case 26: {
            int len;
            struct t8142_pstate *ps = (void *)fdt_getprop(adt, sgx(), "perf-states", &len);
            ps[2].hz = ps[1].hz;
            break;
        }
        case 27: {
            int len;
            struct t8142_pstate *ps = (void *)fdt_getprop(adt, sgx(), "perf-states-sram", &len);
            ps[2].mv = 2001;
            break;
        }
        case 28:
            chip_id = T8140;
            noop = 1;
            break;
        case 29:
            expect = 1;
            mutate_cal(dt, 0x100, 0x66);
            break; /* Unknown coefficients pass through. */
        case 30: {
            const u8 *p = fdt_getprop(dt, gpu, "apple,t8142-gpu-calibration", NULL);
            u8 short_cal[511];
            memcpy(short_cal, p, sizeof(short_cal));
            assert(
                !fdt_setprop(dt, gpu, "apple,t8142-gpu-calibration", short_cal, sizeof(short_cal)));
            break;
        }
        case 31: {
            int len;
            struct t8142_pstate *ps = (void *)fdt_getprop(adt, sgx(), "perf-states", &len);
            ps[3].hz++;
            break;
        }
        case 32:
            expect = 1;
            assert(!fdt_add_mem_rsv(dt, RAM_BASE, RAM_SIZE));
            break;
        case 33: {
            int alias = fdt_path_offset(dt, "/aliases");
            assert(!fdt_setprop_string(dt, alias, "gpu", "/soc"));
            break;
        }
        case 34:
            assert(!fdt_setprop_string(dt, gpu, "status", "okay"));
            assert(!fdt_delprop(dt, gpu, "apple,t8142-gpu-calibration"));
            break;
        case 35:
            expect = 1;
            break; /* Repeat preparation. */
        case 37:
        case 38:
        case 39:
        case 40:
        case 41:
            fail_alloc = scenario - 36;
            break;
        case 42:
            unstable = 1;
            break;
        case 43:
            trap_read = 1;
            break;
        case 47:
            assert(!fdt_delprop(dt, gpu, "status"));
            assert(!fdt_pack(dt));
            break;
        case 36:
            break; /* Production identity rejects synthetic firmware. */
        default:
            assert(!"unknown scenario");
    }
    /* Expected rollback includes only the unconditional disabled status. */
    memcpy(before, dt, CAPACITY);
    if (!noop && scenario != 33) {
        int n = t8142_gpu_node(before);
        if (n >= 0 && fdt_setprop_string(before, n, "status", "disabled"))
            fdt_nop_node(before, n);
    }
    if (scenario == 36)
        assert(!dt_set_gpu_t8142_26(dt));
    else
        assert(!t8142_set_gpu(dt, images));
    if (expect) {
        assert_enabled(dt);
        if (scenario == 35) {
            memcpy(before, dt, CAPACITY);
            assert(!t8142_set_gpu(dt, images));
            assert_enabled(dt);
            assert(fdt_totalsize(before) == fdt_totalsize(dt));
            /* Property insertion can reorder nodes once; handles/ranges/count stay fixed. */
        }
        assert(!memcmp((const u8 *)ram + 6 * 0x200000 + 0x23c, &(u64){RAM_BASE + 4 * 0x200000}, 8));
        assert(((u8 *)ram + 6 * 0x200000)[images[0].entropy[0] + 8] == 0xa5);
    } else {
        assert(!memcmp(before, dt, fdt_totalsize(dt)));
    }
    free(dt);
    free(before);
    free(adt);
    free(ram);
    puts("PASS");
    return 0;
}
