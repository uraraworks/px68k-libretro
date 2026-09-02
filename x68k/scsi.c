/*
* SCSI.C - External SCSI board (CZ-6BS1)
* Supported by taking over SCSI IOCS (SPC is not emulated)
* Built-in SCSI (dummy) IPL is defined in winx68k.c
*/

#include "common.h"
#include "../libretro/dosio.h"
#include "winx68k.h"
#include "m68000.h"
#include "scsi.h"
#include "x68kmemory.h"

/* SCSI のセクタI/O(ホスト側)。実体は WebX68k の src/core-shim.c。
 * 決定2 により emscripten のファイルシステムは経由しない。 */
extern int webx68k_scsi_get_size(void);
extern int webx68k_scsi_read_sector(unsigned int lba, unsigned char *buf);

uint8_t	SCSIIPL[0x2000];

/* SCSI IOCS 観測フックのログ件数 (詳細は SCSI_IOCSPort_Write を参照) */
static int SCSIIOCSLogCount = 0;

/* SCSI IOCS ($F5) の呼び出し口。ROMスタブの "move.b d1, $e9f800" の宛先 */
#define SCSI_IOCS_PORT      0x00e9f800

/* 自己検査の状態: 0=通常, 1=検査中(まだ届いていない), 2=検査中(届いた) */
static int SCSIIOCSSelfTest = 0;

void SCSI_Init(void)
{
	/* Original SCSI ROM
	 * Operation: When SCSI IOCS is called, the SCSI IOCS number is output to $e9f800.
	 * Booting from a SCSI device is not possible, the initialization routine only sets the vector for SCSI IOCS ($F5).
	 */
	static	uint8_t	SCSIIMG[] = {
		0x00, 0xea, 0x00, 0x34,				/* $ea0020 Entry address for SCSI startup */
		0x00, 0xea, 0x00, 0x36,				/* $ea0024 Entry address of IOCS vector setting (must be 8 bytes before "Human") */
		0x00, 0xea, 0x00, 0x4a,				/* $ea0028 SCSI IOCS entry address */
		0x48, 0x75, 0x6d, 0x61,				/* $ea002c ↓ */
		0x6e, 0x36, 0x38, 0x6b,				/* $ea0030 ID "Human68k" (always right before the startup entry point) */
		0x4e, 0x75,							/* $ea0034 "rts" (startup entry point, does nothing) */
		0x23, 0xfc, 0x00, 0xea, 0x00, 0x4a,	/* $ea0036 ↓ (IOCS vector setting entry point) */
		0x00, 0x00, 0x07, 0xd4,				/* $ea003c "move.l #$ea004a, $7d4.l" (IOCS $F5 vector setting) */
		0x74, 0xff,							/* $ea0040 "moveq #-1, d2" */
		0x4e, 0x75,							/* $ea0042 "rts" */
		0x53, 0x43, 0x53, 0x49, 0x45, 0x58,	/* $ea0044 ID "SCSIEX" (SCSI card ID) */
		0x13, 0xc1, 0x00, 0xe9, 0xf8, 0x00,	/* $ea004a "move.b d1, $e9f800" (SCSI IOCS call entry point) */
		0x4e, 0x75,							/* $ea0050 "rts" */
	};
	int i;
	uint8_t tmp;
	SCSIIOCSLogCount = 0;
	memset(SCSIIPL, 0, 0x2000);
	memcpy(&SCSIIPL[0x20], SCSIIMG, sizeof(SCSIIMG));
	for (i=0; i<0x2000; i+=2)
	{
		tmp = SCSIIPL[i];
		SCSIIPL[i] = SCSIIPL[i+1];
		SCSIIPL[i+1] = tmp;
	}

	/* 陽性対照: SCSI IOCS の呼び出し口 $e9f800 への書き込みが本当に
	 * SCSI_IOCSPort_Write へ届くかを、CPU と同じ書き込み経路で毎回確かめる。
	 * ここが黙って死ぬと「IOCS 呼び出しが0件」という観測が
	 * 「呼ばれていない」と「フックが繋がっていない」の区別を失う。 */
	SCSIIOCSSelfTest = 1;
	cpu_writemem24(SCSI_IOCS_PORT, 0xab);
	if (log_cb)
	{
		if (SCSIIOCSSelfTest == 2)
			log_cb(RETRO_LOG_INFO, "[SCSI-IOCS] selftest ok: $e9f800 への書き込みがフックへ届いた\n");
		else
			log_cb(RETRO_LOG_ERROR, "[SCSI-IOCS] selftest FAILED: $e9f800 への書き込みがフックへ届かない\n");
	}
	SCSIIOCSSelfTest = 0;

	/* ホスト側セクタI/Oの疎通確認。イメージが繋がっていれば、
	 * FORMAT.X が書く SCSI ディスクIDがセクタ0の先頭に見えるはずである
	 * (期待値は既知の基準器から取っている。docs/STORAGE-SCSI.md
	 * 「移行後イメージの構造」参照)。 */
	{
		static uint8_t sec0[512];
		int size = webx68k_scsi_get_size();
		if (size <= 0)
		{
			if (log_cb)
				log_cb(RETRO_LOG_INFO, "[SCSI] イメージ未設定 (デバイス無しとして扱う)\n");
		}
		else if (webx68k_scsi_read_sector(0, sec0) != 0)
		{
			if (log_cb)
				log_cb(RETRO_LOG_ERROR, "[SCSI] セクタ0の読み出しに失敗した (size=%d)\n", size);
		}
		else if (log_cb)
		{
			log_cb(RETRO_LOG_INFO,
				"[SCSI] image size=%d bytes (%d sectors) sector0=\"%c%c%c%c%c%c%c%c\" sector0[8..11]=$%02x%02x%02x%02x\n",
				size, size / 512,
				sec0[0], sec0[1], sec0[2], sec0[3], sec0[4], sec0[5], sec0[6], sec0[7],
				sec0[8], sec0[9], sec0[10], sec0[11]);
		}
	}
}

/* --- SCSI IOCS ($F5) の観測フック ---------------------------------------
 * 上の SCSIIMG は SCSI IOCS 呼び出しを "move.b d1, $e9f800" として外へ出す。
 * $e9f800 は mem_wrap の書き込み表で wm_nop に落ちており、呼び出しは
 * 黙って捨てられていた。まず「誰が・どのコマンドで・どんな引数で呼ぶか」を
 * 実測するため、この段階では動作を変えずログだけ取る。 */
#define SCSI_IOCS_LOG_MAX   64

void FASTCALL SCSI_IOCSPort_Write(uint32_t adr, uint8_t data)
{
	if ((adr & ~1) != (SCSI_IOCS_PORT & ~1))
		return;
	if (SCSIIOCSSelfTest)
	{
		SCSIIOCSSelfTest = 2;   /* 自己検査の書き込みはログに数えない */
		return;
	}
	if (SCSIIOCSLogCount >= SCSI_IOCS_LOG_MAX)
		return;
	SCSIIOCSLogCount++;
	if (log_cb)
		log_cb(RETRO_LOG_INFO,
			"[SCSI-IOCS] #%d adr=$%06x cmd=$%02x d1=$%08x d2=$%08x d3=$%08x d4=$%08x d5=$%08x a1=$%08x pc=$%08x\n",
			SCSIIOCSLogCount, (unsigned)adr, (unsigned)data,
			(unsigned)m68000_get_reg(M68K_D1), (unsigned)m68000_get_reg(M68K_D2),
			(unsigned)m68000_get_reg(M68K_D3), (unsigned)m68000_get_reg(M68K_D4),
			(unsigned)m68000_get_reg(M68K_D5), (unsigned)m68000_get_reg(M68K_A1),
			(unsigned)m68000_get_reg(M68K_PC));
}

void SCSI_Cleanup(void) { }
void FASTCALL SCSI_Write(uint32_t adr, uint8_t data) { }

uint8_t FASTCALL SCSI_Read(uint32_t adr)
{
	return SCSIIPL[(adr^1)&0x1fff];
}
