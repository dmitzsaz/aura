/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef UTILS_H
#define UTILS_H
#include "types.h"
#include <stdio.h>
#include <string.h>
#define BIT(n) (1UL << (n))
#define GENMASK(h, l) ((BIT((h) + 1) - 1) & ~(BIT(l) - 1))
#define FIELD_PREP(f, v) (((v) * ((f) & -(f))) & (f))
#define ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))
#define min(a, b) ((a) < (b) ? (a) : (b))
#define SZ_16K 0x4000
#define T8142 0x8142
extern u32 chip_id;
u32 read32(u64 addr);
void write32(u64 addr, u32 value);
void set32(u64 addr, u32 value);
void clear32(u64 addr, u32 value);
void mask32(u64 addr, u32 mask, u32 value);
void mask16(u64 addr, u16 mask, u16 value);
int poll32(u64 addr, u32 mask, u32 target, u32 timeout);
void udelay(u32 us);
void mdelay(u32 ms);
#endif
