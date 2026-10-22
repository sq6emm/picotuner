# Applies the BATC wiki's RP2040-HAT-C changes for PicoTunerWH (run inside the repo root).
import re
p='port/ioLibrary_Driver/src/w5x00_spi.c'; s=open(p).read()
if 'wizchip_initialize_ewj' not in s:
    s=s.replace('#include "w5x00_spi.h"\n','#include "w5x00_spi.h"\n\n#define USE_SPI_DMA\n',1) if '#define USE_SPI_DMA' not in s else s
    s=s.replace('spi_init(SPI_PORT, 5000 * 1000);','spi_init(SPI_PORT, 125 * 1000 * 1000 / 4);')
    m=re.search(r'void wizchip_initialize\(void\)\n\{.*?\n\}\n',s,re.S); f=m.group(0)
    ewj=f.replace('wizchip_initialize(void)','wizchip_initialize_ewj(void)')
    ewj=re.sub(r'\n\s*/\* Check PHY link status \*/\n\s*do\n.*?while \(temp == PHY_LINK_OFF\);\n','\n',ewj,flags=re.S)
    s=s.replace(f,f+'\n'+ewj)
    open(p,'w').write(s)
h='port/ioLibrary_Driver/inc/w5x00_spi.h'; t=open(h).read()
if 'wizchip_initialize_ewj' not in t:
    t=t.replace('void wizchip_initialize(void);','void wizchip_initialize(void);\nvoid wizchip_initialize_ewj(void);')
    open(h,'w').write(t)
e='examples/CMakeLists.txt'; u=open(e).read()
for target in ('picotunewh', 'ptboot'):
    if f'add_subdirectory({target})' not in u: u += f'add_subdirectory({target})\n'
open(e,'w').write(u)
c='CMakeLists.txt'; v=open(c).read()
if 'if(NOT DEFINED WIZNET_CHIP)' not in v:
    v=v.replace('set(WIZNET_CHIP W5100S)','if(NOT DEFINED WIZNET_CHIP)\n    set(WIZNET_CHIP W5100S)\nendif()')
    open(c,'w').write(v)
print('ok')
