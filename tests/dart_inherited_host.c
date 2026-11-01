/* SPDX-License-Identifier: MIT */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include "types.h"
#define BIT(n) (1ULL << (n))
#define GENMASK(h,l) ((~0ULL >> (63-(h))) & (~0ULL << (l)))
#define FIELD_GET(m,v) (((v) & (m)) >> __builtin_ctzll(m))
#define FIELD_PREP(m,v) (((u64)(v) << __builtin_ctzll(m)) & (m))
#define SZ_16K 0x4000UL
#define SZ_1M 0x100000UL
#define ALIGN_UP(v,a) (((v)+(a)-1)&~((a)-1))
#define ARRAY_SIZE(a) (sizeof(a)/sizeof((a)[0]))
#define bail(...) do {fprintf(stderr,__VA_ARGS__);return -1;} while(0)
#define bail_cleanup(...) do {fprintf(stderr,__VA_ARGS__);ret=-1;goto err;} while(0)
#define min(a,b) ((a)<(b)?(a):(b))
#include "dart.h"
#include "libfdt/libfdt.h"
static u32 regs[0x2000/4];
static unsigned reads,allocations,frees;
static void *adt;
static char tree[8192];
static void *dt=tree;
#define T8142 0x8142
#define T8140 0x8140
static u32 chip_id=T8142;
static u64 *pages;
#include "adt_segments.inc"
static struct adt_segment_ranges firmware_segments[64];
static u32 firmware_length;
static u64 firmware_dram_size=4*SZ_16K;
struct adt_property { u32 size; char value[]; };
static int adt_path_offset(const void *a,const char *p)
{ (void)a;if(!strcmp(p,"/chosen"))return 2;assert(!strcmp(p,"fixture/display-fw"));return 3; }
static const void *adt_getprop(const void *a,int n,const char *k,u32 *len)
{ (void)a;if(n==3){assert(!strcmp(k,"segment-ranges"));*len=firmware_length;return firmware_segments;}
 assert(n==2);static u64 value;*len=8;
 if(!strcmp(k,"dram-base"))value=(uintptr_t)pages;
 else {assert(!strcmp(k,"dram-size"));value=firmware_dram_size;}
 return &value; }
static int adt_getprop_copy(const void*a,int n,const char*k,void*out,size_t size)
{u32 len;const void*p=adt_getprop(a,n,k,&len);if(!p||len!=size)return -1;memcpy(out,p,size);return size;}
#define ADT_GETPROP(a,n,k,v) adt_getprop_copy(a,n,k,v,sizeof(*(v)))
static dart_dev_t *unused_locked(void *d,u32 p,int s) { (void)d;(void)p;(void)s;abort(); }
static dart_dev_t *unused_init(void *d,u32 p,int s,bool k) { (void)d;(void)p;(void)s;(void)k;abort(); }
#define dart_init_fdt_locked unused_locked
#define dart_init_fdt unused_init
static const struct adt_property *adt_get_property(const void*a,int n,const char*k)
{ (void)a;(void)n;(void)k;abort(); }
static int adt_setprop(void*a,int n,const char*k,const void*v,u32 l)
{ (void)a;(void)n;(void)k;(void)v;(void)l;abort(); }
static u64 dt_get_address(void *dt,int node)
{ (void)dt;(void)node;return (uintptr_t)regs; }
static u32 read32(uintptr_t p)
{ assert(p>=(uintptr_t)regs && p<(uintptr_t)(regs+sizeof(regs)/4));reads++;return *(u32*)p; }
static void write32(uintptr_t p,u32 v) { (void)p;(void)v;abort(); }
static int is_heap(void *p) { (void)p;abort(); }
static void dart_t8110_tlb_invalidate(dart_dev_t *d) { (void)d;abort(); }
static int dart_map_page(dart_dev_t*d,uintptr_t a,uintptr_t b,u32 f)
{ (void)d;(void)a;(void)b;(void)f;abort(); }
static void dart_unmap_page(dart_dev_t*d,uintptr_t a) { (void)d;(void)a;abort(); }
static u64 *dart_get_l2(dart_dev_t*d,u32 a) { (void)d;(void)a;abort(); }
static void *host_calloc(size_t n,size_t s) { allocations++;return calloc(n,s); }
static void host_free(void *p) { frees++;free(p); }
#define calloc host_calloc
#define free host_free
#include "dart_inherited.inc"
#undef calloc
#undef free
static int node;
static const unsigned sid=23;
static void reset(void)
{
 memset(regs,0,sizeof(regs));memset(pages,0,4*SZ_16K);
 assert(!fdt_create_empty_tree(tree,sizeof(tree)));
 node=fdt_add_subnode(tree,0,"iommu");assert(node>=0);
 assert(!fdt_setprop_string(tree,node,"compatible","apple,t8110-dart"));
 assert(!fdt_setprop_u32(tree,node,"phandle",1));
 regs[(DART_T8110_TCR_OFF+4*sid)/4]=DART_T8110_TCR_TRANSLATE_ENABLE;
 regs[(DART_T8110_TTBR_OFF+4*sid)/4]=DART_T8110_TTBR_VALID|FIELD_PREP(DART_T8110_TTBR_ADDR,(uintptr_t)pages>>DART_T8110_TTBR_SHIFT);
 pages[0]=FIELD_PREP(DART_T6000_PTE_OFFSET,((uintptr_t)pages+SZ_16K)>>DART_PTE_OFFSET_SHIFT)|DART_PTE_VALID;
 pages[SZ_16K/8]=FIELD_PREP(DART_T6000_PTE_OFFSET,((uintptr_t)pages+2*SZ_16K)>>DART_PTE_OFFSET_SHIFT)|DART_PTE_VALID;
 reads=allocations=frees=0;
}
static dart_dev_t *open_view(void)
{ return dart_init_fdt_inherited(tree,1,sid,(uintptr_t)pages,4*SZ_16K); }
static void refused(void)
{ assert(!open_view());assert(allocations==frees); }
static unsigned aliases;
static u64 alias_iova[4],alias_size[4];
static int visit_alias(u64 iova,u64 size,void *opaque)
{ assert(opaque==&aliases && aliases<4);alias_iova[aliases]=iova;alias_size[aliases++]=size;return 0; }
static int fail_alias(u64 iova,u64 size,void *opaque)
{ (void)iova;(void)size;(void)opaque;return -42; }
static void display_tree(void)
{
 int iommu=fdt_path_offset(tree,"/iommu");
 fdt64_t dma[]={cpu_to_fdt64(1ULL<<40),cpu_to_fdt64(1ULL<<36)};
 assert(!fdt_setprop(tree,iommu,"apple,dma-range",dma,sizeof(dma)));
 int display=fdt_add_subnode(tree,0,"display");assert(display>=0);
 fdt32_t ios[]={cpu_to_fdt32(1),cpu_to_fdt32(sid)};
 assert(!fdt_setprop(tree,display,"iommus",ios,sizeof(ios)));
 assert(fdt_add_subnode(tree,0,"reserved-memory")>=0);
 regs[DART_T8110_PROTECT/4]=1;
}
static void display_unmapped_log(void)
{
 reset();display_tree();
 firmware_segments[0]=(struct adt_segment_ranges){.phys=(uintptr_t)pages+2*SZ_16K,.remap=1ULL<<40,.size=SZ_16K};
 firmware_segments[1]=(struct adt_segment_ranges){.phys=(uintptr_t)pages+3*SZ_16K,.remap=(1ULL<<40)+0x45d8000,.size=SZ_16K,.unk=10};
 firmware_length=2*sizeof(firmware_segments[0]);
 dart_dev_t*d=open_view();assert(d);
 assert(!dart_translate_silent(d,firmware_segments[1].remap));dart_shutdown(d);
 assert(!dt_reserve_m5_display_segments_for("/display","fixture/display-fw"));
 assert(dt_display_phys_reserved(firmware_segments[0].phys,SZ_16K));
 assert(dt_display_phys_reserved(firmware_segments[1].phys,SZ_16K));
 int parent=fdt_path_offset(tree,"/reserved-memory"),child,maps=0;
 fdt_for_each_subnode(child,tree,parent) {
  int len;const fdt32_t*p=fdt_getprop(tree,child,"iommu-addresses",&len);
  if(!p)continue;assert(len==20);
  assert(fdt64_ld((const fdt64_t*)(p+1))==(1ULL<<40));maps++;
 }
 assert(maps==1); /* No invented mapping at the ADT-only log address. */
 reset();display_tree();firmware_segments[0].phys=(uintptr_t)pages+4*SZ_16K;
 assert(dt_reserve_m5_display_segments_for("/display","fixture/display-fw")<0);
 reset();display_tree();firmware_segments[0].phys=(uintptr_t)pages+2*SZ_16K;
 firmware_length=sizeof(firmware_segments[0])-1;
 assert(dt_reserve_m5_display_segments_for("/display","fixture/display-fw")<0);
 reset();display_tree();firmware_length=2*sizeof(firmware_segments[0]);
 pages[SZ_16K/8]=0; /* An empty stream cannot publish a successful handoff. */
 assert(dt_reserve_m5_display_segments_for("/display","fixture/display-fw")<0);
 assert(allocations==frees);
 puts("M5 display: unmapped ADT log preserved physically, no invented IOVA, invalid ranges/empty stream refused PASS");
}
#ifdef M5_DISPLAY_FIXTURES
struct recorded_display_map {u64 iova,phys,size;};
struct recorded_display {
 const char *name;
 u64 dram_size;
 unsigned num_segments,num_maps;
 struct adt_segment_ranges segments[64];
 struct recorded_display_map maps[32];
};
#include "m5_display_fixtures.inc"
static void display_recorded_layouts(void)
{
 pages=mmap((void*)0x200000000ULL,8*SZ_16K,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANON,-1,0);
 assert(pages!=MAP_FAILED && !((uintptr_t)pages&(SZ_16K-1)));
 for(unsigned k=0;k<ARRAY_SIZE(recorded_displays);k++) {
  const struct recorded_display*r=&recorded_displays[k];
  reset();memset(pages,0,8*SZ_16K);display_tree();firmware_dram_size=r->dram_size;
  assert(r->num_segments<=64&&r->num_maps<=32);
  memcpy(firmware_segments,r->segments,r->num_segments*sizeof(firmware_segments[0]));
  firmware_length=r->num_segments*sizeof(firmware_segments[0]);
  for(unsigned i=0;i<r->num_segments;i++)firmware_segments[i].phys+=(uintptr_t)pages;
  u64 expected_bytes=0;
  for(unsigned i=0;i<r->num_maps;i++) {
   const struct recorded_display_map*m=&r->maps[i];expected_bytes+=m->size;
   for(u64 off=0;off<m->size;off+=SZ_16K) {
    u64 va=m->iova+off,pa=(uintptr_t)pages+m->phys+off;
    unsigned index=(va>>25)&0x7ff,page=(va>>14)&0x7ff;
    assert(!(va>>36)&&index<7);
    u64*l2=pages+(index+1)*SZ_16K/8;
    pages[index]=DART_PTE_VALID|FIELD_PREP(DART_T6000_PTE_OFFSET,(uintptr_t)l2>>DART_PTE_OFFSET_SHIFT);
    assert(!l2[page]);l2[page]=DART_PTE_VALID|FIELD_PREP(DART_T6000_PTE_OFFSET,pa>>DART_PTE_OFFSET_SHIFT);
   }
  }
  assert(!dt_reserve_m5_display_segments_for("/display","fixture/display-fw"));
  for(unsigned i=0;i<r->num_segments;i++) {
   const struct adt_segment_ranges*s=&firmware_segments[i];
   for(u64 off=0;off<s->size;off+=SZ_16K)assert(dt_display_phys_reserved(s->phys+off,SZ_16K));
  }
  int parent=fdt_path_offset(tree,"/reserved-memory"),child;u64 published=0;
  fdt_for_each_subnode(child,tree,parent) {
   int len;const fdt32_t*p=fdt_getprop(tree,child,"iommu-addresses",&len);
   if(!p)continue;assert(len==20);
   u64 va=fdt64_ld((const fdt64_t*)(p+1))-(1ULL<<40);
   u64 size=fdt64_ld((const fdt64_t*)(p+3));published+=size;
   for(u64 off=0;off<size;off+=SZ_16K) {
    bool found=false;
    for(unsigned i=0;i<r->num_maps;i++) {
     const struct recorded_display_map*m=&r->maps[i];
     if(va+off>=m->iova&&va+off-m->iova<m->size) {
      assert(dt_display_phys_reserved((uintptr_t)pages+m->phys+va+off-m->iova,SZ_16K));found=true;
     }
    }
    assert(found); /* ADT-only addresses never become made-up DMA reservations. */
   }
  }
  assert(published==expected_bytes&&allocations==frees);
  printf("Recorded display %s: %u segments, %u observed runs, all %llu mapped bytes preserved PASS\n",
         r->name,r->num_segments,r->num_maps,(unsigned long long)published);
 }
 assert(!munmap(pages,8*SZ_16K));firmware_dram_size=4*SZ_16K;
}
#endif
int main(void)
{
 pages=mmap((void*)0x200000000ULL,4*SZ_16K,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANON,-1,0);
 assert(pages!=MAP_FAILED && !((uintptr_t)pages&(SZ_16K-1)));
 assert(((uintptr_t)pages>>DART_T8110_TTBR_SHIFT)<=FIELD_GET(DART_T8110_TTBR_ADDR,DART_T8110_TTBR_ADDR));
 reset();assert(!fdt_delprop(tree,node,"phandle"));
 u32 ph=dt_ensure_phandle(node);assert(ph && dt_ensure_phandle(node)==ph);
 for(unsigned lock=0;lock<2;lock++) {
  reset();regs[DART_T8110_PROTECT/4]=lock;
  dart_dev_t*d=open_view();assert(d && d->read_only && d->locked==lock);
  assert(reads==3 && allocations==1 && frees==0);
  assert(dart_translate(d,0)==pages+2*SZ_16K/8);
  assert(dart_search_range(d,(uintptr_t)pages+2*SZ_16K,SZ_16K)==0);
  unsigned before=reads;u64 snapshot[4*SZ_16K/8];memcpy(snapshot,pages,sizeof(snapshot));
  assert(dart_map_flags(d,0,pages,SZ_16K,0)==-1);
  assert(dart_setup_pt_region(d,"/unused",sid,0)==-1);
  dart_unmap(d,0,SZ_16K);dart_free_l2(d,0);
  assert(!memcmp(snapshot,pages,sizeof(snapshot)) && before==reads);
  /* A changed borrowed L2 pointer must never be dereferenced. */
  pages[0]=DART_PTE_VALID|FIELD_PREP(DART_T6000_PTE_OFFSET,0x4000>>DART_PTE_OFFSET_SHIFT);
  assert(!dart_translate_silent(d,0));
  assert(DART_IS_ERR(dart_search_range(d,(uintptr_t)pages+2*SZ_16K,SZ_16K)));
  dart_shutdown(d);assert(allocations==frees && before==reads);
 }
 for(unsigned lock=0;lock<2;lock++) {
  reset();regs[DART_T8110_PROTECT/4]=lock;
  /* The inactive remap selector and DAPF bypass do not bypass translation. */
  regs[(DART_T8110_TCR_OFF+4*sid)/4]|=0x300|DART_T8110_TCR_BYPASS_DAPF;
  fdt32_t io[]={cpu_to_fdt32(1),cpu_to_fdt32(sid)};
  assert(!fdt_setprop(tree,node,"iommus",io,sizeof(io)));
  dart_dev_t*d=dt_init_dart_by_node(node,0);
  if(lock) {
   assert(d && fdt_num_mem_rsv(tree)==2);dart_shutdown(d);
   d=dt_init_dart_by_node(node,0);assert(d && fdt_num_mem_rsv(tree)==2);dart_shutdown(d);
  } else assert(!d && !fdt_num_mem_rsv(tree));
  assert(allocations==frees);
 }
 reset();regs[DART_T8110_PROTECT/4]=1;
 u64 *l2=pages+SZ_16K/8;
 l2[1]=FIELD_PREP(DART_T6000_PTE_OFFSET,((uintptr_t)pages+3*SZ_16K)>>DART_PTE_OFFSET_SHIFT)|DART_PTE_VALID;
 l2[7]=l2[0]; /* A second non-contiguous alias of the same physical page. */
 dart_dev_t*view=open_view();assert(view);aliases=0;
 assert(!dart_walk_inherited_range(view,(uintptr_t)pages+2*SZ_16K,2*SZ_16K,visit_alias,&aliases));
 assert(aliases==2&&alias_iova[0]==0&&alias_size[0]==2*SZ_16K&&alias_iova[1]==7*SZ_16K&&alias_size[1]==SZ_16K);
 /* Execute the production FDT publisher too: fragmented aliases are IOVA
  * reservations, never a false direct mapping of the complete carveout. */
 int iommu=fdt_path_offset(tree,"/iommu");
 fdt64_t dma[]={cpu_to_fdt64(1ULL<<40),cpu_to_fdt64(1ULL<<36)};
 assert(!fdt_setprop(tree,iommu,"apple,dma-range",dma,sizeof(dma)));
 int display=fdt_add_subnode(tree,0,"display");assert(display>=0);
 fdt32_t ios[]={cpu_to_fdt32(1),cpu_to_fdt32(sid)};
 assert(!fdt_setprop(tree,display,"iommus",ios,sizeof(ios)));
 u32 owner=dt_ensure_phandle(display);assert(owner);
 assert(fdt_add_subnode(tree,0,"reserved-memory")>=0);
 assert(!dt_t8142_reserve_fragmented(view,"/display",owner,(uintptr_t)pages+2*SZ_16K,2*SZ_16K));
 int parent=fdt_path_offset(tree,"/reserved-memory"),child,count=0;
 fdt_for_each_subnode(child,tree,parent) {
  int len;const fdt32_t*tuple=fdt_getprop(tree,child,"iommu-addresses",&len);
  assert(tuple&&len==20&&fdt32_to_cpu(tuple[0])==owner);
  assert(!fdt_getprop(tree,child,"reg",NULL));
  u64 high=fdt64_ld((const fdt64_t*)(tuple+1));assert(high==(1ULL<<40)||high==((1ULL<<40)+7*SZ_16K));
  count++;
 }
 assert(count==2);
 const fdt32_t*regions=fdt_getprop(tree,fdt_path_offset(tree,"/display"),"memory-region",&count);
 assert(regions&&count==8);
 assert(fdt_stringlist_count(tree,fdt_path_offset(tree,"/display"),"memory-region-names")==2);
 assert(dart_walk_inherited_range(view,(uintptr_t)pages+2*SZ_16K,2*SZ_16K,fail_alias,NULL)==-42);
 assert(dart_walk_inherited_range(view,(uintptr_t)pages+2*SZ_16K+1,SZ_16K,visit_alias,&aliases)<0);
 pages[0]=DART_PTE_VALID|FIELD_PREP(DART_T6000_PTE_OFFSET,0x4000>>DART_PTE_OFFSET_SHIFT);
 assert(dart_walk_inherited_range(view,(uintptr_t)pages+2*SZ_16K,SZ_16K,visit_alias,&aliases)<0);
 dart_shutdown(view);assert(allocations==frees);
 reset();regs[(DART_T8110_TCR_OFF+4*sid)/4]=9;refused(); /* Unsupported four-level stream. */
 reset();regs[(DART_T8110_TCR_OFF+4*sid)/4]=0;refused();
 reset();regs[(DART_T8110_TCR_OFF+4*sid)/4]|=DART_T8110_TCR_BYPASS_DART;refused();
 reset();regs[(DART_T8110_TCR_OFF+4*sid)/4]|=DART_T8110_TCR_REMAP_EN;refused();
 reset();regs[(DART_T8110_TTBR_OFF+4*sid)/4]=0;refused();
 reset();regs[(DART_T8110_TTBR_OFF+4*sid)/4]=DART_T8110_TTBR_VALID|1;refused();
 reset();pages[0]=DART_PTE_VALID|FIELD_PREP(DART_T6000_PTE_OFFSET,((uintptr_t)pages+4*SZ_16K)>>DART_PTE_OFFSET_SHIFT);refused();
 reset();assert(!dart_init_fdt_inherited(tree,1,sid,(uintptr_t)pages,SZ_16K-1));assert(!reads);
 reset();assert(!dart_init_fdt_inherited(tree,1,-1,(uintptr_t)pages,4*SZ_16K));assert(!reads);
 reset();assert(!dart_init_fdt_inherited(tree,1,256,(uintptr_t)pages,4*SZ_16K));assert(!reads);
 reset();assert(!dart_init_fdt_inherited(tree,99,sid,(uintptr_t)pages,4*SZ_16K));assert(!reads);
 reset();assert(!dart_init_fdt_inherited(tree,1,sid,UINT64_MAX-SZ_16K,4*SZ_16K));assert(!reads);
 reset();assert(!fdt_setprop_string(tree,node,"compatible","apple,t6000-dart"));refused();assert(!reads);
 display_unmapped_log();
 assert(!munmap(pages,4*SZ_16K));
#ifdef M5_DISPLAY_FIXTURES
 display_recorded_layouts();
#endif
 puts("Inherited DART: locked/unlocked views, malformed modes/roots/L2, mutation refusal and cleanup PASS (ASan/UBSan)");
}
