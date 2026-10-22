#ifndef __CHECK_FLOW_H
#define __CHECK_FLOW_H

#include "stm32f10x.h"

/* Trang thai quy trinh kiem tra PCB */
typedef enum {
    CHECK_STATE_IDLE = 0,
    CHECK_STATE_STARTING,          /* Gui lenh cho ESP32-S3 vao che do CHECK */
    CHECK_STATE_STARTING_WAIT_ACK,  /* Cho ACK, mat ACK thi gui lai */
    CHECK_STATE_WAIT_PCB,          /* Bang chay, cho LDR phat hien PCB */
    CHECK_STATE_VISUAL_WAIT,       /* PCB dung, cho nhan PA4 de check ngoai quan */
    CHECK_STATE_VISUAL_WAIT_RESPONSE,/* Cho ESP32 tra VISUAL_RESULT, retry cung board */
    CHECK_STATE_POWER_WAIT,        /* Ngoai quan PASS, cho nhan PA5 de do dong */
    CHECK_STATE_POWER_WAIT_RESPONSE, /* Cho ESP32 tra POWER_RESULT, retry cung payload */
    CHECK_STATE_NEXT_WAIT_ACK,      /* Cho ACK CHECK_NEXT, retry cung board index */
    CHECK_STATE_WAIT_IR_CLEAR,     /* Da xu ly xong PCB, cho tia thong 300ms */
    CHECK_STATE_ERROR              /* Loi giao tiep / ESP32 */
} check_state_t;

#define CHECK_STATUS_PASS  0x00
#define CHECK_STATUS_FAIL  0x01

void          Check_Init(void);
void          Check_Enter(void);
void          Check_Update(void);
uint8_t       Check_IsActive(void);
check_state_t Check_GetState(void);

#endif
