/* SPDX-License-Identifier: GPL-2.0-only */

/*
 * T8142 (M5) internal PCIe: train the links of every root port the ADT
 * describes, so the OS can drive the root complex as a generic ECAM host.
 * The sequence follows macOS AppleT8142PCIe / APCIECoreRCGen4: root complex
 * power, AXI2AF and PHY tunables, Gen4 PHY power-up, root complex enable;
 * then per port the refclk buffer, port reset, config tunables, root port DBI
 * tunables, endpoint power, PERST# and the LTSSM.
 *
 * ADT /arm-io/apcie reg map: 0 ECAM, 1 RC common, 2 PHY glue (common block at
 * +0x4000), 3 PHY control, 4 PHY IP, 5 AXI2AF (6 and 7 are not used here); six per
 * port: 0 port config, 2 PHY lane.
 *
 * What is left to the OS: the root complex and every trained port running,
 * links up at the ADT speed, endpoints powered and out of reset. Generic ECAM
 * hosts also get bus numbers (secondary bus = port order + 1). A native Apple
 * host programs its own bus numbers and can train a READY port with its link
 * down. RID-to-SID and MSI
 * maps and DARTs remain untouched; Linux installs its own routes. No endpoint
 * configuration writes or DMA are issued here.
 */

#include "adt.h"
#include "devicetree.h"
#include "pcie.h"
#include "pmgr.h"
#include "smc.h"
#include "tunables.h"
#include "utils.h"

#include "libfdt/libfdt.h"

#define APCIE "/arm-io/apcie"

#define APCIE_SHARED_REGS 8
#define APCIE_PORT_REGS   6
#define APCIE_MAX_PORTS   4

#define REG_ECAM   0
#define REG_RC     1
#define REG_GLUE   2
#define REG_PHY    3
#define REG_PHY_IP 4
#define REG_AXI    5

#define PORT_REG_CONFIG 0
#define PORT_REG_LANE   2

/* RC common */
#define RC_CTRL_4      0x4
#define RC_RUN         0x50
#define RC_RUN_EN      BIT(0)
#define RC_GTB_CFG     0x54
#define RC_GTB_CFG_VAL 0x140
#define RC_RUN_ACK     0x58
#define RC_RUN_ACK_OK  BIT(0)

/* PHY glue common block */
#define GLUE_COMMON          0x4000
#define GLUE_COMMON_CLK_MODE BIT(0)
#define GLUE_COMMON_REFCLK   BIT(31)

/* PHY control and PHY lanes share the clock request layout */
#define PHY_CTRL          0x0
#define PHY_CTRL_CLK0REQ  BIT(0)
#define PHY_CTRL_CLK1REQ  BIT(1)
#define PHY_CTRL_CLK0ACK  BIT(2)
#define PHY_CTRL_CLK1ACK  BIT(3)
#define PHY_CTRL_RESET    BIT(4)
#define PHY_CTRL_LANE_EN0 BIT(9)
#define PHY_CTRL_LANE_EN1 BIT(10)
#define PHY_CTRL_DYN_PG   BIT(27)
#define PHY_CTRL_CLKS    (PHY_CTRL_CLK0REQ | PHY_CTRL_CLK1REQ | PHY_CTRL_CLK0ACK | PHY_CTRL_CLK1ACK)
#define PHY_AUX          0x4
#define PHY_AUX_PRE_PLL  BIT(0)
#define PHY_AUX_POST_PLL BIT(4)
#define PHY_STATUS       0x8
#define PHY_STATUS_PCLK  BIT(0)

#define PHY_IP_MISC      0x90
#define PHY_IP_MISC_HOLD BIT(0)

/* Port config */
#define PORT_LTSSMCTL       0x80
#define PORT_LTSSMCTL_START BIT(0)
#define PORT_LINKSTS        0x208
#define PORT_LINKSTS_UP     BIT(0)
#define PORT_LINKSTS_BUSY   BIT(2)
#define PORT_APPCLK         0x800
#define PORT_APPCLK_EN      BIT(0)
#define PORT_APPCLK_CGDIS   BIT(8)
#define PORT_STATUS         0x804
#define PORT_STATUS_RUN     BIT(0)
#define PORT_PERST          0x82c
#define PORT_PERST_DIS      BIT(0)
#define PORT_RID2SID        0x3000
#define PORT_RID2SID_COUNT  16
#define PORT_MSIMAP         0x3800
#define PORT_MSIMAP_COUNT   512

/* Root port config space (ECAM) */
#define PCI_BUS_NUMBERS      0x18
#define PCIE_CAP             0x70
#define PCIE_LNKCAP          (PCIE_CAP + 0x0c)
#define PCIE_LNKCAP_SLS      GENMASK(3, 0)
#define PCIE_LNKCAP_MLW      GENMASK(9, 4)
#define PCIE_LNKCAP2         (PCIE_CAP + 0x2c)
#define PCIE_LNKCAP2_SLS     GENMASK(6, 1)
#define PCIE_LNKCTL2         (PCIE_CAP + 0x30)
#define PCIE_LNKCTL2_TLS     GENMASK(3, 0)
#define DWC_PORT_LINK_CTRL   0x710
#define DWC_PORT_LINK_MODE   GENMASK(21, 16)
#define DWC_LINK_WIDTH_SPEED 0x80c
#define DWC_LINK_WIDTH       GENMASK(12, 8)
#define DWC_SPEED_CHANGE     BIT(17)
#define DWC_DBI_RO_WR        0x8bc
#define DWC_DBI_RO_WR_EN     BIT(0)

/* AP GPIO pin registers */
#define GPIO_DATA     BIT(0)
#define GPIO_MODE     GENMASK(3, 1)
#define GPIO_MODE_OUT 1
#define GPIO_PERIPH   GENMASK(6, 5)
#define GPIO_INPUT_EN BIT(9)

#define ECAM_PORT_STRIDE (1 << 15)
#define ECAM_BUS_SHIFT   20

/* Endpoint power, then PERST#, then the LTSSM, as the macOS port start does. */
#define ENDPOINT_POWER_MS 150
#define PERST_TO_LTSSM_MS 100

/* ADT function-* records */
struct adt_function_gpio {
    u32 phandle;
    char four_cc[4];
    u32 pin;
    u32 arg;
} PACKED;

struct adt_function_smc_key {
    u32 phandle;
    char four_cc[4];
    u32 key;
    u32 value;
} PACKED;

struct adt_function_port_control {
    u32 phandle;
    char four_cc[4];
    u32 bridge;
} PACKED;

struct apcie_port {
    char path[64];
    u32 index;
    u32 phandle;
    u64 config;
    u64 lane;
    u64 ecam;
    u64 perst, clkreq;
    u32 clkreq_arg;
};

struct apcie {
    int path[8];
    u64 ecam, rc, glue, phy, phy_ip, axi;
    u32 nports;
    struct apcie_port ports[APCIE_MAX_PORTS];
};

static int apcie_wait(u64 addr, u32 mask, u32 target, u32 timeout_us, const char *what)
{
    if (poll32(addr, mask, target, timeout_us)) {
        printf("pcie: timeout waiting for %s (0x%x)\n", what, read32(addr));
        return -1;
    }
    return 0;
}

static int apcie_tunables(const char *path, const char *prop, u64 base)
{
    if (tunables_apply_local_addr(path, prop, base)) {
        printf("pcie: applying %s/%s failed\n", path, prop);
        return -1;
    }
    return 0;
}

/* The ADT describes the ports present on this machine as pci-bridgeN children. */
static int apcie_describe(struct apcie *pc, u32 ports)
{
    int node = adt_path_offset_trace(adt, APCIE, pc->path);
    u32 nports, len;

    if (node < 0 || !adt_is_compatible(adt, node, "apcie,t8142"))
        return -1;
    if (ADT_GETPROP(adt, node, "#ports", &nports) < 0 || !nports || nports > APCIE_MAX_PORTS ||
        !adt_getprop(adt, node, "reg", &len) ||
        len != 16 * (APCIE_SHARED_REGS + APCIE_PORT_REGS * nports)) {
        printf("pcie: unexpected %s port geometry\n", APCIE);
        return -1;
    }
    if (adt_get_reg(adt, pc->path, "reg", REG_ECAM, &pc->ecam, NULL) ||
        adt_get_reg(adt, pc->path, "reg", REG_RC, &pc->rc, NULL) ||
        adt_get_reg(adt, pc->path, "reg", REG_GLUE, &pc->glue, NULL) ||
        adt_get_reg(adt, pc->path, "reg", REG_PHY, &pc->phy, NULL) ||
        adt_get_reg(adt, pc->path, "reg", REG_PHY_IP, &pc->phy_ip, NULL) ||
        adt_get_reg(adt, pc->path, "reg", REG_AXI, &pc->axi, NULL))
        return -1;

    int bridge = node;
    pc->nports = 0;
    ADT_FOREACH_CHILD(adt, bridge)
    {
        struct apcie_port *port;
        u32 index;
        const char *name = adt_get_name(adt, bridge);

        if (strncmp(name, "pci-bridge", 10))
            continue;
        if (ADT_GETPROP(adt, bridge, "apcie-port", &index) != sizeof(index) || index >= nports)
            return -1;
        if (!(ports & BIT(index)))
            continue;
        ports &= ~BIT(index);
        if (pc->nports == ARRAY_SIZE(pc->ports))
            return -1;
        port = &pc->ports[pc->nports];
        port->index = index;
        if (ADT_GETPROP(adt, bridge, "AAPL,phandle", &port->phandle) != sizeof(port->phandle))
            return -1;
        snprintf(port->path, sizeof(port->path), "%s/%s", APCIE, name);
        int base = APCIE_SHARED_REGS + APCIE_PORT_REGS * port->index;
        if (adt_get_reg(adt, pc->path, "reg", base + PORT_REG_CONFIG, &port->config, NULL) ||
            adt_get_reg(adt, pc->path, "reg", base + PORT_REG_LANE, &port->lane, NULL))
            return -1;
        port->ecam = pc->ecam + (u64)port->index * ECAM_PORT_STRIDE;
        pc->nports++;
    }
    return pc->nports && !ports ? 0 : -1;
}

static bool apcie_phy_running(struct apcie *pc)
{
    return (read32(pc->phy + PHY_CTRL) & (PHY_CTRL_CLKS | PHY_CTRL_RESET)) == PHY_CTRL_CLKS &&
           (read32(pc->phy + PHY_AUX) & (PHY_AUX_PRE_PLL | PHY_AUX_POST_PLL)) ==
               (PHY_AUX_PRE_PLL | PHY_AUX_POST_PLL);
}

/* Request both clocks of a PHY control or lane register and release its reset. */
static int apcie_phy_clocks(u64 reg, const char *what)
{
    set32(reg, PHY_CTRL_CLK0REQ);
    if (apcie_wait(reg, PHY_CTRL_CLK0ACK, PHY_CTRL_CLK0ACK, 100000, what))
        return -1;
    udelay(1);
    set32(reg, PHY_CTRL_CLK1REQ);
    if (apcie_wait(reg, PHY_CTRL_CLK1ACK, PHY_CTRL_CLK1ACK, 100000, what))
        return -1;
    clear32(reg, PHY_CTRL_RESET);
    udelay(1);
    return 0;
}

static int apcie_init_rc(struct apcie *pc)
{
    /*
     * iBoot can leave the RUN acknowledgement set with RUN itself cleared and
     * the PHY in reset, so RUN is what tells a running root complex from a
     * cold one. A running one is adopted only with its PHY up; anything in
     * between is refused rather than reset under a possible user.
     */
    if (read32(pc->rc + RC_RUN) & RC_RUN_EN) {
        if (!(read32(pc->rc + RC_RUN_ACK) & RC_RUN_ACK_OK) || !apcie_phy_running(pc)) {
            printf("pcie: root complex half started (ack 0x%x, phy 0x%x/0x%x), not touching it\n",
                   read32(pc->rc + RC_RUN_ACK), read32(pc->phy + PHY_CTRL),
                   read32(pc->phy + PHY_AUX));
            return -1;
        }
        printf("pcie: root complex already running, adopting it\n");
        return 0;
    }

    int node = adt_path_offset(adt, APCIE);
    if (apcie_tunables(APCIE, "apcie-axi2af-tunables", pc->axi) ||
        (adt_getprop(adt, node, "apcie-common-tunables", NULL) &&
         apcie_tunables(APCIE, "apcie-common-tunables", pc->rc)))
        return -1;
    write32(pc->rc + RC_CTRL_4, 0);
    if (apcie_tunables(APCIE, "apcie-phy-tunables", pc->glue) ||
        apcie_wait(pc->glue + GLUE_COMMON, GLUE_COMMON_REFCLK, GLUE_COMMON_REFCLK, 250000,
                   "reference clock"))
        return -1;

    /* AppleT8142PCIe::_initializeGen4Phy */
    if (apcie_phy_clocks(pc->phy + PHY_CTRL, "PHY clocks"))
        return -1;
    set32(pc->phy + PHY_AUX, PHY_AUX_PRE_PLL);
    if (apcie_tunables(APCIE, "apcie-phy-ip-pll-tunables", pc->phy_ip) ||
        apcie_tunables(APCIE, "apcie-phy-ip-auspma-tunables", pc->phy_ip))
        return -1;
    clear32(pc->phy_ip + PHY_IP_MISC, PHY_IP_MISC_HOLD);
    set32(pc->phy + PHY_AUX, PHY_AUX_POST_PLL);
    if (apcie_wait(pc->phy + PHY_STATUS, PHY_STATUS_PCLK, PHY_STATUS_PCLK, 250000, "PHY pclk"))
        return -1;

    set32(pc->glue + GLUE_COMMON, GLUE_COMMON_CLK_MODE);
    set32(pc->phy + PHY_CTRL, PHY_CTRL_DYN_PG);

    /* APCIECoreRCGen4::_enableRootComplex */
    write32(pc->rc + RC_GTB_CFG, RC_GTB_CFG_VAL);
    set32(pc->rc + RC_RUN, RC_RUN_EN);
    return apcie_wait(pc->rc + RC_RUN_ACK, RC_RUN_ACK_OK, RC_RUN_ACK_OK, 250000,
                      "root complex RUN");
}

/* The register of pin @fn's GPIO, from the ADT node its phandle names. */
static int apcie_gpio(const struct adt_function_gpio *fn, u64 *reg)
{
    int arm_io = adt_path_offset(adt, "/arm-io");
    int node = arm_io;

    if (arm_io < 0 || memcmp(fn->four_cc, "OIPG", 4))
        return -1;
    ADT_FOREACH_CHILD(adt, node)
    {
        u32 phandle;
        int path[8];
        char name[64];
        u64 base, size;

        if (ADT_GETPROP(adt, node, "AAPL,phandle", &phandle) < 0 || phandle != fn->phandle)
            continue;
        snprintf(name, sizeof(name), "/arm-io/%s", adt_get_name(adt, node));
        if (adt_path_offset_trace(adt, name, path) < 0 ||
            adt_get_reg(adt, path, "reg", 0, &base, &size) || size < 4 ||
            fn->pin > (size - 4) / 4 || base > UINT64_MAX - size)
            return -1;
        *reg = base + 4 * fn->pin;
        return 0;
    }
    return -1;
}

static int apcie_port_gpio(struct apcie_port *port, const char *prop, u64 *reg, u32 *arg)
{
    int node = adt_path_offset(adt, port->path);
    struct adt_function_gpio fn;

    if (node < 0 || ADT_GETPROP(adt, node, prop, &fn) != sizeof(fn) || apcie_gpio(&fn, reg)) {
        printf("pcie: %s has no usable %s\n", port->path, prop);
        return -1;
    }
    *arg = fn.arg;
    return 0;
}

/* Write an SMC key function (pKW4) unless this port already wrote the same key. */
static int apcie_smc_function(smc_dev_t *smc, int node, const char *prop, bool pulse, u32 *done,
                              u32 *ndone)
{
    struct adt_function_smc_key fn;

    if (ADT_GETPROP(adt, node, prop, &fn) != sizeof(fn) || memcmp(fn.four_cc, "4WKp", 4))
        return 0;
    for (u32 i = 0; i < *ndone; i++)
        if (done[i] == fn.key)
            return 0;
    if (*ndone < 8)
        done[(*ndone)++] = fn.key;

    printf("pcie: %s: SMC key %c%c%c%c = 0x%x\n", prop, fn.key >> 24, fn.key >> 16, fn.key >> 8,
           fn.key, fn.value | 1);
    if (pulse) {
        if (smc_write_u32(smc, fn.key, fn.value))
            return -1;
        mdelay(50);
    }
    return smc_write_u32(smc, fn.key, fn.value | 1) ? -1 : 0;
}

static int apcie_endpoint_functions(smc_dev_t *smc, int node, u32 *done, u32 *ndone)
{
    if (apcie_smc_function(smc, node, "function-reg_on", false, done, ndone) ||
        apcie_smc_function(smc, node, "function-sd_pwr_en", false, done, ndone))
        return -1;
    return 0;
}

static bool apcie_controls_port(int node, u32 bridge)
{
    static const char *const props[] = {"function-pcie_port_control",
                                        "function-pcie_port_control_sd"};

    for (u32 i = 0; i < ARRAY_SIZE(props); i++) {
        struct adt_function_port_control fn;
        if (ADT_GETPROP(adt, node, props[i], &fn) == sizeof(fn) && !memcmp(fn.four_cc, "CtrP", 4) &&
            fn.bridge == bridge)
            return true;
    }
    return false;
}

/*
 * Power the endpoint behind a port through the SMC: the bridge's own reset
 * line, the power enables of its children, and those of any node that names
 * the bridge in a pcie_port_control function (such as the Wi-Fi module's
 * /amfm). A port with no such function has an always-on endpoint.
 */
static int apcie_endpoint_power(struct apcie_port *port)
{
    int bridge = adt_path_offset(adt, port->path);
    u32 done[8], ndone = 0;
    int ret = -1;

    if (bridge < 0)
        return -1;
    smc_dev_t *smc = smc_init();
    if (!smc)
        return -1;

    if (apcie_smc_function(smc, bridge, "function-reset", true, done, &ndone))
        goto out;
    int child = bridge;
    ADT_FOREACH_CHILD(adt, child)
    {
        if (apcie_endpoint_functions(smc, child, done, &ndone))
            goto out;
    }
    int node = 0;
    ADT_FOREACH_CHILD(adt, node)
    {
        if (apcie_controls_port(node, port->phandle) &&
            apcie_endpoint_functions(smc, node, done, &ndone))
            goto out;
    }
    ret = 0;
out:
    smc_shutdown(smc);
    return ret;
}

static u32 apcie_link_speed(struct apcie_port *port)
{
    int bridge = adt_path_offset(adt, port->path);
    u32 speed = 0;

    if (bridge < 0 || ADT_GETPROP(adt, bridge, "maximum-link-speed", &speed) < 0)
        return 0;
    /* Some endpoints supply the real limit below a bridge capped at Gen1. */
    if (speed == 1) {
        int child = adt_first_child_offset(adt, bridge);
        u32 target;
        if (child >= 0) {
            if (ADT_GETPROP(adt, child, "target-link-speed", &target) == sizeof(target) && target)
                speed = target;
            else if (ADT_GETPROP(adt, child, "expected-link-speed", &target) == sizeof(target) &&
                     target)
                speed = target;
        }
    }
    return min(speed, 4U);
}

/* APCIECoreRCGen4Port::_initializeRefclkBuffer; the port's registers need it first. */
static int apcie_port_refclk(struct apcie_port *port)
{
    if (apcie_phy_clocks(port->lane, "lane clocks"))
        return -1;
    set32(port->lane, PHY_CTRL_LANE_EN0);
    set32(port->lane, PHY_CTRL_LANE_EN1);
    return 0;
}

/* APCIECoreRCGen4Port::enablePortHardware */
static int apcie_port_enable(struct apcie_port *port)
{
    static const struct {
        u32 off, val;
    } reset[] = {
        {0x88, 0x110}, {0x80, 0},  {0x84, 0},         {0x104, 0xfffffff0}, {0x124, 0x100},
        {0x16c, 0},    {0x13c, 0}, {0x800, 0x100100}, {0x808, 0x1001ff},   {0x82c, 0x10000},
    };
    static const struct {
        u32 off, val;
    } reset2[] = {
        {0x130, 0x3020000}, {0x140, 0x10}, {0x144, 0x253770}, {0x21c, 0}, {0x834, 0}, {0x83c, 0},
    };

    for (u32 i = 0; i < ARRAY_SIZE(reset); i++)
        write32(port->config + reset[i].off, reset[i].val);
    for (u32 i = 0; i < ARRAY_SIZE(reset2); i++)
        write32(port->config + reset2[i].off, reset2[i].val);

    if (apcie_tunables(port->path, "apcie-config-tunables", port->config))
        return -1;
    set32(port->config + PORT_APPCLK, PORT_APPCLK_EN);
    set32(port->config + PORT_PERST, PORT_PERST_DIS);
    if (apcie_wait(port->config + PORT_STATUS, PORT_STATUS_RUN, PORT_STATUS_RUN, 250000,
                   "port RUN"))
        return -1;
    clear32(port->config + PORT_APPCLK, PORT_APPCLK_CGDIS);
    return apcie_wait(port->config + PORT_LINKSTS, PORT_LINKSTS_BUSY, 0, 250000, "port idle");
}

/* Root port DBI: ADT tunables, then the ADT link speed on a single lane. */
static int apcie_port_root(struct apcie_port *port)
{
    u32 speed = apcie_link_speed(port);

    if (!speed) {
        printf("pcie: %s has no maximum-link-speed\n", port->path);
        return -1;
    }

    set32(port->ecam + DWC_DBI_RO_WR, DWC_DBI_RO_WR_EN);
    if (apcie_tunables(port->path, "pcie-rc-tunables", port->ecam) ||
        apcie_tunables(port->path, "pcie-rc-gen3-shadow-tunables", port->ecam) ||
        apcie_tunables(port->path, "pcie-rc-gen4-shadow-tunables", port->ecam)) {
        clear32(port->ecam + DWC_DBI_RO_WR, DWC_DBI_RO_WR_EN);
        return -1;
    }
    mask32(port->ecam + PCIE_LNKCAP, PCIE_LNKCAP_SLS | PCIE_LNKCAP_MLW,
           FIELD_PREP(PCIE_LNKCAP_SLS, speed) | FIELD_PREP(PCIE_LNKCAP_MLW, 1));
    mask32(port->ecam + PCIE_LNKCAP2, PCIE_LNKCAP2_SLS,
           FIELD_PREP(PCIE_LNKCAP2_SLS, (1 << speed) - 1));
    mask16(port->ecam + PCIE_LNKCTL2, PCIE_LNKCTL2_TLS, FIELD_PREP(PCIE_LNKCTL2_TLS, speed));
    mask32(port->ecam + DWC_PORT_LINK_CTRL, DWC_PORT_LINK_MODE, FIELD_PREP(DWC_PORT_LINK_MODE, 1));
    mask32(port->ecam + DWC_LINK_WIDTH_SPEED, DWC_LINK_WIDTH, FIELD_PREP(DWC_LINK_WIDTH, 1));
    set32(port->ecam + DWC_LINK_WIDTH_SPEED, DWC_SPEED_CHANGE);
    clear32(port->ecam + DWC_DBI_RO_WR, DWC_DBI_RO_WR_EN);
    return 0;
}

static int apcie_port_link(struct apcie *pc, struct apcie_port *port, u32 bus, bool native)
{
    if (apcie_endpoint_power(port)) {
        printf("pcie: %s: powering the endpoint failed\n", port->path);
        return -1;
    }
    mdelay(ENDPOINT_POWER_MS);

    mask32(port->perst, GPIO_DATA | GPIO_MODE | GPIO_PERIPH,
           FIELD_PREP(GPIO_MODE, GPIO_MODE_OUT) | GPIO_DATA);
    mdelay(PERST_TO_LTSSM_MS);

    write32(port->config + PORT_LTSSMCTL, PORT_LTSSMCTL_START);
    if (apcie_wait(port->config + PORT_LINKSTS, PORT_LINKSTS_UP, PORT_LINKSTS_UP, 1000000,
                   "link up"))
        return 1;

    /* Linux programs bus numbers while enumerating the native Apple host. */
    if (native)
        return 0;

    /* Give the generic root port its bus number and check the endpoint. */
    write32(port->ecam + PCI_BUS_NUMBERS, (bus << 16) | (bus << 8));
    u32 ident = read32(pc->ecam + ((u64)bus << ECAM_BUS_SHIFT));
    printf("pcie: %s: link up (0x%x), bus %u endpoint %04x:%04x\n", port->path,
           read32(port->config + PORT_LINKSTS), bus, ident & 0xffff, ident >> 16);
    if ((ident & 0xffff) == 0xffff || !(ident & 0xffff))
        return -1;
    return 0;
}

static int apcie_init_port(struct apcie *pc, struct apcie_port *port, u32 bus, bool native)
{
    u32 lane = read32(port->lane);

    /* A linked port belongs to the previous stage. Never replay its reset. */
    if ((lane & (PHY_CTRL_CLKS | PHY_CTRL_RESET | PHY_CTRL_LANE_EN0 | PHY_CTRL_LANE_EN1)) ==
            (PHY_CTRL_CLKS | PHY_CTRL_LANE_EN0 | PHY_CTRL_LANE_EN1) &&
        (read32(port->config + PORT_APPCLK) & PORT_APPCLK_EN) &&
        (read32(port->config + PORT_STATUS) & PORT_STATUS_RUN)) {
        if (native || (read32(port->config + PORT_LINKSTS) & PORT_LINKSTS_UP)) {
            mask32(port->clkreq, GPIO_PERIPH | GPIO_INPUT_EN,
                   FIELD_PREP(GPIO_PERIPH, port->clkreq_arg) | GPIO_INPUT_EN);
            printf("pcie: %s already ready, adopting it\n", port->path);
            return 0;
        }
        return -1;
    }

    /* Never assert an endpoint reset that the previous stage released. */
    if ((read32(port->perst) & (GPIO_DATA | GPIO_MODE | GPIO_PERIPH)) !=
        FIELD_PREP(GPIO_MODE, GPIO_MODE_OUT)) {
        printf("pcie: %s external PERST# is not asserted, refusing reset\n", port->path);
        return -1;
    }
    /* Partial clock requests may belong to an earlier, failed initialization. */
    if (lane & (PHY_CTRL_CLK0REQ | PHY_CTRL_CLK1REQ))
        return -1;
    if (apcie_port_refclk(port))
        return -1;
    /* Only now is the local register bank accessible (especially port 2). */
    if (read32(port->config + PORT_LINKSTS) & PORT_LINKSTS_UP)
        return -1;
    for (u32 i = 0; i < PORT_RID2SID_COUNT; i++)
        if (read32(port->config + PORT_RID2SID + 4 * i))
            return -1;
    if (apcie_port_enable(port) || apcie_port_root(port))
        return -1;
    mask32(port->clkreq, GPIO_PERIPH | GPIO_INPUT_EN,
           FIELD_PREP(GPIO_PERIPH, port->clkreq_arg) | GPIO_INPUT_EN);

    int ret = apcie_port_link(pc, port, bus, native);
    /* The native host owns endpoint reset/power and can train a down link. */
    return native && ret == 1 ? 0 : ret;
}

/* Validate every window, tunable and GPIO before the first power/MMIO operation. */
static int apcie_preflight(struct apcie *pc)
{
    static const struct {
        int index;
        u64 minimum;
        const char *tunable;
    } shared[] = {
        {REG_ECAM, 1 << ECAM_BUS_SHIFT, NULL},
        {REG_RC, RC_RUN_ACK + 4, "apcie-common-tunables"},
        {REG_GLUE, GLUE_COMMON + 4, "apcie-phy-tunables"},
        {REG_PHY, PHY_STATUS + 4, NULL},
        {REG_PHY_IP, PHY_IP_MISC + 4, "apcie-phy-ip-pll-tunables"},
        {REG_AXI, 4, "apcie-axi2af-tunables"},
    };
    u64 base, size, ecam_size;

    for (u32 i = 0; i < ARRAY_SIZE(shared); i++) {
        if (adt_get_reg(adt, pc->path, "reg", shared[i].index, &base, &size) || !base ||
            size < shared[i].minimum || base > UINT64_MAX - size)
            return -1;
        if (shared[i].tunable) {
            int ret = tunables_validate_local(APCIE, shared[i].tunable, size);
            if (ret && !(ret == 1 && shared[i].index == REG_RC))
                return -1;
        }
        if (shared[i].index == REG_PHY_IP &&
            tunables_validate_local(APCIE, "apcie-phy-ip-auspma-tunables", size))
            return -1;
    }
    if (adt_get_reg(adt, pc->path, "reg", REG_ECAM, &base, &ecam_size) ||
        ecam_size < ((u64)(pc->nports + 1) << ECAM_BUS_SHIFT))
        return -1;
    for (u32 i = 0; i < pc->nports; i++) {
        struct apcie_port *port = &pc->ports[i];
        int index = APCIE_SHARED_REGS + APCIE_PORT_REGS * port->index;
        u32 perst_arg;

        if (adt_get_reg(adt, pc->path, "reg", index, &base, &size) || !base ||
            size < PORT_MSIMAP + 4 * PORT_MSIMAP_COUNT || base > UINT64_MAX - size ||
            tunables_validate_local(port->path, "apcie-config-tunables", size))
            return -1;
        if (adt_get_reg(adt, pc->path, "reg", index + PORT_REG_LANE, &base, &size) || !base ||
            size < 4 || base > UINT64_MAX - size)
            return -1;
        static const char *const props[] = {
            "pcie-rc-tunables",
            "pcie-rc-gen3-shadow-tunables",
            "pcie-rc-gen4-shadow-tunables",
        };
        for (u32 j = 0; j < ARRAY_SIZE(props); j++)
            if (tunables_validate_local(port->path, props[j], ECAM_PORT_STRIDE))
                return -1;
        if (!apcie_link_speed(port) ||
            apcie_port_gpio(port, "function-perst", &port->perst, &perst_arg) || perst_arg ||
            apcie_port_gpio(port, "function-clkreq", &port->clkreq, &port->clkreq_arg) ||
            port->clkreq_arg > 3)
            return -1;
    }
    return 0;
}

static int apcie_init(u32 ports, bool native)
{
    struct apcie pc;

    if (apcie_describe(&pc, ports) || apcie_preflight(&pc))
        return -1;
    /* Includes the ST0_AUS5 parents; virtual no_ps parents have no register. */
    if (pmgr_adt_power_enable(APCIE) || apcie_init_rc(&pc))
        return -1;
    for (u32 i = 0; i < pc.nports; i++) {
        if (apcie_init_port(&pc, &pc.ports[i], i + 1, native)) {
            printf("pcie: %s bring-up failed\n", pc.ports[i].path);
            return -1;
        }
    }
    return 0;
}

/* The enabled FDT PCI host whose configuration space is the ADT's ECAM window. */
static int apcie_fdt_host(void *dt)
{
    int path[8];
    u64 ecam;
    int node = -1;

    if (adt_path_offset_trace(adt, APCIE, path) < 0 ||
        adt_get_reg(adt, path, "reg", REG_ECAM, &ecam, NULL))
        return -1;
    while ((node = fdt_node_offset_by_prop_value(dt, node, "device_type", "pci", 4)) >= 0) {
        const char *status = fdt_getprop(dt, node, "status", NULL);
        if (status && strcmp(status, "okay") && strcmp(status, "ok"))
            continue;
        if (dt_get_address(dt, node) == ecam)
            return node;
    }
    return -1;
}

/*
 * Bring up the T8142 internal PCIe ports for an FDT that describes their ECAM
 * host. If anything fails, the host is disabled: the OS would hang on its first
 * config access through a port left half trained. Returns a negative value
 * only if the FDT could not be updated.
 */
int pcie_t8142_prepare_fdt(void *dt)
{
    static int result = 1;
    static u32 initialized_ports;
    static bool prepared_native;

    if (chip_id != T8142)
        return 0;
    int host = apcie_fdt_host(dt);
    if (host < 0)
        return 0;

    bool native = !fdt_node_check_compatible(dt, host, "apple,t8142-pcie");
    u32 ports = 0;
    if (native) {
        int child;
        fdt_for_each_subnode(child, dt, host)
        {
            const char *status = fdt_getprop(dt, child, "status", NULL);
            int len;
            const fdt32_t *reg = fdt_getprop(dt, child, "reg", &len);
            if (status && strcmp(status, "okay") && strcmp(status, "ok"))
                continue;
            if (!reg || len != 5 * sizeof(*reg) || (fdt32_ld(reg) >> 11) >= APCIE_MAX_PORTS)
                goto disable;
            ports |= BIT(fdt32_ld(reg) >> 11);
        }
    } else {
        int node = adt_path_offset(adt, APCIE);
        int bridge = node;
        ADT_FOREACH_CHILD(adt, bridge)
        {
            u32 index;
            if (ADT_GETPROP(adt, bridge, "apcie-port", &index) == sizeof(index) &&
                index < APCIE_MAX_PORTS)
                ports |= BIT(index);
        }
    }
    if (!ports)
        goto disable;
    /* The hardware is set up once; subsequent DTs cannot add cold ports. */
    if (result > 0) {
        result = apcie_init(ports, native);
        initialized_ports = ports;
        prepared_native = native;
    }
    if ((ports & ~initialized_ports) || (!native && prepared_native))
        goto disable;
    if (!result)
        return 0;

disable:
    printf("pcie: T8142 PCIe bring-up failed; disabling %s\n", fdt_get_name(dt, host, NULL));
    return fdt_setprop_string(dt, host, "status", "disabled") ? -1 : 0;
}
