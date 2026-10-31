/* SPDX-License-Identifier: MIT */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "adt.h"
#include "soc.h"
#include "libfdt/libfdt.h"
#include "kboot_gpu_t8142_fields.h"
int dt_set_gpu_t8142(void *);
u32 chip_id=T8142;
void *adt;
static char tree[262144];
static u32 count=4,valid=3,banks=1,period=8,loaded=1;
static u64 dram=0x10000000000ULL,dram_size=0x1000000;
static struct {u32 hz,mv;} core[4],sram[4];
static u64 bases[4],sizes[4];
static struct adt_segment_ranges segments[2][3];
static const char *paths[]={"/arm-io/sgx","/chosen","/arm-io/gfx-asc","/arm-io/gfx-asc/iop-gfx-nub","/arm-io/gfx1-asc","/arm-io/gfx1-asc/iop-gfx1-nub"};
static const char *properties[]={"gpu-region","gfx-shared-region","gfx-handoff","gfx-shared-l2-region"};
int adt_path_offset(const void *unused,const char *p)
{ (void)unused;for(int i=0;i<6;i++)if(!strcmp(p,paths[i]))return i;return -1; }
const void *adt_getprop(const void *unused,int node,const char *name,u32 *size)
{
 (void)unused;const void *p=NULL;u32 bytes=0;
 #define PROP(n,v) if(!strcmp(name,n)){p=&v;bytes=sizeof(v);}
 if(node==0){
  PROP("perf-state-count",count);PROP("gpu-num-perf-states",valid);PROP("perf-state-table-count",banks);
  PROP("gpu-power-sample-period",period);PROP("perf-states",core);PROP("perf-states-sram",sram);
  for(int i=0;i<4;i++){char key[64];snprintf(key,sizeof(key),"%s-base",properties[i]);PROP(key,bases[i]);snprintf(key,sizeof(key),"%s-size",properties[i]);PROP(key,sizes[i]);}
 } else if(node==1){PROP("dram-base",dram);PROP("dram-size",dram_size);}
 else if(node==3||node==5){PROP("pre-loaded",loaded);}
 else if(node==2||node==4){
  static const char names[]="__TEXT;__DATA;__DATA_SHARED_RO";
  PROP("segment-names",names);
  if(!strcmp(name,"segment-ranges")){p=segments[node==4];bytes=sizeof(segments[0]);}
 }
 #undef PROP
 if(size)*size=bytes;return p;
}
static int gpu(void){return fdt_path_offset(tree,"gpu");}
static void reset(void)
{
 chip_id=T8142;count=4;valid=3;banks=1;period=8;loaded=1;dram_size=0x1000000;
 assert(!fdt_create_empty_tree(tree,sizeof(tree)));
 assert(!fdt_setprop_string(tree,0,"compatible","apple,j704"));
 assert(fdt_add_subnode(tree,0,"reserved-memory")>=0);
 int n=fdt_add_subnode(tree,0,"opp-table");assert(n>=0);assert(!fdt_setprop_u32(tree,n,"phandle",1));
 n=fdt_add_subnode(tree,0,"gpu");assert(n>=0);
 assert(!fdt_setprop_string(tree,n,"compatible","apple,agx-t8142"));assert(!fdt_setprop_string(tree,n,"status","disabled"));
 assert(!fdt_setprop_u32(tree,n,"operating-points-v2",1));
 fdt32_t manual[103*5],states[12];
 for(unsigned i=0;i<103;i++){
  manual[5*i]=cpu_to_fdt32(t8142_hal300_fields[i].object);manual[5*i+1]=cpu_to_fdt32(t8142_hal300_fields[i].offset);
  manual[5*i+2]=cpu_to_fdt32(t8142_hal300_fields[i].width);manual[5*i+3]=manual[5*i+4]=0;
 }
 assert(!fdt_setprop(tree,n,"apple,hal300-derived",manual,sizeof(manual)));
 for(unsigned i=0;i<4;i++){
  core[i].hz=i*200000000;core[i].mv=700+i*10;sram[i]=core[i];
  states[3*i]=cpu_to_fdt32(core[i].hz);states[3*i+1]=states[3*i+2]=cpu_to_fdt32(core[i].mv);
  bases[i]=dram+0x10000*(i+1);sizes[i]=0x4000;
 }
 assert(!fdt_setprop(tree,n,"apple,hal300-source-pstates",states,sizeof(states)));
 assert(!fdt_setprop_u32(tree,n,"apple,hal300-source-period",period));
 for(unsigned i=0;i<2;i++)for(unsigned j=0;j<3;j++){
  u64 base=dram+0x100000+i*0x100000+j*0x10000;
  segments[i][j]=(struct adt_segment_ranges){base,base,base,0x4000,0};
 }
 n=fdt_add_subnode(tree,0,"aliases");assert(n>=0);assert(!fdt_setprop_string(tree,n,"gpu","/gpu"));
}
static void rejected(void)
{
 assert(!dt_set_gpu_t8142(tree));
 assert(!strcmp(fdt_getprop(tree,gpu(),"status",NULL),"disabled"));
 assert(fdt_first_subnode(tree,fdt_path_offset(tree,"/reserved-memory"))<0);
}
int main(void)
{
 reset();assert(!dt_set_gpu_t8142(tree));assert(!strcmp(fdt_getprop(tree,gpu(),"status",NULL),"okay"));
 assert(fdt_stringlist_count(tree,gpu(),"memory-region-names")==10);
 int table=fdt_node_offset_by_phandle(tree,1),n,k=0;
 fdt_for_each_subnode(n,tree,table)k++;assert(k==4);
 assert(!dt_set_gpu_t8142(tree));assert(fdt_stringlist_count(tree,gpu(),"memory-region-names")==10);
 reset();fdt_delprop(tree,gpu(),"apple,hal300-derived");rejected();
 reset();fdt_setprop_u32(tree,gpu(),"apple,hal300-derived",0);rejected();
 reset();fdt_delprop(tree,gpu(),"apple,hal300-source-pstates");rejected();
 reset();fdt_setprop_u32(tree,gpu(),"apple,hal300-source-period",42);rejected();
 reset();core[1].hz++;rejected();
 reset();banks=2;rejected();
 reset();valid=16;rejected();
 reset();count=17;rejected();
 reset();loaded=0;rejected();
 reset();sizes[0]=0;rejected();
 reset();bases[0]++;rejected();
 reset();bases[0]=dram+dram_size;rejected();
 reset();bases[1]=bases[0];rejected();
 reset();segments[0][1].remap++;rejected();
 reset();period=0;rejected();
 reset();chip_id=0x8140;char before[sizeof(tree)];memcpy(before,tree,sizeof(tree));
 assert(!dt_set_gpu_t8142(tree));assert(!memcmp(before,tree,sizeof(tree)));
 puts("T8142 GPU producer: valid/repeat + 16 refusal/other-chip cases PASS");
}
