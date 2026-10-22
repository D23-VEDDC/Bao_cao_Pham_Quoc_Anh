#include "FreeRTOS.h"
#include "task.h"
#include "stm32f10x.h"

/* ================================================================
 * File nay BAT BUOC phai co khi dung FreeRTOS tren Cortex-M3.
 *
 * Port FreeRTOS ARM_CM3 can 3 interrupt handler duoi day goi dung ham
 * cua no. Du an nay TRUOC GIO KHONG CO file stm32f10x_it.c nao (chi
 * dung handler mac dinh yeu (weak) tu startup_stm32f10x_md.s), nen o
 * day dinh nghia THAT (strong) de tu dong ghi de ban weak mac dinh -
 * khong can sua startup file.
 *
 * QUAN TRONG: du an nay khong dung ngat nao khac (UART dang polling
 * qua Protocol_RxPoll, I2C cung polling), nen khong co xung dot handler
 * nao khac can xu ly them.
 * ================================================================ */

void PendSV_Handler(void) {
    xPortPendSVHandler();
}

void SVC_Handler(void) {
    vPortSVCHandler();
}

void SysTick_Handler(void) {
    xPortSysTickHandler();
}

/* ================================================================
 * HOOK BAO LOI - giup debug nhanh neu cau hinh RAM (configTOTAL_HEAP_SIZE,
 * stack cua vAppTask trong xTaskCreate) chua du cho project thuc te.
 * Dat breakpoint o 2 ham nay khi debug neu nghi ngo tran bo nho.
 * ================================================================ */
void vApplicationStackOverflowHook(TaskHandle_t xTask, char *pcTaskName) {
    (void)xTask;
    (void)pcTaskName;
    /* Task bi tran stack - tang so word truyen cho xTaskCreate() trong main.c */
    taskDISABLE_INTERRUPTS();
    for (;;) {
    }
}

void vApplicationMallocFailedHook(void) {
    /* Het heap FreeRTOS (configTOTAL_HEAP_SIZE trong FreeRTOSConfig.h) -
       tang configTOTAL_HEAP_SIZE neu RAM con du, hoac giam stack cac task. */
    taskDISABLE_INTERRUPTS();
    for (;;) {
    }
}
