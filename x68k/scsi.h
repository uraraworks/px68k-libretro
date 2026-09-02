#ifndef _WINX68K_SCSI_H
#define _WINX68K_SCSI_H

#include <stdint.h>
#include "common.h"

extern	uint8_t	SCSIIPL[0x2000];

void SCSI_Init(void);
void SCSI_Cleanup(void);

uint8_t FASTCALL SCSI_Read(uint32_t adr);
void FASTCALL SCSI_Write(uint32_t adr, uint8_t data);
/* SCSI IOCS ($F5) の呼び出し口 $e9f800 への書き込み */
void FASTCALL SCSI_IOCSPort_Write(uint32_t adr, uint8_t data);

/* 本物SCSI ROM使用時のみ、ゲストPCを60フレームに1回・上限40行で
 * [SCSI-PC] としてログへ出す。retro_run から毎フレーム呼ぶ想定。 */
void SCSI_LogPcIfRealRom(void);

/* webx68k_scsi_*()(SPC関連・ログ関連・返答関連の設定取得、EM_JS経由)を
 * 毎フレームまとめて読み直し、static へキャッシュする。SPCレジスタへの
 * アクセスのたびにEM_JSを呼ぶと本物ROMのポーリング回数(何十万回)ぶん
 * JS往復してしまい起動が桁違いに遅くなる実測不具合への対策。
 * SCSI_Init内で最低1回、以降は retro_run から毎フレーム
 * (SCSI_LogPcIfRealRom() の隣で)呼ぶ想定。本物ROM未使用時は何もしない。 */
void SCSI_RefreshHostConfig(void);

#endif /* _WINX68K_SCSI_H */
