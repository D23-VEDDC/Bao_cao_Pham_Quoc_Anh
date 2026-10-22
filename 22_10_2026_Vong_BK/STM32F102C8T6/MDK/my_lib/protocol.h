#ifndef __PROTOCOL_H
#define __PROTOCOL_H

#include "stm32f10x.h"
#include <string.h>

#define FRAME_STX        0xAA
#define FRAME_ETX        0x55
#define FRAME_MAX_DATA   36
#define RESULT_PASS 0x00

typedef enum {
    /* ---------------- STM32 -> ESP32-S3 ---------------- */
    CMD_LEARN_START      = 0x10,
    CMD_CAPTURE_SAMPLE   = 0x11,
    CMD_POWER_SAMPLE     = 0x12,
    CMD_TRAIN_START      = 0x13,
    CMD_PING             = 0x14,

    /* ---------------- CHECK: STM32 -> ESP32-S3 ---------- */
    CMD_CHECK_START      = 0x20,  /* ESP32 vao che do kiem tra */
    CMD_VISUAL_CHECK     = 0x21,  /* Yeu cau chay AI ngoai quan PCB */
    CMD_POWER_CHECK      = 0x22,  /* Gui Imax/Iavg PCB dang kiem tra */
    CMD_CHECK_NEXT       = 0x23,  /* Chuyen sang PCB tiep theo */

    /* ---------------- ESP32-S3 -> STM32 ----------------- */
    CMD_CAPTURE_ACK      = 0x91,
    CMD_POWER_ACK        = 0x92,
    CMD_TRAIN_DONE       = 0x93,
    CMD_ERROR             = 0x94,
    CMD_ACK               = 0xFF,

    /* ---------------- CHECK: ESP32-S3 -> STM32 ---------- */
    CMD_VISUAL_RESULT    = 0xA1,  /* data[0]: 0=PASS, 1=FAIL */
    CMD_POWER_RESULT     = 0xA2   /* data[0]: 0=PASS, 1=FAIL */
} cmd_id_t;

typedef enum {
    ERR_CAMERA_FAIL       = 0x01,
    ERR_FEATURE_FAIL      = 0x02,
    ERR_MEMORY_FULL       = 0x03,
    ERR_TRAIN_FAIL        = 0x04,
    ERR_INA219_FAIL       = 0x05,
    ERR_UNKNOWN           = 0xFF
} err_code_t;

typedef struct {
    uint8_t cmd;
    uint8_t len;
    uint8_t data[FRAME_MAX_DATA];
} uart_frame_t;

/* Bo parser UART khong blocking: dung de cho doi ACK/RESULT ma khong khoa main loop. */
typedef struct {
    uint8_t state;
    uint8_t len;
    uint8_t cmd;
    uint8_t index;
    uint8_t crc_recv;
    uint8_t data[FRAME_MAX_DATA];
} protocol_rx_context_t;

uint8_t  Protocol_CRC8(const uint8_t *data, uint8_t len);
void     Protocol_SendFrame(USART_TypeDef *USARTx, uint8_t cmd,
                            const uint8_t *data, uint8_t len);
void     Protocol_RxReset(protocol_rx_context_t *ctx);
void     Protocol_RxFlush(USART_TypeDef *USARTx);
uint8_t  Protocol_RxPoll(USART_TypeDef *USARTx, protocol_rx_context_t *ctx,
                         uart_frame_t *out_frame);
void     Protocol_PackFloat(uint8_t *dst, float value);
float    Protocol_UnpackFloat(const uint8_t *src);

#endif
