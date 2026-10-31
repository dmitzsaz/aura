#include "types.h"
extern void *adt;
struct adt_segment_ranges { u64 phys, iova, remap; u32 size, unk; } PACKED;
int adt_path_offset(const void *, const char *);
const void *adt_getprop(const void *, int, const char *, u32 *);
