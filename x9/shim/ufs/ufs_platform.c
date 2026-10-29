/* Pure current-FDT validation for MT6993 UFS0 host reset. No MMIO here. */
#include "../fdt.h"
static uint32_t be32(const uint8_t *p)
{return (uint32_t)p[0]<<24|(uint32_t)p[1]<<16|(uint32_t)p[2]<<8|p[3];}
static uint64_t be64(const uint8_t *p){return (uint64_t)be32(p)<<32|be32(p+4);}
int x9u_x9_reset_route(uintptr_t fdt)
{
 const uint8_t *p;size_t n;
 if(!fdt_property(fdt,"/clock-controller@16840000","reg",&p,&n)||n!=16||be64(p)!=0x16840000UL||be64(p+8)<0x150)return 0;
 if(!fdt_property(fdt,"/clock-controller@16840000/reset-controller","phandle",&p,&n)||n!=4)return 0;
 uint32_t phandle=be32(p);
 if(!fdt_property(fdt,"/soc/ufshci@16810000","resets",&p,&n)||n!=24)return 0;
 for(unsigned i=0;i<3;i++)if(be32(p+8*i)!=phandle||be32(p+8*i+4)!=i+1)return 0;
 static const char names[]="unipro_rst\0crypto_rst\0hci_rst";
 if(!fdt_property(fdt,"/soc/ufshci@16810000","reset-names",&p,&n)||n!=sizeof(names))return 0;
 for(unsigned i=0;i<n;i++)if(p[i]!=(uint8_t)names[i])return 0;
 if(!fdt_property(fdt,"/clock-controller@16840000/reset-controller","ti,reset-bits",&p,&n)||n!=112)return 0;
 for(unsigned i=1;i<4;i++) {
  const uint8_t *row=p+i*28;
  if(be32(row)!=0x148||be32(row+4)!=i-1||be32(row+8)!=0x14c||be32(row+12)!=i-1||be32(row+24)!=0x1c)return 0;
 }
 return 1;
}
