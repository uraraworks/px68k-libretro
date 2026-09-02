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

#endif /* _WINX68K_SCSI_H */
