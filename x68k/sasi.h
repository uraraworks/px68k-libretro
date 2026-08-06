#ifndef _WINX68K_SASI_H
#define _WINX68K_SASI_H

#include <stdint.h>
#include "common.h"

void SASI_Init(void);
uint8_t FASTCALL SASI_Read(uint32_t adr);
void FASTCALL SASI_Write(uint32_t adr, uint8_t data);
int SASI_IsReady(void);
int SASI_StateAction(StateMem *sm, int load, int data_only);

/* Misc: HDD(SASI)アクセスランプ用。データ転送が発生したフレームで1になる。
 * WebX68k向け。retro_run() の毎フレーム先頭で0クリアされる想定。 */
extern int SASI_IsAccessing;
extern int SASI_Dirty;

#endif /* _WINX68K_SASI_H */
