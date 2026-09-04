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
	/* PCでの絞り込みは「pc_lo が負(既定 -1)なら無効」とする。
	 * 2026-09-03 実測: 以前は `pc_lo <= pc_hi` だけで判定していたため、
	 * 既定値 (-1, -1) が「有効かつ範囲は -1 のみ」と解釈され、
	 * 実PC(必ず0以上)が全件捨てられていた。自己検査だけが
	 * selftest_active でこの絞り込みを迂回するので陽性対照は通り続け、
	 * 「フックは繋がっているのに1件も観測されない」という偽の沈黙になった。 */
	if (!webx68k_ram_watch_selftest_active && webx68k_ram_watch_pc_lo >= 0 &&
	    webx68k_ram_watch_pc_lo <= webx68k_ram_watch_pc_hi)
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
static uint8_t rm_main(uint32_t addr); /* 読み出し監視の自己検査から使うための前方宣言 */

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

/*
 * ゲストRAM「読み出し」監視用フック(WebX68k-storage側)。
 * 書き込み監視(webx68k_ram_watch_*)と同じ流儀。番地範囲は
 * __webx68kMemReadWatchLo/Hi、読んだ側のPC範囲は
 * __webx68kMemReadWatchPcLo/Hi (既定は両方無効=-1、PCは絞らず=lo>hi)。
 * ホットパス(rm_main、全メモリ読み出しが必ず通る)からは
 * static変数の比較1回だけで早期脱出できるようにする。
 *
 * ポーリングで同じ番地を延々読み続けるケース(SCSI ROMの待ちループ等)で
 * ログ上限2000件をあっという間に食い潰してしまうため、直前と
 * (番地, PC)が同一の連続アクセスは1行に圧縮し「×N回」でまとめる。
 */
int32_t webx68k_mem_read_watch_lo = -1;
int32_t webx68k_mem_read_watch_hi = -1;
int32_t webx68k_mem_read_watch_pc_lo = -1;
int32_t webx68k_mem_read_watch_pc_hi = -1;
int webx68k_mem_read_watch_count = 0;
/* 調査用(2026-09-04): 記録を「実行トレース(webx68k_trace_*)のトリガ発火後」
 * だけに限定するゲート。既定0=従来通り無条件(過去のwatchA/watchB等の
 * 使い方を壊さない)。1にすると webx68k_trace_enabled が立つまで
 * webx68k_mem_read_watch_check() は何もしない。SCSI/SASIの分岐条件を
 * 突き合わせる際、無関係な場面(起動時のディレクトリ走査等)の読み出しが
 * 同じPC範囲を通って紛れ込むのを防ぐために追加した。 */
int webx68k_mem_read_watch_require_trigger = 0;

/* webx68k_trace_enabled の実体は本ファイル下方(実行トレース節)にある static int。
 * 上の require_trigger ゲートから使うため前方参照する。 */
static int webx68k_trace_enabled;

/* 自己検査実行中はPCでの絞り込みを適用しない(理由はwebx68k_ram_watch_selftest_active参照) */
static int webx68k_mem_read_watch_selftest_active = 0;

/* 直前に出力したログの圧縮用状態 */
static int      webx68k_mem_read_watch_has_last = 0;
static uint32_t webx68k_mem_read_watch_last_addr = 0;
static uint32_t webx68k_mem_read_watch_last_pc = 0;
static uint8_t  webx68k_mem_read_watch_last_data = 0;
static long     webx68k_mem_read_watch_repeat = 0;
/* 保留中エントリのうち、既に(確定出力または定期フラッシュで)出力済みの件数。
 * 「件数は累積でなく前回出力からの増分」にするための基準線。
 * 新しい保留エントリを開始するたびに0へ戻す。 */
static long     webx68k_mem_read_watch_reported = 0;

/* 溜めていた繰り返し行を、前回出力からの増分だけ確定出力する
 * (次のアドレスに移る/監視終了時)。保留は空にする。 */
static void webx68k_mem_read_watch_flush(void)
{
	long inc;

	if (!webx68k_mem_read_watch_has_last)
		return;

	inc = webx68k_mem_read_watch_repeat - webx68k_mem_read_watch_reported;
	if (inc > 1)
		printf("[SCSI-MEMR] R adr=$%06x data=$%02x pc=$%08x (x%ld\xe5\x9b\x9e)\n",
		       (unsigned)webx68k_mem_read_watch_last_addr,
		       (unsigned)webx68k_mem_read_watch_last_data,
		       (unsigned)webx68k_mem_read_watch_last_pc,
		       inc);
	else if (inc == 1)
		printf("[SCSI-MEMR] R adr=$%06x data=$%02x pc=$%08x\n",
		       (unsigned)webx68k_mem_read_watch_last_addr,
		       (unsigned)webx68k_mem_read_watch_last_data,
		       (unsigned)webx68k_mem_read_watch_last_pc);
	/* inc <= 0: 既に定期フラッシュで全件出力済みなので何も出さない */

	webx68k_mem_read_watch_has_last = 0;
	webx68k_mem_read_watch_repeat = 0;
	webx68k_mem_read_watch_reported = 0;
}

/*
 * 保留中の圧縮エントリを、前回出力(確定出力または前回の定期フラッシュ)
 * からの増分件数だけ「(継続中)」付きで出力する。保留自体はクリアしない
 * (監視は継続する)。
 *
 * 目的: ゲストが同じ値を読み続けるポーリングのまま実行が終わると、
 * webx68k_mem_read_watch_flush() は「変化したとき」にしか呼ばれないため
 * ログに1行も残らず「一度も読まれていない」と誤読する事故があった
 * (2026-09-02、[SCSI-BUS]/[SCSI-MEMR]/[SCSI-RAM]のログ全体で計4回発生)。
 * SCSI_LogPcIfRealRom()(60フレームに1回)から呼び、沈黙を防ぐ。
 * 増分が0(前回から変化なし)なら何も出さない。
 */
void webx68k_mem_read_watch_flush_periodic(void)
{
	long inc;

	if (!webx68k_mem_read_watch_has_last)
		return;

	inc = webx68k_mem_read_watch_repeat - webx68k_mem_read_watch_reported;
	if (inc <= 0)
		return;

	if (inc > 1)
		printf("[SCSI-MEMR] R adr=$%06x data=$%02x pc=$%08x (x%ld\xe5\x9b\x9e、継続中)\n",
		       (unsigned)webx68k_mem_read_watch_last_addr,
		       (unsigned)webx68k_mem_read_watch_last_data,
		       (unsigned)webx68k_mem_read_watch_last_pc,
		       inc);
	else
		printf("[SCSI-MEMR] R adr=$%06x data=$%02x pc=$%08x (継続中)\n",
		       (unsigned)webx68k_mem_read_watch_last_addr,
		       (unsigned)webx68k_mem_read_watch_last_data,
		       (unsigned)webx68k_mem_read_watch_last_pc);

	webx68k_mem_read_watch_reported = webx68k_mem_read_watch_repeat;
}

static void webx68k_mem_read_watch_check(uint32_t addr, uint8_t val)
{
	uint32_t pc;

	if (webx68k_mem_read_watch_lo > webx68k_mem_read_watch_hi)
		return;
	if ((int32_t)addr < webx68k_mem_read_watch_lo || (int32_t)addr > webx68k_mem_read_watch_hi)
		return;
	if (webx68k_mem_read_watch_require_trigger && !webx68k_mem_read_watch_selftest_active &&
	    !webx68k_trace_enabled)
		return;

	pc = m68000_get_reg(M68K_PC);
	/* PCでの絞り込みは「pc_lo が負(既定 -1)なら無効」とする。
	 * 2026-09-03 実測: 以前は `pc_lo <= pc_hi` だけで判定していたため、
	 * 既定値 (-1, -1) が「有効かつ範囲は -1 のみ」と解釈され、
	 * 実PC(必ず0以上)が全件捨てられていた。自己検査だけが
	 * selftest_active でこの絞り込みを迂回するので陽性対照は通り続け、
	 * 「フックは繋がっているのに1件も観測されない」という偽の沈黙になった。 */
	if (!webx68k_mem_read_watch_selftest_active && webx68k_mem_read_watch_pc_lo >= 0 &&
	    webx68k_mem_read_watch_pc_lo <= webx68k_mem_read_watch_pc_hi)
	{
		if ((int32_t)pc < webx68k_mem_read_watch_pc_lo || (int32_t)pc > webx68k_mem_read_watch_pc_hi)
			return;
	}

	if (webx68k_mem_read_watch_has_last &&
	    addr == webx68k_mem_read_watch_last_addr &&
	    pc == webx68k_mem_read_watch_last_pc &&
	    val == webx68k_mem_read_watch_last_data)
	{
		webx68k_mem_read_watch_repeat++;
		return; /* 圧縮中。件数はflush時に1件として数える */
	}

	/* アドレス/PC/値が変わったので、直前の繰り返しを確定させる */
	webx68k_mem_read_watch_flush();

	if (webx68k_mem_read_watch_count >= 2000)
		return;

	webx68k_mem_read_watch_count++;
	webx68k_mem_read_watch_has_last = 1;
	webx68k_mem_read_watch_last_addr = addr;
	webx68k_mem_read_watch_last_pc = pc;
	webx68k_mem_read_watch_last_data = val;
	webx68k_mem_read_watch_repeat = 1;
	webx68k_mem_read_watch_reported = 0;
}

/*
 * 陽性対照: 監視範囲が有効なときだけ、範囲内の先頭番地を1回読み、
 * webx68k_mem_read_watch_check() 経由でログに出たかどうかを自己検査する。
 * このぶんのログ件数はカウントから除外する。
 */
void webx68k_mem_read_watch_selftest(void)
{
	uint32_t addr;
	int before;

	if (webx68k_mem_read_watch_lo > webx68k_mem_read_watch_hi)
		return; /* 監視無効なら自己検査もしない */
	if (webx68k_mem_read_watch_lo < 0)
		return;

	addr = (uint32_t)webx68k_mem_read_watch_lo;
	before = webx68k_mem_read_watch_count;

	webx68k_mem_read_watch_selftest_active = 1;
	(void)rm_main(addr); /* フック経由の読み出し(本番と同じ経路) */
	webx68k_mem_read_watch_selftest_active = 0;

	/* 自己検査は圧縮バッファに乗るのでflushして確定させてから判定する */
	webx68k_mem_read_watch_flush();

	if (webx68k_mem_read_watch_count == before)
	{
		printf("[SCSI-MEMR] ERROR: self-test read from $%06x was not observed "
		       "(hook not wired?)\n", (unsigned)addr);
	}
	else
	{
		webx68k_mem_read_watch_count = before; /* 自己検査ぶんは件数に数えない */
	}
}

/*
 * 実行トレース(調査用、2026-09-04): SCSIの端数セクタ書き戻しが出ない件の分岐点を
 * 探すため、トリガ(x68k/scsi.c / x68k/sasi.c から webx68k_trace_start() を呼ぶ)
 * 以降に実行された全メモリアクセスのPCを記録する。
 * ホットパス(rm_main/wm_cnt、全アクセスが必ず通る)からは
 * webx68k_trace_enabled の比較1回だけで早期脱出できるようにする。
 *
 * 溜め込まず都度printfする(既存の[SCSI-RAM]/[SCSI-MEMR]と同じ流儀)。
 * 件数上限で打ち切り、打ち切ったこと自体を必ずログに出す
 * (「途中で切れた」を「そこで止まった」と読み違えないため)。
 * PC範囲はHuman68k本体が乗る低位番地帯に既定で絞る(ドライバ内部のコードは
 * 分岐の比較対象ではないため)。
 * 2度目以降のトリガ発火は無視する(最初に立ったトリガのみ有効。
 * SCSI実行とSASI実行は別プロセス起動なので、同一プロセス内で両方が
 * 発火することは無いが、念のため多重発火を防いでおく)。
 */
static int webx68k_trace_enabled = 0;
static int webx68k_trace_truncated = 0;
static int webx68k_trace_count = 0;
static char webx68k_trace_tag[16] = "TRACE";
#define WEBX68K_TRACE_MAX 10000
int32_t webx68k_trace_pc_lo = 0x00008000;
int32_t webx68k_trace_pc_hi = 0x00020000;
/* 調査用(2026-09-04): 「SCSI側が我々のドライバ($190000付近)を呼んでいるか」を
 * 確定するため、Human68k本体の範囲($8000-$20000)に加えてドライバヘッダ帯
 * ($180000-$1a0000、我々のドライバヘッダ$190000・SCSIのDPB$190034を含む)も
 * 同時に記録できるよう、互いに素な第2範囲を追加した。無効化するには
 * pc2_lo > pc2_hi にする(既定は有効)。 */
int32_t webx68k_trace_pc2_lo = 0x00180000;
int32_t webx68k_trace_pc2_hi = 0x001a0000;

void webx68k_trace_start(const char *tag)
{
	if (webx68k_trace_enabled)
		return;
	webx68k_trace_enabled = 1;
	if (tag)
	{
		size_t n = strlen(tag);
		if (n >= sizeof(webx68k_trace_tag))
			n = sizeof(webx68k_trace_tag) - 1;
		memcpy(webx68k_trace_tag, tag, n);
		webx68k_trace_tag[n] = '\0';
	}
	if (webx68k_trace_pc2_lo <= webx68k_trace_pc2_hi)
		printf("[%s-TRACE] トリガ発火。以降のPCを記録開始(範囲=$%08x-$%08x および $%08x-$%08x 上限=%d)\n",
		       webx68k_trace_tag, (unsigned)webx68k_trace_pc_lo, (unsigned)webx68k_trace_pc_hi,
		       (unsigned)webx68k_trace_pc2_lo, (unsigned)webx68k_trace_pc2_hi, WEBX68K_TRACE_MAX);
	else
		printf("[%s-TRACE] トリガ発火。以降のPCを記録開始(範囲=$%08x-$%08x 上限=%d)\n",
		       webx68k_trace_tag, (unsigned)webx68k_trace_pc_lo, (unsigned)webx68k_trace_pc_hi,
		       WEBX68K_TRACE_MAX);
}

static void webx68k_trace_record(uint32_t pc)
{
	int in_range1, in_range2;

	if (!webx68k_trace_enabled)
		return;
	in_range1 = (pc >= (uint32_t)webx68k_trace_pc_lo && pc <= (uint32_t)webx68k_trace_pc_hi);
	in_range2 = (webx68k_trace_pc2_lo <= webx68k_trace_pc2_hi) &&
	            (pc >= (uint32_t)webx68k_trace_pc2_lo && pc <= (uint32_t)webx68k_trace_pc2_hi);
	if (!in_range1 && !in_range2)
		return;
	if (webx68k_trace_count >= WEBX68K_TRACE_MAX)
	{
		if (!webx68k_trace_truncated)
		{
			webx68k_trace_truncated = 1;
			printf("[%s-TRACE] 上限%dに達したため打ち切った(以降は記録しない)\n",
			       webx68k_trace_tag, WEBX68K_TRACE_MAX);
		}
		return;
	}
	printf("[%s-TRACE] #%d pc=$%08x\n", webx68k_trace_tag, webx68k_trace_count, (unsigned)pc);
	webx68k_trace_count++;
}

/*
 * デバイスドライバ入口フック(調査用、2026-09-04): 「Human68kが我々のSCSIドライバへ
 * 書き戻し($08)を発行しない」件の比較対象として、成功している側
 * (Human68k内蔵HARDDSKドライバ=SASI用)が受け取る要求ヘッダをそのまま覗く。
 * 逆アセはせず、走らせて外から観測するだけ。
 *
 * ストラテジ入口PC・インタラプト入口PCをJS側(core-shim.cのwebx68k_drv_hook_refresh()
 * 経由)から指定し、そこに来た時点のa5(要求ヘッダ番地。x68k/scsi.cのSCSIReqHeaderAddrと
 * 同じ流儀で、我々のドライバではa5で来ることを実測済み)を控えて26バイトダンプする。
 * 既定は全部無効(strategy/interrupt < 0)で、1バイトも挙動を変えない。
 * ホットパス(rm_main/wm_cnt)からは先頭の比較1回だけで早期脱出できるようにする。
 */
int32_t webx68k_drv_hook_strategy  = -1; /* ストラテジ入口のPC。負なら無効 */
int32_t webx68k_drv_hook_interrupt = -1; /* インタラプト入口のPC。負なら無効 */
int32_t webx68k_drv_hook_outside   = 0x00010000; /* このPC未満へ戻ったら「呼び出し元へ復帰した」とみなす */

static int      webx68k_drv_hook_count = 0;
static int      webx68k_drv_hook_truncated = 0;
static uint32_t webx68k_drv_hook_req_addr = 0;
static int       webx68k_drv_hook_pending = 0;
static int       webx68k_drv_hook_busy = 0; /* cpu_readmem24経由の再入防止 */
#define WEBX68K_DRV_HOOK_MAX 3000

uint32_t cpu_readmem24(uint32_t addr); /* 本ファイル下方で定義。前方宣言して先に使う */

static void webx68k_drv_hook_dump(const char *tag, uint32_t addr)
{
	uint8_t buf[26];
	unsigned i;

	if (webx68k_drv_hook_count >= WEBX68K_DRV_HOOK_MAX)
	{
		if (!webx68k_drv_hook_truncated)
		{
			webx68k_drv_hook_truncated = 1;
			printf("[DRV-HOOK] 上限%dに達したため打ち切った(以降は記録しない)\n", WEBX68K_DRV_HOOK_MAX);
		}
		return;
	}
	if (addr == 0 || addr >= 0x00c00000)
		return;

	webx68k_drv_hook_busy = 1;
	for (i = 0; i < sizeof(buf); i++)
		buf[i] = (uint8_t)cpu_readmem24(addr + i);
	webx68k_drv_hook_busy = 0;

	printf("[DRV-HOOK] %s addr=$%08x:"
	       " %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x"
	       " %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x\n",
	       tag, (unsigned)addr,
	       buf[0], buf[1], buf[2], buf[3], buf[4], buf[5], buf[6], buf[7], buf[8], buf[9],
	       buf[10], buf[11], buf[12], buf[13], buf[14], buf[15], buf[16], buf[17], buf[18], buf[19],
	       buf[20], buf[21], buf[22], buf[23], buf[24], buf[25]);
	webx68k_drv_hook_count++;
}

static void webx68k_drv_hook_check(uint32_t pc)
{
	static uint32_t last_pc = 0xffffffff;

	/* 既定(両方無効)では比較1回だけで早期脱出。既存トレースと同じ流儀。 */
	if (webx68k_drv_hook_strategy < 0 && webx68k_drv_hook_interrupt < 0)
		return;
	if (webx68k_drv_hook_busy)
		return; /* cpu_readmem24経由の再入 */
	if (pc == last_pc)
		return; /* 1命令あたり複数回メモリアクセスが来るため */
	last_pc = pc;

	if (webx68k_drv_hook_strategy >= 0 && pc == (uint32_t)webx68k_drv_hook_strategy)
	{
		webx68k_drv_hook_req_addr = m68000_get_reg(M68K_A5);
		printf("[DRV-HOOK] ストラテジ pc=$%08x a1=$%08x a5=$%08x\n",
		       (unsigned)pc, (unsigned)m68000_get_reg(M68K_A1), (unsigned)webx68k_drv_hook_req_addr);
		webx68k_drv_hook_dump("S-before", webx68k_drv_hook_req_addr);
		return;
	}

	if (webx68k_drv_hook_interrupt >= 0 && pc == (uint32_t)webx68k_drv_hook_interrupt)
	{
		webx68k_drv_hook_dump("I-before", webx68k_drv_hook_req_addr);
		webx68k_drv_hook_pending = 1;
		return;
	}

	if (webx68k_drv_hook_pending && pc < (uint32_t)webx68k_drv_hook_outside)
	{
		webx68k_drv_hook_dump("I-after ", webx68k_drv_hook_req_addr);
		webx68k_drv_hook_pending = 0;
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
	if (webx68k_trace_enabled)
		webx68k_trace_record(m68000_get_reg(M68K_PC));
	/* 既定(両方無効)では m68000_get_reg() すら呼ばない。ここは全メモリ
	 * アクセスが通るため、無効時に関数呼び出しを増やすとエミュレータの
	 * 速度そのものが変わり、既存の計測値と条件が揃わなくなる。 */
	if (webx68k_drv_hook_strategy >= 0 || webx68k_drv_hook_interrupt >= 0)
		webx68k_drv_hook_check(m68000_get_reg(M68K_PC));
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
	uint8_t v;

	addr &= 0x00ffffff;
	if (addr < 0x00c00000) /* Use RAM upto 12MB */
		v = MEM[addr ^ 1];
	else if (addr < 0x00e00000)
		v = GVRAM_Read(addr);
	else
		v = MemReadTable[(addr >> 13) & 0xff](addr);

	if (webx68k_trace_enabled)
		webx68k_trace_record(m68000_get_reg(M68K_PC));
	/* 既定(両方無効)では m68000_get_reg() すら呼ばない。ここは全メモリ
	 * アクセスが通るため、無効時に関数呼び出しを増やすとエミュレータの
	 * 速度そのものが変わり、既存の計測値と条件が揃わなくなる。 */
	if (webx68k_drv_hook_strategy >= 0 || webx68k_drv_hook_interrupt >= 0)
		webx68k_drv_hook_check(m68000_get_reg(M68K_PC));

	if (webx68k_mem_read_watch_lo <= webx68k_mem_read_watch_hi)
		webx68k_mem_read_watch_check(addr, v);

	return v;
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
