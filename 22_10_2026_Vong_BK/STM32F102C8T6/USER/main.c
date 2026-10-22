#include "FreeRTOS.h"
#include "task.h"

#include "ina219.h"
#include "uart_lib.h"
#include "st7735.h"
#include "hw_config.h"
#include "protocol.h"
#include "learn_flow.h"
#include "check_flow.h"
#include <stdio.h>

char text_buf[32];

/* ======================================================
   CAU HINH NGOAI VI (GIU NGUYEN 100% SO VOI BAN GOC)
   ====================================================== */
void Hardware_Config(void) {
    GPIO_InitTypeDef GPIO_InitStruct;

    RCC_APB2PeriphClockCmd(RCC_APB2Periph_GPIOA |
                           RCC_APB2Periph_GPIOC |
                           RCC_APB2Periph_AFIO, ENABLE);

    /*
     * PA15 duoc dung lam ENA L298N.
     * PA15 mac dinh la chan JTAG, vi vay tat JTAG nhung van giu SWD
     * de ST-LINK con debug duoc.
     */
    GPIO_PinRemapConfig(GPIO_Remap_SWJ_JTAGDisable, ENABLE);

    /* Relay PC14 */
    GPIO_InitStruct.GPIO_Pin   = RELAY_PIN;
    GPIO_InitStruct.GPIO_Mode  = GPIO_Mode_Out_PP;
    GPIO_InitStruct.GPIO_Speed = GPIO_Speed_50MHz;
    GPIO_Init(RELAY_PORT, &GPIO_InitStruct);
    RELAY_OFF();

    /* LED trang thai PA6/PA7/PA8 - cau hinh som de LED DO sang ngay khi khoi dong */
    GPIO_InitStruct.GPIO_Pin   = LED_RED_PIN | LED_YELLOW_PIN | LED_GREEN_PIN;
    GPIO_InitStruct.GPIO_Mode  = GPIO_Mode_Out_PP;
    GPIO_InitStruct.GPIO_Speed = GPIO_Speed_50MHz;
    GPIO_Init(GPIOA, &GPIO_InitStruct);
    LED_MODE_POWER();

    /* L298N: tat bang chuyen ngay khi cap nguon */
    GPIO_InitStruct.GPIO_Pin   = MOTOR_IN1_PIN | MOTOR_IN2_PIN | MOTOR_ENA_PIN;
    GPIO_InitStruct.GPIO_Mode  = GPIO_Mode_Out_PP;
    GPIO_InitStruct.GPIO_Speed = GPIO_Speed_50MHz;
    GPIO_Init(GPIOA, &GPIO_InitStruct);
    GPIO_ResetBits(GPIOA, MOTOR_IN1_PIN | MOTOR_IN2_PIN | MOTOR_ENA_PIN);
}

/* ======================================================
   MAN HINH STANDBY (GIU NGUYEN 100%)
   ====================================================== */
void UI_Show_Standby(void) {
    LED_MODE_POWER();

    ST7735_FillScreen(BLACK);
    ST7735_DrawString(10, 10, "PTIT - VEDC 2026", YELLOW, BLACK);
    ST7735_DrawString(15, 40, "SYSTEM STANDBY", WHITE, BLACK);
    ST7735_DrawString(10, 70, "PB0 : LEARN", YELLOW, BLACK);
    ST7735_DrawString(10, 95, "PA3 : CHECK", GREEN, BLACK);
}

/* ======================================================
   TASK UNG DUNG DUY NHAT - CHAY DUNG TOAN BO LOGIC FSM CU
   ======================================================
   Day la ban port FreeRTOS "toi thieu rui ro": thay vi tach nho thanh
   nhieu task doc lap (co the lam lech thu tu/thoi gian tuong doi giua
   cac buoc so voi ban goc), toan bo vong lap dieu phoi Learn/Check/Standby
   duoc giu y nguyen, chi chay BEN TRONG 1 task FreeRTOS thay vi while(1)
   truc tiep trong main(). Hanh vi quan sat duoc (thu tu man hinh, thoi
   gian debounce, timeout UART...) khong doi so voi ban bare-metal.

   LOI ICH van co duoc du chi 1 task:
     - delay_ms()/vTaskDelay() nhuong CPU that su cho Idle task thay vi
       block cung CPU nhu SysTick busy-wait truoc day.
     - De mo rong sau nay: them task moi (vd task doc cam bien nen, task
       gui log MQTT qua UART khac...) ma khong dung cham vao FSM hien tai.
     - Co san co che vApplicationStackOverflowHook/MallocFailedHook de bat
       loi bo nho som thay vi treo may kho hieu.

   LUU Y: toan bo khoi tao phan cung duoc chuyen VAO TRONG task nay (thay vi
   o main() nhu ban goc), vi ST7735_Init() va mot so ham goi delay_ms(), ma
   delay_ms() gio dung vTaskDelay() - CHI hop le sau khi scheduler da chay. */
static void vAppTask(void *pvParameters) {
    (void)pvParameters;

    UART_configure(USART1, 115200);
    Hardware_Config();
    ST7735_Init();
    /* INA219 duoc khoi tao lai moi lan do sau khi relay cap nguon PCB. */
    Learn_Init();
    Check_Init();
    UI_Show_Standby();

    for (;;) {
        if (Learn_IsActive()) {
            /* Trong Learn: chi Learn_Update duoc phep dieu khien */
            Learn_Update();

            if (Learn_GetState() == LEARN_STATE_IDLE) {
                UI_Show_Standby();
            }
        }
        else if (Check_IsActive()) {
            /* Trong Check: chi Check_Update duoc phep dieu khien */
            Check_Update();
            /* 1 ms cadence for LDR confirmation and PWM ramp. */
            vTaskDelay(1);

            /* Neu sau nay muon tao nut thoat Check co the xu ly tai day */
        }
        else {
            /* Standby */
            if (GPIO_ReadInputDataBit(BTN_LEARN_PORT, BTN_LEARN_PIN) == 0) {
                delay_ms(50);
                if (GPIO_ReadInputDataBit(BTN_LEARN_PORT, BTN_LEARN_PIN) == 0) {
                    while (GPIO_ReadInputDataBit(BTN_LEARN_PORT, BTN_LEARN_PIN) == 0) {
                        delay_ms(10);
                    }
                    delay_ms(30);
                    Learn_Enter();
                }
            }
            else if (GPIO_ReadInputDataBit(BTN_CHECK_PORT, BTN_CHECK_PIN) == 0) {
                delay_ms(50);
                if (GPIO_ReadInputDataBit(BTN_CHECK_PORT, BTN_CHECK_PIN) == 0) {
                    while (GPIO_ReadInputDataBit(BTN_CHECK_PORT, BTN_CHECK_PIN) == 0) {
                        delay_ms(10);
                    }
                    delay_ms(30);
                    Check_Enter();
                }
            }
            else {
                /* Khong co nut nao duoc nhan: nhuong 1 tick cho Idle task
                   thay vi quay vong tron hoan toan - khong lam cham phan
                   ung nut bam (toi da cham 1ms), dung tinh than FreeRTOS. */
                vTaskDelay(1);
            }
        }
    }
}

/* ======================================================
   CHUONG TRINH CHINH
   ====================================================== */
int main(void) {
    /* Port FreeRTOS Cortex-M3 yeu cau dung 4 bit preemption priority,
       0 bit subpriority - BAT BUOC dat truoc khi vTaskStartScheduler(). */
    NVIC_PriorityGroupConfig(NVIC_PriorityGroup_4);

    /* Tao task ung dung duy nhat. Stack 512 word (2KB) - du cho cac bien
       cuc bo trong learn_flow.c/check_flow.c/st7735.c; theo doi qua
       vApplicationStackOverflowHook() neu can tang. */
    xTaskCreate(vAppTask, "AppTask", 512, NULL, 2, NULL);

    vTaskStartScheduler();

    /* Chi toi day neu het heap FreeRTOS luc tao task/idle task ban dau -
       khong bao gio xay ra trong dieu kien binh thuong. */
    for (;;) {
    }
}
