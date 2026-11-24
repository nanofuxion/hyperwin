/* Host parser regression test. Build after tools/efi/build-avf-compat.sh:
 * clang -fsanitize=address,undefined -Iinclude -Ibuild/avf-esp
 *   guest/efi/tests/cpufreq_discovery.c -o /tmp/cpufreq-discovery-test
 */
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <stdio.h>
static uint8_t *test_window;
#define HW_WINDOW_PA ((uintptr_t)test_window)
#include "../cpu_acpi.c"
static void be(uint8_t *p,uint32_t x) {p[0]=x>>24;p[1]=x>>16;p[2]=x>>8;p[3]=x;}
static void word(uint8_t **p,uint32_t v){be(*p,v);*p+=4;}
static void prop(uint8_t **p,uint32_t name,const void *v,uint32_t n){word(p,3);word(p,n);word(p,name);memcpy(*p,v,n);*p+=(n+3)&~3;}
int main(void){
 test_window=calloc(1,HW_DIAG+4096); assert(test_window);
 _Alignas(8) uint8_t f[1024]={0},sys[128]={0},tables[24]={0};
 uint32_t regs[1024]={1234,0,7,0,0,9};
 const char strings[]="#address-cells\0#size-cells\0compatible\0reg\0";
 uint8_t *p=f+40,cell[4],regdata[16]={0};
 word(&p,1);word(&p,0);be(cell,2);prop(&p,0,cell,4);prop(&p,15,cell,4);
 word(&p,1);memcpy(p,"cpufreq",8);p+=8;
 const char compat[]="qemu,virtual-cpufreq";prop(&p,27,compat,sizeof compat);
 be(regdata,(uintptr_t)regs>>32);be(regdata+4,(uintptr_t)regs);be(regdata+12,sizeof regs);prop(&p,38,regdata,16);
 word(&p,2);word(&p,2);word(&p,9);
 uint32_t structure=p-(f+40),str_off=p-f;memcpy(p,strings,sizeof strings);uint32_t total=str_off+sizeof strings;
 be(f,0xd00dfeed);be(f+4,total);be(f+8,40);be(f+12,str_off);be(f+20,17);be(f+24,16);be(f+32,sizeof strings);be(f+36,structure);
 const uint8_t guid[]={0xd5,0x21,0xb6,0xb1,0x9c,0xf1,0xa5,0x41,0x83,0x0b,0xd9,0x15,0x2c,0x69,0xaa,0xe0};
 memcpy(tables,guid,16);*(void**)(tables+16)=f;*(uint64_t*)(sys+0x68)=1;*(void**)(sys+0x70)=tables;
 struct hw_header h={0};assert(discover_virt_cpufreq(sys,&h)==2);assert(h.virt_cpufreq_base==(uintptr_t)regs);assert(h.virt_cpufreq_size==sizeof regs);
 be(f+36,structure+1024);assert(discover_virt_cpufreq(sys,&h)==0);assert(h.virt_cpufreq_base==0);be(f+36,structure);
 be(f+32,1);assert(discover_virt_cpufreq(sys,&h)==0);be(f+32,sizeof strings);
 be(f+40,3);assert(discover_virt_cpufreq(sys,&h)==0);
 puts("PASS: root MMIO discovery, truncated structure, invalid strings, invalid root");free(test_window);return 0;
}