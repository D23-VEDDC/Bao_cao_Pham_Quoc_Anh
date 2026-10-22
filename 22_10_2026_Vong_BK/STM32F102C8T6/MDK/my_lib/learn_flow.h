#ifndef __LEARN_FLOW_H
#define __LEARN_FLOW_H

#include "stm32f10x.h"

#define LEARN_SAMPLE_COUNT    5  /* So mau chuan can hoc: 5 anh + 5 lan do dong */

typedef enum {
    LEARN_STATE_IDLE = 0,           /* Khong o trong che do Learn */
    LEARN_STATE_INTRO,               /* Vua nhan nut LEARN, hien thi trang thai 1 luc */
    LEARN_STATE_SEND_LEARN_START,    /* Gui CMD_LEARN_START lan dau */
    LEARN_STATE_WAIT_LEARN_START_ACK,/* Cho ACK + tu dong gui lai neu mat ACK */
    LEARN_STATE_WAIT_VISION_CAPTURE, /* Cho nguoi dung nhan CAPTURE (lap 5 lan) */
    LEARN_STATE_WAIT_VISION_RESPONSE, /* ESP32 dang xu ly anh - khong timeout, retry cung index */
    LEARN_STATE_WAIT_POWER_MEASURE,  /* Cho nguoi dung nhan MEASURE (lap 5 lan) */
    LEARN_STATE_WAIT_POWER_RESPONSE,  /* Da do xong, cho ESP32 ACK ma khong khoa main loop */
    LEARN_STATE_POWER_ERROR_RETRY,    /* I2C loi: giu nguyen mau dong hien tai, cho do lai */
    LEARN_STATE_READY_TRAIN,         /* Da du 5+5 mau, cho nhan TRAIN */
    LEARN_STATE_TRAINING,            /* Da gui CMD_TRAIN_START, cho ESP32 hoc xong */
    LEARN_STATE_DONE,                /* Hoc xong, hien thi ket qua roi tu thoat */
    LEARN_STATE_ERROR                /* Loi o buoc bat ky, cho nhan LEARN de thu lai */
} learn_state_t;

/* Goi 1 lan luc khoi dong he thong (cau hinh 4 nut bam Learn/Capture/Measure/Train) */
void            Learn_Init(void);

/* Goi khi nguoi dung nhan nut LEARN tu man hinh Standby de bat dau quy trinh */
void            Learn_Enter(void);

/* Goi lien tuc trong vong lap while(1) chinh cua main() - la trai tim cua FSM */
void            Learn_Update(void);

/* Tra ve 1 neu dang trong quy trinh Learn (de main() biet khong chay logic khac) */
uint8_t         Learn_IsActive(void);

/* Tra ve trang thai hien tai, phuc vu debug/log neu can */
learn_state_t   Learn_GetState(void);

#endif
