#include "uart_lib.h"
#include <stdio.h>
#include "stm32f10x.h"                  // Device header

unsigned long UART_GetBaudRate(USART_TypeDef *USARTx, unsigned long Baud)
{
    unsigned long div;// phan nguyen
    unsigned long dec;// lam tron phan thap phan
    unsigned long final;//ket qua cuoi cung
    double clock;
    double temp;// phan thap phan 

    if(USARTx == USART1)
        clock = 72000000.0;
    else
        clock = 36000000.0;

    div = (unsigned long)(clock / (Baud * 16));// phan nguyen

    temp = 16 * ((clock / (Baud * 16)) - div);// phan thap phan

    dec = (unsigned long)(temp + 0.5);     // Làm tròn

    if(dec == 16)
    {
        dec = 0;
        div++;
    }

    final = (div << 4) | dec;

    return final;
}

void UART_configure(USART_TypeDef* USARTx,uint32_t baudrate){
	   /*Mo che do ngoai vi AF*/
	   RCC->APB2ENR |= 1<<0;
	   /* Enable Clock USART */
     if(USARTx== USART1){
		     RCC->APB2ENR |= 1<<14;// cap clock cho UART1
		 }
		 else if(USARTx==USART2){
		       RCC->APB1ENR |= 1<<17;
		 }
		 else{// USART3
		    RCC->APB2ENR |= 1<<18;
		 }
		//Baurdrate 
		 if(USARTx==USART1){//APB2
		    //USARTx->BRR = UART_GetBaudRate(USARTx,baudrate);
			 USARTx->BRR = 72000000/baudrate;
		 }
		 else{//APB1
		    //USARTx->BRR = UART_GetBaudRate(USARTx,baudrate);
			 USARTx->BRR = 36000000/baudrate;
		  }
			
		// enable TX,RX
    USARTx->CR1 |= 1 << 3;			//TX
    USARTx->CR1 |= 1 << 2;     //RX
		
		/* Enable USART */
   USARTx->CR1 |= 1<<13; 		

   //------- cau hinh  TX,RX ----------
  if(USARTx== USART1){
	     /* Enable clock GPIOA */
    RCC->APB2ENR |= 1<<2;// 0x04
    /* PA9 - TX: AF Push Pull, 50MHz */
    GPIOA->CRH |=  (0xB << 4);      // MODE9=11 (50MHz), CNF9=10 (AF PP)
    /* PA10 - RX: Floating input */
    GPIOA->CRH |=  (0x4 << 8);      // MODE10=00, CNF10=01
	}			
	else if(USARTx == USART2){
	   /* Enable clock GPIOA */
    RCC->APB2ENR |= 1<<2;// 0x04
    /* PA2 - TX: AF Push Pull, 50MHz */
    GPIOA->CRL |=  (0xB << 8);      // MODE9=11 (50MHz), CNF9=10 (AF PP)
    /* PA3 - RX: Floating input */
    GPIOA->CRL |=  (0x4 << 12);      // MODE10=00, CNF10=01
	}
	else if (USARTx == USART3){
	    /* Enable clock GPIOB */
    RCC->APB2ENR |= 1<<3;// 0x08
    /* PB10 - TX: AF Push Pull, 50MHz */
    GPIOB->CRH |=  (0xB << 8);      // MODE9=11 (50MHz), CNF9=10 (AF PP)
    /* PB11 - RX: Floating input */
    GPIOB->CRH |=  (0x4 << 12);      // MODE10=00, CNF10=01
	}
			 
}
// ham sen 1 ky tu
void UART_SendChar(USART_TypeDef *USARTx, char c){
    while (!(USARTx->SR & USART_SR_TXE));  // cho truyen xong 1 ki tu TXE=1
	   USARTx->DR = c;                     	

}
// ham send string
void UART_SendString(USART_TypeDef *USARTx, char *str){
    while (*str)
    {
        UART_SendChar(USARTx, *str++);
    }

}
// ham nhan 1 ky tu
char UART_Recieve(USART_TypeDef *USARTx){
	   char c ;
     while(!(USARTx -> SR & USART_SR_RXNE));// cho cho den khi nhan duoc 1 ky tu
	   c = USARTx ->DR;
	   return c;
}

void Serial_Printf(USART_TypeDef *USARTx, const char *fmt, ...)
{
    char buffer[128];

    va_list args;

    va_start(args, fmt);

    vsprintf(buffer, fmt, args);

    va_end(args);

    UART_SendString(USARTx, buffer);
}

 
