/* SPDX-License-Identifier: GPL-2.0-only */
#include "adt.h"
#include "kboot_nvme.h"
#include "utils.h"

#include "libfdt/libfdt.h"

/* The ANS firmware selects whether the linear SQ aperture is in NVMe or NVMMU. */
int dt_set_t8142_nvme(void *dt)
{
    int path[8], len;
    u64 addr, size;

    if (chip_id != T8142)
        return 0;
    int node = fdt_node_offset_by_compatible(dt, -1, "apple,t8142-nvme-ans2");
    if (node < 0)
        return 0;
    int index = fdt_stringlist_search(dt, node, "reg-names", "nvme");
    int parent = fdt_parent_offset(dt, node);
    const fdt32_t *reg = fdt_getprop(dt, node, "reg", &len);

    if (index < 0 || !reg || fdt_address_cells(dt, parent) != 2 ||
        fdt_size_cells(dt, parent) != 2 || len % 16 || index >= len / 16 ||
        adt_path_offset_trace(adt, "/arm-io/ans", path) < 0 ||
        adt_get_reg(adt, path, "reg", 9, &addr, &size) || !size || addr > UINT64_MAX - size ||
        size < 0x10000)
        goto disable;

    /* Keep the OS bus address; only the firmware-dependent span changes. */
    fdt32_t cells[2] = {cpu_to_fdt32(size >> 32), cpu_to_fdt32(size)};
    return fdt_setprop_inplace_namelen_partial(dt, node, "reg", 3, index * 16 + 8, cells,
                                               sizeof(cells));

disable:
    printf("FDT: no usable T8142 ANS register inventory; disabling NVMe\n");
    return fdt_setprop_string(dt, node, "status", "disabled");
}
