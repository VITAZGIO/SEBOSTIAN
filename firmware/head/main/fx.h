// Звуковые эффекты и тесты динамиков/микрофона (играют через микшер, источник FX).
#pragma once
#include <stdbool.h>

void fx_tone(float hz, int ms, int amp, bool left, bool right);   // поставить в очередь
void fx_dzyn(void);                    // «дзынь»: услышал «Себастьян», говори команду
void fx_pik(int hz, int ms);           // короткий «пик» перед записью (TRAIN), ждёт окончания
void fx_lr_test(void);                 // оба 1 стук, левый 3, правый 2 (блокирует до конца)
void fx_mic_test(void);                // запись 2 с -> воспроизведение (блокирует)
