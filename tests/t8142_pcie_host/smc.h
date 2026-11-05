/* SPDX-License-Identifier: GPL-2.0-only */
#include "types.h"
typedef struct smc_dev smc_dev_t;
smc_dev_t *smc_init(void);
int smc_write_u32(smc_dev_t *smc, u32 key, u32 value);
int smc_shutdown(smc_dev_t *smc);
