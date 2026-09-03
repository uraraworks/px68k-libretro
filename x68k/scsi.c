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
#include "sram.h"

/* SCSI のセクタI/O(ホスト側)。実体は WebX68k の src/core-shim.c。
 * 決定2 により emscripten のファイルシステムは経由しない。 */
extern int webx68k_scsi_get_size(void);
extern int webx68k_scsi_init_d2(void);
extern int webx68k_scsi_init_a4(void);
extern int webx68k_scsi_drv_attr(void);
extern int webx68k_scsi_sram_init(void);
/* ドライバをゲストRAMへ置く経路(Human68kの門1/門2を満たすため)。
 * 実体は WebX68k の src/core-shim.c。既定はどちらも0(=無効)。 */
extern unsigned int webx68k_scsi_drv_ram(void);
extern unsigned int webx68k_scsi_drv_ram_from(void);
/* 初期化コマンド($00)への返答値。再ビルドせずに振れるよう JS 側から読む。
 * 意味の確定していない欄の切り分け用。既定値は元のハードコード値と同じ。 */
extern int webx68k_scsi_reply_err(void);
extern int webx68k_scsi_reply_units(void);
extern int webx68k_scsi_reply_end(void);
extern int webx68k_scsi_reply_bpb(void);
extern int webx68k_scsi_reply_status(void);
extern int webx68k_scsi_read_sector(unsigned int lba, unsigned char *buf);
/* ホストコマンド処理(ストラテジ$40/インタラプト$41)の最後に設定する d0。
 * 既定 -1(何もしない)。呼び出し時の値のまま戻す従来の挙動と同じ。 */
extern int webx68k_scsi_reply_d0(void);
extern int webx68k_scsi_reply_init_once(void);
/* デバイスドライバヘッダ +$00(次のヘッダ)に書く値。既定 $ffffffff
 * (このドライバで最後、従来と同じ)。 */
extern unsigned int webx68k_scsi_drv_next(void);
/* 本物の外部SCSIボードROMイメージ(8192バイト)の差し替え経路。
 * webx68k_scsi_rom_len() が $2000 のときだけ本物ROMとして扱う。
 * 0 のときは従来どおり自前スタブを使う。逆アセンブルはせず、
 * 本物ROMを走らせて挙動を実測するためのオラクルとして使う。 */
extern int webx68k_scsi_rom_len(void);
extern int webx68k_scsi_rom_byte(int i);

/* SPC(MB89352)のセレクト応答をホストから振るための欄。本物ROM使用時のみ使う。
 * $ea0001+2n というレジスタの当てはめは実測ではなく知識であり未実測
 * (SCSI_SpcRegIndex/SCSI_SpcSelectCheck のコメントも参照)。 */
extern int webx68k_scsi_spc_ints_sel(void);
extern int webx68k_scsi_spc_ints_timeout(void);
/* セレクトに応答するSCSI IDに相当するTEMP($ea0017)値(既定-1)。
 * TEMPの値がこれと一致したときだけセレクト成功にする(応答する相手を1つに絞る)。
 * 負値(既定の-1)は「どのTEMP値でも成功させる」の意味。
 * 2026-09-02実測: ROM内蔵ルーチンはTEMP=$07を、RAM上へ転送されて動くルーチン
 * (pc=$0001cd8c)はTEMP=$0fを使う。TEMPが表す意味(SCSI ID?コマンド種別?)は
 * 未確定。$07固定だとRAM側が延々セレクトを再試行し(実測で約14,800回)6分超
 * かかる/$0f固定だとROM側が通らないため、両方に応答する必要があり既定を
 * -1(どれでも成功)にした。 core-shim.c の js_scsi_spc_target 参照。 */
extern int webx68k_scsi_spc_target(void);
extern int webx68k_scsi_spc_ssts(void);
extern int webx68k_scsi_spc_psns(void);
/* 掃引モード(-2)での開始値。本物ROMは1回の起動でPSNS/SSTSを16回程度しか
 * 読まない(1ターゲットにつき2回試して諦める)ため、1回の実行では
 * 0〜15付近しか試せない。開始値をずらして複数回実行すれば全256値を
 * 試せる。既定0(従来どおり0から)。詳細は SCSI_SpcSweepRead 参照。 */
extern int webx68k_scsi_spc_psns_base(void);
extern int webx68k_scsi_spc_ssts_base(void);
/* PSNSの「交互」モード(-3)でのA/B値。既定 $8a / $0a。
 * SCSI_SpcSweepRead 冒頭のコメント参照。 */
extern int webx68k_scsi_spc_psns_a(void);
extern int webx68k_scsi_spc_psns_b(void);
/* PCTL($ea0011)書き込みでSSTSのbit7を落とすかどうか(既定1=落とす)。
 * 実測に基づく仕様ではなく実験的な規則。詳細は SCSI_SpcWrite コメント参照。 */
extern int webx68k_scsi_spc_clear_on_pctl(void);
/* SPCの転送状態機械(COMMAND/DATAIN/STATUS/MSGIN)用。webx68k_scsi_spc_psns()が
 * 既定(-1)のときだけ働く。詳細は下の SCSI_SpcSetPhase 等のコメント参照。 */
extern int webx68k_scsi_spc_phase_bits(void);
extern int webx68k_scsi_spc_ints_xfer(void);
extern int webx68k_scsi_spc_ints_disc(void);
/* CDBをDREGでなくTEMP($ea0017)経由で受け取る仮説の有効/無効。既定1(有効)。
 * 2026-09-02の実測(ROMがDREGに一切書かずTEMP経由に見える並びを繰り返した)を
 * 受けた未実測の仮説。詳細は SCSI_SpcXferStart のコメント参照。 */
extern int webx68k_scsi_spc_cdb_from_temp(void);
/* DATAIN中に渡すべきバイトが残っている間、SSTSへ立てるビット。既定 $08。
 * 実測(2026-09-02): READ CAPACITY応答(DATAIN)の直前にROMがTC(TCH=$ea0019/
 * TCM=$ea001b/TCL=$ea001d)=8を書き、SCMD上位3bit=100($80)を書いたあと
 * pc=$ea13de でSSTS($ea000d)を95回ポーリングし続けた。ROMが「データが
 * 来た」を示すビットを待っていると読み、そのビットを立てる/落とす当てはめ。
 * bit7(接続中、既存のSCSI_SpcSstsSetBit7Reason)とは別にORする。
 * 値はwebx68k_scsi_spc_ssts_data_bit()で再ビルドせず振れる(正解探索用)。 */
extern int webx68k_scsi_spc_ssts_data_bit(void);
/* TCが0(=渡すべきバイトを渡し切った)のとき立てる当てはめのビット。既定$10。
 * 実測(2026-09-02): データビット単体のパルス化だけでは2コマンド目で止まり、
 * 掃引(-2)で抜けた瞬間の値が$b0(=$80|$20|$10)だったことから、待ちが2種類
 * (データ用意済み/TC=0)あり1ビットでは両方を満たせないと判断した。
 * こちらはパルスではなく、TCが残っている間は落とし0になったら立てたまま
 * にする。値はwebx68k_scsi_spc_ssts_tc0_bit()で再ビルドせず振れる。 */
extern int webx68k_scsi_spc_ssts_tc0_bit(void);

/*
 * ホスト設定のフレーム単位キャッシュ (2026-09-02 追加)。
 *
 * 背景(実測): 本物SCSI ROM使用時、上のwebx68k_scsi_*() 群(EM_JS=wasm→JSの
 * 往復)をSPCレジスタへのアクセスのたびに直接呼んでいたところ、ROMが
 * レジスタを何十万回もポーリングするため往復回数が桁違いに増え、
 * 基準(本物ROM無し・38秒起動)に対し本物ROM+SPC状態機械で568秒かけても
 * 起動しないところまで悪化した。
 *
 * 対策: これらの設定値は「ホストがどう振るか」という設定であって
 * ゲスト実行中に頻繁に変わるものではないため、フレーム単位(1/60秒に1回)
 * でまとめて読み直し、以降はこの static へのアクセスだけで済ませる。
 * 取り込みは SCSI_RefreshHostConfig() が行い、SCSI_Init() で最低1回、
 * 以降は retro_run から毎フレーム(SCSI_LogPcIfRealRom() の隣)呼ぶ。
 *
 * 【重要】キャッシュ対象の一覧(新しい設定を webx68k_scsi_*() 側に足す
 * ときは、SPC関連・ログ関連([SCSI-BUS]の上限類)・返答関連のいずれかで
 * あれば必ずここにも足すこと。ここに足し忘れると「設定を変えても
 * 効かない」という分かりにくい不具合になる。取りこぼしのまま
 * アクセスごとに直接呼ぶのは新設せず、必ずこのキャッシュ経由にする):
 *   SPC関連:    spc_ints_sel, spc_ints_timeout, spc_target, spc_ssts,
 *               spc_psns, spc_psns_base, spc_ssts_base, spc_psns_a,
 *               spc_psns_b, spc_clear_on_pctl, spc_phase_bits,
 *               spc_ints_xfer, spc_ints_disc, spc_cdb_from_temp,
 *               spc_ssts_data_bit, spc_ssts_tc0_bit
 *   ログ関連:   bus_log_max, bus_pc_limit
 *   返答関連:   reply_err, reply_units, reply_end, reply_bpb,
 *               reply_status, reply_d0, reply_init_once
 *
 * 対象外(キャッシュしない): get_size/read_sector(可変引数または
 * ディスク差し替えに追随させたいため)、init_d2/init_a4/drv_attr/
 * sram_init/drv_next/rom_len/rom_byte(いずれもSCSI_Init内でしか
 * 読まない一回きりの値のため)。
 */
static int SCSIHostSpcIntsSel;
static int SCSIHostSpcIntsTimeout;
static int SCSIHostSpcTarget;
static int SCSIHostSpcSsts;
static int SCSIHostSpcPsns;
static int SCSIHostSpcPsnsBase;
static int SCSIHostSpcSstsBase;
static int SCSIHostSpcPsnsA;
static int SCSIHostSpcPsnsB;
static int SCSIHostSpcClearOnPctl;
static int SCSIHostSpcPhaseBits;
static int SCSIHostSpcIntsXfer;
static int SCSIHostSpcIntsDisc;
static int SCSIHostSpcCdbFromTemp;
static int SCSIHostSpcSstsDataBit;
static int SCSIHostSpcSstsTc0Bit;
static int SCSIHostBusLogMax;
static int SCSIHostBusPcLimit;
static int SCSIHostReplyErr;
static int SCSIHostReplyUnits;
static uint32_t SCSIHostReplyEnd;
static uint32_t SCSIHostReplyBpb;
static int SCSIHostReplyStatus;
static int SCSIHostReplyD0;
static int SCSIHostReplyInitOnce;

/* drv_attr/drv_next はキャッシュ対象外(SCSI_Init内でしか読まない一回きりの
 * 値)だが、起動時の陽性対照ログ(SCSI_RefreshHostConfig内)に含めるために
 * SCSI_Init が読んだ値をここへ控えておく。本物ROM使用中はその区画の書き込み
 * 自体を飛ばすため、区別できるよう既定値をあり得ない印(drv_attrは書かれない
 * ことを示す負数、drv_nextは0)にしておく。 */
static int SCSIInitDrvAttrForLog = -1;
static int SCSIInitDrvNextForLog = 0;

/* このフレームで一度も取り込んでいない(=SCSI_Init直後にまだ
 * SCSI_RefreshHostConfigを呼んでいない)ことの検出用。 */
static int SCSIHostConfigLoaded = 0;

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

static int SCSIEntryCallCount = 0;
static int SCSIZeroCallCount = 0;

/* 本物ROM使用中フラグ。true のとき、自前スタブ前提の処理
 * (d2/a4の即値差し替え・観測用テーブル/スタブの書き込み・$ea0110の窓selftest・
 * BPB表ポインタ)を飛ばし、窓への書き込みも SCSIIPL へ反映しない(ROMのまま)。 */
static int SCSIUsingRealRom = 0;

/* SCSIイメージが無い/読めないときに通常起動を巻き込まないためのガード。
 * SCSI_Init の先頭で必ず0へ戻し、BPBを基準器イメージから写せたとき
 * (have_bpb が立ったとき)だけ1にする。0のままなら、ベクタ設定エントリの
 * a4/d2 は JS 側の設定に関わらず「ドライバ無し」相当の値を強制し、
 * SCSI_HostCommand() のRAMへドライバを組み立てる処理も丸ごと飛ばす。 */
static int SCSIImageReady = 0;

/* 本物ROM使用時のみ、ゲストPCを定期的にログへ出す診断
 * ([SCSI-PC])。retro_run から毎フレーム呼ばれる想定。
 * 60フレームに1回サンプルし、上限40行で止める。 */
static int SCSIPcLogFrameCount = 0;
static int SCSIPcLogCount = 0;

/* 前方宣言: [SCSI-BUS] の保留中エントリを増分だけ「(継続中)」付きで
 * 吐き出す(詳細は定義箇所のコメント参照)。SCSI_LogPcIfRealRom() から
 * 呼ぶためここで宣言する(定義は本ファイル後方の SCSI_BusLog 関連の
 * すぐ後ろ)。 */
static void SCSI_BusLogFlushPeriodic(void);

/*
 * 保留中の圧縮ログ(「変化したときにまとめて出す」方式)は、ゲストが
 * 同じ値を延々ポーリングしたまま実行が終わると1行も出ない。そのせいで
 * 「アクセスが止まった」「一度も読まれていない」と誤読する事故が
 * 2026-09-02に計4回発生した([SCSI-BUS]/[SCSI-MEMR]/[SCSI-RAM]を跨いで)。
 * 対策として、保留中エントリを定期的に(このSCSI_LogPcIfRealRom経由、
 * 60フレームに1回)「(継続中)」付きで吐き出す。件数は累積でなく前回
 * 出力からの増分。[SCSI-RAM]の書き込みログは元々1件ごとに即時出力で
 * 保留を持たないため、この定期フラッシュの対象外(沈黙の心配がない)。
 */
void SCSI_LogPcIfRealRom(void)
{
	if (!SCSIUsingRealRom)
		return;
	SCSIPcLogFrameCount++;
	if (SCSIPcLogFrameCount % 60 != 0)
		return;
	if (SCSIPcLogCount < 40)
	{
		if (log_cb)
			log_cb(RETRO_LOG_INFO, "[SCSI-PC] frame=%d pc=$%08x\n",
				SCSIPcLogFrameCount, (unsigned)m68000_get_reg(M68K_PC));
		SCSIPcLogCount++;
	}
	/* [SCSI-PC]の40行上限とは無関係に、保留中の圧縮ログは60フレーム
	 * ごとに毎回吐き出す(沈黙のほうが遥かに問題であるため)。 */
	SCSI_BusLogFlushPeriodic();
	webx68k_mem_read_watch_flush_periodic();
}

/* SPC(MB89352)レジスタの簡易状態(本物ROM使用時のみ有効)。
 * 詳細・実測/未実測の区別は SCSI_SpcRegIndex 付近のコメントを参照。 */
#define SCSI_SPC_REG_COUNT 16
static uint8_t SCSISpcReg[SCSI_SPC_REG_COUNT];

/* $ea0000〜$ea1fff 全域への読み書きの統合ログ([SCSI-BUS])。
 * 同じ(番地,種別,PC)が連続するときは数えるだけにし、変化した時点で
 * まとめて1行出す(「×N回」)。上限4000件で、達したらその旨を1回だけ出す。
 * この上限(SCSI_BusLogGate)は [SCSI-SPC] の追加診断ログとも共通で、
 * SPC域も例外なくここでカウントされる(詳細は SCSI_BusLogGate の
 * コメントを参照)。
 * 既定値はホストから webx68k_scsi_bus_log_max() で上書きできる
 * (既定 4000、詳細は core-shim.c の globalThis.__webx68kBusLogMax)。 */
#define SCSI_BUS_LOG_MAX_DEFAULT    4000
extern int webx68k_scsi_bus_log_max(void);
static int SCSIBusLogCount = 0;
static int SCSIBusLogCapped = 0;
/* [SCSI-BUS]ログの早期脱出(SCSI_BusLogShouldSkip、定義は本ファイル後方の
 * SCSI_Write直前)が一度案内済みかどうか。SCSI_Init()で起動のたびに戻す。 */
static int SCSIBusLogFastPathAnnounced = 0;
static uint32_t SCSIBusLastAddr = 0;
static int SCSIBusLastIsWrite = -1;	/* -1 = 保留中の記録なし */
static uint32_t SCSIBusLastPC = 0;
static uint8_t SCSIBusLastValue = 0;
static int SCSIBusRunCount = 0;
/* 保留中エントリのうち、既に(確定出力または定期フラッシュで)出力済みの件数。
 * 「件数は累積でなく前回出力からの増分」にするための基準線。
 * 新しい保留エントリを開始するたび(SCSI_BusLogFlushの末尾)に0へ戻す。 */
static int SCSIBusRunReported = 0;

/* SPC(SCSIコントローラ)のポート領域。ROMがSPCをどう叩くかを見るのが
 * 今回の観測目的そのものなので、下の命令フェッチ除外heuristicに関わらず
 * この範囲への読み書きは必ずログへ出す。ただし出力件数の上限
 * (SCSI_BUS_LOG_MAX)自体は除外判定と無関係に全域で共通に効く。 */
#define SCSI_SPC_PORT_LO    0x00ea0000
#define SCSI_SPC_PORT_HI    0x00ea0020	/* 排他的上限 ($ea001f まで) */

/* 命令フェッチとみなして除外した件数、および除外報告を済ませたかどうか。 */
static int SCSIBusFetchExcluded = 0;
static int SCSIBusFetchExcludedReported = 0;

/* 除外件数の報告(最後、またはログ上限到達時に1回だけ)。 */
static void SCSI_BusReportExcluded(void)
{
	if (SCSIBusFetchExcludedReported)
		return;
	SCSIBusFetchExcludedReported = 1;
	if (SCSIBusFetchExcluded > 0 && log_cb)
		log_cb(RETRO_LOG_INFO, "[SCSI-BUS] 命令フェッチとみなして除外: %d件\n", SCSIBusFetchExcluded);
}

/* SCSI_BUS_LOG_MAX の上限は [SCSI-BUS] の通常行だけでなく、SPC域の
 * 追加診断ログ([SCSI-SPC] のセレクト応答・INTSクリアなど)にも共通で
 * 適用する。「SPC域はフェッチ除外heuristicの対象外」というのは
 * SCSI_BusLog 内の除外判定だけの話であり、上限そのものは全ログで
 * 共有する(そうしないと本物ROMのセレクトやり直しループでSPCの
 * 追加診断行だけが無制限に出て34MB級のログになる、という実測不具合
 * があった)。ログを出す全箇所はこの関数を通すこと。 */
static void SCSI_BusReportPcDropped(void);

static int SCSI_BusLogGate(void)
{
	int log_max;
	if (SCSIBusLogCapped)
		return 0;
	/* JS側(core-shim.c js_scsi_bus_log_max)が未設定時に既定
	 * SCSI_BUS_LOG_MAX_DEFAULT を返すので、ここでは素直に使う。 */
	log_max = SCSIHostBusLogMax;
	if (SCSIBusLogCount >= log_max)
	{
		SCSIBusLogCapped = 1;
		if (log_cb)
			log_cb(RETRO_LOG_INFO, "[SCSI-BUS] ログ上限(%d件)に達したため以降は出力を止める\n", log_max);
		SCSI_BusReportExcluded();
		SCSI_BusReportPcDropped();
		return 0;
	}
	SCSIBusLogCount++;
	return 1;
}

/* 同じPC(命令アドレス)からのログ対象アクセスが通算で閾値を超えたら、
 * そのPCからは以後ログを出さず件数だけ数え続ける。
 *
 * 採った圧縮方式: 実測では本物ROMが $ea0009(INTS)→$ea0005(SCMD)→…と
 * 数命令おきに複数レジスタを巡回するため、直前1件だけを見る既存の
 * run-length圧縮(SCSIBusRunCount)では捕まらない(「直近16件の並び一致」
 * を検出する案も検討したが、リングバッファと部分列比較が要り実装が
 * 複雑になるため見送った)。代わりに「同じPCからの通算件数が閾値を
 * 超えたらそのPC以降は出さない」という単純な方式を採用する。
 * PCはハッシュ表を使わず線形探索(想定エントリ数は数個程度)。
 * テーブルが埋まった場合(想定外に多種のPCが来た場合)は制限せず
 * 常に許可する(安全側に倒す)。
 *
 * この閾値(既定32)はホストから webx68k_scsi_bus_pc_limit() で
 * 上書きできる(globalThis.__webx68kBusPcLimit)。0 を渡すと
 * 「抑制しない=全件出す」になる(セレクト成功後にROMが何をしているかを
 * 正確に追いたい場面向け)。抑制で落とした件数はPCごとに数え続けており、
 * SCSI_BusReportPcDropped() でログ上限到達時・SCSI_Cleanup時に
 * 上位10件だけまとめて出す(「0件」と「抑制されて0行」を区別するため、
 * 抑制が一度も発動していなければこのログ自体を出さない)。 */
#define SCSI_BUS_PC_TRACK          64
#define SCSI_BUS_PC_THRESHOLD_DEFAULT  32
extern int webx68k_scsi_bus_pc_limit(void);
static uint32_t SCSIBusPcTrackPc[SCSI_BUS_PC_TRACK];
static int SCSIBusPcTrackCount[SCSI_BUS_PC_TRACK];
static int SCSIBusPcTrackUsed = 0;

static int SCSI_BusPcAllow(uint32_t pc)
{
	int i;
	int limit = SCSIHostBusPcLimit;
	if (limit <= 0)
		return 1;	/* 0以下(既定は32) = 抑制しない */
	for (i = 0; i < SCSIBusPcTrackUsed; i++)
	{
		if (SCSIBusPcTrackPc[i] == pc)
		{
			SCSIBusPcTrackCount[i]++;
			if (SCSIBusPcTrackCount[i] == limit + 1)
			{
				if (log_cb)
					log_cb(RETRO_LOG_INFO,
						"[SCSI-BUS] pc=$%08x からのログ対象アクセスが通算%d件を超えたため、"
						"以後このPCからのログを止める(件数のみ数え続ける)\n",
						(unsigned)pc, limit);
			}
			return SCSIBusPcTrackCount[i] <= limit;
		}
	}
	if (SCSIBusPcTrackUsed < SCSI_BUS_PC_TRACK)
	{
		SCSIBusPcTrackPc[SCSIBusPcTrackUsed] = pc;
		SCSIBusPcTrackCount[SCSIBusPcTrackUsed] = 1;
		SCSIBusPcTrackUsed++;
	}
	return 1;
}

/* 上のキャッシュ static 群を webx68k_scsi_*() から一括で読み直す。
 * SCSI_Init() で最低1回、以降は retro_run から毎フレーム
 * (SCSI_LogPcIfRealRom() の隣で)呼ぶ想定。
 *
 * 【2026-09-02 実測で判明した事故】かつては「本物ROM未使用時はこれらの
 * 値を一切参照しないはず」という思い込みで、この関数の先頭に
 * `if (!SCSIUsingRealRom) return;` を置いていた。ところが reply_* 群
 * (SCSI_HandleRequestHeader、自前スタブの d2 テーブル経由でも使う)は
 * SCSIUsingRealRom の真偽に関わらず参照されており、自前スタブ経路では
 * このガードのせいで一度もキャッシュへ取り込まれず、C の static 既定値
 * (=0)のまま固定されていた(JS側のEM_JS既定値 units=1/end=$00ea0500/
 * bpb=$00ea0600/status=-1/d0=-1 は一切効かない)。呼び出し側に
 * --reply-units 等を渡しても「unit数=0 終了addr=$00000000
 * BPB表ptr=$00000000」のまま変化しないという分かりにくい不具合になった。
 * この関数はもともと1/60秒に1回しか呼ばれない(高頻度なのはSPCレジスタへの
 * 個々のアクセスであって、この関数自体の呼び出し回数ではない)ため、
 * 自前スタブ経路でも毎フレーム全項目読み直すコストは無視できる。
 * よって早期returnはやめ、SCSIUsingRealRomの真偽に関わらず常に全項目を
 * 読み直す。
 *
 * 【重要】設定を1つ足すたびに、このキャッシュへの読み込みと、下の
 * 陽性対照ログの両方に足すこと。片方を忘れると「設定したのに効かない」
 * が無言で起きる(2026-09-02に発生)。 */
void SCSI_RefreshHostConfig(void)
{
	SCSIHostSpcIntsSel = webx68k_scsi_spc_ints_sel();
	SCSIHostSpcIntsTimeout = webx68k_scsi_spc_ints_timeout();
	SCSIHostSpcTarget = webx68k_scsi_spc_target();
	SCSIHostSpcSsts = webx68k_scsi_spc_ssts();
	SCSIHostSpcPsns = webx68k_scsi_spc_psns();
	SCSIHostSpcPsnsBase = webx68k_scsi_spc_psns_base();
	SCSIHostSpcSstsBase = webx68k_scsi_spc_ssts_base();
	SCSIHostSpcPsnsA = webx68k_scsi_spc_psns_a();
	SCSIHostSpcPsnsB = webx68k_scsi_spc_psns_b();
	SCSIHostSpcClearOnPctl = webx68k_scsi_spc_clear_on_pctl();
	SCSIHostSpcPhaseBits = webx68k_scsi_spc_phase_bits();
	SCSIHostSpcIntsXfer = webx68k_scsi_spc_ints_xfer();
	SCSIHostSpcIntsDisc = webx68k_scsi_spc_ints_disc();
	SCSIHostSpcCdbFromTemp = webx68k_scsi_spc_cdb_from_temp();
	SCSIHostSpcSstsDataBit = webx68k_scsi_spc_ssts_data_bit();
	SCSIHostSpcSstsTc0Bit = webx68k_scsi_spc_ssts_tc0_bit();
	SCSIHostBusLogMax = webx68k_scsi_bus_log_max();
	SCSIHostBusPcLimit = webx68k_scsi_bus_pc_limit();
	SCSIHostReplyErr = webx68k_scsi_reply_err();
	SCSIHostReplyUnits = webx68k_scsi_reply_units();
	SCSIHostReplyEnd = (uint32_t)webx68k_scsi_reply_end();
	SCSIHostReplyBpb = (uint32_t)webx68k_scsi_reply_bpb();
	SCSIHostReplyStatus = webx68k_scsi_reply_status();
	SCSIHostReplyD0 = webx68k_scsi_reply_d0();
	SCSIHostReplyInitOnce = webx68k_scsi_reply_init_once();

	/* 陽性対照: キャッシュ後も設定が本当に効いているかを1行で確認できる
	 * ようにする。既定値のままならホストからの指定が届いていない
	 * (取り込みそのものが壊れている、または呼び忘れ)と分かる。
	 * 返答系(reply_*)とdrv_attr/drv_nextも必ずここに含める。
	 * 本物ROM使用中・未使用中(自前スタブ)のどちらでも必ず出す
	 * (SCSIUsingRealRomによる出し分けはしない。2026-09-02、自前スタブ
	 * 経路だけこの行自体が出ない状態になっていたことがある)。 */
	if (!SCSIHostConfigLoaded && log_cb)
	{
		/* target は負値(既定-1=「どのTEMPでも成功」)を取りうるので $%02x で
		 * マスクすると $ff 等に化けて見えなくなる。10進のまま出す。
		 * drv_attr/drv_next は本物ROM使用中は SCSI_Init がその区画自体を
		 * 書かないため、既定の印(attr=-1, next=0)がそのまま出る。 */
		int spc_target = SCSIHostSpcTarget;
		log_cb(RETRO_LOG_INFO,
			"[SCSI] ホスト設定キャッシュ読み込み: ints_sel=$%02x ssts_data_bit=%d target=%d clear_on_pctl=%d"
			" | reply_err=$%02x reply_units=%d reply_end=$%08x reply_bpb=$%08x reply_status=%d reply_d0=%d reply_init_once=%d"
			" drv_attr=$%04x drv_next=$%08x (本物ROM使用中=%d)\n",
			(unsigned)(SCSIHostSpcIntsSel & 0xff), SCSIHostSpcSstsDataBit, spc_target, SCSIHostSpcClearOnPctl,
			(unsigned)(SCSIHostReplyErr & 0xff), SCSIHostReplyUnits,
			(unsigned)SCSIHostReplyEnd, (unsigned)SCSIHostReplyBpb,
			SCSIHostReplyStatus, SCSIHostReplyD0, SCSIHostReplyInitOnce,
			(unsigned)(SCSIInitDrvAttrForLog & 0xffff), (unsigned)SCSIInitDrvNextForLog,
			SCSIUsingRealRom);
	}
	SCSIHostConfigLoaded = 1;
}

/* SCSI_BusPcAllow の抑制で落とした件数を、PCごとに上位10件だけ
 * まとめて出す(SCSI_Cleanup、またはログ上限到達時に1回だけ)。
 * 「抑制が一度も発動していない」場合はこのログ自体を出さないことで、
 * 0件だったのか抑制されて何も見えていないのかを区別できるようにする。 */
static int SCSIBusPcDroppedReported = 0;

static void SCSI_BusReportPcDropped(void)
{
	int limit;
	int i, j, n;
	uint32_t top_pc[10];
	int top_dropped[10];

	if (SCSIBusPcDroppedReported)
		return;
	SCSIBusPcDroppedReported = 1;

	if (!log_cb)
		return;
	limit = SCSIHostBusPcLimit;
	if (limit <= 0)
	{
		log_cb(RETRO_LOG_INFO, "[SCSI-BUS] PC別抑制: 無効(webx68kBusPcLimit=0につき全件出力)\n");
		return;
	}

	n = 0;
	for (i = 0; i < SCSIBusPcTrackUsed; i++)
	{
		int dropped = SCSIBusPcTrackCount[i] - limit;
		int pos;
		if (dropped <= 0)
			continue;
		pos = n < 10 ? n : 9;
		if (n < 10)
			n++;
		else if (dropped <= top_dropped[9])
			continue;
		for (j = pos; j > 0 && top_dropped[j - 1] < dropped; j--)
		{
			top_dropped[j] = top_dropped[j - 1];
			top_pc[j] = top_pc[j - 1];
		}
		top_dropped[j] = dropped;
		top_pc[j] = SCSIBusPcTrackPc[i];
	}

	if (n == 0)
	{
		log_cb(RETRO_LOG_INFO,
			"[SCSI-BUS] PC別抑制件数: 0件(閾値%d件を超えたPCなし。抑制は一度も発動していない)\n",
			limit);
		return;
	}

	log_cb(RETRO_LOG_INFO, "[SCSI-BUS] PC別抑制件数(上位%d件、閾値%d件):\n", n, limit);
	for (i = 0; i < n; i++)
		log_cb(RETRO_LOG_INFO, "[SCSI-BUS]   pc=$%08x dropped=%d件\n",
			(unsigned)top_pc[i], top_dropped[i]);
}

/* ストラテジ(ホストコマンド $40)呼び出し時に a5 で渡された要求ヘッダの
 * アドレスを控えておく。0 = 未取得。インタラプト($41)側で使う。 */
static uint32_t SCSIReqHeaderAddr = 0;

/* ドライバをゲストRAMへ置いたときの「終了アドレス」(base + $34)。
 * 0以外のとき、初期化コマンド($00)応答はこちらを優先し SCSIHostReplyEnd を無視する。
 * Human68kの門1/門2(ファイル先頭コメント参照)を同時に満たすための値。
 * SCSI_Init() で起動のたびに0へ戻す。 */
static uint32_t SCSIDrvRamEnd = 0;

/* 要求ヘッダのコマンド$00(初期化)を処理した回数。 */
static int SCSIReqInitCount = 0;
static int SCSIVectorEntryCount = 0;
/* 直近の要求ヘッダ処理で、インタラプトから戻る d0 に入れたい値。
 * 負なら「触らない」。Human68k のデバイスドライバはエラーを d0 で
 * 返す規約とみられるため、コマンド単位で指定できるようにする
 * (ホスト設定 reply_d0 とは別。こちらが優先)。 */
static int SCSIReqReplyD0 = -1;

/* d2 で渡す観測用テーブル/スタブの呼び出し回数 ($40〜$7f, 64要素) */
#define SCSI_TABLE_ENTRIES 64
static int SCSITableCallCount[SCSI_TABLE_ENTRIES];

/* 保留中の連続アクセス記録を、前回出力からの増分だけ1行にまとめて確定
 * 出力する(番地/PC/種別が変わった時、またはSCSI_Cleanup時)。保留は空にする。
 * periodic!=0 のときは確定させず(保留を残したまま)「(継続中)」付きで
 * 増分だけ出す定期フラッシュとして働く(SCSI_BusLogFlushPeriodo参照)。 */
static void SCSI_BusLogFlushCommon(int periodic)
{
	int inc;

	if (SCSIBusLastIsWrite < 0)
		return;

	inc = SCSIBusRunCount - SCSIBusRunReported;
	if (inc > 0 && SCSI_BusPcAllow(SCSIBusLastPC) && SCSI_BusLogGate())
	{
		if (log_cb)
		{
			if (inc <= 1)
				log_cb(RETRO_LOG_INFO,
					periodic ? "[SCSI-BUS] #%d %s adr=$%06x data=$%02x pc=$%08x (継続中)\n"
					         : "[SCSI-BUS] #%d %s adr=$%06x data=$%02x pc=$%08x\n",
					SCSIBusLogCount, SCSIBusLastIsWrite ? "W" : "R",
					(unsigned)SCSIBusLastAddr, SCSIBusLastValue, (unsigned)SCSIBusLastPC);
			else
				log_cb(RETRO_LOG_INFO,
					periodic ? "[SCSI-BUS] #%d %s adr=$%06x data=$%02x pc=$%08x ×%d回(継続中)\n"
					         : "[SCSI-BUS] #%d %s adr=$%06x data=$%02x pc=$%08x ×%d回\n",
					SCSIBusLogCount, SCSIBusLastIsWrite ? "W" : "R",
					(unsigned)SCSIBusLastAddr, SCSIBusLastValue, (unsigned)SCSIBusLastPC, inc);
		}
	}

	if (periodic)
		SCSIBusRunReported = SCSIBusRunCount;
	else
	{
		SCSIBusLastIsWrite = -1;
		SCSIBusRunCount = 0;
		SCSIBusRunReported = 0;
	}
}

static void SCSI_BusLogFlush(void)
{
	SCSI_BusLogFlushCommon(0);
}

/*
 * 保留中の[SCSI-BUS]エントリを、前回出力からの増分だけ「(継続中)」付きで
 * 吐き出す(保留自体はクリアしない=監視を継続する)。
 *
 * 目的: 「変化したときにまとめて出す」圧縮方式では、ゲストが同じ
 * (番地,種別,PC,値)を延々ポーリングしたまま実行が終わると1行も出ない。
 * そのせいで「アクセスが止まった」「一度も読まれていない」と誤読する
 * 事故が2026-09-02に計4回発生した。SCSI_LogPcIfRealRom()(60フレームに
 * 1回)・SCSI_Cleanup()(実行終了時)から呼び、沈黙を防ぐ。
 * 増分が0(前回から変化なし)なら何も出さない。
 */
static void SCSI_BusLogFlushPeriodic(void)
{
	SCSI_BusLogFlushCommon(1);
}

/* $ea0000〜$ea1fff 全域への読み書きを記録する。同じ(番地,種別,PC)が
 * 連続するときは数えるだけにし、変化した時点で直前の記録をまとめて出す。
 *
 * heuristic: 読み出し(is_write==0)で、SPCポート($ea0000〜$ea001f)以外かつ
 * 現在のPCから±8バイト以内の番地であれば「命令フェッチ」とみなして
 * カウントのみ行いログには出さない。px68k ではこの領域の命令フェッチも
 * SCSI_Read を通るため、フェッチが大半を占めてしまい観測目的(ROMが
 * SPCのポートをどう叩くか)がログに埋もれてしまうことへの対策。
 * あくまでアドレスとPCの近さだけで判定する経験則であり、実際に
 * その番地が命令として読まれたことを厳密に確認しているわけではない。
 * 書き込みは命令フェッチでは起こらないため対象外(常時ログに出す)。 */
static void SCSI_BusLog(uint32_t adr, int is_write, uint8_t data, uint32_t pc)
{
	if (!is_write && !(adr >= SCSI_SPC_PORT_LO && adr < SCSI_SPC_PORT_HI))
	{
		uint32_t diff = (adr > pc) ? (adr - pc) : (pc - adr);
		if (diff <= 8)
		{
			SCSIBusFetchExcluded++;
			return;
		}
	}

	if (SCSIBusLastIsWrite == is_write && SCSIBusLastAddr == adr && SCSIBusLastPC == pc)
	{
		SCSIBusRunCount++;
		SCSIBusLastValue = data;
		return;
	}
	SCSI_BusLogFlush();
	SCSIBusLastAddr = adr;
	SCSIBusLastIsWrite = is_write;
	SCSIBusLastPC = pc;
	SCSIBusLastValue = data;
	SCSIBusRunCount = 1;
}

/* コマンド$04(読み出し)で使う、BPBから読み取ったパーティション諸元。
 * SCSI_Init が BPB を写せたときだけ設定し、それ以外は0のままにして
 * SCSI_HandleRequestHeader 側で「まだBPB未確定」を検出できるようにする。 */
static uint32_t SCSIPartStartBlocks = 0;	/* パーティション開始(1024バイトブロック単位) */
static uint32_t SCSIPartSectorBytes = 0;	/* 1セクタのバイト数(BPBから) */

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
		/* d2 は「Human68k が後で呼ぶルーチンのアドレス」である(実測)。
		 * 元の px68k スタブは #-1(＝無し)を返していた。#0 を返したところ
		 * Human68k が $00000000 へ飛び「おかしな命令を実行しました
		 * (SR=$2009:PC=$00000000)」になったことで確定した。
		 * ここでは自前ルーチン($ea00a0)を渡し、呼ばれ方を観測する。 */
		0x28, 0x7c, 0x00, 0x00, 0x00, 0x00,	/* "movea.l #0, a4" 即値は SCSI_Init で差し替える。
											 * $00000000 へ飛んだ瞬間のレジスタダンプで a4 だけが
											 * 空だったため、a4 も初期化ルーチンの戻り値では
											 * ないかを試すための枠。既定 0 は元の状態と同じ。 */
		0x24, 0x3c, 0xff, 0xff, 0xff, 0xff,	/* "move.l #-1, d2" 即値は SCSI_Init で差し替える */
		0x4e, 0x75,							/* "rts" */
		/* $ea0096-$ea009f 詰め物 */
		0x00, 0x00, 0x00, 0x00, 0x00,
		0x00, 0x00, 0x00, 0x00, 0x00,
		/* $ea00a0 Human68k から呼ばれるルーチン(観測のみ) */
		0x13, 0xfc, 0x00, 0x05, 0x00, 0xe9, 0xf8, 0x02,	/* "move.b #$05, $e9f802" */
		0x4e, 0x75,							/* "rts" */
		/* $ea00aa-$ea00bf 詰め物 */
		0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
		0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
		/* $ea00c0 d2 で渡す構造体(仮)。どのオフセットが使われるか分からないので
		 * 16個ぶん全てを観測ルーチン($ea00a0)へ向けておく。 */
		0x00, 0xea, 0x00, 0xa0,
		0x00, 0xea, 0x00, 0xa0,
		0x00, 0xea, 0x00, 0xa0,
		0x00, 0xea, 0x00, 0xa0,
		0x00, 0xea, 0x00, 0xa0,
		0x00, 0xea, 0x00, 0xa0,
		0x00, 0xea, 0x00, 0xa0,
		0x00, 0xea, 0x00, 0xa0,
		0x00, 0xea, 0x00, 0xa0,
		0x00, 0xea, 0x00, 0xa0,
		0x00, 0xea, 0x00, 0xa0,
		0x00, 0xea, 0x00, 0xa0,
		0x00, 0xea, 0x00, 0xa0,
		0x00, 0xea, 0x00, 0xa0,
		0x00, 0xea, 0x00, 0xa0,
		0x00, 0xea, 0x00, 0xa0,
	};
	int i;
	uint8_t tmp;
	int rom_len;
	SCSIIOCSLogCount = 0;
	SCSIEntryCallCount = 0;
	SCSIZeroCallCount = 0;
	SCSIBusLogCount = 0;
	SCSIBusLogCapped = 0;
	SCSIBusLogFastPathAnnounced = 0;
	SCSIBusLastIsWrite = -1;
	SCSIBusRunCount = 0;
	SCSIBusPcTrackUsed = 0;
	SCSIBusFetchExcludedReported = 0;
	SCSIBusFetchExcluded = 0;
	SCSIBusPcDroppedReported = 0;
	SCSIUsingRealRom = 0;
	SCSIImageReady = 0;
	SCSIHostConfigLoaded = 0;	/* 起動のたびに陽性対照ログを出し直す */
	SCSIInitDrvAttrForLog = -1;	/* 本物ROM使用中はこの区画を書かないため既定の印に戻す */
	SCSIInitDrvNextForLog = 0;
	SCSIReqHeaderAddr = 0;
	SCSIReqInitCount = 0;
	SCSIVectorEntryCount = 0;
	SCSIDrvRamEnd = 0;
	SCSIPartStartBlocks = 0;
	SCSIPartSectorBytes = 0;
	memset(SCSITableCallCount, 0, sizeof(SCSITableCallCount));
	memset(SCSIIPL, 0, 0x2000);
	memset(SCSISpcReg, 0, sizeof(SCSISpcReg));

	/* 本物の外部SCSIボードROMイメージ(8192バイト)が流し込まれていれば、
	 * 自前スタブの代わりにそれを使う。逆アセンブルはせず、本物を走らせて
	 * 挙動を実測するためのオラクルとして扱う。 */
	rom_len = webx68k_scsi_rom_len();
	if (rom_len == 0x2000)
	{
		int k;
		for (k = 0; k < 0x2000; k++)
		{
			int b = webx68k_scsi_rom_byte(k);
			SCSIIPL[k] = (b >= 0) ? (uint8_t)(b & 0xff) : 0x00;
		}
		SCSIUsingRealRom = 1;
		if (log_cb)
			log_cb(RETRO_LOG_INFO,
				"[SCSI] 本物のSCSI ROMイメージ(%d バイト)を読み込んだ。自前スタブの処理(d2/a4差し替え・観測用テーブル/スタブ・$ea0110窓selftest・BPB表ポインタ)を飛ばす\n",
				rom_len);
		/* SCSI_RefreshHostConfig() は本物ROM使用の有無に関わらず必要な
		 * ため(reply_*は自前スタブ経路でも参照する)、SCSI_Init末尾で
		 * SCSIUsingRealRom確定後にまとめて1回呼ぶ。ここでは呼ばない。 */
	}
	else
	{
		if (rom_len != 0 && log_cb)
			log_cb(RETRO_LOG_ERROR,
				"[SCSI] ROMサイズが不正 ($%04x, 期待 $2000)。自前スタブを使う\n", rom_len);
		memcpy(&SCSIIPL[0x20], SCSIIMG, sizeof(SCSIIMG));
	}

	/* d2/a4 で渡す窓 $ea0100 を、Human68k デバイスドライバヘッダの一般形
	 * (+0 次のヘッダ4 / +4 属性ワード2 / +6 ストラテジ4 / +10 インタラプト4 /
	 *  +14 名前8、以降未実測)で埋める。$ea0000〜のバイト入れ替えループより
	 * 前に、SCSIIMG と同じ自然なバイト順で書く。
	 * スタブは従来どおり $ea0200 から10バイト×64個: move.b #(0x40+k),$e9f802 / rts
	 * - stub[0] = ストラテジ、stub[1] = インタラプト
	 * - $ea0116〜$ea01ff は従来どおり4バイトのポインタ表とし、
	 *   要素は stub[2 + (オフセット-0x116)/4] を指す(未知のオフセットが
	 *   使われたら k で捕まえるため)。
	 * 本物ROM使用中はこの書き込み(自前スタブ前提)を丸ごと飛ばす。 */
	if (!SCSIUsingRealRom)
	{
		int k;
		uint32_t off_hdr   = 0x100;	/* SCSIIPL内オフセット ($ea0100) */
		uint32_t off_stub  = 0x200;	/* SCSIIPL内オフセット ($ea0200) */
		uint32_t off_table = 0x116;	/* SCSIIPL内オフセット ($ea0116) */
		uint32_t off;

		for (k = 0; k < SCSI_TABLE_ENTRIES; k++)
		{
			uint32_t sp = off_stub + (uint32_t)k * 10;
			SCSIIPL[sp + 0] = 0x13;
			SCSIIPL[sp + 1] = 0xfc;
			SCSIIPL[sp + 2] = 0x00;
			SCSIIPL[sp + 3] = (uint8_t)(0x40 + k);
			SCSIIPL[sp + 4] = 0x00;
			SCSIIPL[sp + 5] = 0xe9;
			SCSIIPL[sp + 6] = 0xf8;
			SCSIIPL[sp + 7] = 0x02;
			SCSIIPL[sp + 8] = 0x4e;
			SCSIIPL[sp + 9] = 0x75;
		}

		/* +$00 次のヘッダ。既定 $ffffffff(このドライバで最後、従来と同じ)。
		 * 意味が未確定のため、再ビルドせずに JS 側から振れるようにしてある。 */
		{
			uint32_t v = webx68k_scsi_drv_next();
			SCSIIPL[off_hdr + 0x00] = (uint8_t)((v >> 24) & 0xff);
			SCSIIPL[off_hdr + 0x01] = (uint8_t)((v >> 16) & 0xff);
			SCSIIPL[off_hdr + 0x02] = (uint8_t)((v >> 8) & 0xff);
			SCSIIPL[off_hdr + 0x03] = (uint8_t)(v & 0xff);
			/* 起動時の陽性対照ログ(SCSI_RefreshHostConfig)向けに控える。 */
			SCSIInitDrvNextForLog = (int)v;
		}

		/* +$04 属性ワード。値の意味が未確定のため、再ビルドせずに
		 * ホスト(JS)側から振れるようにしてある。既定 $0000。 */
		{
			int attr = webx68k_scsi_drv_attr();
			SCSIIPL[off_hdr + 0x04] = (uint8_t)((attr >> 8) & 0xff);
			SCSIIPL[off_hdr + 0x05] = (uint8_t)(attr & 0xff);
			/* 起動時の陽性対照ログ(SCSI_RefreshHostConfig)向けに控える。 */
			SCSIInitDrvAttrForLog = attr;
		}

		/* +$06 ストラテジ = stub[0] */
		{
			uint32_t v = 0x00ea0200 + 0u * 10u;
			SCSIIPL[off_hdr + 0x06] = (uint8_t)((v >> 24) & 0xff);
			SCSIIPL[off_hdr + 0x07] = (uint8_t)((v >> 16) & 0xff);
			SCSIIPL[off_hdr + 0x08] = (uint8_t)((v >> 8) & 0xff);
			SCSIIPL[off_hdr + 0x09] = (uint8_t)(v & 0xff);
		}
		/* +$0a インタラプト = stub[1] */
		{
			uint32_t v = 0x00ea0200 + 1u * 10u;
			SCSIIPL[off_hdr + 0x0a] = (uint8_t)((v >> 24) & 0xff);
			SCSIIPL[off_hdr + 0x0b] = (uint8_t)((v >> 16) & 0xff);
			SCSIIPL[off_hdr + 0x0c] = (uint8_t)((v >> 8) & 0xff);
			SCSIIPL[off_hdr + 0x0d] = (uint8_t)(v & 0xff);
		}
		/* +$0e ユニット数1のつもり + 名前"SCSI   " */
		SCSIIPL[off_hdr + 0x0e] = 0x01;
		SCSIIPL[off_hdr + 0x0f] = 'S';
		SCSIIPL[off_hdr + 0x10] = 'C';
		SCSIIPL[off_hdr + 0x11] = 'S';
		SCSIIPL[off_hdr + 0x12] = 'I';
		SCSIIPL[off_hdr + 0x13] = ' ';
		SCSIIPL[off_hdr + 0x14] = ' ';
		SCSIIPL[off_hdr + 0x15] = ' ';

		/* $ea0116〜$ea01ff: 4バイトのポインタ表。stub[2]以降を指す。 */
		for (off = off_table; off + 4 <= off_stub; off += 4)
		{
			int idx = 2 + (int)((off - off_table) / 4);
			uint32_t v = 0x00ea0200 + (uint32_t)idx * 10;
			SCSIIPL[off + 0] = (uint8_t)((v >> 24) & 0xff);
			SCSIIPL[off + 1] = (uint8_t)((v >> 16) & 0xff);
			SCSIIPL[off + 2] = (uint8_t)((v >> 8) & 0xff);
			SCSIIPL[off + 3] = (uint8_t)(v & 0xff);
		}

		if (log_cb)
		{
			uint32_t hdr_next = ((uint32_t)SCSIIPL[off_hdr + 0] << 24) | ((uint32_t)SCSIIPL[off_hdr + 1] << 16) |
				((uint32_t)SCSIIPL[off_hdr + 2] << 8) | SCSIIPL[off_hdr + 3];
			uint32_t hdr_attr = ((uint32_t)SCSIIPL[off_hdr + 4] << 8) | SCSIIPL[off_hdr + 5];
			uint32_t hdr_strategy = ((uint32_t)SCSIIPL[off_hdr + 6] << 24) | ((uint32_t)SCSIIPL[off_hdr + 7] << 16) |
				((uint32_t)SCSIIPL[off_hdr + 8] << 8) | SCSIIPL[off_hdr + 9];
			uint32_t hdr_interrupt = ((uint32_t)SCSIIPL[off_hdr + 10] << 24) | ((uint32_t)SCSIIPL[off_hdr + 11] << 16) |
				((uint32_t)SCSIIPL[off_hdr + 12] << 8) | SCSIIPL[off_hdr + 13];
			uint32_t tbl0 = ((uint32_t)SCSIIPL[off_table + 0] << 24) | ((uint32_t)SCSIIPL[off_table + 1] << 16) |
				((uint32_t)SCSIIPL[off_table + 2] << 8) | SCSIIPL[off_table + 3];
			log_cb(RETRO_LOG_INFO,
				"[SCSI] デバイスドライバヘッダ書き込み確認: next=$%08x(設定値) attr=$%04x strategy=$%08x(期待$%08x) interrupt=$%08x(期待$%08x) unit=$%02x name=\"%c%c%c%c%c%c%c\" table[0]=$%08x(期待$%08x)\n",
				hdr_next, hdr_attr, hdr_strategy, 0x00ea0200u, hdr_interrupt, 0x00ea0200u + 10u,
				SCSIIPL[off_hdr + 0x0e],
				SCSIIPL[off_hdr + 0x0f], SCSIIPL[off_hdr + 0x10], SCSIIPL[off_hdr + 0x11],
				SCSIIPL[off_hdr + 0x12], SCSIIPL[off_hdr + 0x13], SCSIIPL[off_hdr + 0x14], SCSIIPL[off_hdr + 0x15],
				tbl0, 0x00ea0200u + 2u*10u);
		}
	}
	/* $ea0600: ユニット0のBPB(ドライブパラメータブロック)へのポインタ
	 * ($00ea0610)を置き、$ea0610 以降にBPBの実体を置く。
	 * バイト入れ替えループより前に、自然なバイト順で書く。
	 * 本物ROM使用中は飛ばす(自前スタブ前提のため)。
	 *
	 * BPBの中身は知識で組み立てず、**基準器イメージから写す**。
	 * 根拠(2026-09-03 実測): 基準器イメージ(100MB SCSI)の
	 *   - LBA0 先頭が "X68SCSI1"
	 *   - LBA4($800) にパーティション表。先頭4バイト "X68K" + 12バイトの
	 *     ヘッダのあと、16バイトのエントリ(名前8 + 開始4 + サイズ4、いずれもBE、
	 *     **単位は1024バイトブロック**)。実測値は 名前="Human68k" 開始=$20 サイズ=$18c00 で、
	 *     $18c00 * 1024 = 103,809,024 バイトとイメージ実サイズが整合する
	 *   - パーティション先頭($20 * 1024 = $8000)がブートセクタで、
	 *     2バイト分岐 + 16バイトOEM("SHARP/KG    1.00") のあと **+$12 から BPB**
	 * であった。+$12 からの並びを次のように読むと、FATのセクタ数が
	 * 独立に計算した値と一致した(総セクタ101376 / 2セクタ per クラスタ =
	 * 50688クラスタ、FAT16なので 50688*2 = 101376バイト = 1024バイトセクタで99、
	 * 実際の値は100)。16bit値はすべてビッグエンディアン:
	 *   +0  word 1セクタのバイト数   = $0400 (1024)
	 *   +2  byte 1クラスタのセクタ数 = 2
	 *   +3  byte FATの個数           = 2
	 *   +4  word 予約セクタ数        = 1
	 *   +6  word ルートdirエントリ数 = $0200 (512)
	 *   +8  word 総セクタ数(16bit)   = 0 (65535超なので下の32bit欄を使う)
	 *   +10 byte メディアバイト      = $f7
	 *   +11 byte FATのセクタ数       = 100
	 *   +12 long 総セクタ数(32bit)   = 101376
	 *   +16 long パーティション開始  = 32
	 * ただし「Human68k が実際にどの欄をどこまで読むか」は未実測であるため、
	 * 解釈はログに出すだけにして、**写す範囲は +$12 から20バイトそのまま**とする。
	 * どこまで読まれるかは --mem-read-watch=0xea0610:0xea0630 で実測する。 */
	if (!SCSIUsingRealRom)
	{
		static uint8_t ptbl[512];
		static uint8_t boot[512];
		uint32_t off_bpbptr = 0x600;
		uint32_t off_bpb    = 0x610;
		uint32_t v = 0x00ea0610;
		int have_bpb = 0;

		/* BPB表は「ユニットごとの4バイトポインタの配列」である(2026-09-03 実測:
		 * Human68k は pc=$8328 で要素0を読んだあと、2台目のために要素1
		 * ($ea0604)を読みに来た。そこが0だと「１セクタあたりのバイト数が
		 * 大きすぎます」で止まる)。要素は4つとも同じBPBを指しておく。
		 * ユニット数の申告と表の要素数の整合は未確定のため、多めに置く。 */
		{
			int e;
			for (e = 0; e < 4; e++)
			{
				SCSIIPL[off_bpbptr + e * 4 + 0] = (uint8_t)((v >> 24) & 0xff);
				SCSIIPL[off_bpbptr + e * 4 + 1] = (uint8_t)((v >> 16) & 0xff);
				SCSIIPL[off_bpbptr + e * 4 + 2] = (uint8_t)((v >> 8) & 0xff);
				SCSIIPL[off_bpbptr + e * 4 + 3] = (uint8_t)(v & 0xff);
			}
		}
		if (log_cb)
		{
			uint32_t rb = ((uint32_t)SCSIIPL[off_bpbptr + 0] << 24) | ((uint32_t)SCSIIPL[off_bpbptr + 1] << 16) |
				((uint32_t)SCSIIPL[off_bpbptr + 2] << 8) | SCSIIPL[off_bpbptr + 3];
			log_cb(RETRO_LOG_INFO, "[SCSI] BPB表ポインタ書き込み確認: $ea0600=$%08x(期待$%08x)\n", rb, v);
		}

		/* パーティション表は LBA 4 ($800)。ホスト側の読み出し単位は512バイト。 */
		if (webx68k_scsi_read_sector(4, ptbl) != 0)
		{
			if (log_cb)
				log_cb(RETRO_LOG_ERROR, "[SCSI-BPB] パーティション表(LBA4)の読み出しに失敗した\n");
		}
		else if (!(ptbl[0] == 'X' && ptbl[1] == '6' && ptbl[2] == '8' && ptbl[3] == 'K'))
		{
			if (log_cb)
				log_cb(RETRO_LOG_ERROR,
					"[SCSI-BPB] LBA4 に \"X68K\" 署名が無い (先頭4バイト=$%02x%02x%02x%02x)\n",
					ptbl[0], ptbl[1], ptbl[2], ptbl[3]);
		}
		else
		{
			/* 先頭エントリのみ使う(このドライバはユニット数1で申告しているため)。
			 * エントリは 16バイト: 名前8 + 開始4(BE) + サイズ4(BE)、単位1024バイト。 */
			const uint8_t *e = &ptbl[16];
			uint32_t part_start = ((uint32_t)e[8] << 24) | ((uint32_t)e[9] << 16) | ((uint32_t)e[10] << 8) | e[11];
			uint32_t part_size  = ((uint32_t)e[12] << 24) | ((uint32_t)e[13] << 16) | ((uint32_t)e[14] << 8) | e[15];
			uint32_t boot_lba   = part_start * 2;	/* 1024バイト単位 → 512バイト単位 */

			if (log_cb)
				log_cb(RETRO_LOG_INFO,
					"[SCSI-BPB] パーティション0: 名前=\"%c%c%c%c%c%c%c%c\" 開始=%u(1KB単位) サイズ=%u(1KB単位) ブートセクタLBA=%u\n",
					e[0], e[1], e[2], e[3], e[4], e[5], e[6], e[7],
					(unsigned)part_start, (unsigned)part_size, (unsigned)boot_lba);

			if (part_size == 0)
			{
				if (log_cb)
					log_cb(RETRO_LOG_ERROR, "[SCSI-BPB] パーティション0のサイズが0\n");
			}
			else if (webx68k_scsi_read_sector(boot_lba, boot) != 0)
			{
				if (log_cb)
					log_cb(RETRO_LOG_ERROR, "[SCSI-BPB] ブートセクタ(LBA%u)の読み出しに失敗した\n", (unsigned)boot_lba);
			}
			else
			{
				uint32_t sect_size = ((uint32_t)boot[0x12] << 8) | boot[0x13];
				if (sect_size != 256 && sect_size != 512 && sect_size != 1024 && sect_size != 2048)
				{
					/* 1セクタのバイト数がありえない値なら、BPBの位置か
					 * イメージの想定が違う。ゼロのままにして誤った値を渡さない。 */
					if (log_cb)
						log_cb(RETRO_LOG_ERROR,
							"[SCSI-BPB] ブートセクタ+$12 が BPB らしくない (1セクタのバイト数=$%04x) OEM=\"%c%c%c%c%c%c%c%c\"\n",
							(unsigned)sect_size,
							boot[2], boot[3], boot[4], boot[5], boot[6], boot[7], boot[8], boot[9]);
				}
				else
				{
					int j;
					for (j = 0; j < 20; j++)
						SCSIIPL[off_bpb + j] = boot[0x12 + j];
					have_bpb = 1;
					SCSIImageReady = 1;
					SCSIPartStartBlocks = part_start;
					SCSIPartSectorBytes = sect_size;
					if (log_cb)
					{
						uint32_t total32 = ((uint32_t)boot[0x1e] << 24) | ((uint32_t)boot[0x1f] << 16) |
							((uint32_t)boot[0x20] << 8) | boot[0x21];
						uint32_t offs32 = ((uint32_t)boot[0x22] << 24) | ((uint32_t)boot[0x23] << 16) |
							((uint32_t)boot[0x24] << 8) | boot[0x25];
						log_cb(RETRO_LOG_INFO,
							"[SCSI-BPB] $ea0610 へBPBを写した(20バイト): 1セクタ=%uバイト クラスタ=%uセクタ FAT数=%u 予約=%u"
							" ルートdir=%u 総セクタ(16)=%u メディア=$%02x FATセクタ数=%u 総セクタ(32)=%u 開始=%u\n",
							(unsigned)sect_size, boot[0x14], boot[0x15],
							(unsigned)(((uint32_t)boot[0x16] << 8) | boot[0x17]),
							(unsigned)(((uint32_t)boot[0x18] << 8) | boot[0x19]),
							(unsigned)(((uint32_t)boot[0x1a] << 8) | boot[0x1b]),
							boot[0x1c], boot[0x1d], (unsigned)total32, (unsigned)offs32);
					}
				}
			}
		}

		if (!have_bpb && log_cb)
			log_cb(RETRO_LOG_ERROR, "[SCSI-BPB] BPBを作れなかった。$ea0610 以降はゼロのままである\n");
	}

	/* ベクタ設定エントリが返す d2 の即値を差し替える。
	 * 意味が未確定のため、再ビルドせずに値を振れるようにしてある。
	 * 位置は決め打ちなので、命令語($243c = move.l #imm,d2)を照合してから書く。 */
	/* 実験: SCSI ローダが書くはずの SRAM 既定値を、こちらで書いてしまう。
	 * 資料『Inside X68000』図8 によれば、$ed006f に 'V'($56) が無いとき
	 * SCSI ローダが 'V' と $ed0070=$07 / $ed0071=$00 を書く。
	 * 手元の環境は3番地とも $ff(＝ローダが走っていない)であったため、
	 * この状態が Human68k の判断を変えるのかを確かめる。既定では書かない。 */
	if (webx68k_scsi_sram_init())
	{
		/* SRAM は書き込み許可を開けないと無言で捨てられる(実測: 開けずに
		 * 書いたら読み返しが $ff のままだった)。開けたら必ず閉じる。 */
		SRAM_WriteEnable(1);
		SRAM_Write(0x00ed006f, 0x56);
		SRAM_Write(0x00ed0070, 0x07);
		SRAM_Write(0x00ed0071, 0x00);
		SRAM_WriteEnable(0);
		if (log_cb)
			log_cb(RETRO_LOG_INFO, "[SCSI] SRAM に SCSI ローダ相当の既定値を書いた (読み返し $%02x $%02x $%02x)\n",
				SRAM_Read(0x00ed006f), SRAM_Read(0x00ed0070), SRAM_Read(0x00ed0071));
	}

	/* d2/a4 の即値差し替えは自前スタブ前提の処理。本物ROM使用中は飛ばす。 */
	if (!SCSIUsingRealRom)
	{
		if (SCSIIPL[0x88] == 0x28 && SCSIIPL[0x89] == 0x7c &&
			SCSIIPL[0x8e] == 0x24 && SCSIIPL[0x8f] == 0x3c)
		{
			int a4 = webx68k_scsi_init_a4();
			int d2 = webx68k_scsi_init_d2();

			if (!SCSIImageReady)
			{
				/* イメージが無いか読めない(BPBを作れなかった)ときは、
				 * JS側の設定に関わらずドライバを名乗らない(従来の挙動)。 */
				a4 = 0x00000000;
				d2 = (int)0xffffffff;
				if (log_cb)
					log_cb(RETRO_LOG_INFO,
						"[SCSI] イメージが無いか読めないため、ドライバとして名乗らない (a4=$00000000 d2=$ffffffff)\n");
			}

			SCSIIPL[0x8a] = (uint8_t)((a4 >> 24) & 0xff);
			SCSIIPL[0x8b] = (uint8_t)((a4 >> 16) & 0xff);
			SCSIIPL[0x8c] = (uint8_t)((a4 >> 8) & 0xff);
			SCSIIPL[0x8d] = (uint8_t)(a4 & 0xff);
			SCSIIPL[0x90] = (uint8_t)((d2 >> 24) & 0xff);
			SCSIIPL[0x91] = (uint8_t)((d2 >> 16) & 0xff);
			SCSIIPL[0x92] = (uint8_t)((d2 >> 8) & 0xff);
			SCSIIPL[0x93] = (uint8_t)(d2 & 0xff);
			if (log_cb)
				log_cb(RETRO_LOG_INFO, "[SCSI] ベクタ設定エントリが返す a4 = $%08x, d2 = $%08x\n",
					(unsigned)a4, (unsigned)d2);
		}
		else if (log_cb)
			log_cb(RETRO_LOG_ERROR, "[SCSI] 即値の位置がずれている ($%02x$%02x / $%02x$%02x)\n",
				SCSIIPL[0x88], SCSIIPL[0x89], SCSIIPL[0x8e], SCSIIPL[0x8f]);
	}

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

	/* 陽性対照: $ea0100〜$ea07ff の「書ける窓」への書き込みが
	 * SCSI_Write を経由して実際に SCSIIPL へ反映されるかを、
	 * CPU と同じ書き込み経路(cpu_writemem24)で確かめる。
	 * 書き込み経路がそもそも SCSI_Write に来ていない可能性があるため必須。
	 * 検査後は元の値(表の要素0の該当バイト)へ戻す。
	 * 本物ROM使用中はこの窓自体を使わない(自前スタブ前提)ので飛ばす。 */
	if (!SCSIUsingRealRom)
	{
		uint8_t orig = cpu_readmem24(0x00ea0110);
		cpu_writemem24(0x00ea0110, 0x5a);
		{
			uint8_t back = cpu_readmem24(0x00ea0110);
			if (log_cb)
			{
				if (back == 0x5a)
					log_cb(RETRO_LOG_INFO, "[SCSI] selftest ok: $ea0110 への窓書き込みが効いている\n");
				else
					log_cb(RETRO_LOG_ERROR, "[SCSI] selftest FAILED: $ea0110 への窓書き込みが反映されない (読み返し=$%02x)\n", back);
			}
		}
		cpu_writemem24(0x00ea0110, orig);
	}

	/* 読み込んだ内容の確認: $ea0044〜6バイトが "SCSIEX" であること。
	 * 本物ROM/自前スタブのどちらでも、読み込めたかどうかを毎回確かめる。
	 * SCSI_Read と同じ読み出し経路(バイト入れ替え後の (adr^1)&0x1fff)で見る。 */
	{
		static const char expect[6] = "SCSIEX";
		uint8_t got[6];
		int ok = 1;
		int j;
		for (j = 0; j < 6; j++)
		{
			got[j] = SCSIIPL[((0xea0044 + j) ^ 1) & 0x1fff];
			if (got[j] != (uint8_t)expect[j])
				ok = 0;
		}
		if (log_cb)
		{
			if (ok)
				log_cb(RETRO_LOG_INFO, "[SCSI] $ea0044 の確認 ok: \"SCSIEX\"\n");
			else
				log_cb(RETRO_LOG_ERROR,
					"[SCSI] $ea0044 の確認 FAILED: \"%c%c%c%c%c%c\" (期待 \"SCSIEX\")\n",
					got[0], got[1], got[2], got[3], got[4], got[5]);
		}
	}

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

	/* SCSIUsingRealRom・drv_attr/drv_next(SCSIInitDrvAttrForLog/
	 * SCSIInitDrvNextForLog)が確定した後にキャッシュを読み直す。
	 * 本物ROM使用・自前スタブのどちらでも必ず1回呼ぶ(reply_*は
	 * 自前スタブ経路でも参照するため、SCSIUsingRealRomで出し分けない。
	 * 2026-09-02、ここが本物ROM使用中だけの呼び出しになっていて
	 * 自前スタブ経路のreply_*が既定値ゼロのまま固定される事故があった)。 */
	SCSI_RefreshHostConfig();
}

/* インタラプト(ホストコマンド $41)から呼ばれる、要求ヘッダのHLE処理。
 * ストラテジ($40)で控えた SCSIReqHeaderAddr が指す先を実際のCPU読み書き
 * 経路(cpu_readmem24/cpu_writemem24)で処理する。
 *
 * 要求ヘッダの一般的な形(知識であって未実測。実測できたのは長さ22で
 * +0=長さ, +1=ユニット, +2=コマンドの3バイトのみ):
 *   +0 長さ / +1 ユニット番号 / +2 コマンド / +3 エラーコード /
 *   +4..12 予約 / +13 ユニット数 / +14 処理終了アドレス(4) /
 *   +18 パラメータ(4) */
static void SCSI_HandleRequestHeader(void)
{
	uint8_t buf[26];
	uint32_t addr = SCSIReqHeaderAddr;
	uint32_t i;
	uint8_t cmdnum;

	if (addr == 0)
	{
		if (log_cb)
			log_cb(RETRO_LOG_ERROR,
				"[SCSI-REQ] インタラプトが呼ばれたが要求ヘッダのアドレスを控えていない(ストラテジ未呼び出し)\n");
		return;
	}

	for (i = 0; i < sizeof(buf); i++)
		buf[i] = cpu_readmem24(addr + i);
	if (log_cb)
		log_cb(RETRO_LOG_INFO,
			"[SCSI-REQ] 処理前 addr=$%08x:"
			" %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x"
			" %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x\n",
			(unsigned)addr,
			buf[0], buf[1], buf[2], buf[3], buf[4], buf[5], buf[6], buf[7], buf[8], buf[9],
			buf[10], buf[11], buf[12], buf[13], buf[14], buf[15], buf[16], buf[17], buf[18], buf[19],
			buf[20], buf[21], buf[22], buf[23], buf[24], buf[25]);

	SCSIReqReplyD0 = -1;	/* コマンドごとに設定し直す */
	cmdnum = buf[2];

	if (cmdnum == 0x00)
	{
		int reply_err = SCSIHostReplyErr;
		int reply_units = SCSIHostReplyUnits;
		uint32_t reply_end = (SCSIDrvRamEnd != 0) ? SCSIDrvRamEnd : SCSIHostReplyEnd;
		uint32_t reply_bpb = SCSIHostReplyBpb;
		int reply_status = SCSIHostReplyStatus;

		SCSIReqInitCount++;

		if (SCSIHostReplyInitOnce != 0 && SCSIReqInitCount >= 2)
		{
			/* 2回目以降: 「ドライバは無い」で返答する。終了アドレスは
			 * 要求ヘッダの入力(処理前に読んだ buf[14..17])をそのまま
			 * 返し、メモリを消費しないことを示す。 */
			uint32_t in_end = ((uint32_t)buf[14] << 24) | ((uint32_t)buf[15] << 16) |
				((uint32_t)buf[16] << 8) | (uint32_t)buf[17];

			/* 【重要】ユニット数に 0 を返してはいけない。2026-09-03 実測:
			 * Human68k は要求ヘッダ +13 を d7 の上位ワードに置き、
			 *   $8344 sub.l d5,d7 / cmp.l d5,d7 / bcs 抜ける  (d5=$00010000)
			 * でユニット数ぶん回す。0 だと最初の減算で $ffff0000 に借り引きし、
			 * 符号なし比較で抜けられず約65535回まわる。その間 BPB表の要素を
			 * 4バイトずつ消費し、表の外まで読んで
			 * 「１セクタあたりのバイト数が大きすぎます」で止まる。
			 * よって「もう無い」はユニット数ではなく、ステータスワードの
			 * エラービット($8000)で伝える。 */
			cpu_writemem24(addr + 3, (uint8_t)(reply_err & 0xff));			/* +3 エラーコード */
			cpu_writemem24(addr + 13, 0x01);					/* +13 ユニット数(0は禁止。上のコメント参照) */
			cpu_writemem24(addr + 14, buf[14]);					/* +14..17 処理終了アドレス(入力のまま) */
			cpu_writemem24(addr + 15, buf[15]);
			cpu_writemem24(addr + 16, buf[16]);
			cpu_writemem24(addr + 17, buf[17]);
			/* BPB表ポインタは通常時と同じ値を返す。2026-09-03 実測: ここを
			 * 0 にすると Human68k はそれでも辿りにいき、「１セクタあたりの
			 * バイト数が大きすぎます」で止まる。ユニット数0でも参照される。 */
			cpu_writemem24(addr + 18, (uint8_t)((reply_bpb >> 24) & 0xff));	/* +18..21 パラメータ(BPB表ポインタ) */
			cpu_writemem24(addr + 19, (uint8_t)((reply_bpb >> 16) & 0xff));
			cpu_writemem24(addr + 20, (uint8_t)((reply_bpb >> 8) & 0xff));
			cpu_writemem24(addr + 21, (uint8_t)(reply_bpb & 0xff));
			/* ステータスワード +4 にエラービット($8000)を立てて「登録するな」と伝える。 */
			cpu_writemem24(addr + 4, 0x80);
			cpu_writemem24(addr + 5, 0x00);
			if (log_cb)
				log_cb(RETRO_LOG_INFO,
					"[SCSI-REQ] 初期化コマンド($00) 処理 #%d: 2回目以降のため「ドライバ無し」で返答した"
					" (unit数=0 終了addr=$%08x 入力のまま)\n",
					SCSIReqInitCount, (unsigned)in_end);
		}
		else
		{
			cpu_writemem24(addr + 3, (uint8_t)(reply_err & 0xff));			/* +3 エラーコード */
			cpu_writemem24(addr + 13, (uint8_t)(reply_units & 0xff));		/* +13 ユニット数 */
			cpu_writemem24(addr + 14, (uint8_t)((reply_end >> 24) & 0xff));	/* +14..17 処理終了アドレス */
			cpu_writemem24(addr + 15, (uint8_t)((reply_end >> 16) & 0xff));
			cpu_writemem24(addr + 16, (uint8_t)((reply_end >> 8) & 0xff));
			cpu_writemem24(addr + 17, (uint8_t)(reply_end & 0xff));
			cpu_writemem24(addr + 18, (uint8_t)((reply_bpb >> 24) & 0xff));	/* +18..21 パラメータ(BPB表ポインタ) */
			cpu_writemem24(addr + 19, (uint8_t)((reply_bpb >> 16) & 0xff));
			cpu_writemem24(addr + 20, (uint8_t)((reply_bpb >> 8) & 0xff));
			cpu_writemem24(addr + 21, (uint8_t)(reply_bpb & 0xff));
			if (reply_status >= 0)
			{
				/* +4 にワードとして書く。-1(既定)のときは何もしない(元の状態と同じ)。 */
				cpu_writemem24(addr + 4, (uint8_t)((reply_status >> 8) & 0xff));
				cpu_writemem24(addr + 5, (uint8_t)(reply_status & 0xff));
			}
			if (log_cb)
				log_cb(RETRO_LOG_INFO,
					"[SCSI-REQ] 初期化コマンド($00) 処理 #%d: err=$%02x unit数=%d 終了addr=$%08x(%s) BPB表ptr=$%08x status=%d%s\n",
					SCSIReqInitCount, (unsigned)(reply_err & 0xff), reply_units,
					(unsigned)reply_end, (SCSIDrvRamEnd != 0) ? "RAM配置" : "設定値",
					(unsigned)reply_bpb, reply_status,
					(reply_status >= 0) ? "(+4に書いた)" : "(未指定・+4は変更なし)");
		}
	}
	else if (cmdnum == 0x04)
	{
		/* 読み出し。要求ヘッダ +14..17=転送先アドレス +18..21=セクタ数
		 * +22..25=開始論理セクタ(パーティション先頭を0とする)。 */
		uint32_t addr_dst = ((uint32_t)buf[14] << 24) | ((uint32_t)buf[15] << 16) |
			((uint32_t)buf[16] << 8) | (uint32_t)buf[17];
		uint32_t count = ((uint32_t)buf[18] << 24) | ((uint32_t)buf[19] << 16) |
			((uint32_t)buf[20] << 8) | (uint32_t)buf[21];
		uint32_t start = ((uint32_t)buf[22] << 24) | ((uint32_t)buf[23] << 16) |
			((uint32_t)buf[24] << 8) | (uint32_t)buf[25];

		if (SCSIPartSectorBytes == 0 || SCSIPartStartBlocks == 0)
		{
			if (log_cb)
				log_cb(RETRO_LOG_ERROR,
					"[SCSI-READ] パーティション諸元が未確定のため読み出しを拒否した\n");
			cpu_writemem24(addr + 3, 0x02);
		}
		else if (count == 0)
		{
			cpu_writemem24(addr + 3, 0x00);
		}
		else if (count > 256)
		{
			if (log_cb)
				log_cb(RETRO_LOG_ERROR,
					"[SCSI-READ] セクタ数=%u が上限256を超えるため拒否した\n", (unsigned)count);
			cpu_writemem24(addr + 3, 0x02);
		}
		else
		{
			uint32_t s;
			int failed = 0;

			for (s = 0; s < count && !failed; s++)
			{
				uint32_t logsec = start + s;
				uint32_t host_lba_base = (SCSIPartStartBlocks * 1024 + logsec * SCSIPartSectorBytes) / 512;
				uint32_t sub_sectors = SCSIPartSectorBytes / 512;
				uint32_t k;

				for (k = 0; k < sub_sectors && !failed; k++)
				{
					uint8_t sec512[512];

					if (webx68k_scsi_read_sector(host_lba_base + k, sec512) != 0)
					{
						if (log_cb)
							log_cb(RETRO_LOG_ERROR,
								"[SCSI-READ] ホストLBA=%u の読み出しに失敗した(論理セクタ=%u)\n",
								(unsigned)(host_lba_base + k), (unsigned)logsec);
						failed = 1;
					}
					else
					{
						uint32_t n;
						uint32_t base = addr_dst + s * SCSIPartSectorBytes + k * 512;
						for (n = 0; n < 512; n++)
							cpu_writemem24(base + n, sec512[n]);
					}
				}
			}

			cpu_writemem24(addr + 3, failed ? 0x02 : 0x00);

			if (log_cb)
			{
				uint32_t host_lba_first = (SCSIPartStartBlocks * 1024 + start * SCSIPartSectorBytes) / 512;
				log_cb(RETRO_LOG_INFO,
					"[SCSI-READ] 論理セクタ=%u 個数=%u 転送先=$%08x"
					" (1セクタ=%uバイト パーティション開始=%uブロック 先頭LBA=%u)\n",
					(unsigned)start, (unsigned)count, (unsigned)addr_dst,
					(unsigned)SCSIPartSectorBytes, (unsigned)SCSIPartStartBlocks,
					(unsigned)host_lba_first);
				if (!failed)
				{
					uint8_t rb[16];
					uint32_t n;
					for (n = 0; n < 16; n++)
						rb[n] = cpu_readmem24(addr_dst + n);
					log_cb(RETRO_LOG_INFO,
						"[SCSI-READ] 転送後の先頭16バイト: %02x %02x %02x %02x %02x %02x %02x %02x"
						" %02x %02x %02x %02x %02x %02x %02x %02x\n",
						rb[0], rb[1], rb[2], rb[3], rb[4], rb[5], rb[6], rb[7],
						rb[8], rb[9], rb[10], rb[11], rb[12], rb[13], rb[14], rb[15]);
				}
			}
		}
	}
	else if (cmdnum == 0x08)
	{
		/* 書き込み。要求ヘッダの形は $04(読み出し)と同じ(実測 2026-09-03:
		 * +13 メディアバイト / +14 転送元 / +18 セクタ数 / +22 開始セクタ)。
		 *
		 * ホスト側のイメージは Range 付きXHRで読むだけの経路しか無く、
		 * まだ書き戻せない(決定2 の OPFS 経路が未実装)。ここで err=$00 を
		 * 返すと「書けた」と嘘をつくことになり、ゲストのFATキャッシュが
		 * 実体とずれる。**書けないなら書けないと言う**のが正しい。
		 * 返し方(エラーコードの置き場所)は未実測なので、+3 と +4..5 の
		 * 両方に書いて画面の反応で確かめる。 */
		uint32_t w_addr = ((uint32_t)buf[14] << 24) | ((uint32_t)buf[15] << 16) |
			((uint32_t)buf[16] << 8) | (uint32_t)buf[17];
		uint32_t w_count = ((uint32_t)buf[18] << 24) | ((uint32_t)buf[19] << 16) |
			((uint32_t)buf[20] << 8) | (uint32_t)buf[21];
		uint32_t w_start = ((uint32_t)buf[22] << 24) | ((uint32_t)buf[23] << 16) |
			((uint32_t)buf[24] << 8) | (uint32_t)buf[25];

		cpu_writemem24(addr + 3, 0x0a);		/* エラーコード。$00 は「ファイル共有違反」になった(実測)ので $13 を試す */
		cpu_writemem24(addr + 4, 0x81);		/* ステータス上位: bit15=エラー, bit8=処理終了 */
		cpu_writemem24(addr + 5, 0x0a);
		/* 【実測 2026-09-03】エラーが伝わる経路は **要求ヘッダ +4..5 の
		 * ステータスワード** である。画面に「エラー($810A)が発生しました」と
		 * 出て、ここに書いた値がそのまま表示された。d0 は経路ではない
		 * (d0 だけ立てても止まったままだった)が、害は無いので併せて立てておく。
		 * エラーコードの割り当ては未特定: $00 は「ファイル共有違反です」と
		 * 表示されて誤解を招くので使わない。$0a/$13 は名前が無く生の値が出る。 */
		SCSIReqReplyD0 = 0x800a;
		if (log_cb)
			log_cb(RETRO_LOG_INFO,
				"[SCSI-REQ] 書き込みコマンド($08) は未対応。書き込み保護として断る"
				" (論理セクタ=%u 個数=%u 転送元=$%08x)\n",
				(unsigned)w_start, (unsigned)w_count, (unsigned)w_addr);
	}
	else
	{
		if (log_cb)
			log_cb(RETRO_LOG_INFO,
				"[SCSI-REQ] 未対応コマンド $%02x: 内容を観測するため err=$00 だけ書く\n",
				(unsigned)cmdnum);
		cpu_writemem24(addr + 3, 0x00);
	}

	for (i = 0; i < sizeof(buf); i++)
		buf[i] = cpu_readmem24(addr + i);
	if (log_cb)
		log_cb(RETRO_LOG_INFO,
			"[SCSI-REQ] 処理後 addr=$%08x:"
			" %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x"
			" %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x\n",
			(unsigned)addr,
			buf[0], buf[1], buf[2], buf[3], buf[4], buf[5], buf[6], buf[7], buf[8], buf[9],
			buf[10], buf[11], buf[12], buf[13], buf[14], buf[15], buf[16], buf[17], buf[18], buf[19],
			buf[20], buf[21], buf[22], buf[23], buf[24], buf[25]);
}

/* 自前ROMコードからの依頼を処理する。まだ何も登録せず、観測に必要なことだけ行う。 */
static void SCSI_HostCommand(uint8_t cmd)
{
	static uint8_t sec0[512];
	uint32_t i;

	if (cmd == 0x06)
	{
		uint32_t sp = m68000_get_reg(M68K_A7);
		if (log_cb)
			log_cb(RETRO_LOG_INFO,
				"[SCSI] $00000000 に飛んできた #%d sr=$%04x sp=$%08x\n"
				"        d0=$%08x d1=$%08x d2=$%08x d3=$%08x d4=$%08x d5=$%08x d6=$%08x d7=$%08x\n"
				"        a0=$%08x a1=$%08x a2=$%08x a3=$%08x a4=$%08x a5=$%08x a6=$%08x\n"
				"        stack: [0]=$%08x [1]=$%08x [2]=$%08x [3]=$%08x\n",
				++SCSIZeroCallCount,
				(unsigned)m68000_get_reg(M68K_SR), (unsigned)sp,
				(unsigned)m68000_get_reg(M68K_D0), (unsigned)m68000_get_reg(M68K_D1),
				(unsigned)m68000_get_reg(M68K_D2), (unsigned)m68000_get_reg(M68K_D3),
				(unsigned)m68000_get_reg(M68K_D4), (unsigned)m68000_get_reg(M68K_D5),
				(unsigned)m68000_get_reg(M68K_D6), (unsigned)m68000_get_reg(M68K_D7),
				(unsigned)m68000_get_reg(M68K_A0), (unsigned)m68000_get_reg(M68K_A1),
				(unsigned)m68000_get_reg(M68K_A2), (unsigned)m68000_get_reg(M68K_A3),
				(unsigned)m68000_get_reg(M68K_A4), (unsigned)m68000_get_reg(M68K_A5),
				(unsigned)m68000_get_reg(M68K_A6),
				(unsigned)cpu_readmem24_dword(sp), (unsigned)cpu_readmem24_dword(sp + 4),
				(unsigned)cpu_readmem24_dword(sp + 8), (unsigned)cpu_readmem24_dword(sp + 12));
		return;
	}
	if (cmd == 0x05)
	{
		if (log_cb)
			log_cb(RETRO_LOG_INFO,
				"[SCSI] d2で渡したルーチンが呼ばれた #%d pc=$%08x sr=$%04x\n"
				"        d0=$%08x d1=$%08x d2=$%08x d3=$%08x d4=$%08x d5=$%08x d6=$%08x d7=$%08x\n"
				"        a0=$%08x a1=$%08x a2=$%08x a3=$%08x a4=$%08x a5=$%08x a6=$%08x a7=$%08x\n",
				++SCSIEntryCallCount,
				(unsigned)m68000_get_reg(M68K_PC), (unsigned)m68000_get_reg(M68K_SR),
				(unsigned)m68000_get_reg(M68K_D0), (unsigned)m68000_get_reg(M68K_D1),
				(unsigned)m68000_get_reg(M68K_D2), (unsigned)m68000_get_reg(M68K_D3),
				(unsigned)m68000_get_reg(M68K_D4), (unsigned)m68000_get_reg(M68K_D5),
				(unsigned)m68000_get_reg(M68K_D6), (unsigned)m68000_get_reg(M68K_D7),
				(unsigned)m68000_get_reg(M68K_A0), (unsigned)m68000_get_reg(M68K_A1),
				(unsigned)m68000_get_reg(M68K_A2), (unsigned)m68000_get_reg(M68K_A3),
				(unsigned)m68000_get_reg(M68K_A4), (unsigned)m68000_get_reg(M68K_A5),
				(unsigned)m68000_get_reg(M68K_A6), (unsigned)m68000_get_reg(M68K_A7));
		return;
	}
	if (cmd == 0x03)
	{
		if (log_cb)
			log_cb(RETRO_LOG_INFO, "[SCSI] ベクタ設定エントリが呼ばれた(陽性対照) (pc=$%08x)\n",
				(unsigned)m68000_get_reg(M68K_PC));

		/* ベクタ設定エントリは実測で複数回呼ばれる(2026-09-03)。Human68k は
		 * 「次のドライバはあるか」を聞き続けているとみられ、毎回同じヘッダを
		 * 返すと同じドライバが2台ぶん登録され、DPBの並びが壊れて暴走ジャンプ
		 * (アドレスエラー)になる。reply_init_once が立っているときは、
		 * 2回目以降は a4 に 0 を返して「もう無い」と伝える。 */
		SCSIVectorEntryCount++;
		if (SCSIHostReplyInitOnce != 0 && SCSIVectorEntryCount >= 2)
		{
			/* a4 に 0 を返すのは不可(実測 2026-09-03)。Human68k は番地0を
			 * ドライバヘッダとして読みに行き($831e btst #5,$4(a1))、暴走する。
			 * 「もう無い」は d2 側で伝える(px68k の元スタブも d2=-1 を返していた)。 */
			uint32_t k;
			for (k = 0; k < 4; k++)
				SCSIIPL[((0x00ea0090 + k) ^ 1) & 0x1fff] = 0xff;
			if (log_cb)
				log_cb(RETRO_LOG_INFO,
					"[SCSI] ベクタ設定エントリ #%d: 2回目以降のため d2 に $ffffffff を返す(もうドライバは無い)\n",
					SCSIVectorEntryCount);
			return;
		}
		/* SCSI 用に追加された SRAM の3番地を記録する(資料『Inside X68000』図8)。
		 * $ED006F が 'V'($56) のとき $ED0070/$ED0071 が有効。
		 * $ED0070: bit3 = 0:内蔵 / 1:オプションボード、下位3bit = 本体のSCSI ID。
		 * $ED0071: SASIフラグ。
		 * 資料の記述が手元の環境で成り立っているかを実測で確かめるために出す。
		 * peek8 は SRAM を経由しないため、必ず SRAM_Read() で読む。 */
		if (log_cb)
			log_cb(RETRO_LOG_INFO, "[SCSI] SRAM $ed006f=$%02x('%c') $ed0070=$%02x $ed0071=$%02x\n",
				SRAM_Read(0x00ed006f),
				(SRAM_Read(0x00ed006f) >= 0x20 && SRAM_Read(0x00ed006f) < 0x7f) ? SRAM_Read(0x00ed006f) : '.',
				SRAM_Read(0x00ed0070), SRAM_Read(0x00ed0071));

		/* d2 に -1 以外を返すと Human68k は $00000000 を実行して落ちる。
		 * そこは RAM なので、印を出して戻るだけのコードを置いておけば
		 * 「飛んできた瞬間」のレジスタとスタックが取れる。
		 * $0-$7 はリセットベクタだが、リセットまで読まれないので上書きしてよい。 */
		{
			static const uint8_t stub[] = {
				0x13, 0xfc, 0x00, 0x06, 0x00, 0xe9, 0xf8, 0x02,	/* move.b #$06, $e9f802 */
				0x4e, 0x75										/* rts */
			};
			uint32_t k;
			for (k = 0; k < sizeof(stub); k++)
				cpu_writemem24(k, stub[k]);
		}

		/*
		 * Human68k の初期化コマンド返答は次の2つを同時に満たさないと拒否される
		 * (実行トレースで確認済み):
		 *   門1: 終了アドレス + $10000 < $00200000
		 *   門2: 終了アドレス - $22 >= a1(デバイスドライバヘッダの番地)
		 * 自前スタブのヘッダは SCSIボードROMの窓($00ea0100)にあり、この2つを
		 * 同時に満たせないため、ヘッダとスタブ本体をゲストRAMへ組み立てる。
		 * base が0(既定、両設定とも無効)のときは何もせず従来どおり。
		 */
		if (!SCSIImageReady)
		{
			if (log_cb)
				log_cb(RETRO_LOG_INFO,
					"[SCSI] イメージが無いか読めないため、RAMへドライバを組み立てる処理を飛ばす\n");
		}
		else
		{
			uint32_t from = webx68k_scsi_drv_ram_from();
			uint32_t base = 0;

			if (from != 0)
				base = cpu_readmem24_dword(from);
			else
				base = webx68k_scsi_drv_ram();

			if (base != 0)
			{
				uint32_t next = webx68k_scsi_drv_next();
				int attr = webx68k_scsi_drv_attr();
				uint32_t strategy = base + 0x20;
				uint32_t interrupt = base + 0x2a;
				static const uint8_t strategy_code[] = {
					0x13, 0xfc, 0x00, 0x40, 0x00, 0xe9, 0xf8, 0x02, 0x4e, 0x75
				};
				static const uint8_t interrupt_code[] = {
					0x13, 0xfc, 0x00, 0x41, 0x00, 0xe9, 0xf8, 0x02, 0x4e, 0x75
				};
				static const char name[7] = "SCSIHDD";
				uint32_t k;
				uint32_t rb0, rb6, rb10;
				int rb4;
				uint8_t rb14;
				char rname[8];

				cpu_writemem24(base + 0x00, (uint8_t)((next >> 24) & 0xff));
				cpu_writemem24(base + 0x01, (uint8_t)((next >> 16) & 0xff));
				cpu_writemem24(base + 0x02, (uint8_t)((next >> 8) & 0xff));
				cpu_writemem24(base + 0x03, (uint8_t)(next & 0xff));

				cpu_writemem24(base + 0x04, (uint8_t)((attr >> 8) & 0xff));
				cpu_writemem24(base + 0x05, (uint8_t)(attr & 0xff));

				cpu_writemem24(base + 0x06, (uint8_t)((strategy >> 24) & 0xff));
				cpu_writemem24(base + 0x07, (uint8_t)((strategy >> 16) & 0xff));
				cpu_writemem24(base + 0x08, (uint8_t)((strategy >> 8) & 0xff));
				cpu_writemem24(base + 0x09, (uint8_t)(strategy & 0xff));

				cpu_writemem24(base + 0x0a, (uint8_t)((interrupt >> 24) & 0xff));
				cpu_writemem24(base + 0x0b, (uint8_t)((interrupt >> 16) & 0xff));
				cpu_writemem24(base + 0x0c, (uint8_t)((interrupt >> 8) & 0xff));
				cpu_writemem24(base + 0x0d, (uint8_t)(interrupt & 0xff));

				cpu_writemem24(base + 0x0e, 0x01);	/* ユニット数 */

				for (k = 0; k < 7; k++)
					cpu_writemem24(base + 0x0f + k, (uint8_t)name[k]);

				for (k = 0; k < sizeof(strategy_code); k++)
					cpu_writemem24(base + 0x20 + k, strategy_code[k]);
				for (k = 0; k < sizeof(interrupt_code); k++)
					cpu_writemem24(base + 0x2a + k, interrupt_code[k]);

				SCSIDrvRamEnd = base + 0x34;

				if (log_cb)
					log_cb(RETRO_LOG_INFO,
						"[SCSI-DRV] ドライバをRAMへ置いた base=$%08x (from=$%08x の中身 / 直接指定) next=$%08x attr=$%04x\n"
						"           strategy=$%08x interrupt=$%08x 終了アドレス=$%08x\n",
						(unsigned)base, (unsigned)from, (unsigned)next, (unsigned)(attr & 0xffff),
						(unsigned)strategy, (unsigned)interrupt, (unsigned)SCSIDrvRamEnd);

				/* 陽性対照: 書けたことを cpu_readmem24 で読み返して確かめる。 */
				rb0 = cpu_readmem24_dword(base + 0x00);
				rb4 = (cpu_readmem24(base + 0x04) << 8) | cpu_readmem24(base + 0x05);
				rb6 = cpu_readmem24_dword(base + 0x06);
				rb10 = cpu_readmem24_dword(base + 0x0a);
				rb14 = cpu_readmem24(base + 0x0e);
				for (k = 0; k < 7; k++)
				{
					uint8_t c = cpu_readmem24(base + 0x0f + k);
					rname[k] = (c >= 0x20 && c < 0x7f) ? (char)c : '.';
				}
				rname[7] = '\0';
				if (log_cb)
					log_cb(RETRO_LOG_INFO,
						"[SCSI-DRV] 書き戻し確認: base+0=$%08x base+4=$%04x base+6=$%08x base+10=$%08x base+14=$%02x 名前=\"%s\"\n",
						(unsigned)rb0, (unsigned)rb4, (unsigned)rb6, (unsigned)rb10, (unsigned)rb14, rname);

				/*
				 * ROMスタブが a4 に積む即値をヘッダ番地(base)へ差し替える。
				 * SCSI_Init のバイト入れ替えループを通った後なので ^1 で添字を作る。
				 * $ea0088/$ea0089 が movea.l 命令語であることを照合してから書く。
				 */
				if (SCSIIPL[((0x00ea0088) ^ 1) & 0x1fff] == 0x28 &&
					SCSIIPL[((0x00ea0089) ^ 1) & 0x1fff] == 0x7c)
				{
					for (k = 0; k < 4; k++)
						SCSIIPL[((0x00ea008a + k) ^ 1) & 0x1fff] = (uint8_t)((base >> (24 - k * 8)) & 0xff);
				}
				else if (log_cb)
					log_cb(RETRO_LOG_ERROR,
						"[SCSI-DRV] a4の即値位置がずれている ($%02x$%02x)\n",
						SCSIIPL[((0x00ea0088) ^ 1) & 0x1fff], SCSIIPL[((0x00ea0089) ^ 1) & 0x1fff]);
			}
		}
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
	if (cmd >= 0x40 && cmd <= 0x7f)
	{
		int k = cmd - 0x40;
		uint32_t sp = m68000_get_reg(M68K_A7);
		const char *kname = (k == 0) ? "ストラテジ" : (k == 1) ? "インタラプト" : "表";
		if (log_cb)
		{
			log_cb(RETRO_LOG_INFO,
				"[SCSI] d2で渡した%s +$%02x(要素 %d)が呼ばれた #%d pc=$%08x sr=$%04x sp=$%08x\n"
				"        d0=$%08x d1=$%08x d2=$%08x d3=$%08x d4=$%08x d5=$%08x d6=$%08x d7=$%08x\n"
				"        a0=$%08x a1=$%08x a2=$%08x a3=$%08x a4=$%08x a5=$%08x a6=$%08x\n"
				"        stack: [0]=$%08x [1]=$%08x [2]=$%08x [3]=$%08x\n",
				kname, (unsigned)(k * 10), k, ++SCSITableCallCount[k],
				(unsigned)m68000_get_reg(M68K_PC), (unsigned)m68000_get_reg(M68K_SR), (unsigned)sp,
				(unsigned)m68000_get_reg(M68K_D0), (unsigned)m68000_get_reg(M68K_D1),
				(unsigned)m68000_get_reg(M68K_D2), (unsigned)m68000_get_reg(M68K_D3),
				(unsigned)m68000_get_reg(M68K_D4), (unsigned)m68000_get_reg(M68K_D5),
				(unsigned)m68000_get_reg(M68K_D6), (unsigned)m68000_get_reg(M68K_D7),
				(unsigned)m68000_get_reg(M68K_A0), (unsigned)m68000_get_reg(M68K_A1),
				(unsigned)m68000_get_reg(M68K_A2), (unsigned)m68000_get_reg(M68K_A3),
				(unsigned)m68000_get_reg(M68K_A4), (unsigned)m68000_get_reg(M68K_A5),
				(unsigned)m68000_get_reg(M68K_A6),
				(unsigned)cpu_readmem24_dword(sp), (unsigned)cpu_readmem24_dword(sp + 4),
				(unsigned)cpu_readmem24_dword(sp + 8), (unsigned)cpu_readmem24_dword(sp + 12));

			/* 要求ヘッダがどのレジスタで渡るかを特定するため、a0〜a6 のうち
			 * 0でなく $00ffffff 以下(ゲストRAM/ROM空間)のものは、指す先16バイトを
			 * 実際にダンプする。 */
			{
				static const int a_regs[7] = { M68K_A0, M68K_A1, M68K_A2, M68K_A3, M68K_A4, M68K_A5, M68K_A6 };
				static const char *a_names[7] = { "a0", "a1", "a2", "a3", "a4", "a5", "a6" };
				int ai;
				for (ai = 0; ai < 7; ai++)
				{
					uint32_t av = m68000_get_reg(a_regs[ai]);
					if (av != 0 && av <= 0x00ffffff)
					{
						log_cb(RETRO_LOG_INFO,
							"[SCSI] %s=$%08x: %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x\n",
							a_names[ai], (unsigned)av,
							cpu_readmem24(av + 0), cpu_readmem24(av + 1), cpu_readmem24(av + 2), cpu_readmem24(av + 3),
							cpu_readmem24(av + 4), cpu_readmem24(av + 5), cpu_readmem24(av + 6), cpu_readmem24(av + 7),
							cpu_readmem24(av + 8), cpu_readmem24(av + 9), cpu_readmem24(av + 10), cpu_readmem24(av + 11),
							cpu_readmem24(av + 12), cpu_readmem24(av + 13), cpu_readmem24(av + 14), cpu_readmem24(av + 15));
					}
				}
			}
		}

		/* k==0(ストラテジ): 要求ヘッダのアドレスを a5 から控える。
		 * k==1(インタラプト): 控えたアドレスをもとに要求ヘッダを処理する(HLE)。
		 * $41..$7f の他要素の観測経路(上のダンプ)はそのまま残す。 */
		if (k == 0)
		{
			SCSIReqHeaderAddr = m68000_get_reg(M68K_A5);
			if (log_cb)
				log_cb(RETRO_LOG_INFO,
					"[SCSI] ストラテジ: 要求ヘッダのアドレスを a5=$%08x として控えた\n",
					(unsigned)SCSIReqHeaderAddr);
		}
		else if (k == 1)
		{
			SCSI_HandleRequestHeader();
		}

		/* ストラテジ/インタラプトから戻る d0。既定 -1(何もしない・呼び出し時の
		 * 値のまま)。0以上のときだけ再ビルドせずに振れるようにしてある。
		 * 返答の中身をどう振っても起動が止まる件の残る候補のひとつ。 */
		if (k == 0 || k == 1)
		{
			/* コマンド単位の指定(SCSIReqReplyD0)があればそちらを優先する。 */
			int reply_d0 = (k == 1 && SCSIReqReplyD0 >= 0) ? SCSIReqReplyD0 : SCSIHostReplyD0;
			if (reply_d0 >= 0)
			{
				uint32_t before = m68000_get_reg(M68K_D0);
				m68000_set_reg(M68K_D0, (uint32_t)reply_d0);
				if (log_cb)
					log_cb(RETRO_LOG_INFO,
						"[SCSI] %s: d0 を $%08x -> $%08x に設定した\n",
						kname, (unsigned)before, (unsigned)reply_d0);
			}
		}
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

void SCSI_Cleanup(void)
{
	/* 実行終了時、保留中のまま埋もれていた圧縮エントリを最後にもう一度
	 * 「(継続中)」付きで吐き出しておく(ゲストが同じ値を延々ポーリング
	 * したまま終了した場合、直後のSCSI_BusLogFlush()による確定出力だけ
	 * では増分計算上は0件になり得るため、実際に読み書きが最後まで
	 * 続いていたことをこちらの行で示す)。 */
	SCSI_BusLogFlushPeriodic();
	webx68k_mem_read_watch_flush_periodic();
	SCSI_BusLogFlush();
	SCSI_BusReportExcluded();
	SCSI_BusReportPcDropped();
}

/* d2 が指すヘッダ構造体で、我々が用意した窓(64バイト)より先の
 * オフセットが読み書きされているのではないか、という仮説(a)(b)を
 * 一度に測るための「書ける窓」。$ea0100〜$ea07ff への書き込みを
 * 実際に SCSIIPL へ反映し、範囲外は従来どおり捨てる。
 * 本物ROM使用中はこの窓への書き込みも反映しない(ROMのまま)。
 * ログは $ea0000〜$ea1fff の全域を対象に [SCSI-BUS] へ一本化して取る
 * (詳細は SCSI_BusLog を参照)。 */
#define SCSI_WINDOW_LO 0x00ea0100
#define SCSI_WINDOW_HI 0x00ea0800	/* 排他的上限 ($ea07ff まで) */

/* ---- SPC(MB89352)レジスタの簡易状態(本物ROM使用時のみ有効) ----
 *
 * 実測(本物の外部SCSIボードROMをpx68kに読ませて観測)で、ROMは
 * $ea0001,03,05,...,$ea001f という「奇数番地」だけを叩いていた。
 * 「$ea0001+2n がn番目のレジスタ」という当てはめ、および各レジスタの
 * 役割(SCMD/INTS/PSNS/SSTS/TEMPなど、下の SCSI_SPC_ADR_* の名前)は
 * MB89352の一般知識からの当てはめであって実測ではない。番地そのもの
 * (どのオフセットに何回どんな値が書かれたか)だけが実測結果である。
 *
 * 自前スタブ経路(SCSIUsingRealRom==0)の挙動はここでは一切変えない。
 * SCSI_Write/SCSI_Read のどちらも、本物ROM使用時に限りこの配列を
 * 経由するよう分岐する(配列本体は SCSIUsingRealRom 付近で宣言)。 */

/* レジスタ名は実測ではなく当てはめ(上記コメント参照)。 */
#define SCSI_SPC_ADR_SCTL   0x00ea0003	/* コントロールレジスタ(当てはめ)。bit7=リセットと解釈(実測: 本物ROMが$90を書く) */
#define SCSI_SPC_ADR_SCMD   0x00ea0005	/* コマンドレジスタ。上位3bit=$001でセレクトと解釈(未実測の当てはめ)。上位3bit=$000はバス開放と解釈(未実測の当てはめ) */
#define SCSI_SPC_ADR_PSNS   0x00ea000b	/* フェーズ信号(当てはめ) */
#define SCSI_SPC_ADR_INTS   0x00ea0009	/* 割り込み要因(当てはめ) */
#define SCSI_SPC_ADR_SSTS   0x00ea000d	/* SPCステータス(当てはめ)。bit7=セレクト成立中(実測: 後述のSCSI_SpcSstsSetBit7コメント参照) */
#define SCSI_SPC_ADR_TEMP   0x00ea0017	/* テンポラリレジスタ(当てはめ)。0以外でイメージありとみなす */
#define SCSI_SPC_ADR_DREG   0x00ea0015	/* データレジスタ(当てはめ)。CDB/データ本体の1バイトずつの受け渡し口 */
#define SCSI_SPC_ADR_TCH    0x00ea0019	/* 転送カウンタ(当てはめ、上位バイトと仮定) */
#define SCSI_SPC_ADR_TCM    0x00ea001b	/* 転送カウンタ(当てはめ、中位バイトと仮定) */
#define SCSI_SPC_ADR_TCL    0x00ea001d	/* 転送カウンタ(当てはめ、下位バイトと仮定) */
#define SCSI_SPC_ADR_PCTL   0x00ea0011	/* パリティ制御レジスタ(当てはめ)。実測: 本物ROMが再試行の入口
					 * ($ea1030)でここへ$00を書いたあと $ea1034 でSSTSを読みに
					 * 来ており、bit7が立ったままだと $ea1038 で無限ループする
					 * (PCサンプラで実測)。この書き込みを「接続の終わり」とみなして
					 * SSTSのbit7を落とすのは実機の仕様ではなく、再試行のたびに
					 * 観測を1つ進めるための実験的な規則(SCSI_SpcWrite参照)。 */

/* adr($ea0000〜$ea001fのSPC領域内)を SCSISpcReg の添字に変換する。
 * 実測されたアクセスはすべて奇数番地だったため n=(adr-$ea0001)/2 とする。
 * 偶数番地(実測なし)は -1 を返し、呼び出し側は何もしない。 */
static int SCSI_SpcRegIndex(uint32_t adr)
{
	if (adr < SCSI_SPC_PORT_LO || adr >= SCSI_SPC_PORT_HI)
		return -1;
	if (!(adr & 1))
		return -1;
	return (int)((adr - (SCSI_SPC_PORT_LO + 1)) >> 1);
}

/* SSTS($ea000d) の bit7 を状態として管理する。
 *
 * 実測(本物SCSI ROMをpx68kに読ませ、SSTSの値を読み出しのたびに
 * 0→255と変える掃引で観測): pc=$ea1034 の読み出しはbit7が立っていると
 * 先へ進まず、pc=$ea109a の読み出しはbit7($80)が立った瞬間に抜けた。
 * この2点の実測から「SSTSのbit7はセレクトが成立して接続中であることを
 * 表す状態であり、固定値ではいけない」と読む(bit7の意味づけ自体は
 * 当てはめであり、実測として言えるのは上の2点のみ)。
 *
 * この状態機械が効くのは webx68k_scsi_spc_ssts() が既定値 -1 を返す
 * ときだけ。-2(従来どおりの掃引モード)は SCSI_SpcSweepRead が読み出す
 * たびに上書きするため無関係、0以上(固定値)は従来どおりホスト指定値を
 * そのまま使う。 */
static void SCSI_SpcSstsSetBit7Reason(int on, const char *reason)
{
	int idx = SCSI_SpcRegIndex(SCSI_SPC_ADR_SSTS);
	uint8_t before, after;

	if (SCSIHostSpcSsts != -1)
		return;	/* 固定値/掃引モードでは状態機械を使わない */

	before = SCSISpcReg[idx];
	after = on ? (uint8_t)(before | 0x80) : (uint8_t)(before & (uint8_t)~0x80);
	if (after == before)
		return;
	SCSISpcReg[idx] = after;

	if (log_cb && SCSI_BusPcAllow(m68000_get_reg(M68K_PC)) && SCSI_BusLogGate())
		log_cb(RETRO_LOG_INFO, "[SCSI-SPC] SSTS bit7=%d (状態機械, 理由=%s, pc=$%08x)\n",
			on ? 1 : 0, reason ? reason : "?", (unsigned)m68000_get_reg(M68K_PC));
}

/* DATAIN中に渡すべきバイトが残っている間、SSTSへ立てる当てはめのビット
 * (既定 $08、webx68k_scsi_spc_ssts_data_bit()参照)。bit7とは別にOR/AND-NOTする。
 * bit7の状態機械(SCSI_SpcSstsSetBit7Reason)と同じく、webx68k_scsi_spc_ssts()が
 * 既定(-1)のときだけ働かせる(固定値/掃引モードでは触らない)。 */
static void SCSI_SpcSstsSetDataBit(int on, const char *reason)
{
	int idx = SCSI_SpcRegIndex(SCSI_SPC_ADR_SSTS);
	int cfg = SCSIHostSpcSstsDataBit;
	uint8_t bit, before, after;

	if (SCSIHostSpcSsts != -1)
		return;	/* 固定値/掃引モードでは状態機械を使わない */
	if (cfg < 0)
		return;	/* -2(掃引)は読み出し時にSCSI_SpcSstsDataSweepReadが動的計算するため、
			 * ここでは登録済みレジスタへ固定ビットを立てない */

	bit = (uint8_t)cfg;
	before = SCSISpcReg[idx];
	after = on ? (uint8_t)(before | bit) : (uint8_t)(before & (uint8_t)~bit);
	if (after == before)
		return;
	SCSISpcReg[idx] = after;

	if (log_cb && SCSI_BusLogGate())
		log_cb(RETRO_LOG_INFO, "[SCSI-SPC] SSTS データビット($%02x)=%d (理由=%s, pc=$%08x)\n",
			bit, on ? 1 : 0, reason ? reason : "?", (unsigned)m68000_get_reg(M68K_PC));
}

/* TCが0のとき立てる当てはめのビット(既定$10、webx68k_scsi_spc_ssts_tc0_bit()
 * 参照)。データビットとは独立にOR/AND-NOTする。パルスにはしない(TCが
 * 残っている間は落としたまま、0になったら立てたままにする、という当てはめ)。 */
static void SCSI_SpcSstsSetTc0Bit(int on, const char *reason)
{
	int idx = SCSI_SpcRegIndex(SCSI_SPC_ADR_SSTS);
	int cfg = SCSIHostSpcSstsTc0Bit;
	uint8_t bit, before, after;

	if (SCSIHostSpcSsts != -1)
		return;	/* 固定値/掃引モードでは状態機械を使わない */
	if (cfg < 0)
		return;

	bit = (uint8_t)cfg;
	before = SCSISpcReg[idx];
	after = on ? (uint8_t)(before | bit) : (uint8_t)(before & (uint8_t)~bit);
	if (after == before)
		return;
	SCSISpcReg[idx] = after;

	if (log_cb && SCSI_BusLogGate())
		log_cb(RETRO_LOG_INFO, "[SCSI-SPC] SSTS TC0ビット($%02x)=%d (理由=%s, pc=$%08x)\n",
			bit, on ? 1 : 0, reason ? reason : "?", (unsigned)m68000_get_reg(M68K_PC));
}


/* セレクト(SCMD書き込みで上位3bitが$001)への応答。
 * 「TEMPが webx68k_scsi_spc_target() の値と一致し、かつホスト側に
 * イメージがある」を成功条件とする(これも実測ではなく仮説)。
 * 2026-09-02: 従来は「TEMPが0以外」だけを条件にしていたため、どのIDを
 * 選んでも必ず成功し、ROMからは同じディスクが8台見えていた(欠陥)。
 * 応答する相手を1つに絞るため、TEMPの値が webx68k_scsi_spc_target()
 * と一致したときだけ成功にする、はずだったが、実測ではROM内蔵ルーチンが
 * TEMP=$07、RAM上で動くルーチン(pc=$0001cd8c)がTEMP=$0fを使っており、
 * 片方だけに固定するともう片方が通らない(RAM側はセレクトを約14,800回
 * 再試行し6分超かかった)。そのため target が負値(既定-1)のときは
 * TEMPの値によらず常に成功させる(「どれでも」応答する)ようにしてある。
 * 成功/タイムアウトのどちらのビットを INTS に立てるかは再ビルドせず
 * ホストから振れる(webx68k_scsi_spc_ints_sel/timeout、既定 $08/$20)。
 * 成功時は SSTS/PSNS にもホスト指定値を反映する。SSTSは既定(-1)なら
 * SCSI_SpcSstsSetBit7 の状態機械に任せ、それ以外(固定値/掃引)は
 * 従来どおりの扱い。 */
/* SCSI_SpcSelectCheck の成否を、後段(SCSI_SpcWrite)から見えるようにする置き場。
 * COMMANDフェーズへ入るのはSetPhase(定義は下の状態機械ブロック)経由なので、
 * SelectCheck自体はこのフラグを立てるだけにする。 */
static int SCSISpcSelectOk = 0;

static void SCSI_SpcSelectCheck(uint8_t scmd)
{
	int idx_ints = SCSI_SpcRegIndex(SCSI_SPC_ADR_INTS);
	int idx_temp = SCSI_SpcRegIndex(SCSI_SPC_ADR_TEMP);
	uint8_t temp;
	int size;
	int target;
	int ok;
	uint8_t ints_bit;

	if ((scmd & 0xe0) != 0x20)
		return;	/* セレクト以外のコマンドとみなし何もしない */

	temp = SCSISpcReg[idx_temp];
	size = webx68k_scsi_get_size();
	target = SCSIHostSpcTarget;
	/* target<0 は「どのTEMP値でも成功」の意味(既定)。実測でROM/RAM双方の
	 * ルーチンが別々のTEMP値を使うため、固定値1つに絞ると片方が通らない。 */
	ok = ((target < 0 || (int)temp == target) && size > 0);
	SCSISpcSelectOk = ok;

	ints_bit = (uint8_t)(ok ? SCSIHostSpcIntsSel : SCSIHostSpcIntsTimeout);
	SCSISpcReg[idx_ints] |= ints_bit;

	/* この診断ログも [SCSI-BUS] と共通の上限(SCSI_BusLogGate)・PC単位の
	 * 抑制(SCSI_BusPcAllow)を通す。本物ROMがセレクト失敗→SSTS再読出し→
	 * 別IDでセレクトやり直し、を延々繰り返すケースでここが無制限に
	 * 出続けると容易にログが数十MBへ膨らむ(実測)ため。 */
	if (log_cb && SCSI_BusPcAllow(m68000_get_reg(M68K_PC)) && SCSI_BusLogGate())
		log_cb(RETRO_LOG_INFO,
			"[SCSI-SPC] セレクト scmd=$%02x TEMP=$%02x 期待=%d size=%d -> %s ints|=$%02x\n",
			scmd, temp, target, size, ok ? "成功" : "タイムアウト", ints_bit);

	if (ok)
	{
		int ssts = SCSIHostSpcSsts;
		int psns = SCSIHostSpcPsns;
		if (ssts == -1)
			SCSI_SpcSstsSetBit7Reason(1, "セレクト成立");	/* 既定は状態機械: セレクト成立でbit7を立てる */
		else if (ssts >= 0)
			SCSISpcReg[SCSI_SpcRegIndex(SCSI_SPC_ADR_SSTS)] = (uint8_t)ssts;
		if (psns >= 0)
			SCSISpcReg[SCSI_SpcRegIndex(SCSI_SPC_ADR_PSNS)] = (uint8_t)psns;
		if (log_cb && (ssts >= 0 || psns >= 0) &&
			SCSI_BusPcAllow(m68000_get_reg(M68K_PC)) && SCSI_BusLogGate())
			log_cb(RETRO_LOG_INFO, "[SCSI-SPC] セレクト成功でssts=$%02x psns=$%02x を設定\n",
				(unsigned)SCSISpcReg[SCSI_SpcRegIndex(SCSI_SPC_ADR_SSTS)],
				(unsigned)SCSISpcReg[SCSI_SpcRegIndex(SCSI_SPC_ADR_PSNS)]);
	}
}

/* ---- SPCの転送状態機械(COMMAND/DATAIN/STATUS/MSGIN) ----
 *
 * 実測(本物SCSI ROMの観測): セレクト成立後、ROMは
 *   PCTL($ea0011)=$02, TEMP($ea0017)=$00, SCMD($ea0005)=$ec(上位3bit=111)
 * の順に書いたあと、pc=$ea144a で PSNS($ea000b) をポーリングし続ける。
 * 固定値では抜けず、2値($8a と $0a)を交互に返すと抜けた
 * (=ハンドシェイク待ちである、という実測)。
 *
 * ここから先の「COMMAND/DATAIN/STATUS/MSGINという名前」「フェーズビットの
 * 値($02/$01/$03/$07)」「REQ($80)や接続中($08)の解釈」「DREGという名前」
 * 「CDB長の判定規則(先頭バイトで6/10バイト)」は、いずれもSCSI一般知識
 * からの当てはめであり実測ではない。実測として言えるのは冒頭の書き込み
 * 順序とPSNSが2値を交互に返すと抜ける、という2点のみ。
 *
 * この状態機械が働くのは webx68k_scsi_spc_psns() が既定値(-1)のときだけ。
 * -2(掃引)/-3(交互)/0以上(固定)のときは、従来どおりそちらを優先し、
 * この状態機械は一切関与しない(SCSI_SpcSweepRead 側の分岐、および
 * SCSI_SpcWrite 内の呼び出し箇所を参照)。
 *
 * ログは [SCSI-BUS] と共通の総件数上限(SCSI_BusLogGate)は掛けるが、
 * 「同一PCから32件で以後抑制」する圧縮(SCSI_BusPcAllow)は掛けない。
 * COMMAND/DATAINの1バイトずつの書き込み・読み出しはほぼ同一PCから
 * 連続して起きるため、そちらを掛けると観測したい中身が消えてしまう
 * (掃引/交互モードのログが同じ理由でSCSI_BusPcAllowを回避しているのと
 * 同様の判断)。その代わりDREGの1バイトログ自体は先頭300件で打ち切り、
 * 以後は件数のみ数える。 */

typedef enum {
	SCSI_SPC_PHASE_BUSFREE = 0,
	SCSI_SPC_PHASE_COMMAND,
	SCSI_SPC_PHASE_DATAIN,
	SCSI_SPC_PHASE_STATUS,
	SCSI_SPC_PHASE_MSGIN
} SCSI_SpcPhase_t;

static SCSI_SpcPhase_t SCSISpcPhase = SCSI_SPC_PHASE_BUSFREE;

/* CDB(コマンド記述子)を最大16バイトまで溜める。 */
static uint8_t SCSISpcCdb[16];
static int SCSISpcCdbLen = 0;		/* ここまでに受け取った本数 */
static int SCSISpcCdbExpected = 0;	/* 先頭バイトから決めた予定本数(6 or 10)。0=未確定 */

/* DATAIN応答データのバッファ。64KB(=128セクタ)まで。実測目的の範囲では
 * 十分大きいはずだが、それを超える要求が来た場合は切り詰めてログに残す。 */
#define SCSI_SPC_DATA_BUF_MAX (64 * 1024)
static uint8_t SCSISpcDataBuf[SCSI_SPC_DATA_BUF_MAX];
static int SCSISpcDataLen = 0;
static int SCSISpcDataPos = 0;

/* REQのハンドシェイク用: DREGへの1バイトアクセスが起きるたびに1を立て、
 * 「次のPSNS読み出し」だけREQを落とし、その時点で0に戻す(=その次からは
 * また立つ)。SCSI_SpcPhasePsns 参照。 */
static int SCSISpcReqLow = 0;

/* SSTSデータビットの「パルス」化用(2026-09-02の実測: 固定値では通らず、
 * 掃引(-2)でだけ通った→「値が変わること」自体を見ているハンドシェイクだと
 * 判明)。REQと同じ作りで、DREGへの1バイト読み出しが起きるたびに1を立て、
 * 「次のSSTS読み出し」だけデータビットを落とし、その時点で0に戻す
 * (=その次からはまた立つ)。SCSI_SpcSstsPulseRead 参照。 */
static int SCSISpcSstsDataBitLow = 0;

/* 転送カウンタ(TC)の実体。SCSI_SpcXferStartData(SCMD上位3bit=100)で
 * TCH/TCM/TCLから読み取った値を初期値とし、DATAINで1バイト渡すたびに
 * 1減らす。TCH/TCM/TCLレジスタそのものは書き戻さない(実測されていない
 * ため当てはめで動かすのはリスクが高い)。0になったらSSTSのTC0ビット
 * (当てはめ)を立てる。 */
static int SCSISpcXferTc = 0;

/* DREGのバイトログの打ち切り(先頭300件、以後は件数のみ)。 */
#define SCSI_SPC_DREG_LOG_MAX 300
static int SCSISpcDregTotal = 0;

static int SCSI_SpcDregLogGate(void)
{
	int allow = (SCSISpcDregTotal < SCSI_SPC_DREG_LOG_MAX);
	SCSISpcDregTotal++;
	if (SCSISpcDregTotal == SCSI_SPC_DREG_LOG_MAX && log_cb)
		log_cb(RETRO_LOG_INFO,
			"[SCSI-SPC] DREGのバイトログは%d件で打ち切り、以後は件数のみ集計する\n",
			SCSI_SPC_DREG_LOG_MAX);
	return allow;
}

static const char *SCSI_SpcPhaseName(SCSI_SpcPhase_t phase)
{
	switch (phase)
	{
	case SCSI_SPC_PHASE_BUSFREE: return "BUSFREE";
	case SCSI_SPC_PHASE_COMMAND: return "COMMAND";
	case SCSI_SPC_PHASE_DATAIN:  return "DATAIN";
	case SCSI_SPC_PHASE_STATUS:  return "STATUS";
	case SCSI_SPC_PHASE_MSGIN:   return "MSGIN";
	default: return "?";
	}
}

/* CDB(先頭 SCSISpcCdbLen バイト、最大16)を16進文字列にしてログへ出す。 */
static void SCSI_SpcLogCdbHex(const char *label)
{
	char buf[16 * 3 + 1];
	int i, n;
	static const char hex[] = "0123456789abcdef";

	if (!log_cb || !SCSI_BusLogGate())
		return;

	n = SCSISpcCdbLen;
	if (n > 16)
		n = 16;
	for (i = 0; i < n; i++)
	{
		uint8_t b = SCSISpcCdb[i];
		buf[i * 3 + 0] = hex[(b >> 4) & 0xf];
		buf[i * 3 + 1] = hex[b & 0xf];
		buf[i * 3 + 2] = ' ';
	}
	buf[(n > 0) ? (n * 3 - 1) : 0] = '\0';

	log_cb(RETRO_LOG_INFO, "[SCSI-SPC] %s (len=%d): %s\n", label, n, buf);
}

/* COMMANDフェーズのPSNSフェーズビット値。既定(-1)は当てはめの $02。
 * webx68k_scsi_spc_phase_bits() で再ビルドせず振れる。 */
static uint8_t SCSI_SpcPhaseBitsFor(SCSI_SpcPhase_t phase)
{
	switch (phase)
	{
	case SCSI_SPC_PHASE_COMMAND:
	{
		int ov = SCSIHostSpcPhaseBits;
		return (uint8_t)((ov >= 0) ? (ov & 0xff) : 0x02);
	}
	case SCSI_SPC_PHASE_DATAIN: return 0x01;
	case SCSI_SPC_PHASE_STATUS: return 0x03;
	case SCSI_SPC_PHASE_MSGIN:  return 0x07;
	default: return 0x00;	/* BUSFREE(当てはめ: フェーズビット無し) */
	}
}

/* PSNS読み出し値の組み立て(状態機械が有効なときのみ呼ばれる)。
 * (REQが立っていれば$80) | $08(接続中) | フェーズのビット、という当てはめ。
 * REQは「DREGアクセス直後の1回だけ落ち、その次からまた立つ」というパルス
 * として実装する(SCSISpcReqLow参照)。
 *
 * 「状態機械のつもりで返していない」事故を切り分けるため、返す直前の値を
 * phase/REQの状態つきで先頭50件だけログへ出す(以後は出さない)。 */
#define SCSI_SPC_PSNS_PHASE_LOG_MAX 50
static int SCSISpcPsnsPhaseLogCount = 0;

static uint8_t SCSI_SpcPhasePsns(void)
{
	uint8_t v;
	int req_low_pending = SCSISpcReqLow;	/* ログ用: このアクセスでREQを落とすかどうか(消費前の値) */

	if (SCSISpcPhase == SCSI_SPC_PHASE_BUSFREE)
	{
		v = 0x00;	/* 接続していない(当てはめ) */
	}
	else
	{
		v = (uint8_t)(0x08 | SCSI_SpcPhaseBitsFor(SCSISpcPhase));
		if (!SCSISpcReqLow)
			v |= 0x80;
		else
			SCSISpcReqLow = 0;	/* 1回落としたら次からまた立てる */
	}

	if (log_cb && SCSI_BusLogGate() && SCSISpcPsnsPhaseLogCount < SCSI_SPC_PSNS_PHASE_LOG_MAX)
	{
		SCSISpcPsnsPhaseLogCount++;
		log_cb(RETRO_LOG_INFO,
			"[SCSI-SPC] PSNS読み出し(状態機械) phase=%s reqLow消費=%d -> $%02x (pc=$%08x)%s\n",
			SCSI_SpcPhaseName(SCSISpcPhase), req_low_pending, (unsigned)v,
			(unsigned)m68000_get_reg(M68K_PC),
			(SCSISpcPsnsPhaseLogCount == SCSI_SPC_PSNS_PHASE_LOG_MAX) ? " (以後この行は抑制)" : "");
	}

	return v;
}

/* フェーズ遷移。転送完了([SCSI-SPC]の意味でSTATUSに入った時点、当てはめ)で
 * INTSへwebx68k_scsi_spc_ints_xfer()のビットを立て、切断(BUSFREEへ入った時点)で
 * SSTSのbit7を落としINTSへwebx68k_scsi_spc_ints_disc()のビットを立てる。
 * どちらも当てはめであり実測ではない。 */
static void SCSI_SpcSetPhase(SCSI_SpcPhase_t phase, const char *reason)
{
	SCSI_SpcPhase_t old = SCSISpcPhase;
	SCSISpcPhase = phase;

	if (log_cb && SCSI_BusLogGate())
		log_cb(RETRO_LOG_INFO, "[SCSI-SPC] フェーズ遷移 %s -> %s (理由=%s)\n",
			SCSI_SpcPhaseName(old), SCSI_SpcPhaseName(phase), reason ? reason : "?");

	if (phase == SCSI_SPC_PHASE_STATUS && old != SCSI_SPC_PHASE_STATUS)
	{
		int idx_ints = SCSI_SpcRegIndex(SCSI_SPC_ADR_INTS);
		uint8_t bit = (uint8_t)SCSIHostSpcIntsXfer;
		SCSISpcReg[idx_ints] |= bit;
		if (log_cb && SCSI_BusLogGate())
			log_cb(RETRO_LOG_INFO, "[SCSI-SPC] 転送完了(当てはめ) INTS|=$%02x\n", bit);
	}

	if (phase == SCSI_SPC_PHASE_BUSFREE)
	{
		int idx_ssts = SCSI_SpcRegIndex(SCSI_SPC_ADR_SSTS);
		int idx_ints = SCSI_SpcRegIndex(SCSI_SPC_ADR_INTS);
		uint8_t bit = (uint8_t)SCSIHostSpcIntsDisc;
		SCSISpcReg[idx_ssts] &= (uint8_t)~0x80;
		SCSISpcReg[idx_ints] |= bit;
		if (log_cb && SCSI_BusLogGate())
			log_cb(RETRO_LOG_INFO, "[SCSI-SPC] 切断(当てはめ) SSTS bit7=0, INTS|=$%02x\n", bit);
	}
}

/* CDBが確定したところでコマンドを解釈し、応答を組み立てる。
 * 対応コマンドはTEST UNIT READY($00)/REZERO UNIT($01)/REQUEST SENSE($03)/
 * INQUIRY($12)/MODE SENSE($1a)/START-STOP UNIT($1b)/READ CAPACITY($25)/
 * READ(6)($08)/READ(10)($28)。それ以外はデータ無しでSTATUSへ進め、
 * ログに「未対応」と明示する(見落とし防止)。
 * 応答データがあればDATAINへ、無ければ直接STATUSへ遷移する。 */
static void SCSI_SpcHandleCommand(void)
{
	uint8_t op = SCSISpcCdb[0];

	SCSISpcDataLen = 0;
	SCSISpcDataPos = 0;

	switch (op)
	{
	case 0x00:	/* TEST UNIT READY */
		if (log_cb && SCSI_BusLogGate())
			log_cb(RETRO_LOG_INFO, "[SCSI-SPC] コマンド=TEST UNIT READY -> データ無し\n");
		break;

	case 0x01:	/* REZERO UNIT: データ無しで正常終了 */
		if (log_cb && SCSI_BusLogGate())
			log_cb(RETRO_LOG_INFO, "[SCSI-SPC] コマンド=REZERO UNIT -> データ無し\n");
		break;

	case 0x1b:	/* START-STOP UNIT: データ無しで正常終了 */
		if (log_cb && SCSI_BusLogGate())
			log_cb(RETRO_LOG_INFO, "[SCSI-SPC] コマンド=START-STOP UNIT -> データ無し\n");
		break;

	case 0x03:	/* REQUEST SENSE: 18バイト、先頭$70(現行エラー無し)、残りゼロ */
	{
		uint8_t *b = SCSISpcDataBuf;
		memset(b, 0x00, 18);
		b[0] = 0x70;
		SCSISpcDataLen = 18;
		if (log_cb && SCSI_BusLogGate())
			log_cb(RETRO_LOG_INFO, "[SCSI-SPC] コマンド=REQUEST SENSE -> 18バイト応答(エラー無し)\n");
		break;
	}

	case 0x12:	/* INQUIRY: 36バイト固定応答。CDBバイト4(割り当て長)がそれより
			 * 小さければその長さに切る。 */
	{
		uint8_t *b = SCSISpcDataBuf;
		unsigned int alloc = SCSISpcCdb[4];
		unsigned int len = 36;
		memset(b, 0x20, 36);
		b[0] = 0x00;	/* 直接アクセスデバイス */
		b[1] = 0x00;
		b[2] = 0x02;
		b[3] = 0x02;
		b[4] = 0x1f;
		memcpy(b + 8, "WebX68k ", 8);
		memcpy(b + 16, "SCSI DISK       ", 16);
		memcpy(b + 32, "1.0 ", 4);
		if (alloc > 0 && alloc < len)
			len = alloc;
		SCSISpcDataLen = (int)len;
		if (log_cb && SCSI_BusLogGate())
			log_cb(RETRO_LOG_INFO, "[SCSI-SPC] コマンド=INQUIRY 割当長=%u -> %uバイト応答\n",
				alloc, len);
		break;
	}

	case 0x1a:	/* MODE SENSE(6): 最小限の応答(当てはめ)。
			 * ブロック記述子つきで、データ長/媒体種別($00)/装置固有($00)/
			 * ブロック記述子長($08)、続けてブロック記述子
			 * (密度コード$00 + ブロック数3バイト(当てはめで0) + 予約1バイト
			 * + ブロック長3バイト=$000200)を返す。CDBバイト4(割り当て長)を
			 * 超えないよう切る。 */
	{
		uint8_t *b = SCSISpcDataBuf;
		unsigned int alloc = SCSISpcCdb[4];
		unsigned int len = 12;
		memset(b, 0x00, len);
		b[0] = 0x0b;	/* モードデータ長(この後に続くバイト数) */
		b[1] = 0x00;	/* 媒体種別 */
		b[2] = 0x00;	/* 装置固有パラメータ */
		b[3] = 0x08;	/* ブロック記述子長 */
		b[4] = 0x00;	/* 密度コード */
		b[5] = 0x00; b[6] = 0x00; b[7] = 0x00;	/* ブロック数(当てはめで0) */
		b[8] = 0x00;	/* 予約 */
		b[9] = 0x00; b[10] = 0x02; b[11] = 0x00;	/* ブロック長=512 */
		if (alloc > 0 && alloc < len)
			len = alloc;
		SCSISpcDataLen = (int)len;
		if (log_cb && SCSI_BusLogGate())
			log_cb(RETRO_LOG_INFO, "[SCSI-SPC] コマンド=MODE SENSE 割当長=%u -> %uバイト応答(当てはめ)\n",
				alloc, len);
		break;
	}

	case 0x25:	/* READ CAPACITY(10): 8バイト(最終LBA + ブロック長512) */
	{
		int size = webx68k_scsi_get_size();
		unsigned int last_lba = (size > 512) ? ((unsigned int)(size / 512) - 1) : 0;
		uint8_t *b = SCSISpcDataBuf;
		b[0] = (uint8_t)((last_lba >> 24) & 0xff);
		b[1] = (uint8_t)((last_lba >> 16) & 0xff);
		b[2] = (uint8_t)((last_lba >> 8) & 0xff);
		b[3] = (uint8_t)(last_lba & 0xff);
		b[4] = 0x00;
		b[5] = 0x00;
		b[6] = 0x02;
		b[7] = 0x00;
		SCSISpcDataLen = 8;
		if (log_cb && SCSI_BusLogGate())
			log_cb(RETRO_LOG_INFO,
				"[SCSI-SPC] コマンド=READ CAPACITY size=%d -> last_lba=%u block=512\n",
				size, last_lba);
		break;
	}

	case 0x08:	/* READ(6) */
	case 0x28:	/* READ(10) */
	{
		unsigned int lba, count, i, bytes;
		int ok = 1;

		if (op == 0x08)
		{
			lba = ((unsigned int)(SCSISpcCdb[1] & 0x1f) << 16)
				| ((unsigned int)SCSISpcCdb[2] << 8) | SCSISpcCdb[3];
			count = (SCSISpcCdb[4] == 0) ? 256u : SCSISpcCdb[4];
		}
		else
		{
			lba = ((unsigned int)SCSISpcCdb[2] << 24) | ((unsigned int)SCSISpcCdb[3] << 16)
				| ((unsigned int)SCSISpcCdb[4] << 8) | SCSISpcCdb[5];
			count = ((unsigned int)SCSISpcCdb[7] << 8) | SCSISpcCdb[8];
		}

		bytes = count * 512u;
		if (bytes > SCSI_SPC_DATA_BUF_MAX)
		{
			if (log_cb && SCSI_BusLogGate())
				log_cb(RETRO_LOG_INFO,
					"[SCSI-SPC] READ(%d) lba=%u count=%u はバッファ上限(%dバイト)を超えるため切り詰める\n",
					(op == 0x08) ? 6 : 10, lba, count, SCSI_SPC_DATA_BUF_MAX);
			count = SCSI_SPC_DATA_BUF_MAX / 512u;
			bytes = count * 512u;
		}

		if (log_cb && SCSI_BusLogGate())
			log_cb(RETRO_LOG_INFO, "[SCSI-SPC] コマンド=READ(%d) lba=%u count=%u\n",
				(op == 0x08) ? 6 : 10, lba, count);

		for (i = 0; i < count; i++)
		{
			if (webx68k_scsi_read_sector(lba + i, SCSISpcDataBuf + i * 512) != 0)
			{
				ok = 0;
				break;
			}
		}

		if (!ok)
		{
			if (log_cb && SCSI_BusLogGate())
				log_cb(RETRO_LOG_INFO, "[SCSI-SPC] READ セクタ読み出しに失敗(lba=%u)\n", lba + i);
			bytes = 0;
		}
		SCSISpcDataLen = (int)bytes;
		break;
	}

	default:
		if (log_cb && SCSI_BusLogGate())
			log_cb(RETRO_LOG_INFO, "[SCSI-SPC] 未対応のコマンド op=$%02x -> データ無し\n", op);
		break;
	}

	if (SCSISpcDataLen > 0)
		SCSI_SpcSetPhase(SCSI_SPC_PHASE_DATAIN, "コマンド応答データあり");
	else
		SCSI_SpcSetPhase(SCSI_SPC_PHASE_STATUS, "コマンド応答データ無し");
}

/* DREG($ea0015)への書き込み。COMMANDフェーズ中のみ意味を持ち、CDBの
 * 1バイトとして溜める。SCSIのCDB長は先頭バイトで決まる
 * ($00〜$1fは6バイト、$20〜$5fは10バイト。これは知識であり、
 * 実測で確かめる対象である)。6バイト受け取った時点で、CDB全体を
 * (予定が6バイトでも10バイトでも)いったんログへ出す。 */
/* CDBの1バイトを受け取ったときの共通処理。srcはログ表示用のラベル
 * ("DREG" または "TEMP,仮説")で、口が2つ(DREG/TEMP)あることをログで
 * 区別できるようにする(下のコメント、および webx68k_scsi_spc_cdb_from_temp
 * 参照)。 */
static void SCSI_SpcCommandByteGeneric(uint8_t data, const char *src)
{
	int allow_log = SCSI_SpcDregLogGate();

	if (SCSISpcCdbLen < 16)
		SCSISpcCdb[SCSISpcCdbLen] = data;
	SCSISpcCdbLen++;

	if (SCSISpcCdbLen == 1)
	{
		if (data <= 0x1f)
			SCSISpcCdbExpected = 6;
		else if (data <= 0x5f)
			SCSISpcCdbExpected = 10;
		else
		{
			SCSISpcCdbExpected = 6;	/* 未知のグループ。とりあえず6バイトと仮定する(当てはめ) */
			if (log_cb && SCSI_BusLogGate())
				log_cb(RETRO_LOG_INFO,
					"[SCSI-SPC] 未知のCDBグループ op=$%02x (6バイトと仮定、当てはめ)\n", data);
		}
	}

	if (log_cb && allow_log && SCSI_BusLogGate())
		log_cb(RETRO_LOG_INFO, "[SCSI-SPC] COMMAND書き込み(経路=%s) n=%d/%d data=$%02x\n",
			src, SCSISpcCdbLen, SCSISpcCdbExpected, data);

	if (SCSISpcCdbLen == 6)
		SCSI_SpcLogCdbHex("CDB(6バイト時点)");

	SCSISpcReqLow = 1;	/* このアクセス直後の1回だけREQを落とし、その次からまた立てる */

	if (SCSISpcCdbExpected > 0 && SCSISpcCdbLen >= SCSISpcCdbExpected)
	{
		if (SCSISpcCdbExpected != 6)
			SCSI_SpcLogCdbHex("CDB確定");
		SCSI_SpcHandleCommand();
	}
}

/* DREG($ea0015)への書き込み経由でCDBの1バイトを受け取る(当初の当てはめ)。 */
static void SCSI_SpcCommandByte(uint8_t data)
{
	SCSI_SpcCommandByteGeneric(data, "DREG");
}

/* TEMP($ea0017)経由でCDBの1バイトを受け取る(2026-09-02の仮説、未実測)。
 * 実測の並びが「PCTL=$02 書き込み→PSNS読み出し→TEMPへの1バイト書き込み→
 * SCMD上位3bit=111(転送コマンド)書き込み→pc=$ea144aでPSNSをポーリング」
 * の繰り返しに見えたため、「転送コマンドの書き込みそのものが、直前に
 * TEMPへ書かれた1バイトをCDBとして送った合図である」という仮説を立てて
 * 実装する。webx68k_scsi_spc_cdb_from_temp()が真(既定)のときだけ、
 * SCSI_SpcXferStart から呼ばれる。 */
static void SCSI_SpcCommandByteFromTemp(void)
{
	uint8_t data = SCSISpcReg[SCSI_SpcRegIndex(SCSI_SPC_ADR_TEMP)];
	SCSI_SpcCommandByteGeneric(data, "TEMP,仮説");
}

/* DATAIN/STATUS/MSGINフェーズで、こちら(デバイス側)からホストへ渡す
 * 次の1バイトを取り出す共通処理。srcはログ表示用のラベルで、
 * 「DREGの読み出しで来た」のか「SCMD書き込み(転送コマンド)がバイト
 * 授受の合図、という仮説の経路で来た」のかを区別できるようにする
 * (詳細は SCSI_SpcXferStart のコメント参照)。
 * STATUS/MSGINは仕様どおり(当てはめ)常に1バイト$00とする。 */
static uint8_t SCSI_SpcPhaseOutputByte(const char *src)
{
	uint8_t v = 0x00;
	int allow_log = SCSI_SpcDregLogGate();

	switch (SCSISpcPhase)
	{
	case SCSI_SPC_PHASE_DATAIN:
		if (SCSISpcDataPos < SCSISpcDataLen)
		{
			v = SCSISpcDataBuf[SCSISpcDataPos++];
			if (SCSISpcXferTc > 0)
				SCSISpcXferTc--;	/* TCを実際に減らす(当てはめ、TCH/TCM/TCLレジスタ自体は書き戻さない) */
		}
		if (log_cb && allow_log && SCSI_BusLogGate())
			log_cb(RETRO_LOG_INFO, "[SCSI-SPC] DATAIN受け渡し(経路=%s) pos=%d/%d TC残り=%d data=$%02x\n",
				src, SCSISpcDataPos, SCSISpcDataLen, SCSISpcXferTc, v);
		SCSISpcReqLow = 1;
		if (SCSISpcXferTc <= 0)
		{
			/* TCが0(=渡すべきバイトが残っていない)になった。SSTSの
			 * データビット(当てはめ)を落としTC0ビット(当てはめ)を立てる。
			 * INTS|=ints_xferはSetPhaseのSTATUS入場処理に任せてSTATUSへ
			 * 進む。 */
			SCSI_SpcSstsSetDataBit(0, "DATAIN完了");
			SCSI_SpcSstsSetTc0Bit(1, "TC=0");
			SCSI_SpcSetPhase(SCSI_SPC_PHASE_STATUS, "DATAIN完了(TC=0)");
		}
		else
		{
			/* 実測(2026-09-02): SSTSデータビットは固定値では通らず、
			 * 掃引(-2)でだけ通った→「値が変わること」自体を見ている
			 * ハンドシェイクだった。REQと同じパルス化: 1バイト渡した
			 * 直後の1回だけSSTS読み出しでデータビットを落とし、その次
			 * からまた立てる(SCSI_SpcSstsPulseRead参照)。 */
			SCSISpcSstsDataBitLow = 1;
		}
		break;

	case SCSI_SPC_PHASE_STATUS:
		v = 0x00;
		if (log_cb && allow_log && SCSI_BusLogGate())
			log_cb(RETRO_LOG_INFO, "[SCSI-SPC] STATUS受け渡し(経路=%s) data=$00\n", src);
		SCSISpcReqLow = 1;
		SCSI_SpcSetPhase(SCSI_SPC_PHASE_MSGIN, "STATUS完了");
		break;

	case SCSI_SPC_PHASE_MSGIN:
		v = 0x00;
		if (log_cb && allow_log && SCSI_BusLogGate())
			log_cb(RETRO_LOG_INFO, "[SCSI-SPC] MSGIN受け渡し(経路=%s) data=$00\n", src);
		SCSISpcReqLow = 1;
		SCSI_SpcSetPhase(SCSI_SPC_PHASE_BUSFREE, "MSGIN完了(切断)");
		break;

	default:
		break;
	}

	return v;
}


/* SCMD($ea0005)の上位3bitが$111(=$e0)で書かれたら転送開始とみなす
 * (未実測の当てはめ)。TCH/TCM/TCL(実測: どの並びかは不明)から転送予定
 * バイト数を組み立て、両方の並びをログへ出す。COMMANDフェーズへ入り
 * REQを立てる(=ROMが最初のCDBバイトを送れる状態にする)。 */
/* COMMANDフェーズへ入る共通処理(CDBバッファをリセットし、REQを立てる)。
 * セレクト成立の直後(SCSI_SpcWrite参照)と、転送開始(SCMD上位3bit=111)の
 * どちらからも呼ばれる。 */
static void SCSI_SpcEnterCommandPhase(const char *reason)
{
	SCSISpcCdbLen = 0;
	SCSISpcCdbExpected = 0;
	SCSISpcDataLen = 0;
	SCSISpcDataPos = 0;
	SCSISpcReqLow = 0;	/* 開始直後はREQを立てる(当てはめ): 最初のCDBバイトを送らせる */

	SCSI_SpcSetPhase(SCSI_SPC_PHASE_COMMAND, reason);
}

/* SCMD上位3bit=111(転送コマンド)が書かれたときの処理。
 *
 * 実測(2026-09-02、その1): 状態機械導入後、ROMはセレクト成立直後から
 * COMMANDフェーズのPSNS($8a)を読み続けるだけでDREG($ea0015)には一切書かず、
 * 代わりに「PCTL=$02書き込み→PSNS読み出し→TEMP($ea0017)へ1バイト
 * 書き込み→SCMD上位3bit=111書き込み→pc=$ea144aでPSNSポーリング」という
 * 並びを繰り返していた。ここから「CDBはDREGではなくTEMP経由で1バイト
 * ずつ送られ、転送コマンドの書き込み自体がその合図である」という仮説を
 * 立てた(未実測、webx68k_scsi_spc_cdb_from_temp()で切り替え可能)。
 * 実測(2026-09-02、その2): この仮説どおりCDBが6バイト届いてSTATUSへ
 * 進んだあとも、ROMは同じ並び(PCTL=$03書き込み→PSNS読み出し→転送コマンド
 * 書き込み→pc=$ea1486でPSNSポーリング)を繰り返した。これを見て、
 * 「転送コマンドの書き込みがバイト授受の合図」という仮説をCOMMAND以外の
 * DATAIN/STATUS/MSGINにも広げる(いずれも未実測の当てはめ)。
 *
 * webx68k_scsi_spc_cdb_from_temp() が真(既定)のときは、
 *   - COMMANDフェーズ中: 転送コマンド書き込みをTEMP経由のCDBバイト到着として扱う。
 *   - DATAIN/STATUS/MSGINフェーズ中: 応答バッファの次の1バイトを取り出し、
 *     ROMがどちらを読むか未確定なため TEMP($ea0017) と DREG($ea0015) の
 *     両方の保持値へ同じ値を置く(実際にどちらを読んだかは[SCSI-BUS]の
 *     生ログから後で分かる)。
 * 偽(0)のときは従来どおり(このアクセス自体でCOMMANDフェーズへ入り直す
 * だけで、CDB/応答データはいずれもDREGへのアクセスを待つ)。 */
static void SCSI_SpcXferStart(void)
{
	uint8_t b_tch = SCSISpcReg[SCSI_SpcRegIndex(SCSI_SPC_ADR_TCH)];
	uint8_t b_tcm = SCSISpcReg[SCSI_SpcRegIndex(SCSI_SPC_ADR_TCM)];
	uint8_t b_tcl = SCSISpcReg[SCSI_SpcRegIndex(SCSI_SPC_ADR_TCL)];
	unsigned int as_tch_tcm_tcl = ((unsigned int)b_tch << 16) | ((unsigned int)b_tcm << 8) | b_tcl;
	unsigned int as_tcl_tcm_tch = ((unsigned int)b_tcl << 16) | ((unsigned int)b_tcm << 8) | b_tch;

	if (log_cb && SCSI_BusLogGate())
		log_cb(RETRO_LOG_INFO,
			"[SCSI-SPC] 転送コマンド書き込み(phase=%s) 予定バイト数(TCH,TCM,TCL解釈)=%u 予定バイト数(TCL,TCM,TCH解釈)=%u\n",
			SCSI_SpcPhaseName(SCSISpcPhase), as_tch_tcm_tcl, as_tcl_tcm_tch);

	if (SCSIHostSpcCdbFromTemp)
	{
		if (SCSISpcPhase == SCSI_SPC_PHASE_COMMAND)
		{
			SCSI_SpcCommandByteFromTemp();
		}
		else if (SCSISpcPhase == SCSI_SPC_PHASE_DATAIN
			|| SCSISpcPhase == SCSI_SPC_PHASE_STATUS
			|| SCSISpcPhase == SCSI_SPC_PHASE_MSGIN)
		{
			uint8_t v = SCSI_SpcPhaseOutputByte("SCMD書き込み,仮説");
			SCSISpcReg[SCSI_SpcRegIndex(SCSI_SPC_ADR_TEMP)] = v;
			SCSISpcReg[SCSI_SpcRegIndex(SCSI_SPC_ADR_DREG)] = v;
		}
		else if (log_cb && SCSI_BusLogGate())
		{
			log_cb(RETRO_LOG_INFO,
				"[SCSI-SPC] 転送コマンド書き込みだがBUSFREEのため無視(仮説)\n");
		}
	}
	else
	{
		SCSI_SpcEnterCommandPhase("転送開始(SCMD上位3bit=111)");
	}
}

/* SCMD上位3bit=100($80)が書かれたときの処理(未実測の当てはめ)。
 *
 * 実測(2026-09-02): TEST UNIT READY応答(STATUS/MSGIN、SCMD上位3bit=111の
 * 書き込みで駆動)のあと、READ CAPACITYのCDBが届きDATAINへ入った時点で
 * ROMは別の手順を使った: PCTL=$01書き込み→TC(TCH=$ea0019/TCM=$ea001b/
 * TCL=$ea001d)へ8を書き込み→INTSクリア→SCMD上位3bit=100($80)書き込み、
 * という並びのあとpc=$ea13deでSSTS($ea000d)を95回ポーリングし続けた
 * (応答長8はREAD CAPACITYの8バイトと一致)。111とは異なる値であることから
 * COMMAND/STATUS/MSGINの「1バイトごとに転送コマンドを書く」手順とは別の、
 * DATAIN専用の「まとめて転送を開始する」合図だろう、という当てはめ。
 *
 * ここでは「SSTSに専用のデータビット(当てはめ、既定$08)を立てて、ROMは
 * それをポーリングして待っている」という仮説で実装する。実際にDREG/TEMPの
 * どちらを読むかはSCSI_Read側で両方の口を開けてあるので、読まれた時点で
 * SCSI_SpcPhaseOutputByte が1バイトずつ渡す。応答データそのものは
 * SCSI_SpcHandleCommand が既に用意したバッファ(SCSISpcDataBuf/Len)を
 * そのまま使う(TCは長さの裏取りのためログに出すだけで、上書きはしない)。 */
static void SCSI_SpcXferStartData(void)
{
	uint8_t b_tch = SCSISpcReg[SCSI_SpcRegIndex(SCSI_SPC_ADR_TCH)];
	uint8_t b_tcm = SCSISpcReg[SCSI_SpcRegIndex(SCSI_SPC_ADR_TCM)];
	uint8_t b_tcl = SCSISpcReg[SCSI_SpcRegIndex(SCSI_SPC_ADR_TCL)];
	unsigned int tc = ((unsigned int)b_tch << 16) | ((unsigned int)b_tcm << 8) | b_tcl;
	int remaining = SCSISpcDataLen - SCSISpcDataPos;

	if (log_cb && SCSI_BusLogGate())
		log_cb(RETRO_LOG_INFO,
			"[SCSI-SPC] 転送コマンド書き込み(上位3bit=100、仮説, phase=%s) TC(TCH,TCM,TCL)=%u 応答残り=%d\n",
			SCSI_SpcPhaseName(SCSISpcPhase), tc, remaining);

	if (SCSISpcPhase != SCSI_SPC_PHASE_DATAIN)
	{
		if (log_cb && SCSI_BusLogGate())
			log_cb(RETRO_LOG_INFO,
				"[SCSI-SPC] 転送コマンド(上位3bit=100)書き込みだがDATAINフェーズ外(phase=%s)のため無視(仮説)\n",
				SCSI_SpcPhaseName(SCSISpcPhase));
		return;
	}

	if (tc != (unsigned int)remaining && log_cb && SCSI_BusLogGate())
		log_cb(RETRO_LOG_INFO,
			"[SCSI-SPC] TC(%u)と応答残りバイト数(%d)が一致しない(要再確認、仮説)\n",
			tc, remaining);

	/* TCの実体を初期化する(当てはめ: TCH/TCM/TCLではなく応答残りバイト数を
	 * 正とし、TCレジスタの値は裏取りのログにのみ使う。理由はSCSI_SpcXferStartData
	 * 冒頭のコメント参照)。 */
	SCSISpcXferTc = remaining;

	if (remaining > 0)
	{
		SCSISpcSstsDataBitLow = 0;	/* 開始直後はパルスの「落ち」を保留しない(まず立てて見せる) */
		SCSI_SpcSstsSetDataBit(1, "DATAIN転送開始(SCMD上位3bit=100、仮説)");
		SCSI_SpcSstsSetTc0Bit(0, "DATAIN転送開始(TC>0、仮説)");
	}
	else
	{
		SCSI_SpcSstsSetTc0Bit(1, "DATAIN転送開始だがTC=0(仮説)");
	}
}

/* SPCレジスタへの書き込み(本物ROM使用時のみ呼ばれる)。
 * INTS($ea0009)への書き込みは「割り込み要因のクリア」とみなし、
 * 書かれた値のビットを落とす(INTS &= ~data)。実測でROMは
 * W $ea0009=$00 を行っており、これが「全クリア」の意味なのか
 * 「$00のビットを落とす(=無変化)」の意味なのかは未実測。
 * ここでは data==0 のときを全クリアと解釈する(仮説)。
 * それ以外のレジスタは配列へ記録するのみだが、SCMD($ea0005)への
 * 書き込みは追加でセレクト判定を行い、上位3bitが$000(バス開放と
 * 解釈)ならSSTSのbit7を状態機械経由で落とす。SCTL($ea0003)への
 * 書き込みでbit7($80、実測ではリセットで$90が書かれた)が立っている
 * ときも同様にbit7を落とす。加えてPCTL($ea0011)への書き込みでも
 * 落とす(実験的な規則。実測で本物ROMは再試行の入口で「変数読み→
 * PCTLに$00書き込み→SSTS読み出し」という並びを取っており、SSTSの
 * bit7が立ったままだと先へ進めず無限ループしていたため、PCTL書き込み
 * を「接続の終わり」とみなして落とすことにした。実機の仕様として
 * 測ったものではなく、webx68k_scsi_spc_clear_on_pctl() で無効化できる)。
 * いずれも SCSI_SpcSstsSetBit7Reason を参照。 */
static void SCSI_SpcWrite(uint32_t adr, uint8_t data)
{
	int idx = SCSI_SpcRegIndex(adr);
	if (idx < 0)
		return;

	if (adr == SCSI_SPC_ADR_INTS)
	{
		uint8_t before = SCSISpcReg[idx];
		uint8_t after = (data == 0) ? 0 : (uint8_t)(before & ~data);
		SCSISpcReg[idx] = after;
		/* [SCSI-BUS]と共通の上限・PC単位の抑制を通す(理由は
		 * SCSI_SpcSelectCheck 側のコメントを参照)。 */
		if (log_cb && SCSI_BusPcAllow(m68000_get_reg(M68K_PC)) && SCSI_BusLogGate())
			log_cb(RETRO_LOG_INFO, "[SCSI-SPC] INTSクリア data=$%02x before=$%02x after=$%02x\n",
				data, before, after);
		return;
	}

	SCSISpcReg[idx] = data;

	if (adr == SCSI_SPC_ADR_SCMD)
	{
		SCSI_SpcSelectCheck(data);
		if ((data & 0xe0) == 0x00)
			SCSI_SpcSstsSetBit7Reason(0, "SCMDバス開放");	/* バス開放(上位3bit=$000)でbit7を落とす */
		else if ((data & 0xe0) == 0x20 && SCSISpcSelectOk && SCSIHostSpcPsns == -1)
			/* 実測(2026-09-02): セレクト成立の時点でROMは既にPSNS=$8a
			 * (REQ|接続中|COMMAND)を期待して読みに来ており、転送開始
			 * (SCMD上位3bit=111)を待っていなかった。セレクト成立の
			 * 時点でCOMMANDフェーズへ入れる(未実測の当てはめ)。 */
			SCSI_SpcEnterCommandPhase("セレクト成立");
		else if ((data & 0xe0) == 0xe0 && SCSIHostSpcPsns == -1)
			SCSI_SpcXferStart();	/* 転送開始(上位3bit=$111、未実測の当てはめ)。状態機械が有効なときだけ */
		else if ((data & 0xe0) == 0x80 && SCSIHostSpcPsns == -1)
			SCSI_SpcXferStartData();	/* DATAIN専用の転送開始(上位3bit=$100、未実測の当てはめ) */
	}
	else if (adr == SCSI_SPC_ADR_DREG)
	{
		/* COMMANDフェーズ中のCDB受け取り。状態機械が有効(webx68k_scsi_spc_psns()==-1)
		 * なときだけ意味を持たせる(SCSI_SpcCommandByte参照)。 */
		if (SCSIHostSpcPsns == -1 && SCSISpcPhase == SCSI_SPC_PHASE_COMMAND)
			SCSI_SpcCommandByte(data);
	}
	else if (adr == SCSI_SPC_ADR_SCTL)
	{
		if (data & 0x80)
			SCSI_SpcSstsSetBit7Reason(0, "SCTLリセット");	/* リセット(bit7=$80)でbit7を落とす */
	}
	else if (adr == SCSI_SPC_ADR_PCTL)
	{
		/* 実験的な規則(このファイル冒頭のSCSI_SPC_ADR_PCTLコメント参照):
		 * 再試行のたびに測定を1つ進めるため、PCTLへの書き込みを
		 * 「接続の終わり」とみなしてSSTSのbit7を落とす。実機で確認した
		 * 仕様ではない。webx68k_scsi_spc_clear_on_pctl() で無効化できる
		 * (既定1=落とす、0=従来どおりSCMD/SCTLのみで落とす)。 */
		if (SCSIHostSpcClearOnPctl)
			SCSI_SpcSstsSetBit7Reason(0, "PCTL書き込み(実験的規則)");
	}
}

/* PSNS/SSTSの「掃引」モード。webx68k_scsi_spc_psns/ssts() が -2 を返すとき、
 * その仮想レジスタは実際の読み出し(SCSI_Read)のたびに 0x00〜0xff を1ずつ
 * 増やして返す(レジスタごとに別カウンタ、uint8_t の折り返しでそのまま
 * 0→255→0 と循環する)。
 *
 * 背景: 本物ROMはセレクト成功後もPSNSの読み出し→同じループ先頭へ戻る、を
 * 高速に繰り返すだけで、1値を固定して1回起動しても256通り試すには
 * 現実的でない時間がかかる。読み出すたびに値を変えれば、ROMが同じ
 * ループを回っている間に1回の実行で全値を試せる。
 *
 * ログは「同一PCから32件で以後抑制」する既存の圧縮(SCSI_BusPcAllow)の
 * 対象外にする(掃引の並びそのものが見たいものなので、圧縮されると
 * 肝心の情報が消える)。ただし暴走防止のため全体の4000件上限
 * (SCSI_BusLogGate)は他のログと共通で掛ける。
 *
 * PSNSにはさらに「交互」モード(-3)がある。掃引は「読むたびに+1」なので
 * 連続する2回の読み出しは必ず(v, v+1)の組にしかならず、「ある値の次に
 * 別の特定の値」という決まったハンドシェイクを待っているケースは掃引では
 * 原理的に満たせない。交互モードは __webx68kSpcPsnsA/B(既定 $8a/$0a)を
 * 読み出しのたびに入れ替えて返す。ログは交互なので連続一致にはならず、
 * 既存の32件抑制と別に専用の上限(先頭200行、SCSI_SPC_PSNS_ALT_LOG_MAX)を
 * 持たせて溢れを防ぐ。 */
static uint8_t SCSISpcPsnsSweepNext = 0;
static uint8_t SCSISpcSstsSweepNext = 0;
/* PSNS「交互」モード(-3)用。次に返す側(0=A, 1=B)と、専用ログ上限のための件数。
 * 掃引と違って連続一致にはならない(A,B,A,B,...)ため、既存の「同一PCから32件で
 * 抑制」等とは別に、この行だけ先頭200行で打ち切る専用の上限を持たせる。 */
static uint8_t SCSISpcPsnsAltNextIsB = 0;
static int SCSISpcPsnsAltLogCount = 0;
#define SCSI_SPC_PSNS_ALT_LOG_MAX 200

/* SSTSデータビットの「掃引」モード(webx68k_scsi_spc_ssts_data_bit()が-2)。
 *
 * 実測(2026-09-02): READ CAPACITY応答(DATAIN、TC=8)のあと、固定ビット
 * ($08含む)を試しても pc=$ea13de で $ea000d(SSTS)を95回以上ポーリングし
 * 続けて抜けなかった。ポーリング回数が多いため、待っているビットの当たりを
 * つけるのに掃引が効く(PSNS/SSTSの掃引と同じ発想)。DATAINで渡すべき
 * バイトが残っている間のSSTS読み出しに限り、$80(接続中、これは常に
 * 立てたままにする。実測で落とすと別の待ちが壊れることを確認済み)に
 * 0〜255を1ずつ変えた値をORして返す。DATAIN以外のフェーズや、渡すべき
 * バイトが残っていない状態のSSTSは従来どおり(SCSI_SpcSstsSetBit7Reason /
 * SCSI_SpcSstsSetDataBitが管理する値をそのまま使う)で、この掃引には
 * 一切関与しない(切り分けが壊れるため)。 */
#define SCSI_SPC_SSTS_DATA_SWEEP_LOG_MAX 200
static uint8_t SCSISpcSstsDataSweepNext = 0;
static int SCSISpcSstsDataSweepLogCount = 0;

/* 掃引を働かせる条件: SSTSデータビット設定が掃引(-2)、SSTS本体が既定(-1、
 * 状態機械モード)、DATAINフェーズで、渡すべきバイトがまだ残っていること。 */
static int SCSI_SpcSstsDataSweepActive(void)
{
	return (SCSIHostSpcSstsDataBit == -2)
		&& (SCSIHostSpcSsts == -1)
		&& (SCSISpcPhase == SCSI_SPC_PHASE_DATAIN)
		&& (SCSISpcDataPos < SCSISpcDataLen);
}

static uint8_t SCSI_SpcSstsDataSweepRead(void)
{
	uint8_t v = (uint8_t)(0x80 | SCSISpcSstsDataSweepNext);
	SCSISpcSstsDataSweepNext = (uint8_t)(SCSISpcSstsDataSweepNext + 1);

	if (log_cb && SCSI_BusLogGate() && SCSISpcSstsDataSweepLogCount < SCSI_SPC_SSTS_DATA_SWEEP_LOG_MAX)
	{
		SCSISpcSstsDataSweepLogCount++;
		log_cb(RETRO_LOG_INFO,
			"[SCSI-SPC] SSTSデータビット掃引: 値=$%02x を返した (pc=$%08x)%s\n",
			(unsigned)v, (unsigned)m68000_get_reg(M68K_PC),
			(SCSISpcSstsDataSweepLogCount == SCSI_SPC_SSTS_DATA_SWEEP_LOG_MAX) ? " (以後この行は抑制)" : "");
	}

	return v;
}

/* SSTSデータビットの「パルス」読み出し(固定値モード、cfg>=0のとき)。
 * 実測(2026-09-02): 固定ビット($30/$20/$10のいずれも)では2コマンド目で
 * 止まり、掃引(-2)でだけ通った→ROMは「値が変わること」自体を見ている
 * ハンドシェイクだと判明。COMMANDフェーズのREQパルスと同じ作りにする:
 * DREGへの1バイト読み出し直後の1回だけこのビットを落として返し、
 * その次からはまた立てて返す(SCSISpcSstsDataBitLow参照)。 */
static uint8_t SCSI_SpcSstsPulseRead(uint8_t stored)
{
	int cfg = SCSIHostSpcSstsDataBit;
	uint8_t bit, v;

	if (cfg < 0)
		return stored;	/* 掃引(-2)はSCSI_SpcSstsDataSweepReadが別途扱う */

	bit = (uint8_t)cfg;
	v = stored;

	if (SCSISpcSstsDataBitLow && (stored & bit))
	{
		v = (uint8_t)(stored & (uint8_t)~bit);
		SCSISpcSstsDataBitLow = 0;	/* 1回落としたら次からまた立てる(レジスタ側はONのまま) */

		if (log_cb && SCSI_BusLogGate())
			log_cb(RETRO_LOG_INFO,
				"[SCSI-SPC] SSTSデータビット($%02x)パルス: 今回のみ落として$%02xを返した (pc=$%08x)\n",
				bit, (unsigned)v, (unsigned)m68000_get_reg(M68K_PC));
	}

	return v;
}

static uint8_t SCSI_SpcSweepRead(uint32_t adr, uint8_t stored)
{
	int is_psns = (adr == SCSI_SPC_ADR_PSNS);
	int is_ssts = (adr == SCSI_SPC_ADR_SSTS);
	int cfg;
	uint8_t v;
	const char *name;

	if (!is_psns && !is_ssts)
		return stored;

	cfg = is_psns ? SCSIHostSpcPsns : SCSIHostSpcSsts;

	if (is_psns && cfg == -3)
	{
		/* 交互モード: 読み出しのたびにA/Bを入れ替えて返す。掃引(-2)は
		 * 「読むたびに+1」なので連続2回は必ず(v, v+1)にしかならず、
		 * 「ある値の次に別の特定の値」という決まったハンドシェイクを
		 * 待っているケースは掃引では原理的に満たせない。それを試す。 */
		int a = SCSIHostSpcPsnsA;
		int b = SCSIHostSpcPsnsB;
		v = (uint8_t)(SCSISpcPsnsAltNextIsB ? (b & 0xff) : (a & 0xff));

		if (log_cb && SCSI_BusLogGate() && SCSISpcPsnsAltLogCount < SCSI_SPC_PSNS_ALT_LOG_MAX)
		{
			SCSISpcPsnsAltLogCount++;
			log_cb(RETRO_LOG_INFO, "[SCSI-SPC] PSNS交互: %s($%02x)を返した (pc=$%08x)%s\n",
				SCSISpcPsnsAltNextIsB ? "B" : "A", (unsigned)v,
				(unsigned)m68000_get_reg(M68K_PC),
				(SCSISpcPsnsAltLogCount == SCSI_SPC_PSNS_ALT_LOG_MAX) ? " (以後この行は抑制)" : "");
		}

		SCSISpcPsnsAltNextIsB = (uint8_t)(SCSISpcPsnsAltNextIsB ^ 1);
		return v;
	}

	if (is_psns && cfg == -1)
	{
		/* 既定(-1): SCSI_SpcXferStart で始まる転送状態機械に任せる
		 * (COMMAND/DATAIN/STATUS/MSGIN、詳細は同関数付近のコメント参照)。
		 * 転送がまだ始まっていなければBUSFREEのまま(=$00、従来どおり
		 * PSNSが常に0だったのと結果的に同じ)。 */
		return SCSI_SpcPhasePsns();
	}

	if (cfg != -2)
		return stored;

	if (is_psns)
	{
		int base = SCSIHostSpcPsnsBase;
		v = (uint8_t)((base + SCSISpcPsnsSweepNext) & 0xff);
		SCSISpcPsnsSweepNext = (uint8_t)(SCSISpcPsnsSweepNext + 1);
		name = "PSNS";

		if (log_cb && SCSI_BusLogGate())
			log_cb(RETRO_LOG_INFO, "[SCSI-SPC] %s掃引: base=$%02x 値=$%02x を返した (pc=$%08x)\n",
				name, (unsigned)(base & 0xff), (unsigned)v, (unsigned)m68000_get_reg(M68K_PC));
	}
	else
	{
		int base = SCSIHostSpcSstsBase;
		v = (uint8_t)((base + SCSISpcSstsSweepNext) & 0xff);
		SCSISpcSstsSweepNext = (uint8_t)(SCSISpcSstsSweepNext + 1);
		name = "SSTS";

		if (log_cb && SCSI_BusLogGate())
			log_cb(RETRO_LOG_INFO, "[SCSI-SPC] %s掃引: base=$%02x 値=$%02x を返した (pc=$%08x)\n",
				name, (unsigned)(base & 0xff), (unsigned)v, (unsigned)m68000_get_reg(M68K_PC));
	}

	return v;
}

/*
 * [SCSI-BUS]の総件数上限(SCSIBusLogCapped)に達した後は、本物ROMの
 * 命令フェッチが$ea0000〜$ea1fffから行われるたびにSCSI_Read/Writeが
 * 呼ばれてもログはもう1行も出せない。にもかかわらず従来はそのたびに
 * SCSI_BusLog()経由でm68000_get_reg(M68K_PC)を呼んでいたため、
 * 本物ROM使用時の起動が異常に遅くなっていた(実測: 除外件数900万件超、
 * 基準38秒に対し400〜600秒でも起動しない)。
 *
 * ここでは上限到達後、かつ書き込み監視(webx68k_ram_watch_*)・
 * 読み出し監視(webx68k_mem_read_watch_*)の両方が無効(既定どおり
 * lo>hi)なら、m68000_get_reg()を呼ぶ前に早期脱出する。判定は
 * static変数の比較だけで済ませ、関数呼び出しやJSへの往復は挟まない。
 *
 * 早期脱出はあくまで[SCSI-BUS]ログの出力有無だけに関わるもので、
 * SPCポート域($ea0000〜$ea001f)に対するSCSI_SpcWrite/実際の読み出し
 * 処理(状態機械の動作に必要)はSCSI_Read/Write側で従来どおり行う。
 * ここで省くのはログだけ。
 *
 * 早期脱出に入る瞬間、保留中の圧縮エントリ([SCSI-BUS]の「継続中」表示の
 * もと)があれば沈黙を作らないよう一度だけ吐き出してから、以後この経路を
 * 通らなくなった旨を1回だけ出す。
 */
static int SCSI_BusLogShouldSkip(void)
{
	if (!SCSIBusLogCapped)
		return 0;
	if (webx68k_ram_watch_lo <= webx68k_ram_watch_hi)
		return 0;	/* 書き込み監視が有効 */
	if (webx68k_mem_read_watch_lo <= webx68k_mem_read_watch_hi)
		return 0;	/* 読み出し監視が有効 */

	if (!SCSIBusLogFastPathAnnounced)
	{
		SCSIBusLogFastPathAnnounced = 1;
		SCSI_BusLogFlush();	/* 保留中の圧縮エントリを一度だけ吐き出す(沈黙対策) */
		if (log_cb)
			log_cb(RETRO_LOG_INFO,
				"[SCSI-BUS] ログ上限到達につき、以後はログ経路を通さない(高速化)\n");
	}
	return 1;
}

void FASTCALL SCSI_Write(uint32_t adr, uint8_t data)
{
	int in_window = (adr >= SCSI_WINDOW_LO && adr < SCSI_WINDOW_HI);
	int in_spc = (adr >= SCSI_SPC_PORT_LO && adr < SCSI_SPC_PORT_HI);

	if (adr >= 0x00ea0000 && adr < 0x00ea2000 && !SCSI_BusLogShouldSkip())
		SCSI_BusLog(adr, 1, data, m68000_get_reg(M68K_PC));

	if (SCSIUsingRealRom && in_spc)
	{
		SCSI_SpcWrite(adr, data);
		return;
	}

	if (in_window && !SCSIUsingRealRom)
		SCSIIPL[(adr^1)&0x1fff] = data;
}

uint8_t FASTCALL SCSI_Read(uint32_t adr)
{
	uint8_t data;
	int in_spc = (adr >= SCSI_SPC_PORT_LO && adr < SCSI_SPC_PORT_HI);

	if (SCSIUsingRealRom && in_spc)
	{
		int idx = SCSI_SpcRegIndex(adr);
		int in_output_phase = (SCSISpcPhase == SCSI_SPC_PHASE_DATAIN
			|| SCSISpcPhase == SCSI_SPC_PHASE_STATUS
			|| SCSISpcPhase == SCSI_SPC_PHASE_MSGIN);
		data = (idx >= 0) ? SCSISpcReg[idx] : 0;
		/* 退行の実測(2026-09-02): 一時、TEMP($ea0017)の読み出しもDREGと
		 * 同じくバイトを「消費して」フェーズを進める側にしたところ、
		 * TEST UNIT READYのMSGIN応答がROMの意図しないタイミング
		 * (SCMD書き込みより前のTEMP読み出し)で先にBUSFREEまで進んでしまい、
		 * 後続のSCMD書き込み(上位3bit=111)がBUSFREE化けした状態を掴んで
		 * 無視され、再セレクトに来なくなった。トリガー(消費してフェーズを
		 * 進める側)は実測どおりDREGの読み出しだけに戻す。TEMPは
		 * SCSI_SpcPhaseOutputByte/SCSI_SpcXferStart側で渡した値をミラーして
		 * あるだけの受動的な置き場として扱う(読んでも消費しない)。 */
		if (adr == SCSI_SPC_ADR_DREG && SCSIHostSpcPsns == -1 && in_output_phase)
		{
			uint8_t v = SCSI_SpcPhaseOutputByte("DREG読み出し");
			SCSISpcReg[SCSI_SpcRegIndex(SCSI_SPC_ADR_TEMP)] = v;
			SCSISpcReg[SCSI_SpcRegIndex(SCSI_SPC_ADR_DREG)] = v;
			data = v;
		}
		else if (adr == SCSI_SPC_ADR_SSTS && SCSI_SpcSstsDataSweepActive())
		{
			/* DATAINで渡すべきバイトが残っている間だけの掃引。他のフェーズ・
			 * 他の状態のSSTSはここを通らず従来どおり(SCSI_SpcSweepRead)。 */
			data = SCSI_SpcSstsDataSweepRead();
		}
		else if (adr == SCSI_SPC_ADR_SSTS && SCSIHostSpcSsts == -1
			&& SCSIHostSpcSstsDataBit >= 0)
		{
			/* 固定値モード(cfg>=0)でのパルス化。既定($08)含め、
			 * SCSI_SpcSstsPulseRead参照。 */
			data = SCSI_SpcSstsPulseRead((idx >= 0) ? SCSISpcReg[idx] : 0);
		}
		else
		{
			data = SCSI_SpcSweepRead(adr, data);
		}
	}
	else
	{
		data = SCSIIPL[(adr^1)&0x1fff];
	}

	if (adr >= 0x00ea0000 && adr < 0x00ea2000 && !SCSI_BusLogShouldSkip())
		SCSI_BusLog(adr, 0, data, m68000_get_reg(M68K_PC));
	return data;
}
