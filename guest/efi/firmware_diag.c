/* Read-only AArch64 UEFI diagnostic. Uses firmware console, never guessed MMIO. */
#include <stdint.h>
#include <stddef.h>
typedef uint64_t Status;
typedef struct { uint32_t a; uint16_t b,c; uint8_t d[8]; } Guid;
typedef struct { uint64_t sig; uint32_t rev,size,crc,reserved; } Header;
typedef struct Text Text;
struct Text { void *reset; Status (*output)(Text *, const uint16_t *); };
typedef struct { Guid guid; void *table; } Config;
typedef struct {
 Header header; uint16_t *vendor; uint32_t revision;
 void *in_handle,*in; void *out_handle; Text *out;
 void *err_handle; Text *err; void *runtime,*boot;
 uint64_t count; Config *tables;
} System;
_Static_assert(offsetof(System, out)==64,"UEFI ABI");
_Static_assert(offsetof(System, count)==104,"UEFI ABI");
_Static_assert(offsetof(System, tables)==112,"UEFI ABI");
static Text *console;
static void emit(const char *s) {
 uint16_t b[128]; unsigned n=0;
 while (*s) { b[n++]=(uint8_t)*s++; if(n==127){b[n]=0;console->output(console,b);n=0;} }
 b[n]=0; if(n)console->output(console,b);
}
static void hex(uint64_t x,unsigned digits){char b[17];for(unsigned i=0;i<digits;i++)b[digits-1-i]="0123456789abcdef"[(x>>(i*4))&15];b[digits]=0;emit(b);}
static int equal(const Guid *a,const Guid *b){const uint8_t *x=(const uint8_t*)a,*y=(const uint8_t*)b;for(unsigned i=0;i<16;i++)if(x[i]!=y[i])return 0;return 1;}
static const Guid acpi2={0x8868e871,0xe4f1,0x11d3,{0xbc,0x22,0x00,0x80,0xc7,0x3c,0x88,0x81}};
static const Guid acpi1={0xeb9d2d30,0x2d88,0x11d3,{0x9a,0x16,0x00,0x90,0x27,0x3f,0xc1,0x4d}};
static const Guid fdt={0xb1b621d5,0xf19c,0x41a5,{0x83,0x0b,0xd9,0x15,0x2c,0x69,0xaa,0xe0}};
static uint32_t be32(const uint8_t *b){return (uint32_t)b[0]<<24|(uint32_t)b[1]<<16|(uint32_t)b[2]<<8|b[3];}
/* Hex records preserve every byte for offline dtc inspection. */
static void dump(const char *tag,const uint8_t *p,uint32_t n){
 for(uint32_t i=0;i<n;i+=32){emit(tag);emit(" ");hex(i,8);emit(" ");for(uint32_t j=i;j<n&&j<i+32;j++)hex(p[j],2);emit("\r\n");}
}
Status EfiMain(void *image,System *s){
 (void)image;
 if(!s||!s->out||!s->out->output)return 0x8000000000000002ULL;
 console=s->out;emit("\r\nHW_DIAG_BEGIN\r\n");
 if(s->count>1024||(!s->tables&&s->count)){emit("INVALID_CONFIG_ARRAY\r\n");return 0x8000000000000002ULL;}
 unsigned found=0;
 for(uint64_t i=0;i<s->count;i++){
  Config *c=&s->tables[i]; Guid *g=&c->guid;
  emit("TABLE ");hex(g->a,8);emit("-");hex(g->b,4);emit("-");hex(g->c,4);emit("-");for(unsigned j=0;j<8;j++){hex(g->d[j],2);if(j==1)emit("-");}
  emit(" @ ");hex((uintptr_t)c->table,16);emit("\r\n");
  if(!c->table)continue;
  if(equal(g,&acpi1)||equal(g,&acpi2)){found++;emit("ACPI_RSDP_PRESENT\r\n");dump("RSDP",c->table,equal(g,&acpi2)?36:20);}
  if(equal(g,&fdt)){
   const uint8_t *p=c->table;uint32_t size=be32(p+4);
   if(be32(p)!=0xd00dfeed||size<40||size>1024*1024){emit("FDT_REJECTED_SIZE_OR_MAGIC\r\n");continue;}
   emit("FDT_SIZE ");hex(size,8);emit("\r\n");dump("FDT",p,size);
  }
 }
 if(!found)emit("NO_ACPI_CONFIGURATION_TABLE\r\n");
 emit("HW_DIAG_END\r\n");return 0;
}
