#ifndef _WINX68K_SCC_H
#define _WINX68K_SCC_H

#include <stdint.h>
#include "common.h"

void SCC_IntCheck(void);
void SCC_Init(void);
uint8_t FASTCALL SCC_Read(uint32_t adr);
void FASTCALL SCC_Write(uint32_t adr, uint8_t data);
int SCC_StateAction(StateMem *sm, int load, int data_only);

/* Host serial bridge for SCC channel A (X68000 RS-232C). */
int SCC_SerialReceive(const uint8_t *data, int length);
int SCC_SerialTxAvailable(void);
int SCC_SerialReadTxByte(void);
void SCC_SerialHostReset(void);
void SCC_SerialSetConnected(int connected);
void SCC_SerialSetTxWritable(int writable);
int SCC_SerialGetGuestBaudRate(void);

#ifdef WEBX68K_CORE_TEST_EXPORTS
/* WebX68kの実コア結合テストから割り込み順を観測するための内部API。 */
uint32_t SCC_TestAcknowledgeInterrupt(void);
uint8_t SCC_TestCurrentInterruptCause(void);
#endif

extern int8_t MouseX;
extern int8_t MouseY;
extern uint8_t MouseSt;

#endif /* _WINX68K_SCC_H */
