/*	$Id: mem_wrap.c,v 1.2 2003/12/05 18:07:19 nonaka Exp $	*/

#include "common.h"
#include <string.h>
#include "../m68000/m68000.h"
#include "winx68k.h"

#include "adpcm.h"
#include "bg.h"
#include "crtc.h"
#include "dmac.h"
#include "fdc.h"
#include "gvram.h"
#include "mercury.h"
#include "mfp.h"
#include "midi.h"
#include "ioc.h"
#include "pia.h"
#include "rtc.h"
#include "sasi.h"
#include "scc.h"
#include "scsi.h"
#include "sram.h"
#include "sysport.h"
#include "tvram.h"

#include "fmg_wrap.h"

uint8_t *IPL;
uint8_t *MEM;
static uint8_t *OP_ROM;
uint8_t *FONT;

uint32_t BusErrFlag       = 0;
uint32_t BusErrHandling   = 0;
static uint32_t BusErrAdr = 0;

/*
 * ゲストRAM書き込みの実測用フック(WebX68k-storage側)。
 * JS側 (globalThis.__webx68kRamWatchLo / __webx68kRamWatchHi) が
 * core-shim.c の webx68k_ram_watch_refresh() を通じて毎フレーム先頭で
 * これらの static 変数に書き戻す。既定は無効(lo > hi)。
 * ホットパス(wm_cnt、全RAMバイト書き込みが必ず通る)からは
 * static変数の比較1回だけで早期脱出できるようにする。
 *
 * 書いた側のPCで絞る条件(webx68k_ram_watch_pc_lo/hi、既定は無効=-1)を追加。
 * IPLのメモリクリア等、アドレス範囲だけでは絞り切れずログ上限を埋めて
 * しまうケースに対応するため。既定(lo>hi)ならPCでは絞らず従来どおり。
 */
int32_t webx68k_ram_watch_lo = -1;
int32_t webx68k_ram_watch_hi = -1;
int32_t webx68k_ram_watch_pc_lo = -1;
int32_t webx68k_ram_watch_pc_hi = -1;
int webx68k_ram_watch_count = 0;

/* 自己検査(webx68k_ram_watch_selftest)実行中はPCでの絞り込みを適用しない。
 * 自己検査はretro_run先頭(CPU実行前後の合間)から呼ばれるため、その時点の
 * m68000_get_reg(M68K_PC)はSCSI ROM等の監視対象PC範囲に入っているとは限らず、
 * PC絞り込みを適用すると陽性対照そのものが「観測されなかった」扱いになって
 * しまう。アドレス範囲・件数上限による絞り込みは自己検査にも従来どおり適用する。 */
static int webx68k_ram_watch_selftest_active = 0;

static void webx68k_ram_watch_check(uint32_t addr, uint8_t val)
{
	uint32_t pc;

	if (webx68k_ram_watch_lo > webx68k_ram_watch_hi)
		return;
	if ((int32_t)addr < webx68k_ram_watch_lo || (int32_t)addr > webx68k_ram_watch_hi)
		return;
	if (webx68k_ram_watch_count >= 2000)
		return;

	pc = m68000_get_reg(M68K_PC);
	if (!webx68k_ram_watch_selftest_active && webx68k_ram_watch_pc_lo <= webx68k_ram_watch_pc_hi)
	{
		if ((int32_t)pc < webx68k_ram_watch_pc_lo || (int32_t)pc > webx68k_ram_watch_pc_hi)
			return;
	}

	webx68k_ram_watch_count++;
	printf("[SCSI-RAM] W adr=$%06x data=$%02x pc=$%08x\n",
	       (unsigned)addr, (unsigned)val, (unsigned)pc);
}

/* RAM(addrは0x00c00000未満であること)への実バイト書き込み。
 * wm_cnt と自己検査の両方から使う共通実体(MSB_FIRST規則を1箇所に集約)。
 * フックを経由させたくない場合(自己検査の復元)はこちらを直接呼ぶ。 */
static inline void ram_poke_raw(uint32_t addr, uint8_t val)
{
#ifdef MSB_FIRST
	MEM[addr    ] = val;
#else
	MEM[addr ^ 1] = val;
#endif
}

static void wm_cnt(uint32_t addr, uint8_t val); /* 自己検査から使うための前方宣言 */

/*
 * 陽性対照: 監視範囲が有効なときだけ、範囲内の先頭番地へ1バイト書いて
 * webx68k_ram_watch_check() 経由でログに出たかどうかを自己検査する。
 * フックが繋がっていないのに「書き込みが無かった」と読む事故を防ぐため。
 * このぶんのログ件数はカウントから除外する(復元処理はフックを経由しない)。
 */
void webx68k_ram_watch_selftest(void)
{
	uint32_t addr;
	uint8_t saved, test_val;
	int before;

	if (webx68k_ram_watch_lo > webx68k_ram_watch_hi)
		return; /* 監視無効なら自己検査もしない */
	if (webx68k_ram_watch_lo < 0 || (uint32_t)webx68k_ram_watch_lo >= 0x00c00000)
		return; /* RAM範囲外は自己検査の対象外 */

	addr  = (uint32_t)webx68k_ram_watch_lo;
	before = webx68k_ram_watch_count;

#ifdef MSB_FIRST
	saved = MEM[addr];
#else
	saved = MEM[addr ^ 1];
#endif
	test_val = saved ^ 0xff; /* 必ず値が変わるようにする */

	webx68k_ram_watch_selftest_active = 1;
	wm_cnt(addr, test_val); /* フック経由の書き込み(本番と同じ経路) */
	webx68k_ram_watch_selftest_active = 0;
	ram_poke_raw(addr, saved); /* 元の値へ復元。フックは経由させない */

	if (webx68k_ram_watch_count == before)
	{
		printf("[SCSI-RAM] ERROR: self-test write to $%06x was not observed "
		       "(hook not wired?)\n", (unsigned)addr);
	}
	else
	{
		webx68k_ram_watch_count = before; /* 自己検査ぶんは件数に数えない */
	}
}

/* forward declarations */
static void wm_opm(uint32_t addr, uint8_t val);
static void wm_buserr(uint32_t addr, uint8_t val);
static uint8_t rm_opm(uint32_t addr);
static uint8_t rm_ipl(uint32_t addr);
static uint8_t rm_buserr(uint32_t addr);
static uint8_t rm_font(uint32_t addr);
static uint8_t rm_nop(uint32_t addr) { return 0; }
static void wm_nop(uint32_t addr, uint8_t val) { }

uint8_t (*MemReadTable[])(uint32_t) = {
	TVRAM_Read, TVRAM_Read, TVRAM_Read, TVRAM_Read, TVRAM_Read, TVRAM_Read, TVRAM_Read, TVRAM_Read,
	TVRAM_Read, TVRAM_Read, TVRAM_Read, TVRAM_Read, TVRAM_Read, TVRAM_Read, TVRAM_Read, TVRAM_Read,
	TVRAM_Read, TVRAM_Read, TVRAM_Read, TVRAM_Read, TVRAM_Read, TVRAM_Read, TVRAM_Read, TVRAM_Read,
	TVRAM_Read, TVRAM_Read, TVRAM_Read, TVRAM_Read, TVRAM_Read, TVRAM_Read, TVRAM_Read, TVRAM_Read,
	TVRAM_Read, TVRAM_Read, TVRAM_Read, TVRAM_Read, TVRAM_Read, TVRAM_Read, TVRAM_Read, TVRAM_Read,
	TVRAM_Read, TVRAM_Read, TVRAM_Read, TVRAM_Read, TVRAM_Read, TVRAM_Read, TVRAM_Read, TVRAM_Read,
	TVRAM_Read, TVRAM_Read, TVRAM_Read, TVRAM_Read, TVRAM_Read, TVRAM_Read, TVRAM_Read, TVRAM_Read,
	TVRAM_Read, TVRAM_Read, TVRAM_Read, TVRAM_Read, TVRAM_Read, TVRAM_Read, TVRAM_Read, TVRAM_Read,
	CRTC_Read, VCtrl_Read, DMA_Read, rm_nop, MFP_Read, RTC_Read, rm_nop, SysPort_Read,
	rm_opm, ADPCM_Read, FDC_Read, SASI_Read, SCC_Read, PIA_Read, IOC_Read, rm_nop,
	SCSI_Read, rm_buserr, rm_buserr, rm_buserr, rm_buserr, rm_buserr, rm_buserr, MIDI_Read,
	BG_Read, BG_Read, BG_Read, BG_Read, BG_Read, BG_Read, BG_Read, BG_Read,
#ifndef	NO_MERCURY
	rm_buserr, rm_buserr, rm_buserr, rm_buserr, rm_buserr, rm_buserr, Mcry_Read, rm_buserr,
#else
	rm_buserr, rm_buserr, rm_buserr, rm_buserr, rm_buserr, rm_buserr, rm_buserr, rm_buserr,
#endif
	SRAM_Read, SRAM_Read, SRAM_Read, SRAM_Read, SRAM_Read, SRAM_Read, SRAM_Read, SRAM_Read,
	rm_buserr, rm_buserr, rm_buserr, rm_buserr, rm_buserr, rm_buserr, rm_buserr, rm_buserr,
	rm_buserr, rm_buserr, rm_buserr, rm_buserr, rm_buserr, rm_buserr, rm_buserr, rm_buserr,
	rm_font, rm_font, rm_font, rm_font, rm_font, rm_font, rm_font, rm_font,
	rm_font, rm_font, rm_font, rm_font, rm_font, rm_font, rm_font, rm_font,
	rm_font, rm_font, rm_font, rm_font, rm_font, rm_font, rm_font, rm_font,
	rm_font, rm_font, rm_font, rm_font, rm_font, rm_font, rm_font, rm_font,
	rm_font, rm_font, rm_font, rm_font, rm_font, rm_font, rm_font, rm_font,
	rm_font, rm_font, rm_font, rm_font, rm_font, rm_font, rm_font, rm_font,
	rm_font, rm_font, rm_font, rm_font, rm_font, rm_font, rm_font, rm_font,
	rm_font, rm_font, rm_font, rm_font, rm_font, rm_font, rm_font, rm_font,
	rm_font, rm_font, rm_font, rm_font, rm_font, rm_font, rm_font, rm_font,
	rm_font, rm_font, rm_font, rm_font, rm_font, rm_font, rm_font, rm_font,
	rm_font, rm_font, rm_font, rm_font, rm_font, rm_font, rm_font, rm_font,
	rm_font, rm_font, rm_font, rm_font, rm_font, rm_font, rm_font, rm_font,
	/* In the case of SCSI, will it be rm_buserr? */
	rm_ipl, rm_ipl, rm_ipl, rm_ipl, rm_ipl, rm_ipl, rm_ipl, rm_ipl,
	rm_ipl, rm_ipl, rm_ipl, rm_ipl, rm_ipl, rm_ipl, rm_ipl, rm_ipl,
	rm_ipl, rm_ipl, rm_ipl, rm_ipl, rm_ipl, rm_ipl, rm_ipl, rm_ipl,
	rm_ipl, rm_ipl, rm_ipl, rm_ipl, rm_ipl, rm_ipl, rm_ipl, rm_ipl,
};

void (*MemWriteTable[])(uint32_t, uint8_t) = {
	TVRAM_Write, TVRAM_Write, TVRAM_Write, TVRAM_Write, TVRAM_Write, TVRAM_Write, TVRAM_Write, TVRAM_Write,
	TVRAM_Write, TVRAM_Write, TVRAM_Write, TVRAM_Write, TVRAM_Write, TVRAM_Write, TVRAM_Write, TVRAM_Write,
	TVRAM_Write, TVRAM_Write, TVRAM_Write, TVRAM_Write, TVRAM_Write, TVRAM_Write, TVRAM_Write, TVRAM_Write,
	TVRAM_Write, TVRAM_Write, TVRAM_Write, TVRAM_Write, TVRAM_Write, TVRAM_Write, TVRAM_Write, TVRAM_Write,
	TVRAM_Write, TVRAM_Write, TVRAM_Write, TVRAM_Write, TVRAM_Write, TVRAM_Write, TVRAM_Write, TVRAM_Write,
	TVRAM_Write, TVRAM_Write, TVRAM_Write, TVRAM_Write, TVRAM_Write, TVRAM_Write, TVRAM_Write, TVRAM_Write,
	TVRAM_Write, TVRAM_Write, TVRAM_Write, TVRAM_Write, TVRAM_Write, TVRAM_Write, TVRAM_Write, TVRAM_Write,
	TVRAM_Write, TVRAM_Write, TVRAM_Write, TVRAM_Write, TVRAM_Write, TVRAM_Write, TVRAM_Write, TVRAM_Write,
	CRTC_Write, VCtrl_Write, DMA_Write, wm_nop, MFP_Write, RTC_Write, wm_nop, SysPort_Write,
	wm_opm, ADPCM_Write, FDC_Write, SASI_Write, SCC_Write, PIA_Write, IOC_Write, SCSI_IOCSPort_Write,
	SCSI_Write, wm_buserr, wm_buserr, wm_buserr, wm_buserr, wm_buserr, wm_buserr, MIDI_Write,
	BG_Write, BG_Write, BG_Write, BG_Write, BG_Write, BG_Write, BG_Write, BG_Write,
#ifndef	NO_MERCURY
	wm_buserr, wm_buserr, wm_buserr, wm_buserr, wm_buserr, wm_buserr, Mcry_Write, wm_buserr,
#else
	wm_buserr, wm_buserr, wm_buserr, wm_buserr, wm_buserr, wm_buserr, wm_buserr, wm_buserr,
#endif
	SRAM_Write, SRAM_Write, SRAM_Write, SRAM_Write, SRAM_Write, SRAM_Write, SRAM_Write, SRAM_Write,
	wm_buserr, wm_buserr, wm_buserr, wm_buserr, wm_buserr, wm_buserr, wm_buserr, wm_buserr,
	wm_buserr, wm_buserr, wm_buserr, wm_buserr, wm_buserr, wm_buserr, wm_buserr, wm_buserr,
	/* Any write to the ROM area results in a bus error */
	wm_buserr, wm_buserr, wm_buserr, wm_buserr, wm_buserr, wm_buserr, wm_buserr, wm_buserr,
	wm_buserr, wm_buserr, wm_buserr, wm_buserr, wm_buserr, wm_buserr, wm_buserr, wm_buserr,
	wm_buserr, wm_buserr, wm_buserr, wm_buserr, wm_buserr, wm_buserr, wm_buserr, wm_buserr,
	wm_buserr, wm_buserr, wm_buserr, wm_buserr, wm_buserr, wm_buserr, wm_buserr, wm_buserr,
	wm_buserr, wm_buserr, wm_buserr, wm_buserr, wm_buserr, wm_buserr, wm_buserr, wm_buserr,
	wm_buserr, wm_buserr, wm_buserr, wm_buserr, wm_buserr, wm_buserr, wm_buserr, wm_buserr,
	wm_buserr, wm_buserr, wm_buserr, wm_buserr, wm_buserr, wm_buserr, wm_buserr, wm_buserr,
	wm_buserr, wm_buserr, wm_buserr, wm_buserr, wm_buserr, wm_buserr, wm_buserr, wm_buserr,
	wm_buserr, wm_buserr, wm_buserr, wm_buserr, wm_buserr, wm_buserr, wm_buserr, wm_buserr,
	wm_buserr, wm_buserr, wm_buserr, wm_buserr, wm_buserr, wm_buserr, wm_buserr, wm_buserr,
	wm_buserr, wm_buserr, wm_buserr, wm_buserr, wm_buserr, wm_buserr, wm_buserr, wm_buserr,
	wm_buserr, wm_buserr, wm_buserr, wm_buserr, wm_buserr, wm_buserr, wm_buserr, wm_buserr,
	wm_buserr, wm_buserr, wm_buserr, wm_buserr, wm_buserr, wm_buserr, wm_buserr, wm_buserr,
	wm_buserr, wm_buserr, wm_buserr, wm_buserr, wm_buserr, wm_buserr, wm_buserr, wm_buserr,
	wm_buserr, wm_buserr, wm_buserr, wm_buserr, wm_buserr, wm_buserr, wm_buserr, wm_buserr,
	wm_buserr, wm_buserr, wm_buserr, wm_buserr, wm_buserr, wm_buserr, wm_buserr, wm_buserr,
};


static void wm_buserr(uint32_t addr, uint8_t val)
{
	BusErrFlag = 2;
	BusErrAdr = addr;
}

static void wm_cnt(uint32_t addr, uint8_t val)
{
	addr &= 0x00ffffff;
	if (addr < 0x00c00000) /* Use RAM upto 12MB */
	{
		webx68k_ram_watch_check(addr, val);
		ram_poke_raw(addr, val);
	}
	else if (addr < 0x00e00000)
		GVRAM_Write(addr, val);
	else
		MemWriteTable[(addr >> 13) & 0xff](addr, val);
}


static void wm_main(uint32_t addr, uint8_t val) 
{
	if ((BusErrFlag & 7) == 0)
		wm_cnt(addr, val);
}

static void wm_opm(uint32_t addr, uint8_t val)
{
	uint8_t t = addr & 3;
	if (t == 1)
		OPM_Write(0, val);
	else if (t == 3)
		OPM_Write(1, val);
}

static uint8_t rm_main(uint32_t addr)
{
	addr &= 0x00ffffff;
	if (addr < 0x00c00000) /* Use RAM upto 12MB */
		return MEM[addr ^ 1];
	else if (addr < 0x00e00000)
		return GVRAM_Read(addr);
	return MemReadTable[(addr >> 13) & 0xff](addr);
}

static uint8_t rm_font(uint32_t addr)
{
	return FONT[addr & 0xfffff];
}

static uint8_t rm_ipl(uint32_t addr)
{
	return IPL[(addr & 0x3ffff) ^ 1];
}

static uint8_t rm_opm(uint32_t addr)
{
	if ((addr & 3) == 3)
		return OPM_Read();
	return 0;
}

static uint8_t rm_buserr(uint32_t addr)
{
	BusErrFlag = 1;
	BusErrAdr = addr;

	return 0;
}

static void cpu_setOPbase24(uint32_t addr)
{
	switch ((addr >> 20) & 0xf)
   {
      case 0: case 1: case 2: case 3: case 4: case 5: case 6: case 7:
      case 8: case 9: case 0xa: case 0xb:
         OP_ROM = MEM;
         break;

      case 0xc:
      case 0xd:
         OP_ROM = GVRAM + (addr - 0x00c00000);
         break;

      case 0xe:
         if (addr < 0x00e80000) 
            OP_ROM = TVRAM + (addr - 0x00e00000);
         else if ((addr >= 0x00ea0000) && (addr < 0x00ea2000))
            OP_ROM = SCSIIPL + (addr - 0x00ea0000);
         else if ((addr >= 0x00ed0000) && (addr < 0x00ed4000))
            OP_ROM = SRAM + (addr - 0x00ed0000);
         else
         {
            BusErrFlag = 3;
            BusErrAdr = addr;
            BusErrHandling = 1;
         }
         break;

      case 0xf:
         if ((addr >= 0x00fc0000) && (addr < 0x01000000))
            OP_ROM = IPL + (addr - 0x00fc0000);
         else
         {
            BusErrFlag     = 3;
            BusErrAdr      = addr;
            BusErrHandling = 1;
         }
         break;
   }
}

/*
 * write function
 */
void dma_writemem24(uint32_t addr, uint8_t val)
{
	wm_main(addr, val);
}

void dma_writemem24_word(uint32_t addr, uint16_t val)
{
	if (addr & 1)
   {
		BusErrFlag |= 4;
		return;
	}

	wm_main(addr, (val >> 8) & 0xff);
	wm_main(addr + 1, val & 0xff);
}

void dma_writemem24_dword(uint32_t addr, uint32_t val)
{
	if (addr & 1)
   {
      BusErrFlag |= 4;
      return;
   }

	wm_main(addr, (val >> 24) & 0xff);
	wm_main(addr + 1, (val >> 16) & 0xff);
	wm_main(addr + 2, (val >> 8) & 0xff);
	wm_main(addr + 3, val & 0xff);
}

void cpu_writemem24(uint32_t addr, uint32_t val)
{
	BusErrFlag = 0;

	wm_cnt(addr, val & 0xff);
	if (BusErrFlag & 2)
		BusErrHandling = 1;
}

void cpu_writemem24_word(uint32_t addr, uint32_t val)
{

	if (addr & 1)
		return;

	BusErrFlag = 0;

	wm_cnt(addr, (val >> 8) & 0xff);
	wm_main(addr + 1, val & 0xff);

	if (BusErrFlag & 2)
		BusErrHandling = 1;
}

void cpu_writemem24_dword(uint32_t addr, uint32_t val)
{
	if (addr & 1)
		return;

	BusErrFlag = 0;

	wm_cnt(addr, (val >> 24) & 0xff);
	wm_main(addr + 1, (val >> 16) & 0xff);
	wm_main(addr + 2, (val >> 8) & 0xff);
	wm_main(addr + 3, val & 0xff);

	if (BusErrFlag & 2)
		BusErrHandling = 1;
}

/*
 * read function
 */
uint8_t dma_readmem24(uint32_t addr)
{
	return rm_main(addr);
}

uint16_t dma_readmem24_word(uint32_t addr)
{
	uint16_t v;

	if (addr & 1) {
		BusErrFlag = 3;
		return 0;
	}

	v = rm_main(addr++) << 8;
	v |= rm_main(addr);
	return v;
}

uint32_t 
dma_readmem24_dword(uint32_t addr)
{
	uint32_t v;

	if (addr & 1) {
		BusErrFlag = 3;
		return 0;
	}

	v = rm_main(addr++) << 24;
	v |= rm_main(addr++) << 16;
	v |= rm_main(addr++) << 8;
	v |= rm_main(addr);
	return v;
}

uint32_t 
cpu_readmem24(uint32_t addr)
{
	uint8_t v = rm_main(addr);
	if (BusErrFlag & 1)
		BusErrHandling = 1;
	return (uint32_t) v;
}

uint32_t 
cpu_readmem24_word(uint32_t addr)
{
	uint16_t v;

	if (addr & 1)
		return 0;

	BusErrFlag = 0;

	v = rm_main(addr++) << 8;
	v |= rm_main(addr);
	if (BusErrFlag & 1)
		BusErrHandling = 1;
	return (uint32_t) v;
}

uint32_t 
cpu_readmem24_dword(uint32_t addr)
{
	uint32_t v;

	if (addr & 1)
   {
		BusErrFlag = 3;
		return 0;
	}

	BusErrFlag = 0;

	v = rm_main(addr++) << 24;
	v |= rm_main(addr++) << 16;
	v |= rm_main(addr++) << 8;
	v |= rm_main(addr);
	return v;
}

/*
 * Memory misc
 */
void Memory_Init(void)
{
#if defined (HAVE_CYCLONE)
	cpu_setOPbase24((uint32_t)m68000_get_reg(M68K_PC));
#elif defined (HAVE_C68K)
	cpu_setOPbase24((uint32_t)C68k_Get_PC(&C68K));
#elif defined (HAVE_MUSASHI)
	cpu_setOPbase24((uint32_t)m68k_get_reg(NULL, M68K_REG_PC));
#endif /* HAVE_C68K */ /* HAVE_MUSASHI */
}

void 
Memory_SetSCSIMode(void)
{
	int i;
	for (i = 0xe0; i < 0xf0; i++)
		MemReadTable[i] = rm_buserr;
}
