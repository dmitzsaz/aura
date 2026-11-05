/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef TYPES_H
#define TYPES_H
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
typedef uint8_t u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef unsigned long u64;
typedef long s64;
#define PACKED __attribute__((packed))
#endif
