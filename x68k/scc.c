/*
 *  SCC.C - Z8530 SCC (serial and mouse)
 */

#include "common.h"
#include "scc.h"
#include "m68000.h"
#include "irqh.h"
#include "mouse.h"
#include <string.h>

#define SCC_SERIAL_FIFO_SIZE 4096
#define SCC_SERIAL_PCLK_HZ 5000000

int8_t MouseX = 0;
int8_t MouseY = 0;
uint8_t MouseSt = 0;

static uint8_t SCC_RegNumA   = 0;
static uint8_t SCC_RegSetA   = 0;
static uint8_t SCC_RegsA[16] = {0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0};
static uint8_t SCC_RegsB[16] = {0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0};
static uint8_t SCC_RegNumB   = 0;
static uint8_t SCC_RegSetB   = 0;
static uint8_t SCC_Vector    = 0;
static uint8_t SCC_Dat[3]    = {0, 0, 0};
static uint8_t SCC_DatNum    = 0;

/*
 * Channel A is the X68000 RS-232C port. The browser-side transport owns the
 * physical port; the core only exposes bounded byte FIFOs. Host FIFO contents
 * are deliberately excluded from save states and cleared after a state load.
 */
static uint8_t SCC_SerialRx[SCC_SERIAL_FIFO_SIZE];
static uint8_t SCC_SerialTx[SCC_SERIAL_FIFO_SIZE];
static unsigned int SCC_SerialRxRead = 0;
static unsigned int SCC_SerialRxWrite = 0;
static unsigned int SCC_SerialRxCount = 0;
static unsigned int SCC_SerialTxRead = 0;
static unsigned int SCC_SerialTxWrite = 0;
static unsigned int SCC_SerialTxCount = 0;
static uint8_t SCC_SerialFirstRxPending = 0;
static uint8_t SCC_SerialFirstRxArmed = 0;
static uint8_t SCC_SerialTxInterruptPending = 0;
static uint8_t SCC_SerialHostConnected = 0;
static uint8_t SCC_SerialHostWritable = 0;

enum SCCInterruptCause
{
	SCC_INT_NONE = 0,
	SCC_INT_SERIAL_RX,
	SCC_INT_SERIAL_TX,
	SCC_INT_MOUSE
};

static uint8_t SCC_InterruptCause = SCC_INT_NONE;
static uint8_t SCC_LastInterruptCause = SCC_INT_NONE;
static uint8_t SCC_RoundRobinLast = SCC_INT_NONE;
static uint8_t SCC_MouseInterruptAcknowledged = 0;

static void SCC_ClearInterruptLatches(void)
{
	SCC_InterruptCause = SCC_INT_NONE;
	SCC_LastInterruptCause = SCC_INT_NONE;
	SCC_RoundRobinLast = SCC_INT_NONE;
}

int SCC_StateAction(StateMem *sm, int load, int data_only)
{
	SFORMAT StateRegs[] = 
	{
		SFVAR(MouseX),
		SFVAR(MouseY),
		SFVAR(MouseSt),

		SFVAR(SCC_RegNumA),
		SFVAR(SCC_RegSetA),
		SFARRAY(SCC_RegsB, 16),
		SFVAR(SCC_RegNumB),
		SFVAR(SCC_RegSetB),
		SFVAR(SCC_Vector),
		SFARRAY(SCC_Dat, 3),
		SFVAR(SCC_DatNum),
		/* Keep new fields at the end so old fast save states keep their layout. */
		SFARRAY(SCC_RegsA, 16),
		SFVAR(SCC_MouseInterruptAcknowledged),

		SFEND
	};

	int ret;
	if (load)
	{
		memset(SCC_RegsA, 0, sizeof(SCC_RegsA));
		SCC_MouseInterruptAcknowledged = 0;
	}
	ret = PX68KSS_StateAction(sm, load, data_only, StateRegs, "X68K_SCC", false);
	if (load)
	{
		SCC_ClearInterruptLatches();
		/* IRQH側に残ったロード前のIRQ5を取り下げてから、現在の状態を再評価する。 */
		IRQH_IRQCallBack(5);
		SCC_SerialHostReset();
	}

	return ret;
}

static int SCC_SerialRxInterruptPending(void)
{
	switch (SCC_RegsA[1] & 0x18)
	{
	case 0x08: /* interrupt on first received character */
		return SCC_SerialFirstRxPending != 0;
	case 0x10: /* interrupt on all received characters */
		return SCC_SerialRxCount != 0;
	default:
		return 0;
	}
}

static int SCC_SerialTxInterruptEnabled(void)
{
	return (SCC_RegsA[1] & 0x02) != 0;
}

static int SCC_SerialTxReady(void)
{
	return !SCC_SerialHostConnected ||
		(SCC_SerialHostWritable && SCC_SerialTxCount < SCC_SERIAL_FIFO_SIZE);
}

static int SCC_MouseInterruptPending(void)
{
	return !SCC_MouseInterruptAcknowledged &&
		((SCC_DatNum && ((SCC_RegsB[1] & 0x18) == 0x10)) ||
		((SCC_DatNum == 3) && ((SCC_RegsB[1] & 0x18) == 0x08)));
}

static int SCC_InterruptPending(uint8_t cause)
{
	switch (cause)
	{
	case SCC_INT_SERIAL_RX:
		return SCC_SerialRxInterruptPending();
	case SCC_INT_SERIAL_TX:
		return SCC_SerialTxInterruptPending && SCC_SerialTxInterruptEnabled() &&
			SCC_SerialHostConnected && SCC_SerialHostWritable &&
			SCC_SerialTxCount < SCC_SERIAL_FIFO_SIZE;
	case SCC_INT_MOUSE:
		return SCC_MouseInterruptPending();
	default:
		return 0;
	}
}

static uint32_t SCC_VectorForCause(uint8_t cause)
{
	if (SCC_RegsB[9] & 0x10)
	{
		if (cause == SCC_INT_SERIAL_RX) return (uint32_t)(SCC_Vector & 0x8f) + 0x30;
		if (cause == SCC_INT_SERIAL_TX) return (uint32_t)(SCC_Vector & 0x8f) + 0x10;
		return (uint32_t)(SCC_Vector & 0x8f) + 0x20;
	}
	if (cause == SCC_INT_SERIAL_RX) return (uint32_t)(SCC_Vector & 0xf1) + 12;
	if (cause == SCC_INT_SERIAL_TX) return (uint32_t)(SCC_Vector & 0xf1) + 8;
	return (uint32_t)(SCC_Vector & 0xf1) + 4;
}

static uint32_t SCC_NullInterruptVector(void)
{
	if (SCC_RegsB[9] & 0x10)
		return (uint32_t)(SCC_Vector & 0x8f) + 0x60;
	return (uint32_t)(SCC_Vector & 0xf1) + 6;
}

static void SCC_SerialIntCheck(void);

/* マウスは接続状態にかかわらず、既存のチャネルB専用割り込み経路を使う。 */
static uint32_t FASTCALL SCC_MouseInt(uint8_t irq)
{
	uint32_t vector;

	IRQH_IRQCallBack(irq);
	if ((irq == 5) && (!(SCC_RegsB[9] & 2)))
	{
		if (SCC_RegsB[9] & 1)
		{
			if (SCC_RegsB[9] & 0x10)
				vector = ((uint32_t)(SCC_Vector & 0x8f) + 0x20);
			else
				vector = ((uint32_t)(SCC_Vector & 0xf1) + 4);
		}
		else
			vector = ((uint32_t)SCC_Vector);
		/* 最初のデータ読出しまでは同じパケットを再通知せず、serialを先へ進める。 */
		SCC_MouseInterruptAcknowledged = 1;
		SCC_SerialIntCheck();
		return vector;
	}
	SCC_SerialIntCheck();
	return (uint32_t)(-1);
}

static uint32_t FASTCALL SCC_Int(uint8_t irq)
{
	uint8_t cause;
	uint32_t vector;
	IRQH_IRQCallBack(irq);
	if (irq != 5)
		return (uint32_t)(-1);
	cause = SCC_InterruptCause;
	SCC_InterruptCause = SCC_INT_NONE;
	if (cause == SCC_INT_NONE)
	{
		SCC_IntCheck();
		return (SCC_RegsB[9] & 2) ? (uint32_t)(-1) :
			((SCC_RegsB[9] & 1) ? SCC_NullInterruptVector() : (uint32_t)SCC_Vector);
	}
	if (!SCC_InterruptPending(cause))
	{
		SCC_IntCheck();
		return (SCC_RegsB[9] & 2) ? (uint32_t)(-1) :
			((SCC_RegsB[9] & 1) ? SCC_NullInterruptVector() : (uint32_t)SCC_Vector);
	}
	SCC_LastInterruptCause = cause;
	SCC_RoundRobinLast = cause;
	if (cause == SCC_INT_SERIAL_TX)
		SCC_SerialTxInterruptPending = 0;
	if (SCC_RegsB[9] & 2)
	{
		SCC_IntCheck();
		return (uint32_t)(-1);
	}
	vector = (SCC_RegsB[9] & 1) ? SCC_VectorForCause(cause) : (uint32_t)SCC_Vector;
	SCC_IntCheck();
	return vector;
}

static void SCC_SerialIntCheck(void)
{
	uint8_t offset;

	if (!(SCC_RegsB[9] & 0x08))
		return;
	if (SCC_InterruptCause != SCC_INT_NONE)
	{
		if (SCC_InterruptPending(SCC_InterruptCause))
			return;
		SCC_InterruptCause = SCC_INT_NONE;
	}
	/* マウスを除き、チャネルAのRX/TX間だけをラウンドロビンで選ぶ。 */
	for (offset = 1; offset <= 2; offset++)
	{
		uint8_t cause = (uint8_t)(((SCC_RoundRobinLast + offset - 1) % 2) + 1);
		if (SCC_InterruptPending(cause))
		{
			SCC_InterruptCause = cause;
			IRQH_Int(5, &SCC_Int);
			return;
		}
	}
	if (IRQH_IsPending(5))
		IRQH_IRQCallBack(5);
}

#ifdef WEBX68K_CORE_TEST_EXPORTS
uint32_t SCC_TestAcknowledgeInterrupt(void)
{
	/* テストでも、実際に登録される割り込み経路と同じコールバックを使う。 */
	return (SCC_InterruptCause == SCC_INT_NONE) ? SCC_MouseInt(5) : SCC_Int(5);
}

uint8_t SCC_TestCurrentInterruptCause(void)
{
	return SCC_InterruptCause;
}
#endif

void SCC_IntCheck(void)
{
	if (!(SCC_RegsB[9] & 0x08))
		return;
	if (SCC_MouseInterruptPending())
	{
		/* 既に通知したserialよりマウスを優先し、serial要因自体はFIFOに残す。 */
		if (SCC_InterruptCause != SCC_INT_NONE)
		{
			SCC_InterruptCause = SCC_INT_NONE;
			if (IRQH_IsPending(5))
				IRQH_IRQCallBack(5);
		}
		IRQH_Int(5, &SCC_MouseInt);
		return;
	}
	SCC_SerialIntCheck();
}


void SCC_Init(void)
{
	MouseX = 0;
	MouseY = 0;
	MouseSt = 0;
	SCC_RegNumA = 0;
	SCC_RegSetA = 0;
	memset(SCC_RegsA, 0, sizeof(SCC_RegsA));
	SCC_RegNumB = 0;
	SCC_RegSetB = 0;
	SCC_Vector = 0;
	SCC_DatNum = 0;
	SCC_MouseInterruptAcknowledged = 0;
	SCC_ClearInterruptLatches();
	SCC_SerialHostReset();
}

void SCC_SerialHostReset(void)
{
	SCC_SerialRxRead = 0;
	SCC_SerialRxWrite = 0;
	SCC_SerialRxCount = 0;
	SCC_SerialTxRead = 0;
	SCC_SerialTxWrite = 0;
	SCC_SerialTxCount = 0;
	SCC_SerialFirstRxPending = 0;
	SCC_SerialFirstRxArmed = 0;
	SCC_SerialTxInterruptPending =
		(SCC_SerialHostConnected && SCC_SerialHostWritable && SCC_SerialTxInterruptEnabled()) ? 1 : 0;
	if (SCC_InterruptCause == SCC_INT_SERIAL_RX || SCC_InterruptCause == SCC_INT_SERIAL_TX)
		SCC_InterruptCause = SCC_INT_NONE;
	SCC_IntCheck();
}

void SCC_SerialSetConnected(int connected)
{
	uint8_t next = connected ? 1 : 0;
	if (SCC_SerialHostConnected == next)
		return;
	/* 接続状態の境界では、以前の経路のコールバックを次の状態へ持ち越さない。 */
	SCC_ClearInterruptLatches();
	if (IRQH_IsPending(5))
		IRQH_IRQCallBack(5);
	SCC_SerialHostConnected = next;
	SCC_SerialHostWritable = next;
	SCC_SerialHostReset();
}

void SCC_SerialSetTxWritable(int writable)
{
	uint8_t next = (SCC_SerialHostConnected && writable) ? 1 : 0;
	if (SCC_SerialHostWritable == next)
		return;
	SCC_SerialHostWritable = next;
	if (!next)
		SCC_SerialTxInterruptPending = 0;
	else if (SCC_SerialTxInterruptEnabled() && SCC_SerialTxCount < SCC_SERIAL_FIFO_SIZE)
		SCC_SerialTxInterruptPending = 1;
	SCC_IntCheck();
}

int SCC_SerialGetGuestBaudRate(void)
{
	uint32_t clock_mode;
	uint32_t time_constant;
	uint32_t divisor;

	/* 非同期通信、内蔵BRG、PCLKを送受信クロックに使う標準設定だけを判定する。 */
	if ((SCC_RegsA[4] & 0x0c) == 0 || (SCC_RegsA[14] & 0x03) != 0x03 ||
		(SCC_RegsA[11] & 0x60) != 0x40 || (SCC_RegsA[11] & 0x18) != 0x10)
		return 0;

	switch (SCC_RegsA[4] & 0xc0)
	{
	case 0x00: clock_mode = 1; break;
	case 0x40: clock_mode = 16; break;
	case 0x80: clock_mode = 32; break;
	default: clock_mode = 64; break;
	}
	time_constant = ((uint32_t)SCC_RegsA[13] << 8) | SCC_RegsA[12];
	divisor = 2 * clock_mode * (time_constant + 2);
	return (int)((SCC_SERIAL_PCLK_HZ + divisor / 2) / divisor);
}

int SCC_SerialReceive(const uint8_t *data, int length)
{
	int accepted = 0;
	int i;

	if (!data || length <= 0 || !SCC_SerialHostConnected)
		return 0;

	for (i = 0; i < length; i++)
	{
		if (SCC_SerialRxCount >= SCC_SERIAL_FIFO_SIZE)
			break;
		if (SCC_SerialFirstRxArmed)
		{
			SCC_SerialFirstRxPending = 1;
			SCC_SerialFirstRxArmed = 0;
		}
		SCC_SerialRx[SCC_SerialRxWrite] = data[i];
		SCC_SerialRxWrite = (SCC_SerialRxWrite + 1) % SCC_SERIAL_FIFO_SIZE;
		SCC_SerialRxCount++;
		accepted++;
	}

	if (accepted)
		SCC_IntCheck();
	return accepted;
}

int SCC_SerialTxAvailable(void)
{
	return SCC_SerialHostConnected ? (int)SCC_SerialTxCount : 0;
}

int SCC_SerialReadTxByte(void)
{
	uint8_t data;
	int was_full;

	if (!SCC_SerialHostConnected || !SCC_SerialTxCount)
		return -1;
	was_full = (SCC_SerialTxCount == SCC_SERIAL_FIFO_SIZE);
	data = SCC_SerialTx[SCC_SerialTxRead];
	SCC_SerialTxRead = (SCC_SerialTxRead + 1) % SCC_SERIAL_FIFO_SIZE;
	SCC_SerialTxCount--;
	if (was_full && SCC_SerialHostWritable && SCC_SerialTxInterruptEnabled())
		SCC_SerialTxInterruptPending = 1;
	SCC_IntCheck();
	return data;
}

static void SCC_SerialWriteTxByte(uint8_t data)
{
	if (!SCC_SerialHostConnected)
	{
		SCC_SerialTxInterruptPending = 0;
		return;
	}
	if (SCC_SerialTxCount >= SCC_SERIAL_FIFO_SIZE)
	{
		/* RR0の送信準備を落としているため、無視して書くゲストのデータは実機同様に保持できない。 */
		return;
	}
	SCC_SerialTx[SCC_SerialTxWrite] = data;
	SCC_SerialTxWrite = (SCC_SerialTxWrite + 1) % SCC_SERIAL_FIFO_SIZE;
	SCC_SerialTxCount++;
	if (SCC_SerialHostWritable && SCC_SerialTxInterruptEnabled() &&
		SCC_SerialTxCount < SCC_SERIAL_FIFO_SIZE)
		SCC_SerialTxInterruptPending = 1;
	SCC_IntCheck();
}

static uint8_t SCC_SerialReadRxByte(void)
{
	uint8_t data;

	if (!SCC_SerialRxCount)
		return 0;
	data = SCC_SerialRx[SCC_SerialRxRead];
	SCC_SerialRxRead = (SCC_SerialRxRead + 1) % SCC_SERIAL_FIFO_SIZE;
	SCC_SerialRxCount--;
	SCC_SerialFirstRxPending = 0;
	SCC_IntCheck();
	return data;
}

static void SCC_WriteRegister9(uint8_t data, int channel_a)
{
	uint8_t reset = data & 0xc0;
	if (channel_a && (reset == 0x80 || reset == 0xc0))
	{
		memset(SCC_RegsA, 0, sizeof(SCC_RegsA));
		/* WR2 は共有レジスタであり、チャネル単独リセットでは保持される。 */
		SCC_RegsA[2] = SCC_Vector;
		SCC_SerialHostReset();
	}
	/*
	 * 既存のチャネルB実装はWR9をリセットコマンドとして解釈せず、
	 * 書込み値をそのまま保持していた。マウス転送状態を変えないため、
	 * Bからの書込みではこの動作を維持する。Aからの書込みだけは、
	 * 自己消去するリセット指定ビットを両チャネルの保存値から除く。
	 */
	if (channel_a)
	{
		SCC_RegsA[9] = data & 0x3f;
		SCC_RegsB[9] = data & 0x3f;
	}
	else
	{
		SCC_RegsB[9] = data;
	}
	if (!(data & 0x08))
	{
		/* 共通シリアル経路だけを解除し、従来のマウスACK待ちは維持する。 */
		if (SCC_InterruptCause != SCC_INT_NONE)
		{
			SCC_InterruptCause = SCC_INT_NONE;
			if (IRQH_IsPending(5))
				IRQH_IRQCallBack(5);
		}
		return;
	}
	if (SCC_SerialHostConnected)
		SCC_IntCheck();
}

static void SCC_SerialWriteRegister(uint8_t reg, uint8_t data)
{
	uint8_t old;
	if (reg == 2)
	{
		SCC_RegsB[2] = data;
		SCC_Vector = data;
	}
	else if (reg == 9)
	{
		SCC_WriteRegister9(data, 1);
		return;
	}
	if (reg == 1)
	{
		old = SCC_RegsA[1];
		if ((data & 0x18) == 0x08 && (old & 0x18) != 0x08)
		{
			SCC_SerialFirstRxArmed = 1;
			if (SCC_SerialRxCount)
			{
				SCC_SerialFirstRxPending = 1;
				SCC_SerialFirstRxArmed = 0;
			}
		}
		if (!(data & 0x18))
		{
			SCC_SerialFirstRxArmed = 0;
			SCC_SerialFirstRxPending = 0;
		}
		if (SCC_SerialHostConnected && (data & 0x02) && !(old & 0x02) &&
			SCC_SerialHostWritable && SCC_SerialTxCount < SCC_SERIAL_FIFO_SIZE)
			SCC_SerialTxInterruptPending = 1;
		if (!(data & 0x02))
			SCC_SerialTxInterruptPending = 0;
	}
	SCC_RegsA[reg] = data;
	if (reg == 8)
		SCC_SerialWriteTxByte(data);
	if (SCC_SerialHostConnected)
		SCC_IntCheck();
}

static void SCC_SerialWriteCommand(uint8_t data)
{
	uint8_t command = (data >> 3) & 7;
	uint8_t reg = data & 7;
	SCC_RegNumA = reg;
	SCC_RegSetA = reg != 0;
	if (command == 0)
		return;
	if (command == 1)
	{
		SCC_RegNumA = reg | 8;
		SCC_RegSetA = 1;
		return;
	}
	if (command == 2)
	{
		/* 外部ステータス割り込みは未モデル化のため、解除対象のラッチはない。 */
	}
	else if (command == 3)
	{
		/* Send Abort は同期モード用。非同期シリアルでは状態を変更しない。 */
	}
	else if (command == 4)
	{
		SCC_SerialFirstRxArmed = 1;
		if (SCC_SerialRxCount)
		{
			SCC_SerialFirstRxPending = 1;
			SCC_SerialFirstRxArmed = 0;
		}
	}
	else if (command == 5)
	{
		SCC_SerialTxInterruptPending = 0;
	}
	else if (command == 6)
	{
		/* パリティ・フレーミング・オーバーランは現在モデル化していない。 */
	}
	else if (command == 7)
	{
		/* インサービス管理は未モデル化。pending状態は各受信・送信条件から再評価する。 */
	}
	SCC_IntCheck();
}

void FASTCALL SCC_Write(uint32_t adr, uint8_t data)
{
	if (adr>=0xe98008)
      return;

	if ((adr&7) == 1)
	{
		if (SCC_RegSetB)
		{
			if (SCC_RegNumB == 9)
			{
				SCC_WriteRegister9(data, 0);
			}
			else if (SCC_RegNumB == 5)
			{
				if ( (!(SCC_RegsB[5]&2))&&(data&2)&&(SCC_RegsB[3]&1)
                  &&(!SCC_DatNum) )	
				{
					Mouse_SetData();
					SCC_DatNum = 3;
					SCC_MouseInterruptAcknowledged = 0;
					SCC_Dat[2] = MouseSt;
					SCC_Dat[1] = MouseX;
					SCC_Dat[0] = MouseY;
				}
			}
			else if (SCC_RegNumB == 2)
				SCC_Vector = data;
			if (SCC_RegNumB != 9)
				SCC_RegsB[SCC_RegNumB] = data;
			SCC_RegSetB = 0;
			SCC_RegNumB = 0;
		}
		else
		{
			/*
			 * チャネルBは既存のマウス実装との互換性を優先し、従来の選択規則を維持する。
			 * 実機と違い、ここではコマンドビットを解釈せず「上位ニブルが0の値」をそのまま
			 * レジスタ番号として採用する。そのためチャネルBではPoint High(WR0のcommand 1)を
			 * 使えず、WR9はポインタ値9で直接選択する(実機では9は
			 * 「Reset Tx Int Pending + point 1」に当たる)。チャネルAは
			 * SCC_SerialWriteCommand()側で実機どおりコマンドビットを解釈する。
			 * WebX68kのtest/core-serial-integration.test.tsはこの規則に依存しているため、
			 * チャネルBを実機準拠へ直す場合はテスト側も併せて更新すること。
			 */
			if (!(data&0xf0))
			{
				data &= 15;
				SCC_RegSetB = 1;
				SCC_RegNumB = data;
			}
			else
			{
				SCC_RegSetB = 0;
				SCC_RegNumB = 0;
			}
		}
	}
	else if ((adr&7) == 5)
	{
		if (SCC_RegSetA)
      {
         SCC_RegSetA = 0;
         SCC_SerialWriteRegister(SCC_RegNumA, data);
         SCC_RegNumA = 0;
      }
		else
      {
         SCC_SerialWriteCommand(data);
      }
	}
	else if ((adr&7) == 7)
	{
		SCC_SerialWriteTxByte(data);
	}
}

uint8_t FASTCALL SCC_Read(uint32_t adr)
{
	uint8_t ret = 0;

	if (adr >= 0xe98008)
      return 0;

	if ((adr&7) == 1)
	{
		if (SCC_SerialHostConnected && SCC_RegNumB == 2)
		{
			/* acknowledge後は次のpending要因より、直前に応答した要因のベクタを優先する。 */
			uint8_t cause = SCC_LastInterruptCause;
			if (cause == SCC_INT_NONE)
			{
				cause = SCC_InterruptCause;
				if (cause == SCC_INT_NONE || !SCC_InterruptPending(cause))
				{
					uint8_t offset;
					cause = SCC_INT_NONE;
					for (offset = 1; offset <= 3; offset++)
					{
						uint8_t candidate = (uint8_t)(((SCC_RoundRobinLast + offset - 1) % 3) + 1);
						if (SCC_InterruptPending(candidate))
						{
							cause = candidate;
							break;
						}
					}
				}
			}
			ret = (cause == SCC_INT_NONE) ? (uint8_t)SCC_NullInterruptVector() : (uint8_t)SCC_VectorForCause(cause);
			/* acknowledge済み要因はRR2で一度報告した後に消費し、次回は現在のpending要因を返す。 */
			SCC_LastInterruptCause = SCC_INT_NONE;
		}
		else if (!SCC_RegNumB)
			ret = ((SCC_DatNum)?1:0);
		SCC_RegNumB = 0;
		SCC_RegSetB = 0;
	}
	else if ((adr&7) == 3)
	{
		if (SCC_DatNum)
		{
			/* ACK後の最初の読出しで、次バイトの割り込み評価を再び許可する。 */
			SCC_MouseInterruptAcknowledged = 0;
			SCC_DatNum--;
			ret = SCC_Dat[SCC_DatNum];
		}
	}
	else if ((adr&7) == 5)
	{
		switch(SCC_RegNumA)
		{
		case 0:
			ret = (SCC_SerialTxReady() ? 4 : 0) |
				(SCC_SerialHostConnected ? 0x28 : 0) | (SCC_SerialRxCount ? 1 : 0);
			break;
		case 1:
			ret = SCC_SerialHostConnected && SCC_SerialHostWritable && SCC_SerialTxCount == 0 ? 0x01 : 0;
			break;
		case 2:
			ret = SCC_SerialHostConnected ? SCC_Vector : 0;
			break;
		case 3:
			ret = (SCC_DatNum ? 4 : 0);
			if (SCC_SerialHostConnected)
				ret |= (SCC_SerialRxInterruptPending()?0x20:0) |
					(SCC_InterruptPending(SCC_INT_SERIAL_TX)?0x10:0);
			break;
		case 8:
			ret = SCC_SerialReadRxByte();
			break;
		default:
			/* WR12/WR13/WR15など、保持しているチャネルAレジスタは書込み値を読み戻す。 */
			ret = SCC_SerialHostConnected ? SCC_RegsA[SCC_RegNumA] : 0;
			break;
		}
		SCC_RegNumA = 0;
		SCC_RegSetA = 0;
	}
	else if ((adr&7) == 7)
	{
		ret = SCC_SerialReadRxByte();
	}
	return ret;
}
