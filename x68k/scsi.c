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

/* 自前ROMコードからホスト(C側)を呼ぶための私設ポート。
 * IOCS の呼び出し口($e9f800)とは別にして、ログが混ざらないようにする。 */
#define SCSI_HOST_PORT      0x00e9f802

/* セクタ0の暫定ロード先。実機がどこへ載せるかは未確認であり、
 * この値は「ゲストRAMへ書けるか」を測るための仮置きである。 */
#define SCSI_BOOT_LOAD_ADDR 0x00002000

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
		0x60, 0x2a,							/* $ea0034 "bra.s $ea0060" 起動エントリ。ROMの構造は動かさず
											 * ここを2バイトの分岐に置き換えて自前ルーチンへ飛ばす。
											 * (実測: $ea0020 のポインタを書き換えると IPL がボードごと
											 *  受け付けなくなり、$07d4 が既定値に戻った) */
		0x23, 0xfc, 0x00, 0xea, 0x00, 0x4a,	/* $ea0036 ↓ (IOCS vector setting entry point) */
		0x00, 0x00, 0x07, 0xd4,				/* $ea003c "move.l #$ea004a, $7d4.l" (IOCS $F5 vector setting) */
		0x60, 0x3e,							/* $ea0040 "bra.s $ea0080" 陽性対照。ベクタ設定エントリは
											 * 実測で必ず呼ばれている($07d4 が書き換わる)ので、
											 * こちらの印が出れば「呼ばれれば印が出る」ことの証明になる。
											 * 元の moveq は飛び先で実行する。 */
		0x4e, 0x75,							/* $ea0042 "rts" */
		0x53, 0x43, 0x53, 0x49, 0x45, 0x58,	/* $ea0044 ID "SCSIEX" (SCSI card ID) */
		0x13, 0xc1, 0x00, 0xe9, 0xf8, 0x00,	/* $ea004a "move.b d1, $e9f800" (SCSI IOCS call entry point) */
		0x4e, 0x75,							/* $ea0050 "rts" */

		/* $ea0052-$ea005f 予備 (自前ルーチンを $ea0060 に置くための詰め物) */
		0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
		0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
		/* $ea0060 起動エントリ本体。
		 * 実機の CZ-6BS1 ではここがSCSIバスを走査してデバイスを登録する。
		 * この段階では「呼ばれたか」を測るための印を出すだけで、まだ何も登録しない。 */
		0x13, 0xfc, 0x00, 0x01, 0x00, 0xe9, 0xf8, 0x02,	/* $ea0060 "move.b #$01, $e9f802" 起動エントリに入った */
		0x13, 0xfc, 0x00, 0x02, 0x00, 0xe9, 0xf8, 0x02,	/* $ea0068 "move.b #$02, $e9f802" セクタ0をゲストRAMへ載せろ */
		0x4e, 0x75,							/* $ea0070 "rts" */
		/* $ea0072-$ea007f 詰め物 */
		0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
		0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
		/* $ea0080 ベクタ設定エントリの続き(陽性対照つき) */
		0x13, 0xfc, 0x00, 0x03, 0x00, 0xe9, 0xf8, 0x02,	/* "move.b #$03, $e9f802" ベクタ設定エントリが呼ばれた */
		0x74, 0xff,							/* "moveq #-1, d2" (元の処理) */
		0x4e, 0x75,							/* "rts" */
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

/* 自前ROMコードからの依頼を処理する。まだ何も登録せず、観測に必要なことだけ行う。 */
static void SCSI_HostCommand(uint8_t cmd)
{
	static uint8_t sec0[512];
	uint32_t i;

	if (cmd == 0x03)
	{
		if (log_cb)
			log_cb(RETRO_LOG_INFO, "[SCSI] ベクタ設定エントリが呼ばれた(陽性対照) (pc=$%08x)\n",
				(unsigned)m68000_get_reg(M68K_PC));
		return;
	}
	if (cmd == 0x01)
	{
		if (log_cb)
			log_cb(RETRO_LOG_INFO, "[SCSI] 起動エントリが呼ばれた (pc=$%08x)\n",
				(unsigned)m68000_get_reg(M68K_PC));
		return;
	}
	if (cmd == 0x02)
	{
		if (webx68k_scsi_read_sector(0, sec0) != 0)
		{
			if (log_cb)
				log_cb(RETRO_LOG_INFO, "[SCSI] セクタ0を読めない (デバイス無し)\n");
			return;
		}
		for (i = 0; i < 512; i++)
			cpu_writemem24(SCSI_BOOT_LOAD_ADDR + i, sec0[i]);
		/* 書いた先を CPU と同じ読み出し経路で読み返す。
		 * バイト順の取り違えはここでしか捕まらない。 */
		if (log_cb)
			log_cb(RETRO_LOG_INFO,
				"[SCSI] セクタ0を $%06x へ載せた 読み返し=\"%c%c%c%c%c%c%c%c\"\n",
				(unsigned)SCSI_BOOT_LOAD_ADDR,
				cpu_readmem24(SCSI_BOOT_LOAD_ADDR + 0), cpu_readmem24(SCSI_BOOT_LOAD_ADDR + 1),
				cpu_readmem24(SCSI_BOOT_LOAD_ADDR + 2), cpu_readmem24(SCSI_BOOT_LOAD_ADDR + 3),
				cpu_readmem24(SCSI_BOOT_LOAD_ADDR + 4), cpu_readmem24(SCSI_BOOT_LOAD_ADDR + 5),
				cpu_readmem24(SCSI_BOOT_LOAD_ADDR + 6), cpu_readmem24(SCSI_BOOT_LOAD_ADDR + 7));
		return;
	}
	if (log_cb)
		log_cb(RETRO_LOG_ERROR, "[SCSI] 未知のホストコマンド $%02x\n", (unsigned)cmd);
}

/* --- SCSI IOCS ($F5) の観測フック ---------------------------------------
 * 上の SCSIIMG は SCSI IOCS 呼び出しを "move.b d1, $e9f800" として外へ出す。
 * $e9f800 は mem_wrap の書き込み表で wm_nop に落ちており、呼び出しは
 * 黙って捨てられていた。まず「誰が・どのコマンドで・どんな引数で呼ぶか」を
 * 実測するため、この段階では動作を変えずログだけ取る。 */
#define SCSI_IOCS_LOG_MAX   64

void FASTCALL SCSI_IOCSPort_Write(uint32_t adr, uint8_t data)
{
	if (adr == SCSI_HOST_PORT)
	{
		SCSI_HostCommand(data);
		return;
	}
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
