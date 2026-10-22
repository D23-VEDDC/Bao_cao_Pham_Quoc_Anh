#include "learn_flow.h"
#include "hw_config.h"
#include "protocol.h"
#include "ina219.h"
#include "st7735.h"
#include "uart_lib.h"
#include <stdio.h>
#include <stdint.h>

#define UART_PORT          USART1
/* CAPTURE/TRAIN van cho ESP32 theo blocking nhu yeu cau.
 * RIENG do dong dung state machine polling de khong khoa main loop. */

static learn_state_t learn_state    = LEARN_STATE_IDLE;
static uint8_t        vision_idx    = 0;   /* 1..10, mau anh ngoai quan hien tai */
static uint8_t        power_idx     = 0;   /* 1..10, mau do dong hien tai */
static uint8_t        last_err_code = 0;
static uint8_t        error_page_shown = 0;
static char            text_buf[32];
static protocol_rx_context_t power_rx_ctx;
static protocol_rx_context_t vision_rx_ctx;
static protocol_rx_context_t train_rx_ctx;
static uint8_t         power_tx_data[9];
static uint8_t         vision_tx_data[1];
static INA219_Stats    power_last_stats;

/* ============================================================
 *  THOI GIAN RETRY GIAO TIEP STM32 <-> ESP32-S3
 *
 *  - LEARN_START / CHECK_START / CHECK_NEXT : 3 giay
 *  - CAPTURE ngoai quan                   : 3 phut 20 giay
 *  - POWER_SAMPLE / POWER_CHECK           : 5 giay
 *  - TRAIN                                 : 15 giay
 *
 *  Capture dai vi ESP32-S3 thuc te co the mat ~3 phut de
 *  trich feature 1 anh. uint32_t la bat buoc vi 200000 ms
 *  lon hon gioi han uint16_t (65535 ms).
 * ============================================================ */
#define RETRY_LEARN_START_MS    3000UL
#define RETRY_VISION_MS       200000UL
#define RETRY_POWER_MS          5000UL
#define RETRY_TRAIN_MS         15000UL

/* Retry UART khong timeout: chi gui lai cung mot request dinh ky.
 * Khong bao gio do lai INA219/capture/train chi vi mat ACK. */
static uint32_t learn_start_retry_ms = 0;
static uint32_t vision_retry_ms = 0;
static uint32_t power_retry_ms = 0;
static uint32_t train_retry_ms = 0;

/* Toan bo thong tin ESP32-S3 tra ve trong CMD_TRAIN_DONE (26 bytes cho 5 mau) */
static uint16_t train_patch_count[LEARN_SAMPLE_COUNT];
static float    train_image_threshold = 0.0f;
static uint16_t train_memory_bank_count = 0;
static float    train_imax_dev = 0.0f;
static float    train_iavg_dev = 0.0f;
static uint16_t train_elapsed_ms = 0;

/* ============================================================
 *  DOC NUT BAM (active-low, pull-up) CO CHONG RUNG DON GIAN
 *  Dung blocking delay ngan (30ms) - chap nhan duoc vi quy trinh Learn
 *  la thao tac tuong tac cham cua nguoi van hanh, khong o trong vong lap
 *  thoi gian thuc cua san xuat.
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
    delay_ms(30); /* chong rung khi nha nut */
}

/* ============================================================
 *  CAC MAN HINH TFT CHO TUNG BUOC
 * ============================================================ */
static void UI_Intro(void) {
    ST7735_FillScreen(BLACK);
    ST7735_DrawString(10, 10, "PTIT - VEDC 2026", YELLOW, BLACK);
    ST7735_DrawString(15, 60, "CHE DO LEARN", WHITE, BLACK);
    ST7735_DrawString(10, 90, "Dang khoi tao...", GREEN, BLACK);
}

static void UI_VisionPrompt(uint8_t idx) {
    ST7735_FillScreen(BLACK);
    ST7735_DrawString(10, 10, "LEARN - NGOAI QUAN", YELLOW, BLACK);
    sprintf(text_buf, "Mau chuan: %d / %d", idx, LEARN_SAMPLE_COUNT);
    ST7735_DrawString(10, 50, text_buf, WHITE, BLACK);
    ST7735_DrawString(10, 80, "Dat PCB chuan len bang", WHITE, BLACK);
    ST7735_DrawString(10, 100, "Nhan CAPTURE de chup", GREEN, BLACK);
}

static void UI_VisionCapturing(uint8_t idx) {
    ST7735_FillScreen(BLACK);
    sprintf(text_buf, "Dang chup mau %d...", idx);
    ST7735_DrawString(10, 70, text_buf, WHITE, BLACK);
}

static void UI_PowerPrompt(uint8_t idx) {
    ST7735_FillScreen(BLACK);
    ST7735_DrawString(10, 10, "LEARN - DO DONG", YELLOW, BLACK);
    sprintf(text_buf, "Mau chuan: %d / %d", idx, LEARN_SAMPLE_COUNT);
    ST7735_DrawString(10, 50, text_buf, WHITE, BLACK);
    ST7735_DrawString(10, 80, "Dat PCB chuan len bang", WHITE, BLACK);
    ST7735_DrawString(10, 100, "Nhan MEASURE de do dong", GREEN, BLACK);
}

static void UI_PowerMeasuring(uint8_t idx) {
    ST7735_FillScreen(BLACK);
    sprintf(text_buf, "Dang do dong mau %d...", idx);
    ST7735_DrawString(10, 70, text_buf, WHITE, BLACK);
}

static void UI_PowerWaitingESP32(uint8_t idx) {
    ST7735_FillScreen(BLACK);
    ST7735_DrawString(10, 25, "DO DONG XONG", GREEN, BLACK);
    sprintf(text_buf, "Mau: %d / %d", idx, LEARN_SAMPLE_COUNT);
    ST7735_DrawString(10, 50, text_buf, WHITE, BLACK);
    ST7735_DrawString(10, 80, "Cho ESP32-S3 ACK...", YELLOW, BLACK);
    ST7735_DrawString(10, 110, "Khong khoa he thong", WHITE, BLACK);
}

static void UI_PowerI2CErrorRetry(uint8_t idx) {
    ST7735_FillScreen(RED);
    ST7735_DrawString(8, 12, "LOI INA219 / I2C", WHITE, RED);
    sprintf(text_buf, "Mau %d / %d", idx, LEARN_SAMPLE_COUNT);
    ST7735_DrawString(8, 42, text_buf, WHITE, RED);
    ST7735_DrawString(8, 70, "Khong mat du lieu", WHITE, RED);
    ST7735_DrawString(8, 92, "Nhan MEASURE de do lai", YELLOW, RED);
    ST7735_DrawString(8, 118, "giu nguyen mau hien tai", WHITE, RED);
}

static void UI_ReadyTrain(void) {
    ST7735_FillScreen(BLACK);
    ST7735_DrawString(10, 10, "DA DU DU LIEU", YELLOW, BLACK);
    sprintf(text_buf, "Ngoai quan: %d/%d mau", LEARN_SAMPLE_COUNT, LEARN_SAMPLE_COUNT);
    ST7735_DrawString(10, 45, text_buf, WHITE, BLACK);
    sprintf(text_buf, "Dong dien : %d/%d mau", LEARN_SAMPLE_COUNT, LEARN_SAMPLE_COUNT);
    ST7735_DrawString(10, 65, text_buf, WHITE, BLACK);
    ST7735_DrawString(10, 100, "Nhan TRAIN de hoc", GREEN, BLACK);
}

static void UI_Training(void) {
    ST7735_FillScreen(BLACK);
    ST7735_DrawString(10, 60, "DANG HOC...", WHITE, BLACK);
    ST7735_DrawString(10, 90, "Vui long cho", BLUE, BLACK);
}

static void UI_TrainResultPage1(void) {
    uint8_t i;
    ST7735_FillScreen(BLACK);
    ST7735_DrawString(8, 8, "HOC XONG!", GREEN, BLACK);

    sprintf(text_buf, "Threshold: %.3f", train_image_threshold);
    ST7735_DrawString(4, 28, text_buf, WHITE, BLACK);

    sprintf(text_buf, "MemoryBank: %u", (unsigned)train_memory_bank_count);
    ST7735_DrawString(4, 40, text_buf, WHITE, BLACK);

    for (i = 0; i < LEARN_SAMPLE_COUNT; ++i) {
        sprintf(text_buf, "P%u:%u", (unsigned)(i + 1),
                (unsigned)train_patch_count[i]);
        ST7735_DrawString(4, (uint16_t)(54 + 10 * i), text_buf, YELLOW, BLACK);
    }
}

static void UI_TrainResultPage2(void) {
    ST7735_FillScreen(BLACK);
    ST7735_DrawString(8, 8, "NGUONG DONG DIEN", YELLOW, BLACK);

    sprintf(text_buf, "Imax dev: %.2f", train_imax_dev);
    ST7735_DrawString(4, 38, text_buf, WHITE, BLACK);
    ST7735_DrawString(4, 57, "mA", WHITE, BLACK);

    sprintf(text_buf, "Iavg dev: %.2f", train_iavg_dev);
    ST7735_DrawString(4, 82, text_buf, WHITE, BLACK);
    ST7735_DrawString(4, 101, "mA", WHITE, BLACK);

    ST7735_DrawString(4, 127, "(+/- so voi gia tri TB)", GREEN, BLACK);
}

static void UI_TrainResultPage3(void) {
    ST7735_FillScreen(BLACK);
    ST7735_DrawString(8, 8, "THONG TIN TRAIN", GREEN, BLACK);

    sprintf(text_buf, "Thoi gian: %u ms", (unsigned)train_elapsed_ms);
    ST7735_DrawString(4, 38, text_buf, WHITE, BLACK);

    sprintf(text_buf, "Ngoai quan: %d/%d", LEARN_SAMPLE_COUNT, LEARN_SAMPLE_COUNT);
    ST7735_DrawString(4, 68, text_buf, WHITE, BLACK);
    sprintf(text_buf, "Dong dien : %d/%d", LEARN_SAMPLE_COUNT, LEARN_SAMPLE_COUNT);
    ST7735_DrawString(4, 90, text_buf, WHITE, BLACK);
    ST7735_DrawString(4, 116, "SAN SANG CHECK", GREEN, BLACK);
}

static void UI_Error(uint8_t err_code) {
    ST7735_FillScreen(RED);
    ST7735_DrawString(10, 20, "LOI LEARN!", WHITE, RED);
    if (err_code == ERR_INA219_FAIL) {
        ST7735_DrawString(10, 50, "LOI INA219 / I2C", WHITE, RED);
        ST7735_DrawString(10, 80, "Kiem tra SDA/SCL", WHITE, RED);
    } else {
        ST7735_DrawString(10, 50, "Kiem tra ESP32-S3", WHITE, RED);
        ST7735_DrawString(10, 80, "ESP32 / UART1", WHITE, RED);
    }
    sprintf(text_buf, "Ma loi: 0x%02X", err_code);
    ST7735_DrawString(10, 105, text_buf, WHITE, RED);
    ST7735_DrawString(10, 125, "Nhan LEARN de thu lai", WHITE, RED);
}

/* ============================================================
 *  KHOI TAO 4 NUT BAM RIENG CHO QUY TRINH LEARN
 * ============================================================ */
void Learn_Init(void) {
    GPIO_InitTypeDef gpio;
    RCC_APB2PeriphClockCmd(RCC_APB2Periph_GPIOA | RCC_APB2Periph_GPIOB, ENABLE);

    gpio.GPIO_Mode  = GPIO_Mode_IPU;   /* input pull-up, nhan = muc 0 */
    gpio.GPIO_Speed = GPIO_Speed_50MHz;

    gpio.GPIO_Pin = BTN_LEARN_PIN;
    GPIO_Init(BTN_LEARN_PORT, &gpio);

    gpio.GPIO_Pin = BTN_CAPTURE_PIN;
    GPIO_Init(BTN_CAPTURE_PORT, &gpio);

    gpio.GPIO_Pin = BTN_MEASURE_PIN;
    GPIO_Init(BTN_MEASURE_PORT, &gpio);

    gpio.GPIO_Pin = BTN_TRAIN_PIN;
    GPIO_Init(BTN_TRAIN_PORT, &gpio);

    Protocol_RxReset(&power_rx_ctx);
    learn_state = LEARN_STATE_IDLE;
    error_page_shown = 0;
}

void Learn_Enter(void) {
    uint8_t i;

    LED_MODE_LEARN();
    vision_idx    = 0;
    power_idx     = 0;
    last_err_code = 0;
    error_page_shown = 0;

    train_image_threshold = 0.0f;
    train_memory_bank_count = 0;
    train_imax_dev = 0.0f;
    train_iavg_dev = 0.0f;
    train_elapsed_ms = 0;
    for (i = 0; i < LEARN_SAMPLE_COUNT; ++i) {
        train_patch_count[i] = 0;
    }

    Protocol_RxReset(&power_rx_ctx);
    Protocol_RxReset(&vision_rx_ctx);
    Protocol_RxReset(&train_rx_ctx);
    Protocol_RxFlush(UART_PORT);
    learn_start_retry_ms = 0;
    vision_retry_ms = 0;
    power_retry_ms = 0;
    train_retry_ms = 0;
    learn_state = LEARN_STATE_INTRO;
}

uint8_t Learn_IsActive(void) {
    return (learn_state != LEARN_STATE_IDLE);
}

learn_state_t Learn_GetState(void) {
    return learn_state;
}

/* ============================================================
 *  GIAI MA PAYLOAD CMD_TRAIN_DONE (ESP32-S3 gui 26 bytes cho 5 mau)
 *
 *  DATA[0..9]   : P1..P5 patch count (uint16 little-endian)
 *  DATA[10..13] : image threshold (float little-endian)
 *  DATA[14..15] : Memory Bank count (uint16 little-endian)
 *  DATA[16..19] : Imax deviation cho phep (float, mA)
 *  DATA[20..23] : Iavg deviation cho phep (float, mA)
 *  DATA[24..25] : thoi gian train (ms, uint16 little-endian)
 * ============================================================ */
static uint16_t UnpackU16LE(const uint8_t *src) {
    return (uint16_t)src[0] | ((uint16_t)src[1] << 8);
}

static uint8_t DecodeTrainDone(const uart_frame_t *rx) {
    uint8_t i;

    if ((rx->cmd != CMD_TRAIN_DONE) || (rx->len < 26)) {
        return 0;
    }

    for (i = 0; i < LEARN_SAMPLE_COUNT; ++i) {
        train_patch_count[i] = UnpackU16LE(&rx->data[i * 2]);
    }

    train_image_threshold  = Protocol_UnpackFloat(&rx->data[10]);
    train_memory_bank_count = UnpackU16LE(&rx->data[14]);
    train_imax_dev          = Protocol_UnpackFloat(&rx->data[16]);
    train_iavg_dev          = Protocol_UnpackFloat(&rx->data[20]);
    train_elapsed_ms        = UnpackU16LE(&rx->data[24]);

    return 1;
}

/* ============================================================
 *  GIAO DICH CAPTURE - KHONG BLOCKING + RETRY CUNG SAMPLE INDEX (3p20s)
 * ============================================================ */
static void Start_VisionCapture(uint8_t idx) {
    vision_tx_data[0] = idx;
    Protocol_RxReset(&vision_rx_ctx);
    Protocol_RxFlush(UART_PORT);
    Protocol_SendFrame(UART_PORT, CMD_CAPTURE_SAMPLE, vision_tx_data, 1);
    vision_retry_ms = 0;
    UI_VisionCapturing(idx);
}

/* Tra ve: 1 = ACK dung sample, 2 = ERROR, 0 = chua co frame hop le. */
static uint8_t Poll_VisionCaptureResponse(uint8_t idx) {
    uart_frame_t rx;
    if (!Protocol_RxPoll(UART_PORT, &vision_rx_ctx, &rx)) return 0;

    if (rx.cmd == CMD_ERROR) {
        last_err_code = (rx.len > 0) ? rx.data[0] : ERR_UNKNOWN;
        return 2;
    }
    if (rx.cmd != CMD_CAPTURE_ACK || rx.len < 2) return 0;
    if (rx.data[0] != idx) return 0;  /* ACK cua sample khac -> bo qua */
    if (rx.data[1] != RESULT_PASS) {
        last_err_code = ERR_CAMERA_FAIL;
        return 2;
    }
    return 1;
}

/* ============================================================
 *  DO DONG TAI CHO + CACHE PAYLOAD DE CO THE GUI LAI
 * ============================================================ */
static uint8_t Do_PowerMeasureAndSend(uint8_t idx) {
    UI_PowerMeasuring(idx);

    RELAY_ON();
    delay_ms(150);
    power_last_stats = INA219_Get_Measurement(500, SAFE_CURRENT_LIMIT,
                                              RELAY_PORT, RELAY_PIN);
    RELAY_OFF();

    if (power_last_stats.i2c_error_flag) {
        last_err_code = ERR_INA219_FAIL;
        return 0;
    }

    if (power_last_stats.overcurrent_flag) {
        last_err_code = ERR_UNKNOWN;
        return 0;
    }

    power_tx_data[0] = idx;
    Protocol_PackFloat(&power_tx_data[1], power_last_stats.I_Max);
    Protocol_PackFloat(&power_tx_data[5], power_last_stats.I_Average);

    Protocol_RxReset(&power_rx_ctx);
    Protocol_RxFlush(UART_PORT);
    Protocol_SendFrame(UART_PORT, CMD_POWER_SAMPLE, power_tx_data, 9);
    power_retry_ms = 0;
    UI_PowerWaitingESP32(idx);
    return 1;
}

/* Gui lai dung payload da do - TUYET DOI khong do INA219 lai khi mat ACK. */
static void Retry_PowerSample(void) {
    Protocol_SendFrame(UART_PORT, CMD_POWER_SAMPLE, power_tx_data, 9);
    power_retry_ms = 0;
}

static void Retry_TrainStart(void) {
    Protocol_SendFrame(UART_PORT, CMD_TRAIN_START, NULL, 0);
    train_retry_ms = 0;
}

/* ============================================================
 *  FSM CHINH - GOI LIEN TUC TRONG VONG LAP while(1) CUA main()
 * ============================================================ */
void Learn_Update(void) {
    switch (learn_state) {

    case LEARN_STATE_IDLE:
        break;

    case LEARN_STATE_INTRO:
        UI_Intro();
        delay_ms(1000);
        learn_state = LEARN_STATE_SEND_LEARN_START;
        break;

    case LEARN_STATE_SEND_LEARN_START:
        /* Bat dau mot transaction LEARN_START. Neu ACK mat, state tiep theo
           se tu dong gui lai cung lenh, khong reset du lieu o STM32. */
        Protocol_RxReset(&power_rx_ctx);
        Protocol_RxFlush(UART_PORT);
        Protocol_SendFrame(UART_PORT, CMD_LEARN_START, NULL, 0);
        learn_start_retry_ms = 0;
        learn_state = LEARN_STATE_WAIT_LEARN_START_ACK;
        break;

    case LEARN_STATE_WAIT_LEARN_START_ACK: {
        uart_frame_t rx;
        if (Protocol_RxPoll(UART_PORT, &power_rx_ctx, &rx)) {
            if (rx.cmd == CMD_ACK) {
                vision_idx = 1;
                UI_VisionPrompt(vision_idx);
                learn_state = LEARN_STATE_WAIT_VISION_CAPTURE;
                break;
            }
            if (rx.cmd == CMD_ERROR) {
                last_err_code = (rx.len > 0) ? rx.data[0] : ERR_UNKNOWN;
                learn_state = LEARN_STATE_ERROR;
                break;
            }
        }
        if (learn_start_retry_ms < RETRY_LEARN_START_MS) learn_start_retry_ms++;
        if (learn_start_retry_ms >= RETRY_LEARN_START_MS) {
            Protocol_SendFrame(UART_PORT, CMD_LEARN_START, NULL, 0);
            learn_start_retry_ms = 0;
        }
        delay_ms(1);
        break;
    }

    case LEARN_STATE_WAIT_VISION_CAPTURE:
        /* Prompt da duoc ve khi nhan ACK hoac khi chuyen sang mau ke tiep. */
        if (Button_IsPressed(BTN_CAPTURE_PORT, BTN_CAPTURE_PIN)) {
            Button_WaitRelease(BTN_CAPTURE_PORT, BTN_CAPTURE_PIN);
            Start_VisionCapture(vision_idx);
            learn_state = LEARN_STATE_WAIT_VISION_RESPONSE;
        } else {
            delay_ms(1);
        }
        break;

    case LEARN_STATE_WAIT_VISION_RESPONSE: {
        uint8_t r = Poll_VisionCaptureResponse(vision_idx);
        if (r == 1) {
            if (vision_idx >= LEARN_SAMPLE_COUNT) {
                power_idx = 1;
                UI_PowerPrompt(power_idx);
                learn_state = LEARN_STATE_WAIT_POWER_MEASURE;
            } else {
                vision_idx++;
                UI_VisionPrompt(vision_idx);
                learn_state = LEARN_STATE_WAIT_VISION_CAPTURE;
            }
            break;
        }
        if (r == 2) {
            learn_state = LEARN_STATE_ERROR;
            break;
        }
        if (vision_retry_ms < RETRY_VISION_MS) vision_retry_ms++;
        if (vision_retry_ms >= RETRY_VISION_MS) {
            /* Gui lai cung sample idx; ESP32-S3 phai xu ly idempotent. */
            Protocol_SendFrame(UART_PORT, CMD_CAPTURE_SAMPLE, vision_tx_data, 1);
            vision_retry_ms = 0;
        }
        delay_ms(1);
        break;
    }

    case LEARN_STATE_WAIT_POWER_MEASURE:
        /* Giu nguyen man hinh trong luc cho nut MEASURE. */
        if (Button_IsPressed(BTN_MEASURE_PORT, BTN_MEASURE_PIN)) {
            Button_WaitRelease(BTN_MEASURE_PORT, BTN_MEASURE_PIN);
            if (Do_PowerMeasureAndSend(power_idx)) {
                learn_state = LEARN_STATE_WAIT_POWER_RESPONSE;
            } else if (last_err_code == ERR_INA219_FAIL) {
                UI_PowerI2CErrorRetry(power_idx);
                learn_state = LEARN_STATE_POWER_ERROR_RETRY;
            } else {
                learn_state = LEARN_STATE_ERROR;
            }
        } else {
            delay_ms(1);
        }
        break;

    case LEARN_STATE_POWER_ERROR_RETRY:
        if (Button_IsPressed(BTN_MEASURE_PORT, BTN_MEASURE_PIN)) {
            Button_WaitRelease(BTN_MEASURE_PORT, BTN_MEASURE_PIN);
            UI_PowerPrompt(power_idx);
            learn_state = LEARN_STATE_WAIT_POWER_MEASURE;
        } else {
            delay_ms(1);
        }
        break;

    case LEARN_STATE_WAIT_POWER_RESPONSE: {
        uart_frame_t power_rx_frame;
        if (Protocol_RxPoll(UART_PORT, &power_rx_ctx, &power_rx_frame)) {
            if (power_rx_frame.cmd == CMD_ERROR) {
                last_err_code = (power_rx_frame.len > 0) ? power_rx_frame.data[0] : ERR_UNKNOWN;
                learn_state = LEARN_STATE_ERROR;
                break;
            }
            if (power_rx_frame.cmd == CMD_POWER_ACK && power_rx_frame.len >= 3) {
                if (power_rx_frame.data[0] == power_idx &&
                    power_rx_frame.data[1] == RESULT_PASS &&
                    power_rx_frame.data[2] >= power_idx) {
                    if (power_idx >= LEARN_SAMPLE_COUNT) {
                        UI_ReadyTrain();
                        learn_state = LEARN_STATE_READY_TRAIN;
                    } else {
                        power_idx++;
                        UI_PowerPrompt(power_idx);
                        learn_state = LEARN_STATE_WAIT_POWER_MEASURE;
                    }
                    break;
                }
                /* ACK mau khac -> bo qua, khong tang index. */
            }
        }

        if (power_retry_ms < RETRY_POWER_MS) power_retry_ms++;
        if (power_retry_ms >= RETRY_POWER_MS) {
            Retry_PowerSample();
        }
        delay_ms(1);
        break;
    }

    case LEARN_STATE_READY_TRAIN:
        /* Trang READY da duoc ve mot lan sau ACK cua mau dong thu 10. */
        if (Button_IsPressed(BTN_TRAIN_PORT, BTN_TRAIN_PIN)) {
            Button_WaitRelease(BTN_TRAIN_PORT, BTN_TRAIN_PIN);
            Protocol_RxReset(&train_rx_ctx);
            Protocol_RxFlush(UART_PORT);
            Protocol_SendFrame(UART_PORT, CMD_TRAIN_START, NULL, 0);
            train_retry_ms = 0;
            UI_Training();
            learn_state = LEARN_STATE_TRAINING;
        } else {
            delay_ms(1);
        }
        break;

    case LEARN_STATE_TRAINING: {
        uart_frame_t rx;
        if (Protocol_RxPoll(UART_PORT, &train_rx_ctx, &rx)) {
            if (rx.cmd == CMD_TRAIN_DONE) {
                if (!DecodeTrainDone(&rx)) {
                    last_err_code = ERR_UNKNOWN;
                    learn_state = LEARN_STATE_ERROR;
                } else {
                    UI_TrainResultPage1();
                    learn_state = LEARN_STATE_DONE;
                }
                break;
            }
            if (rx.cmd == CMD_ERROR) {
                last_err_code = (rx.len > 0) ? rx.data[0] : ERR_UNKNOWN;
                learn_state = LEARN_STATE_ERROR;
                break;
            }
        }
        if (train_retry_ms < RETRY_TRAIN_MS) train_retry_ms++;
        if (train_retry_ms >= RETRY_TRAIN_MS) {
            Retry_TrainStart();
        }
        delay_ms(1);
        break;
    }

    case LEARN_STATE_DONE:
        delay_ms(1800);
        UI_TrainResultPage2();
        delay_ms(1800);
        UI_TrainResultPage3();
        delay_ms(1800);
        learn_state = LEARN_STATE_IDLE;
        break;

    case LEARN_STATE_ERROR:
        LED_MODE_LEARN();
        if (!error_page_shown) {
            UI_Error(last_err_code);
            error_page_shown = 1;
        }
        if (Button_IsPressed(BTN_LEARN_PORT, BTN_LEARN_PIN)) {
            Button_WaitRelease(BTN_LEARN_PORT, BTN_LEARN_PIN);
            Learn_Enter();
        } else {
            delay_ms(1);
        }
        break;

    default:
        learn_state = LEARN_STATE_IDLE;
        break;
    }
}
