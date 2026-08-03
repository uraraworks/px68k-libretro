#ifndef _WINX68K_MOUSE_H
#define _WINX68K_MOUSE_H

#include <stdint.h>
#include "common.h"

void Mouse_Init(void);
void Mouse_Event(int wparam, float dx, float dy);
void Mouse_SetData(void);
void Mouse_StartCapture(int flag);

/* WebX68k: ホストからの配線確認用に累積デルタ/ボタン状態/有効フラグを覗く */
float Mouse_PeekDX(void);
float Mouse_PeekDY(void);
int Mouse_PeekStat(void);
int Mouse_IsEnabled(void);

#endif /* _WINX68K_MOUSE_H */
