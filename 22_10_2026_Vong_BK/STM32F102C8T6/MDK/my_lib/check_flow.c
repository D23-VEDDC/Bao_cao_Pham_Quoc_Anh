#include "check_flow.h"
#include "hw_config.h"
#include "protocol.h"
#include "ina219.h"
#include "st7735.h"
#include "FreeRTOS.h"
#include "task.h"
#include <stdio.h>
#include <stdint.h>
#include <string.h>

#define CHECK_UART_PORT      USART1
/* Visual/Start/Next van blocking theo yeu cau.
 * RIENG do dong dung polling de khong khoa main loop. */

static volatile check_state_t check_state = CHECK_STATE_IDLE;
static uint8_t board_idx = 1;
static uint8_t last_err_code = ERR_UNKNOWN;
static char text_buf[32];
static protocol_rx_context_t start_rx_ctx;
static protocol_rx_context_t visual_rx_ctx;
static protocol_rx_context_t power_rx_ctx;
static protocol_rx_context_t next_rx_ctx;
static uint8_t power_tx_data[9];
static INA219_Stats power_last_stats;
static uint8_t visual_tx_data[1];
static uint8_t next_tx_data[1];
static volatile uint8_t motor_running;
static volatile uint8_t motor_duty;
static TickType_t motor_ramp_tick;
static uint8_t ldr_seen;
static uint8_t ldr_blocked;
static volatile uint8_t ldr_armed;
static volatile uint8_t ldr_hit_pending;
static TickType_t ldr_edge_tick;
static TaskHandle_t ldr_task_handle;
/* ============================================================
 *  THOI GIAN RETRY GIAO TIEP CHECK
 *
 *  - CHECK_START / CHECK_NEXT             : 3 giay
 *  - VISUAL_CHECK ngoai quan              : 3 phut 20 giay
 *  - POWER_CHECK do dong                  : 5 giay
 *
 *  Dung uint32_t de luu 200000 ms, tranh overflow uint16_t.
 * ============================================================ */
#define RETRY_CHECK_START_MS    3000UL
#define RETRY_CHECK_VISUAL_MS 200000UL
#define RETRY_CHECK_POWER_MS    5000UL
#define RETRY_CHECK_NEXT_MS     3000UL

static uint32_t start_retry_ms = 0;
static uint32_t visual_retry_ms = 0;
static uint32_t power_retry_ms = 0;
static uint32_t next_retry_ms = 0;

/* ============================================================
 *  NUT BAM: INPUT PULL-UP, NHAN = 0
 * ============================================================ */
static uint8_t Button_IsPressed(GPIO_TypeDef *port, uint16_t pin) {
    if (GPIO_ReadInputDataBit(port, pin) == 0) {
        delay_ms(30);
        if (GPIO_ReadInputDataBit(port, pin) == 0) {
            return 1;
        }
    }
    return 0;
}

static void Button_WaitRelease(GPIO_TypeDef *port, uint16_t pin) {
    while (GPIO_ReadInputDataBit(port, pin) == 0) {
        delay_ms(10);
    }
    delay_ms(30);
}

/* ============================================================
 *  DIEU KHIEN BANG CHUYEN QUA L298N
 *  IN1 = PA11, IN2 = PA12, ENA = PA15/TIM2_CH1 full remap
 * ============================================================ */
static void Conveyor_SetDuty(uint8_t duty) {
    if (duty > 100U) duty = 100U;
    motor_duty = duty;
    TIM2->CCR1 = ((uint32_t)(CONVEYOR_PWM_PERIOD + 1U) * duty) / 100U;
}

static void Conveyor_Run(void) {
    taskENTER_CRITICAL();
    if (motor_running) {
        taskEXIT_CRITICAL();
        return;
    }
    GPIO_SetBits(MOTOR_PORT, MOTOR_IN1_PIN);
    GPIO_ResetBits(MOTOR_PORT, MOTOR_IN2_PIN);
    Conveyor_SetDuty(0U);
    motor_ramp_tick = xTaskGetTickCount();
    motor_running = 1U;
    taskEXIT_CRITICAL();
}

static void Conveyor_Stop(void) {
    /* Ngat PWM ngay; khong ramp xuong khi PCB che tia. */
    taskENTER_CRITICAL();
    Conveyor_SetDuty(0U);
    motor_running = 0U;
    GPIO_ResetBits(MOTOR_PORT, MOTOR_IN1_PIN);
    GPIO_ResetBits(MOTOR_PORT, MOTOR_IN2_PIN);
    taskEXIT_CRITICAL();
}

static void Conveyor_Service(TickType_t now) {
    if (!motor_running || motor_duty >= CONVEYOR_TARGET_DUTY) return;
    if ((TickType_t)(now - motor_ramp_tick) >= pdMS_TO_TICKS(CONVEYOR_RAMP_INTERVAL_MS)) {
        uint8_t duty = (uint8_t)(motor_duty + CONVEYOR_RAMP_STEP);
        if (duty > CONVEYOR_TARGET_DUTY) duty = CONVEYOR_TARGET_DUTY;
        Conveyor_SetDuty(duty);
        motor_ramp_tick = now;
    }
}

/* DO = 0 khi co tia; DO = 1 khi PCB che tia. Goi moi tick trong Check.
 * Chi nhan lan che tiep theo sau khi tia thong lien tuc 300 ms. */
static void LDR_Observe(TickType_t now) {
    uint8_t blocked = (GPIO_ReadInputDataBit(LDR_DETECT_PORT, LDR_DETECT_PIN)
                       == LDR_BLOCKED_LEVEL);
    if (!ldr_seen || blocked != ldr_blocked) {
        ldr_seen = 1U;
        ldr_blocked = blocked;
        ldr_edge_tick = now;
    }
    if (!blocked && !ldr_armed &&
        (TickType_t)(now - ldr_edge_tick) >= pdMS_TO_TICKS(LDR_CLEAR_CONFIRM_MS)) {
        ldr_armed = 1U;
    }
    if (blocked && ldr_armed && motor_running &&
        (check_state == CHECK_STATE_WAIT_PCB ||
         check_state == CHECK_STATE_NEXT_WAIT_ACK ||
         check_state == CHECK_STATE_WAIT_IR_CLEAR) &&
        (TickType_t)(now - ldr_edge_tick) >= pdMS_TO_TICKS(LDR_BLOCK_CONFIRM_MS)) {
        Conveyor_Stop();
        ldr_armed = 0U;
        ldr_hit_pending = 1U;
    }
}

/* High-priority 1 ms sampling remains responsive while the AppTask renders
 * TFT pages or waits for UART/INA219. The task never touches SPI or UART. */
static void LDR_Task(void *argument) {
    TickType_t wake = xTaskGetTickCount();
    (void)argument;
    for (;;) {
        if (check_state != CHECK_STATE_IDLE) {
            TickType_t now = xTaskGetTickCount();
            LDR_Observe(now);
            Conveyor_Service(now);
        }
        vTaskDelayUntil(&wake, pdMS_TO_TICKS(1U));
    }
}

/* ============================================================
 *  GIAO DIEN TFT CHO CHE DO CHECK
 * ============================================================ */
/* Five fixed rows; draw only rows whose text/color changed. This avoids
 * clearing the whole TFT on every state transition or error loop. */
#define UI_ROWS 5U
#define UI_LINE_CHARS 21U
static char ui_cache[UI_ROWS][UI_LINE_CHARS + 1U];
static uint16_t ui_color_cache[UI_ROWS];
static uint16_t ui_background;
static uint8_t ui_valid;

static void UI_SetLine(uint8_t row, const char *text, uint16_t color) {
    char clipped[UI_LINE_CHARS + 1U];
    if (row >= UI_ROWS) return;
    strncpy(clipped, text, UI_LINE_CHARS);
    clipped[UI_LINE_CHARS] = '\0';
    if (strcmp(ui_cache[row], clipped) == 0 && ui_color_cache[row] == color) return;
    ST7735_FillRect(0U, (uint8_t)(row * 32U), 128U, 32U, ui_background);
    strcpy(ui_cache[row], clipped);
    ui_color_cache[row] = color;
    if (clipped[0] != '\0') {
        ST7735_DrawString(4U, (uint16_t)(row * 32U + 10U), clipped,
                          color, ui_background);
    }
}

static void UI_Show(uint16_t bg,
                    const char *l0, uint16_t c0, const char *l1, uint16_t c1,
                    const char *l2, uint16_t c2, const char *l3, uint16_t c3,
                    const char *l4, uint16_t c4) {
    if (!ui_valid || ui_background != bg) {
        ST7735_FillScreen(bg);
        memset(ui_cache, 0, sizeof(ui_cache));
        memset(ui_color_cache, 0, sizeof(ui_color_cache));
        ui_background = bg;
        ui_valid = 1U;
    }
    UI_SetLine(0U, l0, c0);
    UI_SetLine(1U, l1, c1);
    UI_SetLine(2U, l2, c2);
    UI_SetLine(3U, l3, c3);
    UI_SetLine(4U, l4, c4);
}

static void UI_CheckStart(void) {
    UI_Show(BLACK, "PTIT - VEDC 2026", YELLOW,
            "CHE DO KIEM TRA", GREEN, "Dang khoi tao...", WHITE,
            "", WHITE, "", WHITE);
}

static void UI_WaitPCB(void) {
    sprintf(text_buf, "PCB thu: %u", board_idx);
    UI_Show(BLACK, "KIEM TRA BO MACH", GREEN,
            text_buf, WHITE, "Bang dang chay...", WHITE,
            ldr_armed ? "Cho PCB che tia" : "Cho tia thong 300ms", YELLOW,
            "", WHITE);
}

static void UI_PCBDetected(void) {
    sprintf(text_buf, "PCB thu: %u", board_idx);
    UI_Show(BLACK, "PCB DA DEN", GREEN, text_buf, WHITE,
            "BANG DA DUNG", YELLOW, "Nhan PA4 CHECK", WHITE, "", WHITE);
}

static void UI_VisualChecking(void) {
    sprintf(text_buf, "PCB thu: %u", board_idx);
    UI_Show(BLACK, "NGOAI QUAN", GREEN, text_buf, WHITE,
            "ESP32 DANG KIEM...", YELLOW, "", WHITE, "", WHITE);
}

static void UI_VisualPass(void) {
    UI_Show(BLACK, "NGOAI QUAN: PASS", GREEN,
            "KET QUA: PASS 1/2", WHITE, "Nhan PA5 de do dong", YELLOW,
            "", WHITE, "", WHITE);
}

static void UI_VisualFail(void) {
    UI_Show(BLACK, "NGOAI QUAN: FAIL", RED,
            "KET QUA: FAIL 1/2", WHITE, "Cho bo tiep theo...", YELLOW,
            "", WHITE, "", WHITE);
}

static void UI_PowerMeasuring(void) {
    sprintf(text_buf, "PCB thu: %u", board_idx);
    UI_Show(BLACK, "DO DONG DIEN", GREEN, text_buf, WHITE,
            "Dang do INA219...", YELLOW, "", WHITE, "", WHITE);
}

static void UI_PowerValue(float i_max, float i_avg) {
    char max_buf[22];
    char avg_buf[22];
    sprintf(max_buf, "Imax: %.1f mA", i_max);
    sprintf(avg_buf, "Iavg: %.1f mA", i_avg);
    UI_Show(BLACK, "KET QUA DO DONG", GREEN, max_buf, WHITE,
            avg_buf, WHITE, "ESP32 dang phan tich", YELLOW, "", WHITE);
}

static void UI_PowerWaitingESP32(void) {
    UI_SetLine(4U, "Cho ESP32-S3 RESULT", YELLOW);
}

static void UI_PowerI2CErrorRetry(void) {
    sprintf(text_buf, "PCB thu: %u", board_idx);
    UI_Show(RED, "LOI INA219 / I2C", WHITE, text_buf, WHITE,
            "Khong mat PCB hien tai", WHITE, "Nhan PA5 de do lai", YELLOW,
            "Giu ket qua anh", WHITE);
}

static void UI_FinalPass(void) {
    UI_Show(BLACK, "PCB: OK", GREEN, "PASS 2/2 GIAI DOAN", GREEN,
            "Cho PCB tiep theo...", WHITE, "", WHITE, "", WHITE);
}

static void UI_CurrentFail(void) {
    UI_Show(BLACK, "PCB: NG", RED, "PASS 1/2 GIAI DOAN", WHITE,
            "LOI DO DONG DIEN", RED, "Cho PCB tiep theo...", YELLOW,
            "", WHITE);
}

static void UI_OverCurrent(void) {
    UI_Show(RED, "QUA DONG!", WHITE, "DA NGAT RELAY", WHITE,
            "PCB = NG", WHITE, "Cho PCB tiep theo", WHITE, "", WHITE);
}

static void UI_Error(uint8_t err_code) {
    sprintf(text_buf, "Ma loi: 0x%02X", err_code);
    UI_Show(RED, "LOI KIEM TRA!", WHITE,
            err_code == ERR_INA219_FAIL ? "LOI INA219 / I2C" : "Kiem tra ESP32-S3", WHITE,
            err_code == ERR_INA219_FAIL ? "Kiem tra SDA/SCL" : "ESP32 / UART1", WHITE,
            text_buf, WHITE, "Nhan PA3 thu lai", WHITE);
}

/* ============================================================
 *  GIAO TIEP CHECK - NON-BLOCKING + INFINITE RETRY (THOI GIAN TUY BIEN)
 *  Khong co timeout giao tiep. Khi mat ACK/RESULT, STM32 gui lai
 *  cung transaction sau mot khoang nghi; ESP32-S3 xu ly idempotent.
 * ============================================================ */
static void Check_StartRequest(void) {
    Protocol_RxReset(&start_rx_ctx);
    Protocol_RxFlush(CHECK_UART_PORT);
    Protocol_SendFrame(CHECK_UART_PORT, CMD_CHECK_START, NULL, 0);
    start_retry_ms = 0;
}

static void Check_VisualRequest(void) {
    visual_tx_data[0] = board_idx;
    Protocol_RxReset(&visual_rx_ctx);
    Protocol_RxFlush(CHECK_UART_PORT);
    Protocol_SendFrame(CHECK_UART_PORT, CMD_VISUAL_CHECK, visual_tx_data, 1);
    visual_retry_ms = 0;
}

static void Check_PowerRequest(void) {
    Protocol_RxReset(&power_rx_ctx);
    Protocol_RxFlush(CHECK_UART_PORT);
    Protocol_SendFrame(CHECK_UART_PORT, CMD_POWER_CHECK, power_tx_data, 9);
    power_retry_ms = 0;
}

static void Check_NextRequest(void) {
    next_tx_data[0] = board_idx;
    Protocol_RxReset(&next_rx_ctx);
    Protocol_RxFlush(CHECK_UART_PORT);
    Protocol_SendFrame(CHECK_UART_PORT, CMD_CHECK_NEXT, next_tx_data, 1);
    next_retry_ms = 0;
}

static void Check_RetryStart(void) {
    Protocol_SendFrame(CHECK_UART_PORT, CMD_CHECK_START, NULL, 0);
    start_retry_ms = 0;
}

static void Check_RetryVisual(void) {
    Protocol_SendFrame(CHECK_UART_PORT, CMD_VISUAL_CHECK, visual_tx_data, 1);
    visual_retry_ms = 0;
}

static void Check_RetryPower(void) {
    Protocol_SendFrame(CHECK_UART_PORT, CMD_POWER_CHECK, power_tx_data, 9);
    power_retry_ms = 0;
}

static void Check_RetryNext(void) {
    Protocol_SendFrame(CHECK_UART_PORT, CMD_CHECK_NEXT, next_tx_data, 1);
    next_retry_ms = 0;
}

/* Init */
void Check_Init(void) {
    GPIO_InitTypeDef gpio;

    RCC_APB2PeriphClockCmd(RCC_APB2Periph_GPIOA | RCC_APB2Periph_AFIO, ENABLE);

    gpio.GPIO_Pin = LDR_DETECT_PIN;
    gpio.GPIO_Mode = GPIO_Mode_IPU;
    gpio.GPIO_Speed = GPIO_Speed_50MHz;
    GPIO_Init(LDR_DETECT_PORT, &gpio);

    gpio.GPIO_Pin = BTN_CHECK_PIN | BTN_VISUAL_PIN | BTN_POWER_PIN;
    gpio.GPIO_Mode = GPIO_Mode_IPU;
    GPIO_Init(GPIOA, &gpio);

    gpio.GPIO_Pin = LED_RED_PIN | LED_YELLOW_PIN | LED_GREEN_PIN;
    gpio.GPIO_Mode = GPIO_Mode_Out_PP;
    gpio.GPIO_Speed = GPIO_Speed_50MHz;
    GPIO_Init(GPIOA, &gpio);

    gpio.GPIO_Pin = MOTOR_IN1_PIN | MOTOR_IN2_PIN;
    gpio.GPIO_Mode = GPIO_Mode_Out_PP;
    gpio.GPIO_Speed = GPIO_Speed_50MHz;
    GPIO_Init(MOTOR_PORT, &gpio);

    GPIO_PinRemapConfig(GPIO_Remap_SWJ_JTAGDisable, ENABLE);
    GPIO_PinRemapConfig(GPIO_FullRemap_TIM2, ENABLE);

    /* PA15 = TIM2_CH1. TIM2 chay lien tuc, CCR1=0 giu ENA tat.
     * Can thao jumper ENA tren L298N va noi day PA15 den ENA. */
    RCC_APB1PeriphClockCmd(RCC_APB1Periph_TIM2, ENABLE);
    gpio.GPIO_Pin = MOTOR_ENA_PIN;
    gpio.GPIO_Mode = GPIO_Mode_AF_PP;
    gpio.GPIO_Speed = GPIO_Speed_50MHz;
    GPIO_Init(MOTOR_PORT, &gpio);
    TIM2->CR1 = 0U;
    TIM2->CCER = 0U;
    TIM2->PSC = 0U;
    TIM2->ARR = CONVEYOR_PWM_PERIOD;
    TIM2->CCR1 = 0U;
    TIM2->CCMR1 = 0x0068U; /* OC1M=110 PWM mode 1; OC1PE=1 */
    TIM2->EGR = TIM_EGR_UG;
    TIM2->CCER = TIM_CCER_CC1E;
    TIM2->CR1 = TIM_CR1_ARPE | TIM_CR1_CEN;

    Conveyor_Stop();
    LED_MODE_POWER();
    Protocol_RxReset(&start_rx_ctx);
    Protocol_RxReset(&visual_rx_ctx);
    Protocol_RxReset(&power_rx_ctx);
    Protocol_RxReset(&next_rx_ctx);
    check_state = CHECK_STATE_IDLE;
    motor_running = 0U;
    ldr_seen = 0U;
    ldr_armed = 0U;
    ldr_hit_pending = 0U;
    if (ldr_task_handle == NULL) {
        (void)xTaskCreate(LDR_Task, "LDR", 192U, NULL, 3U, &ldr_task_handle);
    }
}

void Check_Enter(void) {
    board_idx = 1;
    last_err_code = ERR_UNKNOWN;

    LED_MODE_CHECK();
    taskENTER_CRITICAL();
    Conveyor_Stop();
    ldr_seen = 0U;
    ldr_armed = 0U;
    ldr_hit_pending = 0U;
    taskEXIT_CRITICAL();
    ui_valid = 0U;
    UI_CheckStart();
    Protocol_RxReset(&start_rx_ctx);
    Protocol_RxReset(&visual_rx_ctx);
    Protocol_RxReset(&power_rx_ctx);
    Protocol_RxReset(&next_rx_ctx);
    Protocol_RxFlush(CHECK_UART_PORT);
    start_retry_ms = visual_retry_ms = power_retry_ms = next_retry_ms = 0;
    check_state = CHECK_STATE_STARTING;
}

uint8_t Check_IsActive(void) {
    return (check_state != CHECK_STATE_IDLE);
}

check_state_t Check_GetState(void) {
    return check_state;
}

/* ============================================================
 *  FSM KIEM TRA
 * ============================================================ */
void Check_Update(void) {
    /* Fallback if the sensor task could not be allocated. */
    if (ldr_task_handle == NULL) {
        TickType_t now = xTaskGetTickCount();
        LDR_Observe(now);
        Conveyor_Service(now);
    }
    switch (check_state) {

    case CHECK_STATE_IDLE:
        break;

    case CHECK_STATE_STARTING:
        Check_StartRequest();
        check_state = CHECK_STATE_STARTING_WAIT_ACK;
        break;

    case CHECK_STATE_STARTING_WAIT_ACK: {
        uart_frame_t rx;
        if (Protocol_RxPoll(CHECK_UART_PORT, &start_rx_ctx, &rx)) {
            if (rx.cmd == CMD_ACK) {
                check_state = ldr_armed ? CHECK_STATE_WAIT_PCB : CHECK_STATE_WAIT_IR_CLEAR;
                Conveyor_Run();
                UI_WaitPCB();
                break;
            }
            if (rx.cmd == CMD_ERROR) {
                last_err_code = (rx.len > 0) ? rx.data[0] : ERR_UNKNOWN;
                check_state = CHECK_STATE_ERROR;
                break;
            }
        }
        if (start_retry_ms < RETRY_CHECK_START_MS) start_retry_ms++;
        if (start_retry_ms >= RETRY_CHECK_START_MS) Check_RetryStart();
        break;
    }

    case CHECK_STATE_WAIT_PCB:
        if (ldr_hit_pending) {
            ldr_hit_pending = 0U;
            UI_PCBDetected();
            check_state = CHECK_STATE_VISUAL_WAIT;
        }
        break;

    case CHECK_STATE_VISUAL_WAIT:
        if (Button_IsPressed(BTN_VISUAL_PORT, BTN_VISUAL_PIN)) {
            Button_WaitRelease(BTN_VISUAL_PORT, BTN_VISUAL_PIN);
            UI_VisualChecking();
            Check_VisualRequest();
            check_state = CHECK_STATE_VISUAL_WAIT_RESPONSE;
        }
        break;

    case CHECK_STATE_VISUAL_WAIT_RESPONSE: {
        uart_frame_t rx;
        if (Protocol_RxPoll(CHECK_UART_PORT, &visual_rx_ctx, &rx)) {
            if (rx.cmd == CMD_ERROR) {
                last_err_code = (rx.len > 0) ? rx.data[0] : ERR_UNKNOWN;
                check_state = CHECK_STATE_ERROR;
                break;
            }
            if (rx.cmd == CMD_VISUAL_RESULT && rx.len >= 1) {
                if (rx.data[0] == CHECK_STATUS_PASS) {
                    UI_VisualPass();
                    check_state = CHECK_STATE_POWER_WAIT;
                } else if (rx.data[0] == CHECK_STATUS_FAIL) {
                    UI_VisualFail();
                    check_state = CHECK_STATE_NEXT_WAIT_ACK;
                    Check_NextRequest();
                    Conveyor_Run();
                }
                break;
            }
        }
        if (visual_retry_ms < RETRY_CHECK_VISUAL_MS) visual_retry_ms++;
        if (visual_retry_ms >= RETRY_CHECK_VISUAL_MS) Check_RetryVisual();
        break;
    }

    case CHECK_STATE_POWER_WAIT:
        if (Button_IsPressed(BTN_POWER_PORT, BTN_POWER_PIN)) {
            Button_WaitRelease(BTN_POWER_PORT, BTN_POWER_PIN);

            UI_PowerMeasuring();
            RELAY_ON();
            delay_ms(150);
            power_last_stats = INA219_Get_Measurement(500, SAFE_CURRENT_LIMIT,
                                                      RELAY_PORT, RELAY_PIN);
            RELAY_OFF();

            if (power_last_stats.i2c_error_flag) {
                last_err_code = ERR_INA219_FAIL;
                UI_PowerI2CErrorRetry();
                check_state = CHECK_STATE_POWER_WAIT;
                break;
            }

            if (power_last_stats.overcurrent_flag) {
                UI_OverCurrent();
                check_state = CHECK_STATE_NEXT_WAIT_ACK;
                Check_NextRequest();
                Conveyor_Run();
                break;
            }

            UI_PowerValue(power_last_stats.I_Max, power_last_stats.I_Average);
            power_tx_data[0] = board_idx;
            Protocol_PackFloat(&power_tx_data[1], power_last_stats.I_Max);
            Protocol_PackFloat(&power_tx_data[5], power_last_stats.I_Average);
            Check_PowerRequest();
            UI_PowerWaitingESP32();
            check_state = CHECK_STATE_POWER_WAIT_RESPONSE;
        }
        break;

    case CHECK_STATE_POWER_WAIT_RESPONSE: {
        uart_frame_t rx;
        if (Protocol_RxPoll(CHECK_UART_PORT, &power_rx_ctx, &rx)) {
            if (rx.cmd == CMD_ERROR) {
                last_err_code = (rx.len > 0) ? rx.data[0] : ERR_UNKNOWN;
                check_state = CHECK_STATE_ERROR;
                break;
            }
            if (rx.cmd == CMD_POWER_RESULT && rx.len >= 1 && rx.data[0] <= CHECK_STATUS_FAIL) {
                if (rx.data[0] == CHECK_STATUS_PASS) UI_FinalPass();
                else UI_CurrentFail();
                check_state = CHECK_STATE_NEXT_WAIT_ACK;
                Check_NextRequest();
                Conveyor_Run();
                break;
            }
        }
        if (power_retry_ms < RETRY_CHECK_POWER_MS) power_retry_ms++;
        if (power_retry_ms >= RETRY_CHECK_POWER_MS) Check_RetryPower();
        break;
    }

    case CHECK_STATE_NEXT_WAIT_ACK: {
        uart_frame_t rx;
        if (Protocol_RxPoll(CHECK_UART_PORT, &next_rx_ctx, &rx)) {
            if (rx.cmd == CMD_ACK) {
                board_idx++;
                if (ldr_hit_pending) {
                    ldr_hit_pending = 0U;
                    check_state = CHECK_STATE_VISUAL_WAIT;
                    UI_PCBDetected();
                } else if (ldr_armed) {
                    check_state = CHECK_STATE_WAIT_PCB;
                    UI_WaitPCB();
                } else {
                    check_state = CHECK_STATE_WAIT_IR_CLEAR;
                    UI_WaitPCB();
                }
                break;
            }
            if (rx.cmd == CMD_ERROR) {
                last_err_code = (rx.len > 0) ? rx.data[0] : ERR_UNKNOWN;
                check_state = CHECK_STATE_ERROR;
                break;
            }
        }
        if (next_retry_ms < RETRY_CHECK_NEXT_MS) next_retry_ms++;
        if (next_retry_ms >= RETRY_CHECK_NEXT_MS) Check_RetryNext();
        break;
    }

    case CHECK_STATE_WAIT_IR_CLEAR:
        if (ldr_hit_pending) {
            ldr_hit_pending = 0U;
            UI_PCBDetected();
            check_state = CHECK_STATE_VISUAL_WAIT;
        } else if (ldr_armed) {
            UI_WaitPCB();
            check_state = CHECK_STATE_WAIT_PCB;
        }
        break;

    case CHECK_STATE_ERROR:
        Conveyor_Stop();
        RELAY_OFF();
        UI_Error(last_err_code);
        if (Button_IsPressed(BTN_CHECK_PORT, BTN_CHECK_PIN)) {
            Button_WaitRelease(BTN_CHECK_PORT, BTN_CHECK_PIN);
            Check_Enter();
        }
        break;

    default:
        Conveyor_Stop();
        RELAY_OFF();
        check_state = CHECK_STATE_IDLE;
        break;
    }
}
