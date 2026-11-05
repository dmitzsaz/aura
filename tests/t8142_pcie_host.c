/* SPDX-License-Identifier: GPL-2.0-only */
#include <assert.h>
#include <stdlib.h>

#include "kboot_display_t8142.c"
#include "kboot_nvme.c"

struct fw_version_info os_firmware = {.version = V27_0};
#define DRAM          0x10000000000UL
#define DART_SCAN     0xb0000000UL
#define DART_FW       0xb1000000UL
#define DART_EXT_SCAN 0xb2000000UL
#define DART_EXT_FW   0xb3000000UL
#include "pcie_t8142.c"

/* Synthetic addresses: no board or private ADT data is embedded in this test. */
#define ECAM    0x10000000UL
#define RC      0x20000000UL
#define GLUE    0x30000000UL
#define PHY     0x40000000UL
#define IP      0x50000000UL
#define AXI     0x60000000UL
#define GPIO    0x70000000UL
#define BANK(p) (0x80000000UL + (p) * 0x10000UL)
#define LANE(p) (0x90000000UL + (p) * 0x10000UL)

void *adt;
u32 chip_id = T8142;
static int scenario, powered, writes, smc_writes, root_tunables;
static int fail_poll, npolls, fail_tunable;
static u32 regfile[2048];
static u64 regaddr[2048];
static unsigned nregs;
static int slot(u64 addr)
{
    for (unsigned i = 0; i < nregs; i++)
        if (regaddr[i] == addr)
            return i;
    assert(nregs < ARRAY_SIZE(regfile));
    regaddr[nregs] = addr;
    return nregs++;
}
static void seed(u64 addr, u32 value)
{
    regfile[slot(addr)] = value;
}
u32 read32(u64 addr)
{
    assert(powered);
    for (unsigned p = 0; p < 3; p++) {
        if (addr >= BANK(p) && addr < BANK(p) + 0x8000)
            assert((regfile[slot(LANE(p))] & 0x1f) == 0xf);
        if (addr >= ECAM + p * ECAM_PORT_STRIDE && addr < ECAM + (p + 1) * ECAM_PORT_STRIDE)
            assert(regfile[slot(BANK(p) + PORT_STATUS)] & PORT_STATUS_RUN);
    }
    if (addr == ECAM + (1 << ECAM_BUS_SHIFT) || addr == ECAM + (2 << ECAM_BUS_SHIFT))
        return 0x123417a0;
    return regfile[slot(addr)];
}
void write32(u64 addr, u32 value)
{
    (void)read32(addr);
    writes++;
    if (addr == PHY || addr == LANE(0) || addr == LANE(2))
        value |= (value & 3) << 2;
    seed(addr, value);
    if (addr == PHY + PHY_AUX && (value & PHY_AUX_POST_PLL)) {
        assert(!(regfile[slot(IP + PHY_IP_MISC)] & BIT(0)));
        seed(PHY + PHY_STATUS, PHY_STATUS_PCLK);
    }
    if (addr == RC + RC_RUN && value & RC_RUN_EN)
        seed(RC + RC_RUN_ACK, RC_RUN_ACK_OK);
    for (unsigned p = 0; p < 3; p += 2) {
        if (addr == BANK(p) + PORT_PERST && value & PORT_PERST_DIS)
            seed(BANK(p) + PORT_STATUS, PORT_STATUS_RUN);
        if (addr == BANK(p) + PORT_LTSSMCTL && value && scenario != 8 && scenario != 16)
            seed(BANK(p) + PORT_LINKSTS, PORT_LINKSTS_UP);
    }
}
void set32(u64 a, u32 v)
{
    write32(a, read32(a) | v);
}
void clear32(u64 a, u32 v)
{
    write32(a, read32(a) & ~v);
}
void mask32(u64 a, u32 m, u32 v)
{
    write32(a, (read32(a) & ~m) | v);
}
void mask16(u64 a, u16 m, u16 v)
{
    mask32(a, m, v);
}
int poll32(u64 a, u32 m, u32 v, u32 timeout)
{
    assert(timeout && timeout <= 1000000);
    if (++npolls == fail_poll)
        return -1;
    return (read32(a) & m) != v;
}
void udelay(u32 us)
{
}
void mdelay(u32 ms)
{
}
int pmgr_adt_power_enable(const char *path)
{
    assert(!strcmp(path, APCIE));
    powered++;
    return 0;
}
int pmgr_adt_is_powered(const char *path)
{
    return scenario == 28 ? 0 : 1;
}
smc_dev_t *smc_init(void)
{
    return (void *)1;
}
int smc_shutdown(smc_dev_t *smc)
{
    return 0;
}
int smc_write_u32(smc_dev_t *smc, u32 key, u32 value)
{
    assert((key == 0x67503133 && value == 0x800001) || (key == 0x67503139 && value == 1));
    smc_writes++;
    return 0;
}
static const char *names[] = {"device-tree", "arm-io", "apcie", "pci-bridge0",
                              "pci-bridge2", "gpio0",  "amfm",  "pcie-sdreader"};
int adt_path_offset(const void *a, const char *path)
{
    if (!strcmp(path, APCIE))
        return 2;
    if (!strcmp(path, "/arm-io"))
        return 1;
    if (!strcmp(path, APCIE "/pci-bridge0"))
        return 3;
    if (!strcmp(path, APCIE "/pci-bridge2"))
        return 4;
    if (!strcmp(path, "/arm-io/gpio0"))
        return 5;
    if (!strcmp(path, "/chosen"))
        return 9;
    if (!strcmp(path, "/arm-io/dart-disp0"))
        return 10;
    if (!strcmp(path, "/arm-io/dart-dcp"))
        return 11;
    if (!strcmp(path, "/arm-io/dart-dispext1"))
        return 12;
    if (!strcmp(path, "/arm-io/dart-dcpext1"))
        return 13;
    if (!strcmp(path, "/arm-io/ans"))
        return 8;
    return -1;
}
int adt_path_offset_trace(const void *a, const char *path, int *trace)
{
    return trace[0] = adt_path_offset(a, path);
}
const char *adt_get_name(const void *a, int node)
{
    return names[node];
}
bool adt_is_compatible(const void *a, int node, const char *compat)
{
    return node == 2 && !strcmp(compat, "apcie,t8142");
}
int adt_get_child_count(const void *a, int n)
{
    return n == 0 ? 2 : n == 1 || n == 2 ? 2 : n == 4 ? 1 : 0;
}
int adt_first_child_offset(const void *a, int n)
{
    return n == 0 ? 1 : n == 1 ? 2 : n == 2 ? 3 : 7;
}
int adt_next_sibling_offset(const void *a, int n)
{
    return n == 1 ? 6 : n == 2 ? 5 : n == 3 ? 4 : -1;
}
const void *adt_getprop(const void *a, int node, const char *prop, u32 *len)
{
    static u32 nports = 3, regs[104];
    if (!strcmp(prop, "clock-frequencies")) {
        static u32 rates[] = {0, 0, 0, 0, 0, 0, 402280000};
        if (len)
            *len = sizeof(rates);
        return rates;
    }
    if (!strcmp(prop, "#ports")) {
        if (len)
            *len = 4;
        return &nports;
    }
    if (!strcmp(prop, "reg")) {
        if (len)
            *len = sizeof(regs);
        return regs;
    }
    return NULL;
}
int adt_getprop_copy(const void *a, int node, const char *prop, void *out, size_t len)
{
    if (!strcmp(prop, "dram-base") || !strcmp(prop, "dram-size")) {
        u64 v = !strcmp(prop, "dram-base") ? DRAM : 0x10000000;
        assert(len == sizeof(v));
        memcpy(out, &v, len);
        return len;
    }
    u32 value;
    if (!strcmp(prop, "#ports"))
        value = 3;
    else if (!strcmp(prop, "apcie-port") && (node == 3 || node == 4))
        value = node == 3 ? 0 : 2;
    else if (!strcmp(prop, "AAPL,phandle"))
        value = node;
    else if (!strcmp(prop, "maximum-link-speed"))
        value = 2;
    else if (!strcmp(prop, "function-perst") || !strcmp(prop, "function-clkreq")) {
        struct adt_function_gpio fn = {5,
                                       {'O', 'I', 'P', 'G'},
                                       (node == 3 ? 0 : 2) +
                                           (!strcmp(prop, "function-clkreq") ? 4 : 0),
                                       !strcmp(prop, "function-clkreq") ? 2 : 0};
        assert(len == sizeof(fn));
        memcpy(out, &fn, len);
        return len;
    } else if ((node == 6 && !strcmp(prop, "function-reg_on")) ||
               (node == 7 && !strcmp(prop, "function-sd_pwr_en"))) {
        struct adt_function_smc_key fn = {
            9, {'4', 'W', 'K', 'p'}, node == 6 ? 0x67503133 : 0x67503139, node == 6 ? 0x800000 : 0};
        assert(len == sizeof(fn));
        memcpy(out, &fn, len);
        return len;
    } else if (node == 6 && !strcmp(prop, "function-pcie_port_control")) {
        struct adt_function_port_control fn = {2, {'C', 't', 'r', 'P'}, 3};
        assert(len == sizeof(fn));
        memcpy(out, &fn, len);
        return len;
    } else
        return -1;
    assert(len == sizeof(value));
    memcpy(out, &value, len);
    return len;
}
int adt_get_reg(const void *a, int *path, const char *prop, int index, u64 *base, u64 *size)
{
    static const u64 shared[] = {ECAM, RC, GLUE, PHY, IP, AXI};
    u64 span = 0x4000;
    if (path[0] >= 10 && path[0] <= 13) {
        *base = DART_SCAN + (path[0] - 10) * 0x1000000;
        span = 0x20000;
    } else if (path[0] == 5) {
        *base = GPIO;
        span = 0x1000;
    } else if (path[0] == 8) {
        *base = 0xa0000000;
        span = scenario == 13 ? 0x30000 : 0x10000;
    } else if (index < 6) {
        *base = shared[index];
        span = index == 0 ? 0x10000000 : index == 2 || index == 4 ? 0x40000 : 0x4000;
    } else {
        assert(index >= 8);
        unsigned port = (index - 8) / 6;
        *base = (index - 8) % 6 == 2 ? LANE(port) : BANK(port);
        span = 0x8000;
    }
    if (size)
        *size = span;
    return 0;
}
int tunables_validate_local(const char *path, const char *prop, u64 span)
{
    assert(!powered);
    return scenario == 3 ? -1 : !strcmp(prop, "apcie-common-tunables") ? 1 : 0;
}
int tunables_apply_local_addr(const char *path, const char *prop, uintptr_t base)
{
    assert(powered);
    if (strstr(prop, "pcie-rc-")) {
        assert(read32(base + DWC_DBI_RO_WR) & DWC_DBI_RO_WR_EN);
        root_tunables++;
    }
    if (fail_tunable && strstr(prop, "pcie-rc-gen3"))
        return -1;
    return 0;
}
u64 dt_get_address(void *dt, int node)
{
    const fdt64_t *reg = fdt_getprop(dt, node, "reg", NULL);
    return reg ? fdt64_ld(reg) : 0;
}
int dart_walk_locked_t8110(uintptr_t base, u64 size, u8 sid, u64 dram, u64 dram_size,
                           int (*visit)(u64, u64, void *), void *opaque)
{
    assert(scenario != 28);
    assert(dram == DRAM && dram_size == 0x10000000);
    assert(size == 0x20000);
    if (scenario == 22 && sid == 16)
        return -1;
    if (scenario == 23 && base == DART_EXT_SCAN)
        return -1;
    /* iBoot's idle external pipe: locked, translating, but no buffers yet. */
    if (scenario >= 29 && scenario <= 31 && base == DART_EXT_SCAN &&
        (scenario == 29 || sid == (scenario == 30 ? 0 : 16)))
        return 0;
    if (scenario == 32 && base == DART_EXT_FW)
        return 0;
    if (visit(0, DRAM + 0x100000, opaque) || visit(SZ_16K, DRAM + 0x104000, opaque))
        return -1;
    /* MMIO must reserve its IOVA without being claimed as system RAM. */
    return visit(0x100000, 0xc0000000, opaque);
}
dart_dev_t *dart_init_fdt_locked(void *dt, u32 phandle, int sid)
{
    return (void *)1;
}
int dart_collect_locked_tables(dart_dev_t *dart, u64 base, u64 size, u64 *pages, size_t capacity,
                               size_t *count)
{
    if (scenario == 26)
        return -1;
    assert(base == DRAM && capacity == 2049);
    *count = 1;
    pages[0] = DRAM + 0x200000;
    return 0;
}
void dart_shutdown(dart_dev_t *dart)
{
}

static void add_alias(void *dt, const char *alias, const char *path)
{
    int aliases = fdt_path_offset(dt, "/aliases");
    assert(!fdt_setprop_string(dt, aliases, alias, path));
}
static void display_tree(void *dt, int capacity)
{
    assert(!fdt_create_empty_tree(dt, capacity));
    assert(!fdt_setprop_u32(dt, 0, "#address-cells", 2));
    assert(!fdt_setprop_u32(dt, 0, "#size-cells", 2));
    assert(fdt_add_subnode(dt, 0, "aliases") >= 0);
    int r = fdt_add_subnode(dt, 0, "reserved-memory");
    assert(r >= 0);
    assert(!fdt_setprop_u32(dt, r, "#address-cells", 2));
    assert(!fdt_setprop_u32(dt, r, "#size-cells", 2));
    assert(!fdt_setprop_empty(dt, r, "ranges"));
    int ref = fdt_add_subnode(dt, 0, "reference");
    assert(!fdt_setprop_u32(dt, ref, "phandle", 20));
    assert(!fdt_setprop_u32(dt, ref, "#clock-cells", 0));
    assert(!fdt_setprop_u32(dt, ref, "clock-frequency", scenario == 25 ? 0 : 24000000));
    for (int i = 0; i < 4; i++) {
        char name[16];
        snprintf(name, sizeof(name), "dart%d", i);
        int n = fdt_add_subnode(dt, 0, name);
        assert(!fdt_setprop_string(dt, n, "compatible", "apple,t8110-dart"));
        assert(!fdt_setprop_u32(dt, n, "phandle", i + 1));
        assert(!fdt_setprop_u32(dt, n, "#iommu-cells", 1));
        fdt64_t reg[] = {cpu_to_fdt64(DART_SCAN + i * 0x1000000), cpu_to_fdt64(0x20000)};
        assert(!fdt_setprop(dt, n, "reg", reg, sizeof(reg)));
        fdt64_t range[] = {cpu_to_fdt64(DRAM), cpu_to_fdt64(1UL << 36)};
        assert(!fdt_setprop(dt, n, "apple,dma-range", range, sizeof(range)));
        assert(!fdt_setprop_string(dt, n, "status", "disabled"));
    }
    const char *owners[] = {"display", "dcp", "piodma", "dcpext1", "ext-piodma"};
    for (int i = 0; i < 5; i++) {
        int n = fdt_add_subnode(dt, 0, owners[i]);
        assert(!fdt_setprop_u32(dt, n, "phandle", 10 + i));
        assert(!fdt_setprop_string(dt, n, "status", "disabled"));
        fdt32_t refs[] = {cpu_to_fdt32(i == 0   ? 1
                                       : i == 1 ? 2
                                       : i == 2 ? 1
                                       : i == 3 ? 4
                                                : 3),
                          cpu_to_fdt32(i == 1 || i == 3   ? 23
                                       : i == 2 || i == 4 ? 16
                                                          : 0),
                          cpu_to_fdt32(3), 0};
        if (scenario == 24 && i == 2)
            refs[1] = cpu_to_fdt32(4);
        assert(!fdt_setprop(dt, n, "iommus", refs, i == 0 ? 16 : 8));
        if (i == 1 || i == 3)
            assert(!fdt_setprop_u32(dt, n, "clocks", 20));
    }
    for (int i = 0; i < 3; i++) {
        char name[16];
        snprintf(name, sizeof(name), "power%d", i);
        int n = fdt_add_subnode(dt, 0, name);
        assert(n >= 0);
        assert(!fdt_setprop_u32(dt, n, "phandle", 30 + i));
        assert(!fdt_setprop_u32(dt, n, "#power-domain-cells", 0));
        assert(!fdt_setprop_string(dt, n, "status", "disabled"));
        if (i)
            assert(!fdt_setprop_u32(dt, n, "power-domains", 29 + i));
    }
    int ext = fdt_path_offset(dt, "/dcpext1");
    assert(!fdt_setprop_u32(dt, ext, "power-domains", 32));
    assert(!fdt_setprop_u32(dt, ext, "mboxes", 33));
    int mailbox = fdt_add_subnode(dt, 0, "mailbox");
    assert(mailbox >= 0);
    assert(!fdt_setprop_u32(dt, mailbox, "phandle", 33));
    assert(!fdt_setprop_string(dt, mailbox, "status", "disabled"));
    int external = fdt_path_offset(dt, "/dcpext1");
    int routes = fdt_add_subnode(dt, external, "typec-routes");
    int route = fdt_add_subnode(dt, routes, "route@0");
    assert(!fdt_setprop_string(dt, route, "status", "disabled"));
    add_alias(dt, "dcp", "/dcp");
    add_alias(dt, "disp0", "/display");
    add_alias(dt, "disp0_piodma", "/piodma");
    add_alias(dt, "dcpext1", "/dcpext1");
    add_alias(dt, "dispext1", "/display");
    add_alias(dt, "dispext1_piodma", "/ext-piodma");
}
static void check_display(void)
{
    int capacity = 128 * 1024;
    void *dt = calloc(1, capacity);
    display_tree(dt, capacity);
    int ret = kboot_t8142_display_reserve(dt, "dcp");
    if (scenario == 22 || scenario == 24 || scenario == 26 || scenario == 28) {
        assert(ret);
        assert(!strcmp(fdt_getprop(dt, fdt_path_offset(dt, "dcp"), "status", NULL), "disabled"));
        free(dt);
        return;
    }
    assert(!ret);
    if (scenario == 27) {
        assert(!kboot_t8142_display_reserve(dt, "dcp"));
        int owner = fdt_path_offset(dt, "dcp"), len;
        const fdt32_t *regions = fdt_getprop(dt, owner, "memory-region", &len);
        assert(regions && len > 0);
        for (int i = 0; i < len / 4; i++)
            assert(fdt_node_offset_by_phandle(dt, fdt32_ld(regions + i)) >= 0);
    }
    ret = kboot_t8142_display_enable(dt, "dcp");
    if (scenario == 25) {
        assert(ret);
        free(dt);
        return;
    }
    assert(!ret);
    bool external = scenario == 21 || scenario == 27 || (scenario >= 29 && scenario <= 31);
    if (external || scenario == 23 || scenario == 32) {
        ret = kboot_t8142_display_reserve(dt, "dcpext1");
        if (scenario == 23 || scenario == 32)
            assert(ret);
        else
            assert(!ret && !kboot_t8142_display_enable(dt, "dcpext1"));
    }
    if (external) {
        int route = fdt_path_offset(dt, "/dcpext1/typec-routes/route@0");
        assert(!strcmp(fdt_getprop(dt, route, "status", NULL), "okay"));
    }
    for (u32 phandle = 30; phandle <= 33; phandle++) {
        int node = fdt_node_offset_by_phandle(dt, phandle);
        assert(!strcmp(fdt_getprop(dt, node, "status", NULL), external ? "okay" : "disabled"));
    }
    assert(!kboot_t8142_display_prune(dt));
    int master = fdt_path_offset(dt, "disp0"), len;
    const fdt32_t *iommus = fdt_getprop(dt, master, "iommus", &len);
    assert(iommus && len == (external ? 16 : 8));
    assert(!strcmp(fdt_getprop(dt, fdt_path_offset(dt, "dcp"), "status", NULL), "okay"));
    const fdt32_t *rate =
        fdt_getprop(dt, fdt_path_offset(dt, "/clock-display-video"), "clock-frequency", &len);
    assert(rate && len == 4 && fdt32_ld(rate) == 402280000);
    int count = fdt_num_mem_rsv(dt);
    assert(count >= 6);
    for (int i = 0; i < count; i++) {
        uint64_t base, size;
        assert(!fdt_get_mem_rsv(dt, i, &base, &size));
        assert(base >= DRAM && size && base + size <= DRAM + 0x10000000);
    }
    free(dt);
}

static void setup(void)
{
    seed(RC + RC_RUN_ACK, 1); /* Retained ACK must not suppress cold init. */
    seed(PHY, PHY_CTRL_RESET);
    seed(IP + PHY_IP_MISC, BIT(0) | BIT(16));
    seed(GLUE + GLUE_COMMON, GLUE_COMMON_REFCLK);
    for (unsigned p = 0; p < 3; p += 2) {
        seed(LANE(p), PHY_CTRL_RESET);
        seed(GPIO + p * 4, FIELD_PREP(GPIO_MODE, GPIO_MODE_OUT));
    }
}
int main(int argc, char **argv)
{
    scenario = atoi(argv[1]);
    setup();
    if (scenario >= 20) {
        check_display();
        return 0;
    }
    unsigned mask = BIT(0) | BIT(2);
    bool native = true;
    if (scenario == 1 || scenario == 2 || scenario == 5) {
        seed(RC + RC_RUN, 1);
        seed(PHY, scenario == 2 ? PHY_CTRL_RESET : PHY_CTRL_CLKS);
        seed(PHY + PHY_AUX, PHY_AUX_PRE_PLL | PHY_AUX_POST_PLL);
        for (unsigned p = 0; p < 3; p += 2) {
            seed(LANE(p), PHY_CTRL_CLKS | PHY_CTRL_LANE_EN0 | PHY_CTRL_LANE_EN1);
            seed(BANK(p) + PORT_APPCLK, PORT_APPCLK_EN);
            seed(BANK(p) + PORT_STATUS, PORT_STATUS_RUN);
            seed(BANK(p) + PORT_LINKSTS, scenario == 5 ? 0 : PORT_LINKSTS_UP);
        }
    }
    if (scenario == 4)
        seed(GPIO, 3); /* External reset already released. */
    if (scenario == 6)
        mask = BIT(0); /* Disabled port 2 must not be accessed. */
    if (scenario == 7)
        fail_poll = 3;
    if (scenario == 9)
        fail_tunable = 1;
    if ((scenario >= 10 && scenario <= 11) || scenario == 19) {
        char dt[8192];
        assert(!fdt_create_empty_tree(dt, sizeof(dt)));
        assert(!fdt_setprop_u32(dt, 0, "#address-cells", 2));
        assert(!fdt_setprop_u32(dt, 0, "#size-cells", 2));
        int h = fdt_add_subnode(dt, 0, "pcie");
        assert(!fdt_setprop_string(dt, h, "compatible", "apple,t8142-pcie"));
        assert(!fdt_setprop_string(dt, h, "device_type", "pci"));
        fdt64_t regs[] = {cpu_to_fdt64(ECAM), cpu_to_fdt64(0x10000000)};
        assert(!fdt_setprop(dt, h, "reg", regs, sizeof(regs)));
        int child = fdt_add_subnode(dt, h, "pci@2,0");
        fdt32_t preg[5] = {cpu_to_fdt32(2 << 11)};
        assert(!fdt_setprop(dt, child, "reg", preg, sizeof(preg)));
        if (scenario == 10)
            assert(!fdt_setprop_string(dt, child, "status", "disabled"));
        if (scenario == 19)
            fail_poll = 1;
        assert(!pcie_t8142_prepare_fdt(dt));
        if (scenario == 19) {
            assert(!strcmp(fdt_getprop(dt, h, "status", NULL), "disabled"));
            int before = writes;
            assert(!pcie_t8142_prepare_fdt(dt) && before == writes);
            return 0;
        }
        if (scenario == 10) {
            assert(!powered);
            assert(!strcmp(fdt_getprop(dt, h, "status", NULL), "disabled"));
        } else {
            assert(smc_writes == 1 && !regfile[slot(BANK(0) + PORT_STATUS)]);
        }
        return 0;
    }
    if (scenario >= 12 && scenario <= 14) {
        char dt[1024];
        assert(!fdt_create_empty_tree(dt, sizeof(dt)));
        assert(!fdt_setprop_u32(dt, 0, "#address-cells", 2));
        assert(!fdt_setprop_u32(dt, 0, "#size-cells", 2));
        int n = fdt_add_subnode(dt, 0, "nvme");
        assert(!fdt_setprop_string(dt, n, "compatible", "apple,t8142-nvme-ans2"));
        assert(!fdt_setprop_string(dt, n, "reg-names", scenario == 14 ? "bad" : "nvme"));
        fdt64_t regs[] = {cpu_to_fdt64(0xa0000000), cpu_to_fdt64(0x30000)};
        assert(!fdt_setprop(dt, n, "reg", regs, sizeof(regs)));
        assert(!dt_set_t8142_nvme(dt));
        if (scenario == 14)
            assert(!strcmp(fdt_getprop(dt, n, "status", NULL), "disabled"));
        else {
            const fdt64_t *reg = fdt_getprop(dt, n, "reg", NULL);
            assert(fdt64_ld(reg + 1) == (scenario == 13 ? 0x30000 : 0x10000));
        }
        assert(!powered);
        return 0;
    }
    if (scenario == 15 || scenario == 16)
        native = false;
    if (scenario == 17)
        seed(LANE(0), PHY_CTRL_CLK0REQ | PHY_CTRL_RESET);
    if (scenario == 18)
        seed(BANK(0) + PORT_RID2SID, 0x1234);
    int ret = apcie_init(mask, native);
    if (scenario == 2 || scenario == 3 || scenario == 4 || scenario == 7 || scenario == 9 ||
        scenario == 16 || scenario == 17 || scenario == 18) {
        assert(ret);
        if (scenario == 3)
            assert(!powered && !writes);
        if (scenario == 9)
            assert(!(regfile[slot(ECAM + DWC_DBI_RO_WR)] & DWC_DBI_RO_WR_EN));
    } else {
        assert(!ret);
        if (scenario != 1 && scenario != 5) {
            assert(!(regfile[slot(IP + PHY_IP_MISC)] & BIT(0)));
            assert(regfile[slot(IP + PHY_IP_MISC)] & BIT(16));
            assert(smc_writes == (scenario == 6 ? 1 : 2));
        } else
            assert(!smc_writes && !root_tunables);
    }
    return 0;
}
