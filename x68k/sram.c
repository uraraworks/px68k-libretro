/*
 *  SRAM.C - SRAM (16kb)
 */

#include	"common.h"
#include	"../libretro/dosio.h"
#include	"prop.h"
#include	"winx68k.h"
#include	"sysport.h"
#include	"x68kmemory.h"
#include	"sram.h"
#include	"../m68000/m68000.h"

static int write_enabled = 0;
uint8_t	SRAM[0x4000];

int SRAM_StateAction(StateMem *sm, int load, int data_only)
{
	SFORMAT StateRegs[] = 
	{
		SFARRAYN(SRAM, 0x4000, "MEM_SRAM"),
		SFVAR(write_enabled),

		SFEND
	};

	int ret = PX68KSS_StateAction(sm, load, data_only, StateRegs, "X68K_SRAM", false);

	return ret;
}

void SRAM_VirusCheck(void)
{
	if ( (cpu_readmem24_dword(0xed3f60)==0x60000002)
	   &&(cpu_readmem24_dword(0xed0010)==0x00ed3f60) )
	{
		SRAM_Cleanup();
		SRAM_Init();
	}
}

void SRAM_WriteEnable(int enable)
{
	if (write_enabled != enable)
	{
		log_cb(RETRO_LOG_DEBUG, "sram write enable = %d\n", enable);
		write_enabled = enable;
	}
}

void SRAM_Init(void)
{
	int i;
	void *fp;

	write_enabled = 0;

	for (i=0; i<0x4000; i++)
		SRAM[i] = 0xFF;

	if ((fp = file_open_c("sram.dat")))
	{
		file_lread(fp, SRAM, 0x4000);
		file_close(fp);

#ifndef MSB_FIRST
		for (i=0; i<0x4000; i+=2)
		{
			uint8_t tmp = SRAM[i];
			SRAM[i]     = SRAM[i+1];
			SRAM[i+1]   = tmp;
		}
#endif
	}
}

void SRAM_Cleanup(void)
{
   int i;
   void *fp;

   for (i=0; i<0x4000; i+=2)
   {
      uint8_t tmp = SRAM[i];
      SRAM[i]     = SRAM[i+1];
      SRAM[i+1]   = tmp;
   }

   if (!(fp = file_open_c("sram.dat")))
      if (!(fp = file_create_c("sram.dat")))
         return;

   file_lwrite(fp, SRAM, 0x4000);
   file_close(fp);
}

uint8_t FASTCALL SRAM_Read(uint32_t adr)
{
	adr &= 0xffff;
	adr ^= 1;
	if (adr<0x4000)
		return SRAM[adr];
	return 0xff;
}


/*
 * 調査用(2026-09-26、段階0): SCSI_Init()で$ed0018(起動デバイス)/$ed000c(ROM起動アドレス)を
 * 書いても、IPL自身が起動直後(pc≒$00ff0010)に$ed000c〜$ed001bをまるごと固定値へ
 * 上書きすることが実測で分かった(docs/STORAGE-SCSI.md「SRAM 段階0」参照。書いたアドレスの
 * 上位バイトを$00/$01で振っても同じ固定値で上書きされ、無条件の書き換えと確認済み)。
 * そのため、この上書き(=$ed001bへの書き込みで完了する)の直後に、こちら側の設定を
 * もう一度書き直す。既定(webx68k_scsi_sram_boot_addr()が0)では一切何もしない。 */
extern int webx68k_scsi_sram_boot_addr(void);
static int webx68k_sram_reinject_active = 0; /* 再注入自身の書き込みで再帰しないようにする */
/*
 * UI実装(段階4)時に判明した不備の是正(2026-09-26): $ed001bへの書き込みはIPL自身の
 * まっさらなSRAM初期化コピー(署名不一致時のみ発生。docs/STORAGE-SCSI.md「段階0」の
 * 訂正参照)だけでなく、Human68k自身の無関係な書き込み(実測: pc=$001a2c02、$ed001aと
 * $ed001bへ元と同じ値$4e/$07を書き戻すだけの処理。段階1a参照)でも発火することが
 * わかっている。再注入を「その起動でIPLがSRAMを初期化した(=この不確定な$ed001b書き込みが
 * 最初に来た)ときの1回だけ」に限定するため、一度発火したら(webx68k_scsi_sram_boot_addr()が
 * 0で何もしない場合も含め)以後は二度と発火しないようにする。各起動(コア再生成、
 * src/libretro-host.tsのinit()が毎回新しいwasmモジュールを作る)ごとにこの静的変数も
 * ゼロから再初期化されるため、明示的なリセット呼び出しは不要。
 * 呼び出し側(JS)は、さらに「起動前のSRAM署名が無効だったとき」だけ
 * webx68k_scsi_sram_boot_addr()が非0を返すよう既に絞っている
 * (src/main.tsのapplyDesiredBootDeviceGlobal())。両方を満たして初めて再注入する。 */
static int webx68k_sram_reinject_done = 0;

/* 段階1a調査用: SRAM_Write の呼び出し経路を区別する。
 * mem_wrap.c のメモリ書き込み表からCPU(m68k_execute実行中のゲスト命令、または
 * cpu_writemem24経由のホストC側直接アクセス)経由で呼ばれる場合は
 * webx68k_sram_write_call_site==0(既定=SRAM_WRITE_SITE_CPU_OR_HOST_MEMIO)のまま。
 * SCSI_Init()や本ファイルの再注入フックなど、SRAM_Write()をCの関数呼び出しとして
 * 直接叩く箇所は、呼ぶ直前にこの変数を立てて区別できるようにする。
 * 重要な注意: m68k_get_reg(PC)は「現在の68000命令のPC」ではなく「次に実行される
 * 命令のPC」であり、WinX68k_Reset()はm68k_pulse_reset()でPCをリセットベクタへ
 * セットした直後(CPUが1命令も実行される前)にSCSI_Init()を呼ぶ。そのため
 * SCSI_Init()内のSRAM_Write呼び出しでもPCは$00ff0010付近を指してしまい、
 * PCだけでは「IPL自身が書いたのか、ホストC側がリセット直後に書いたのか」を
 * 区別できない。呼び出し元ごとの明示フラグが必要な理由はここにある。 */
int webx68k_sram_write_call_site = 0;

void FASTCALL SRAM_Write(uint32_t adr, uint8_t data)
{
	uint32_t orig_adr = adr; /* 下でadr自身を書き換えるため、判定用に元の値を控える */

	if ((adr >= 0x00ed000c && adr <= 0x00ed001b) && log_cb)
		log_cb(RETRO_LOG_INFO, "[SRAM-WRITE] adr=$%08x data=$%02x write_enabled=%d call_site=%d pc=$%08x\n",
			(unsigned)adr, (unsigned)data, write_enabled, webx68k_sram_write_call_site,
			(unsigned)m68k_get_reg(NULL, M68K_REG_PC));
	if ( write_enabled && (adr < 0xed4000) )
	{
		adr       &= 0xffff;
#ifndef MSB_FIRST
		adr       ^= 1;
#endif
		SRAM[adr]  = data;
	}

	/* IPL自身の書き換え(上のコメント参照)が$ed001bまで進んだ直後に、こちらの設定を
	 * 上書きし直す。再注入自身の書き込み(このifを再度満たさないよう再帰は避ける)。
	 * webx68k_sram_reinject_doneで「この起動で最初に来た$ed001b書き込みの1回だけ」に
	 * 絞る(上のコメント参照。boot_addrが0=何もしない場合もここで消費して以後発火しない)。 */
	if (orig_adr == 0x00ed001b && !webx68k_sram_reinject_active && !webx68k_sram_reinject_done)
	{
		int boot_addr = webx68k_scsi_sram_boot_addr();
		webx68k_sram_reinject_done = 1;
		if (boot_addr)
		{
			webx68k_sram_reinject_active = 1;
			webx68k_sram_write_call_site = 2; /* REINJECT */
			SRAM_Write(0x00ed0018, 0xa0);
			SRAM_Write(0x00ed0019, 0x00);
			SRAM_Write(0x00ed000c, (uint8_t)((boot_addr >> 24) & 0xff));
			SRAM_Write(0x00ed000d, (uint8_t)((boot_addr >> 16) & 0xff));
			SRAM_Write(0x00ed000e, (uint8_t)((boot_addr >> 8) & 0xff));
			SRAM_Write(0x00ed000f, (uint8_t)(boot_addr & 0xff));
			webx68k_sram_write_call_site = 0;
			webx68k_sram_reinject_active = 0;
			if (log_cb)
				log_cb(RETRO_LOG_INFO,
					"[SCSI-BOOT-REINJECT] IPL自身の上書き直後に再設定 (要求addr=$%08x, 読み返し $ed0018=$%02x%02x $ed000c=$%02x%02x%02x%02x)\n",
					(unsigned)boot_addr,
					SRAM_Read(0x00ed0018), SRAM_Read(0x00ed0019),
					SRAM_Read(0x00ed000c), SRAM_Read(0x00ed000d),
					SRAM_Read(0x00ed000e), SRAM_Read(0x00ed000f));
		}
	}
}

void FASTCALL SRAM_UpdateBoot(void)
{
	cpu_writemem24(0xe8e00d, 0x31); /* SRAM write permission */
	cpu_writemem24_dword(0xed0040, cpu_readmem24_dword(0xed0040) + 1); /* Estimated operation time(min.) */
	cpu_writemem24_dword(0xed0044, cpu_readmem24_dword(0xed0044) + 1); /* Estimated booting times */
	cpu_writemem24(0xe8e00d, 0x00);
}
