#ifndef FREERTOS_CONFIG_H
#define FREERTOS_CONFIG_H

/* ================================================================
 * FreeRTOSConfig.h cho STM32F103C8T6 (Cortex-M3, 72MHz, 20KB SRAM)
 * Du an VEDC 2026 - cau hinh toi gian, du dung cho 1 task ung dung
 * (vAppTask) chay toan bo Learn/Check FSM nhu ban goc, giu nguyen
 * hanh vi, chi doi SysTick/delay sang co che an toan voi RTOS.
 * ================================================================ */

#define configUSE_PREEMPTION                   1
#define configUSE_IDLE_HOOK                    0
#define configUSE_TICK_HOOK                    0
#define configCPU_CLOCK_HZ                     ( SystemCoreClock )  /* 72 000 000 */
#define configTICK_RATE_HZ                     ( (TickType_t)1000 ) /* 1ms/tick - khop voi delay_ms() cu */
#define configMAX_PRIORITIES                   ( 5 )
#define configMINIMAL_STACK_SIZE                ( (uint16_t)128 )   /* stack Idle task, theo word (4 byte) */
#define configTOTAL_HEAP_SIZE                   ( (size_t)(6 * 1024) ) /* 6KB - con lai ~14KB cho .data/.bss khac */
#define configMAX_TASK_NAME_LEN                 ( 12 )
#define configUSE_16_BIT_TICKS                  0
#define configIDLE_SHOULD_YIELD                 1
#define configUSE_MUTEXES                       1
#define configUSE_RECURSIVE_MUTEXES             0
#define configUSE_COUNTING_SEMAPHORES           0
#define configQUEUE_REGISTRY_SIZE               0
#define configUSE_TIMERS                        0
#define configCHECK_FOR_STACK_OVERFLOW          2   /* bat kiem tra tran stack - bao qua freertos_hooks.c */
#define configUSE_MALLOC_FAILED_HOOK            1   /* bao qua freertos_hooks.c neu het heap FreeRTOS */
#define configUSE_TRACE_FACILITY                0
#define configGENERATE_RUN_TIME_STATS           0
#define configASSERT( x )  if( ( x ) == 0 ) { taskDISABLE_INTERRUPTS(); for( ;; ); }

/* --- API duoc bat (dung trong project: xTaskCreate, vTaskDelay...) --- */
#define INCLUDE_vTaskPrioritySet                1
#define INCLUDE_uxTaskPriorityGet               1
#define INCLUDE_vTaskDelete                     1
#define INCLUDE_vTaskSuspend                    1
#define INCLUDE_vTaskDelay                      1
#define INCLUDE_vTaskDelayUntil                 1
#define INCLUDE_xTaskGetSchedulerState          1
#define INCLUDE_xTaskGetCurrentTaskHandle        1

/* --- Cau hinh ngat theo yeu cau bat buoc cua port Cortex-M3 ---
 * STM32F10x dung 4 bit priority (khong co sub-priority) -> can
 * NVIC_PriorityGroupConfig(NVIC_PriorityGroup_4) TRUOC khi goi
 * vTaskStartScheduler() (da them trong main.c). */
#define configPRIO_BITS                         4
#define configLIBRARY_LOWEST_INTERRUPT_PRIORITY         15
#define configLIBRARY_MAX_SYSCALL_INTERRUPT_PRIORITY     5
#define configKERNEL_INTERRUPT_PRIORITY \
    ( configLIBRARY_LOWEST_INTERRUPT_PRIORITY << (8 - configPRIO_BITS) )
#define configMAX_SYSCALL_INTERRUPT_PRIORITY \
    ( configLIBRARY_MAX_SYSCALL_INTERRUPT_PRIORITY << (8 - configPRIO_BITS) )

/* Du an nay KHONG dung ngat UART (Protocol_RxPoll la polling), nen khong
 * can cau hinh them NVIC cho USART1 trong FreeRTOSConfig nay. Neu sau nay
 * chuyen UART sang ngat, nho dat priority >= configMAX_SYSCALL_INTERRUPT_PRIORITY
 * (so hien thi, khong phai gia tri da dich) de duoc phep goi ham FromISR. */

extern uint32_t SystemCoreClock;

#endif /* FREERTOS_CONFIG_H */
