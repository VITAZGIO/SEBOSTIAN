// TRAIN: запись сэмплов для обучения своего слова «Себастьян» (как Arduino v18).
// /sdcard/kws/pos/p_NNNN.wav — «Себастьян», /kws/neg/n_NNNN.wav — другие слова, /kws/bg/b_NNNN.wav — фон 30 с.
// 16 кГц, 16 бит, моно, СЫРОЙ звук (без AEC/фильтров). Скачать всё: http://<IP головы>/kws.tar
#pragma once

void train_scan(void);
void train_series(int kind);    // 0 = «Себастьян» x10, 1 = другие слова x10
void train_background(void);    // фон 30 с
void train_play_last(void);
void train_del_last(void);
void train_next_tip(void);
extern const char *KWS_TIPS[];
extern const int KWS_TIPS_N;
