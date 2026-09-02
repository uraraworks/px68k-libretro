#ifndef _WINX68K_MEMORY_H
#define _WINX68K_MEMORY_H

#include <stdint.h>
#include "../libretro/common.h"

extern	uint8_t*	IPL;
extern	uint8_t*  	MEM;
extern	uint8_t*	FONT;
extern  uint8_t    SCSIIPL[0x2000];
extern  uint8_t    SRAM[0x4000];
extern  uint8_t    GVRAM[0x80000];
extern  uint8_t   TVRAM[0x80000];

extern	uint32_t	BusErrFlag;

void Memory_Init(void);

uint32_t cpu_readmem24(uint32_t adr);
uint32_t cpu_readmem24_word(uint32_t adr);
uint32_t cpu_readmem24_dword(uint32_t adr);

void cpu_writemem24(uint32_t adr, uint32_t data);
void cpu_writemem24_word(uint32_t adr, uint32_t data);
void cpu_writemem24_dword(uint32_t adr, uint32_t data);

uint8_t dma_readmem24(uint32_t adr);
uint16_t dma_readmem24_word(uint32_t adr);
uint32_t dma_readmem24_dword(uint32_t adr);

void dma_writemem24(uint32_t adr, uint8_t data);
void dma_writemem24_word(uint32_t adr, uint16_t data);
void dma_writemem24_dword(uint32_t adr, uint32_t data);

void Memory_SetSCSIMode(void);

/*
 * ゲストRAM書き込みの実測用フック(WebX68k-storage側)。
 * 監視範囲は core-shim.c の webx68k_ram_watch_refresh() が
 * globalThis.__webx68kRamWatchLo / __webx68kRamWatchHi から毎フレーム反映する。
 * 既定は無効(lo > hi)。詳細は x68k/mem_wrap.c 冒頭のコメント参照。
 */
extern int32_t webx68k_ram_watch_lo;
extern int32_t webx68k_ram_watch_hi;
extern int      webx68k_ram_watch_count;

void webx68k_ram_watch_selftest(void);
void webx68k_ram_watch_refresh(void); /* WebX68k-storage/src/core-shim.c 側で定義 */

#endif
