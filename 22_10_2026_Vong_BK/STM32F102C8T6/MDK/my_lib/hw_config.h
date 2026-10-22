#ifndef __HW_CONFIG_H
#define __HW_CONFIG_H

#include "stm32f10x.h"

/* ================================================================
 *  NUT + CAM BIEN + LED + BANG CHUYEN THEO SO DO MOI
 * ================================================================ */

/* NUT TRONG CHE DO LEARN */
#define BTN_LEARN_PORT        GPIOB
#define BTN_LEARN_PIN         GPIO_Pin_0     /* PB0 - vao che do Learn */

#define BTN_CAPTURE_PORT      GPIOB
#define BTN_CAPTURE_PIN       GPIO_Pin_1     /* PB1 - chup mau chuan */

#define BTN_MEASURE_PORT      GPIOA
#define BTN_MEASURE_PIN       GPIO_Pin_1     /* PA1 - do dong mau chuan */

#define BTN_TRAIN_PORT        GPIOA
#define BTN_TRAIN_PIN         GPIO_Pin_2     /* PA2 - Train */

/* LDR module DO tren PA0: co tia laser = 0, PCB che tia = 1 (3.3V). */
#define LDR_DETECT_PORT       GPIOA
#define LDR_DETECT_PIN        GPIO_Pin_0
#define LDR_BLOCKED_LEVEL     1U
#define LDR_BLOCK_CONFIRM_MS  2U          /* Xac nhan PCB che tia */
#define LDR_CLEAR_CONFIRM_MS  300U        /* PCB cu da roi, tia thong on dinh */

#define BTN_CHECK_PORT        GPIOA
#define BTN_CHECK_PIN         GPIO_Pin_3     /* PA3 - bat dau che do Check */

#define BTN_VISUAL_PORT       GPIOA
#define BTN_VISUAL_PIN        GPIO_Pin_4     /* PA4 - yeu cau ESP32 check ngoai quan */

#define BTN_POWER_PORT        GPIOA
#define BTN_POWER_PIN         GPIO_Pin_5     /* PA5 - do dong PCB dang check */

/* LED trang thai */
#define LED_RED_PORT          GPIOA
#define LED_RED_PIN           GPIO_Pin_6
#define LED_YELLOW_PORT       GPIOA
#define LED_YELLOW_PIN        GPIO_Pin_8
#define LED_GREEN_PORT        GPIOA
#define LED_GREEN_PIN         GPIO_Pin_7

/* LED active HIGH: PAx = 1 -> LED sang */
#define LED_ALL_OFF()         do { GPIO_ResetBits(GPIOA, LED_RED_PIN | LED_YELLOW_PIN | LED_GREEN_PIN); } while (0)
#define LED_MODE_POWER()      do { LED_ALL_OFF(); GPIO_SetBits(GPIOA, LED_RED_PIN); } while (0)
#define LED_MODE_LEARN()      do { LED_ALL_OFF(); GPIO_SetBits(GPIOA, LED_YELLOW_PIN); } while (0)
#define LED_MODE_CHECK()      do { LED_ALL_OFF(); GPIO_SetBits(GPIOA, LED_GREEN_PIN); } while (0)

/* L298N: dieu khien bang chuyen 12V */
#define MOTOR_PORT            GPIOA
#define MOTOR_IN1_PIN         GPIO_Pin_11    /* IN1 */
#define MOTOR_IN2_PIN         GPIO_Pin_12    /* IN2 */
#define MOTOR_ENA_PIN         GPIO_Pin_15    /* TIM2_CH1 full remap, can go jumper ENA */

/* PWM TIM2_CH1/PA15. Dieu chinh toc do tai day (0..100%). */
#define CONVEYOR_TARGET_DUTY       90U     /* Cham hon 75% truoc day */
#define CONVEYOR_RAMP_STEP         1U      /* Moi buoc tang 2% */
#define CONVEYOR_RAMP_INTERVAL_MS  20U     /* Moi 20 ms, den 60% sau ~600 ms */
#define CONVEYOR_PWM_PERIOD        3599U   /* 72 MHz / 3600 = 20 kHz */

/* Relay cap nguon PCB khi do dong */
/* Relay cap nguon PCB - ACTIVE LOW
 * PC14 = 0 -> Relay ON  -> cap nguon PCB
 * PC14 = 1 -> Relay OFF -> ngat nguon PCB
 */
#define RELAY_PORT            GPIOC
#define RELAY_PIN             GPIO_Pin_14

#define RELAY_ON()   GPIO_ResetBits(RELAY_PORT, RELAY_PIN)
#define RELAY_OFF()  GPIO_SetBits(RELAY_PORT, RELAY_PIN)

/* INA219: PB6 = SCL, PB7 = SDA */

/* TFT ST7735: driver thuc te dung PB10/PB11/PB12/PB13/PB15 */

/* Nguong bao ve qua dong */
#define SAFE_CURRENT_LIMIT    1000.0f       /* mA */

#endif
