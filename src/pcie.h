/* SPDX-License-Identifier: MIT AND GPL-2.0-only */

#ifndef PCIE_H
#define PCIE_H

int pcie_prepare_fdt(void *dt);
int pcie_handoff_fdt(void *dt);
int pcie_init(void);
int pcie_shutdown(void);

/* Train the T8142 internal PCIe links; disables their ECAM host on failure. */
int pcie_t8142_prepare_fdt(void *dt);

/* Disable optional J713 PCIe admission after rail/controller/DART failure. */
int pcie_disable_j713_fdt(void *dt);

#endif
