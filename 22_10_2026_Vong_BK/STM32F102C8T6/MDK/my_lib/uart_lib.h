#ifndef __UART_LIB_H_
#define __UART_LIB_H_

#include "stm32f10x.h"                  // Device header
#include <stdarg.h>


void UART_configure(USART_TypeDef* USARTx,uint32_t baudrate);
void UART_SendChar(USART_TypeDef *USARTx, char c);
void UART_SendString(USART_TypeDef *USARTx, char *str);
char UART_Recieve(USART_TypeDef *USARTx);
void Serial_Printf(USART_TypeDef *USARTx, const char *fmt, ...);
#endif
