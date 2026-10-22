#ifndef __INA219_H
#define __INA219_H

#include "stm32f10x.h"

/* Dia chi I2C mac dinh cua INA219 (0x40 << 1) */
#define INA219_ADDRESS        0x80

/* Cac thanh ghi noi bo cua INA219 */
#define INA219_REG_CONFIG     0x00
#define INA219_REG_SHUNTVOLT  0x01
#define INA219_REG_BUSVOLT    0x02
#define INA219_REG_POWER      0x03
#define INA219_REG_CURRENT    0x04
#define INA219_REG_CALIB      0x05

/* Cau truc luu tru du lieu do */
typedef struct {
    float   I_Max;              /* Dong dien lon nhat (mA) */
    float   I_Average;          /* Dong dien trung binh (mA) */
    uint8_t overcurrent_flag;   /* 1: ngat do qua dong */
    uint8_t i2c_error_flag;     /* 1: I2C/INA219 loi -> khong treo */
} INA219_Stats;

/* Ham chuc nang */
uint8_t       INA219_Init(void);
float         INA219_GetCurrent_mA(void);
uint8_t       INA219_ReadCurrent_mA(float *current_mA);

INA219_Stats  INA219_Get_Measurement(uint16_t sample_count, float limit_mA,
                                     GPIO_TypeDef *relay_port, uint16_t relay_pin);

float         INA219_GetBusVoltage_V(void);
void          delay_ms(int time);

#endif
