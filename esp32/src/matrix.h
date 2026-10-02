// 8x8 WS2812 matrix on GPIO14, index = row*8 + col (0 = top-left).
#pragma once
#include <stdint.h>

void matrix_init(void);
void matrix_fill(uint8_t r, uint8_t g, uint8_t b);
void matrix_set(int row, int col, uint8_t r, uint8_t g, uint8_t b);
void matrix_show(void);
