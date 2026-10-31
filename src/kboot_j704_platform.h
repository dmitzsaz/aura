/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Included after dt_append_acio_tunable(): data conversion only, no MMIO. */

static const struct {
    const char *adt_path;
    const char *role;
    struct adt_tunable_info info;
} j704_pcie_tables[] = {
    {"/arm-io/apcie", "shared",
     {"apcie-axi2af-tunables", "apple,tunable-axi2af", 0, 0x1000000, true}},
    {"/arm-io/apcie", "shared",
     {"apcie-phy-tunables", "apple,tunable-phy", 0, 0x40000, true}},
    {"/arm-io/apcie", "shared",
     {"apcie-phy-ip-pll-tunables", "apple,tunable-pll", 0, 0x20000, true}},
    {"/arm-io/apcie", "shared",
     {"apcie-phy-ip-auspma-tunables", "apple,tunable-auspma", 0, 0x20000, true}},
    {"/arm-io/apcie/pci-bridge0", "port0",
     {"apcie-config-tunables", "apple,tunable-config", 0, 0x8000, true}},
    {"/arm-io/apcie/pci-bridge0", "port0",
     {"pcie-rc-tunables", "apple,tunable-root", 0, 0x4000, true}},
    {"/arm-io/apcie/pci-bridge0", "port0",
     {"pcie-rc-gen3-shadow-tunables", "apple,tunable-gen3", 0, 0x4000, true}},
    {"/arm-io/apcie/pci-bridge0", "port0",
     {"pcie-rc-gen4-shadow-tunables", "apple,tunable-gen4", 0, 0x4000, true}},
    {"/arm-io/apcie/pci-bridge2", "port2",
     {"apcie-config-tunables", "apple,tunable-config", 0, 0x8000, true}},
    {"/arm-io/apcie/pci-bridge2", "port2",
     {"pcie-rc-tunables", "apple,tunable-root", 0, 0x4000, true}},
    {"/arm-io/apcie/pci-bridge2", "port2",
     {"pcie-rc-gen3-shadow-tunables", "apple,tunable-gen3", 0, 0x4000, true}},
    {"/arm-io/apcie/pci-bridge2", "port2",
     {"pcie-rc-gen4-shadow-tunables", "apple,tunable-gen4", 0, 0x4000, true}},
};

/* CPU OPPs are supplied by the ADT, including each chip's supported rows. */
static int dt_set_j704_cpu_opps(void)
{
    static const struct {const char *property, *path;} clusters[] = {
        {"voltage-states1-sram", "/opp-table-ecluster"},
        {"voltage-states5-sram", "/opp-table-pcluster"},
    };
    struct cpu_opp {u32 khz, mv;};
    int pmgr=adt_path_offset(adt,"/arm-io/pmgr");
    if (pmgr<0) return -1;
    for (size_t c=0;c<sizeof(clusters)/sizeof(*clusters);c++) {
        u32 bytes;
        const struct cpu_opp *rows=adt_getprop(adt,pmgr,clusters[c].property,&bytes);
        int table=fdt_path_offset(dt,clusters[c].path),child;
        /* Older templates need not describe cpufreq at all. */
        if (table<0) continue;
        if (!rows || !bytes || bytes%sizeof(*rows) || bytes/sizeof(*rows)>32) return -1;
        for (size_t i=0;i<bytes/sizeof(*rows);i++)
            if (!rows[i].khz || (i && rows[i].khz<=rows[i-1].khz)) return -1;
        while ((child=fdt_first_subnode(dt,table))>=0)
            if (fdt_del_node(dt,child)) return -1;
        for (size_t i=0;i<bytes/sizeof(*rows);i++) {
            char name[24];snprintf(name,sizeof(name),"opp%02u",(u32)i+2);
            table=fdt_path_offset(dt,clusters[c].path);
            child=fdt_add_subnode(dt,table,name);
            if (child<0 || fdt_setprop_u64(dt,child,"opp-hz",(u64)rows[i].khz*1000) ||
                fdt_setprop_u32(dt,child,"opp-level",i+2)) return -1;
        }
    }
    return 0;
}

static int dt_set_j704_platform_data(void)
{
    static const char root_path[] = "/j704-platform-data";
    char path[96];
    int root, node, adt_node, ret;

    if (chip_id != T8142 || fdt_node_check_compatible(dt, 0, "apple,j704"))
        return 0;
    root = fdt_path_offset(dt, root_path);
    if (root < 0)
        return 0; /* Older board descriptions have no consumer for this ABI. */
    if (fdt_node_check_compatible(dt, root, "apple,j704-platform-data"))
        return -1;
    fdt_delprop(dt, root, "apple,pcie-tunables-ready");

    for (size_t i = 0; i < sizeof(j704_pcie_tables) / sizeof(*j704_pcie_tables); i++) {
        const struct adt_tunable_info *info = &j704_pcie_tables[i].info;

        adt_node = adt_path_offset(adt, j704_pcie_tables[i].adt_path);
        snprintf(path, sizeof(path), "%s/%s", root_path, j704_pcie_tables[i].role);
        node = fdt_path_offset(dt, path);
        if (adt_node < 0 || node < 0)
            goto refuse;
        /* Repeated DT preparation replaces data instead of concatenating it. */
        fdt_delprop(dt, node, info->fdt_name);
        u32 length = 0;
        if (!adt_getprop(adt, adt_node, info->adt_name, &length) ||
            !length || length > 4096 * sizeof(struct acio_tunable))
            goto refuse;
        ret = dt_append_acio_tunable(adt_node, node, info);
        if (ret)
            goto refuse;
    }
    if (dt_set_j704_cpu_opps()) goto refuse;
    root = fdt_path_offset(dt, root_path);
    if (root < 0 || fdt_setprop_u32(dt, root, "apple,format-version", 1) ||
        fdt_setprop_empty(dt, root, "apple,pcie-tunables-ready"))
        goto refuse;
    printf("FDT: J704 PCIe tunables supplied from the current ADT\n");
    return 0;
refuse:
    root = fdt_path_offset(dt, root_path);
    if (root >= 0)
        fdt_delprop(dt, root, "apple,pcie-tunables-ready");
    printf("FDT: invalid J704 PCIe tunable input; no ready marker published\n");
    return -1;
}
