/* SPDX-License-Identifier: MIT */
#ifndef KBOOT_J704_PERIPHERAL_RANGES_H
#define KBOOT_J704_PERIPHERAL_RANGES_H

/* Pure bounds checking shared by the handoff and controller tests. This does
 * not admit MMIO access, IOMMU mappings or a co-processor start. */
static inline int j704_peripheral_dram_range(u64 physical, u32 bytes, u64 dram_base,
                                           u64 dram_size, u64 *aligned_size)
{
    const u64 page = 0x4000;
    if (!bytes || !dram_size || dram_base > UINT64_MAX - dram_size ||
        (physical & (page - 1)))
        return -1;
    u64 span = ((u64)bytes + page - 1) & ~(page - 1);
    if (physical > UINT64_MAX - span)
        return -1;
    /* Native AOP TEXT/DATA in local SRAM are not DRAM reservations. */
    if (physical < dram_base) {
        if (physical + span > dram_base)
            return -1;
        return 0;
    }
    if (span > dram_size || physical - dram_base > dram_size - span)
        return -1;
    *aligned_size = span;
    return 1;
}

#endif
