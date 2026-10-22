#ifndef __ST7735_H
#define __ST7735_H

#include "stm32f10x.h"

// Màu s?c (RGB565)
#define BLACK   0x0000
#define WHITE   0xFFFF
#define RED     0xF800
#define GREEN   0x07E0
#define BLUE    0x001F
#define YELLOW  0xFFE0

void ST7735_Init(void);
void ST7735_FillScreen(uint16_t color);
void ST7735_FillRect(uint8_t x, uint8_t y, uint8_t width, uint8_t height, uint16_t color);
void ST7735_DrawString(uint16_t x, uint16_t y, char* str, uint16_t color, uint16_t bg);
void ST7735_DrawChar(uint16_t x, uint16_t y, char ch, uint16_t color, uint16_t bg);
#endif