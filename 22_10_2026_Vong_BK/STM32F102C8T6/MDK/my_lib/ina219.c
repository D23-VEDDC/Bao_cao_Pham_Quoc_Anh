#include "ina219.h"
#include "FreeRTOS.h"
#include "task.h"

/*
 * INA219 driver an toan cho he thong VEDC.
 *
 * Loi truoc day:
 *   - cac while(I2C_GetFlagStatus/I2C_CheckEvent) co the cho vo han
 *     neu INA219 mat nguon, mat day, SDA/SCL bi ket hoac I2C1 con dang BUSY.
 *   - sau khi relay cat nguon PCB, INA219 co the reset mat cau hinh.
 *
 * Ban nay:
 *   1) moi buoc I2C deu co guard timeout -> khong bao gio while vo han.
 *   2) khi I2C loi -> STOP + phuc hoi bus + khoi tao lai I2C.
 *   3) moi lan do dong, cau hinh INA219 lai sau khi relay cap nguon PCB.
 *   4) neu mot lan doc bi loi, thu khoi phuc + doc lai 1 lan.
 *   5) neu van loi -> tra i2c_error_flag, caller se tat relay va bao loi.
 */

#define I2C_GUARD_LOOPS      120000UL
#define I2C_RECOVER_LOOPS      2000UL
#define INA219_CONFIG_VALUE    0x399F
#define INA219_CALIB_VALUE     4096

static void short_delay(void) {
    volatile uint32_t i;
    for (i = 0; i < I2C_RECOVER_LOOPS; ++i) {
        __NOP();
    }
}

/* CHANGELOG (port sang FreeRTOS): KHONG con tu lap trinh thanh ghi SysTick
 * truc tiep nua - FreeRTOS kernel da doc quyen su dung SysTick lam tick
 * ngat he thong (xPortSysTickHandler), neu code ung dung van tu y ghi
 * SysTick->CTRL/LOAD/VAL se pha vo nhip cua scheduler ngay lap tuc.
 * Thay vao do dung vTaskDelay(), vua an toan vua NHUONG CPU cho cac task
 * khac (hoac Idle task) trong luc cho, thay vi block cung CPU nhu truoc.
 *
 * LUU Y: vTaskDelay() chi hop le SAU KHI vTaskStartScheduler() da chay.
 * Moi ham khoi tao phan cung co goi delay_ms() (vd ST7735_Init) phai duoc
 * goi TU BEN TRONG 1 task FreeRTOS, KHONG duoc goi truoc vTaskStartScheduler()
 * trong main() - xem USER/main.c, toan bo khoi tao da duoc chuyen vao
 * vAppTask() vi ly do nay. */
void delay_ms(int time) {
    if (time <= 0) {
        return;
    }
    vTaskDelay(pdMS_TO_TICKS((uint32_t)time));
}

static uint8_t wait_event(uint32_t event) {
    uint32_t guard = I2C_GUARD_LOOPS;
    while (guard--) {
        if (I2C_GetFlagStatus(I2C1, I2C_FLAG_AF) != RESET ||
            I2C_GetFlagStatus(I2C1, I2C_FLAG_BERR) != RESET ||
            I2C_GetFlagStatus(I2C1, I2C_FLAG_ARLO) != RESET ||
            I2C_GetFlagStatus(I2C1, I2C_FLAG_OVR) != RESET) {
            return 0;
        }
        if (I2C_CheckEvent(I2C1, event)) {
            return 1;
        }
    }
    return 0;
}

static uint8_t wait_bus_free(void) {
    uint32_t guard = I2C_GUARD_LOOPS;
    while (guard--) {
        if (I2C_GetFlagStatus(I2C1, I2C_FLAG_BUSY) == RESET) {
            return 1;
        }
    }
    return 0;
}

static uint8_t wait_flag(uint32_t flag) {
    uint32_t guard = I2C_GUARD_LOOPS;
    while (guard--) {
        if (I2C_GetFlagStatus(I2C1, flag) != RESET) {
            return 1;
        }
    }
    return 0;
}

static void INA219_I2C_PeripheralReset(void) {
    I2C_Cmd(I2C1, DISABLE);
    I2C_SoftwareResetCmd(I2C1, ENABLE);
    short_delay();
    I2C_SoftwareResetCmd(I2C1, DISABLE);
    I2C_DeInit(I2C1);
}

/* Phuc hoi bus bang 9 xung SCL va STOP thu cong. */
static void INA219_I2C_BusRecover(void) {
    GPIO_InitTypeDef gpio;
    uint8_t i;

    I2C_Cmd(I2C1, DISABLE);

    RCC_APB2PeriphClockCmd(RCC_APB2Periph_GPIOB, ENABLE);

    gpio.GPIO_Pin = GPIO_Pin_6 | GPIO_Pin_7;
    gpio.GPIO_Mode = GPIO_Mode_Out_OD;
    gpio.GPIO_Speed = GPIO_Speed_50MHz;
    GPIO_Init(GPIOB, &gpio);

    GPIO_SetBits(GPIOB, GPIO_Pin_6 | GPIO_Pin_7);
    short_delay();

    /* Neu slave dang giu SDA thap, xung SCL cho den khi nha SDA. */
    for (i = 0; i < 9; ++i) {
        GPIO_ResetBits(GPIOB, GPIO_Pin_6);
        short_delay();
        GPIO_SetBits(GPIOB, GPIO_Pin_6);
        short_delay();
        if (GPIO_ReadInputDataBit(GPIOB, GPIO_Pin_7) != 0) {
            break;
        }
    }

    /* Tao STOP: SDA low -> SCL high -> SDA high */
    GPIO_ResetBits(GPIOB, GPIO_Pin_7);
    short_delay();
    GPIO_SetBits(GPIOB, GPIO_Pin_6);
    short_delay();
    GPIO_SetBits(GPIOB, GPIO_Pin_7);
    short_delay();
}

void I2C1_init(void) {
    I2C_InitTypeDef I2C_InitStructure;
    GPIO_InitTypeDef GPIO_InitStructure;

    RCC_APB1PeriphClockCmd(RCC_APB1Periph_I2C1, ENABLE);
    RCC_APB2PeriphClockCmd(RCC_APB2Periph_GPIOB, ENABLE);

    I2C_DeInit(I2C1);
    I2C_SoftwareResetCmd(I2C1, ENABLE);
    I2C_SoftwareResetCmd(I2C1, DISABLE);
    I2C_ClearFlag(I2C1, I2C_FLAG_AF | I2C_FLAG_BERR | I2C_FLAG_ARLO | I2C_FLAG_OVR);

    GPIO_InitStructure.GPIO_Pin = GPIO_Pin_6 | GPIO_Pin_7;
    GPIO_InitStructure.GPIO_Mode = GPIO_Mode_AF_OD;
    GPIO_InitStructure.GPIO_Speed = GPIO_Speed_50MHz;
    GPIO_Init(GPIOB, &GPIO_InitStructure);

    I2C_InitStructure.I2C_Mode = I2C_Mode_I2C;
    I2C_InitStructure.I2C_DutyCycle = I2C_DutyCycle_2;
    I2C_InitStructure.I2C_OwnAddress1 = 0x00;
    I2C_InitStructure.I2C_Ack = I2C_Ack_Enable;
    I2C_InitStructure.I2C_AcknowledgedAddress = I2C_AcknowledgedAddress_7bit;
    I2C_InitStructure.I2C_ClockSpeed = 100000;
    I2C_Init(I2C1, &I2C_InitStructure);
    I2C_AcknowledgeConfig(I2C1, ENABLE);
    I2C_NACKPositionConfig(I2C1, I2C_NACKPosition_Current);
    I2C_Cmd(I2C1, ENABLE);
}

static uint8_t INA219_I2C_WriteSafe(uint8_t reg, uint16_t value) {
    if (!wait_bus_free()) {
        return 0;
    }

    I2C_GenerateSTART(I2C1, ENABLE);
    if (!wait_event(I2C_EVENT_MASTER_MODE_SELECT)) goto fail;

    I2C_Send7bitAddress(I2C1, INA219_ADDRESS, I2C_Direction_Transmitter);
    if (!wait_event(I2C_EVENT_MASTER_TRANSMITTER_MODE_SELECTED)) goto fail;

    I2C_SendData(I2C1, reg);
    if (!wait_event(I2C_EVENT_MASTER_BYTE_TRANSMITTED)) goto fail;

    I2C_SendData(I2C1, (uint8_t)(value >> 8));
    if (!wait_event(I2C_EVENT_MASTER_BYTE_TRANSMITTED)) goto fail;

    I2C_SendData(I2C1, (uint8_t)(value & 0xFF));
    if (!wait_event(I2C_EVENT_MASTER_BYTE_TRANSMITTED)) goto fail;

    I2C_GenerateSTOP(I2C1, ENABLE);
    return 1;

fail:
    I2C_GenerateSTOP(I2C1, ENABLE);
    I2C_ClearFlag(I2C1, I2C_FLAG_AF | I2C_FLAG_BERR | I2C_FLAG_ARLO | I2C_FLAG_OVR);
    return 0;
}

static uint8_t INA219_I2C_ReadSafe(uint8_t reg, uint16_t *value) {
    uint16_t high;
    uint16_t low;

    if (!wait_bus_free()) {
        return 0;
    }

    I2C_AcknowledgeConfig(I2C1, ENABLE);
    I2C_NACKPositionConfig(I2C1, I2C_NACKPosition_Current);

    I2C_GenerateSTART(I2C1, ENABLE);
    if (!wait_event(I2C_EVENT_MASTER_MODE_SELECT)) goto fail;

    I2C_Send7bitAddress(I2C1, INA219_ADDRESS, I2C_Direction_Transmitter);
    if (!wait_event(I2C_EVENT_MASTER_TRANSMITTER_MODE_SELECTED)) goto fail;

    I2C_SendData(I2C1, reg);
    if (!wait_event(I2C_EVENT_MASTER_BYTE_TRANSMITTED)) goto fail;

    /* Repeated START + receiver, doc 2 byte */
    I2C_GenerateSTART(I2C1, ENABLE);
    if (!wait_event(I2C_EVENT_MASTER_MODE_SELECT)) goto fail;

    /* Dat POS=1 va tat ACK truoc khi nhan 2 byte */
    I2C_NACKPositionConfig(I2C1, I2C_NACKPosition_Next);
    I2C_AcknowledgeConfig(I2C1, DISABLE);
    I2C_Send7bitAddress(I2C1, INA219_ADDRESS, I2C_Direction_Receiver);
    if (!wait_event(I2C_EVENT_MASTER_RECEIVER_MODE_SELECTED)) goto fail;

    /* Cho BTF de co du 2 byte */
    if (!wait_flag(I2C_FLAG_BTF)) goto fail;

    I2C_GenerateSTOP(I2C1, ENABLE);
    high = I2C_ReceiveData(I2C1);
    low  = I2C_ReceiveData(I2C1);

    I2C_AcknowledgeConfig(I2C1, ENABLE);
    I2C_NACKPositionConfig(I2C1, I2C_NACKPosition_Current);

    *value = (uint16_t)((high << 8) | low);
    return 1;

fail:
    I2C_GenerateSTOP(I2C1, ENABLE);
    I2C_AcknowledgeConfig(I2C1, ENABLE);
    I2C_NACKPositionConfig(I2C1, I2C_NACKPosition_Current);
    I2C_ClearFlag(I2C1, I2C_FLAG_AF | I2C_FLAG_BERR | I2C_FLAG_ARLO | I2C_FLAG_OVR);
    return 0;
}

/* Ham public cu, giu ten de khong anh huong code khac. */
void INA219_I2C_Write(uint8_t reg, uint16_t value) {
    (void)INA219_I2C_WriteSafe(reg, value);
}

uint16_t INA219_I2C_Read(uint8_t reg) {
    uint16_t value = 0;
    (void)INA219_I2C_ReadSafe(reg, &value);
    return value;
}

uint8_t INA219_Init(void) {
    uint8_t attempt;
    uint8_t ok1, ok2;

    for (attempt = 0; attempt < 3; ++attempt) {
        I2C1_init();

        ok1 = INA219_I2C_WriteSafe(INA219_REG_CONFIG, INA219_CONFIG_VALUE);
        ok2 = INA219_I2C_WriteSafe(INA219_REG_CALIB, INA219_CALIB_VALUE);
        if (ok1 && ok2) {
            return 1;
        }

        /* Reset peripheral + recover bus truoc khi thu lai. */
        INA219_I2C_PeripheralReset();
        INA219_I2C_BusRecover();
        short_delay();
    }

    I2C1_init();
    return 0;
}

uint8_t INA219_ReadCurrent_mA(float *current_mA) {
    uint16_t raw;

    if (!INA219_I2C_ReadSafe(INA219_REG_CURRENT, &raw)) {
        return 0;
    }

    *current_mA = (float)((int16_t)raw);
    return 1;
}

float INA219_GetCurrent_mA(void) {
    float current;
    if (!INA219_ReadCurrent_mA(&current)) {
        return -99999.0f; /* sentinel loi I2C */
    }
    return current;
}

INA219_Stats INA219_Get_Measurement(uint16_t sample_count, float limit_mA,
                                    GPIO_TypeDef *relay_port, uint16_t relay_pin) {
    float current_now = 0;
    float sum = 0;
    uint16_t i = 0;
    uint8_t retry = 0;

    INA219_Stats stats;
    stats.I_Max = 0;
    stats.I_Average = 0;
    stats.overcurrent_flag = 0;
    stats.i2c_error_flag = 0;

    if (sample_count == 0) {
        stats.i2c_error_flag = 1;
        GPIO_SetBits(relay_port, relay_pin);
        return stats;
    }

    /* Sau khi relay cap nguon PCB, INA219 co the vua khoi dong lai.
       Luon init lai sensor truoc moi lan do. */
    if (!INA219_Init()) {
        stats.i2c_error_flag = 1;
        GPIO_SetBits(relay_port, relay_pin);
        return stats;
    }

    for (i = 0; i < sample_count; ++i) {
        retry = 0;
        while (retry < 3) {
            if (INA219_ReadCurrent_mA(&current_now)) {
                break;
            }

            /* Loi bus: reset peripheral + recover SCL/SDA + init lai sensor. */
            retry++;
            INA219_I2C_PeripheralReset();
            INA219_I2C_BusRecover();
            short_delay();
            if (!INA219_Init()) {
                /* INA219 chua san sang, vong retry se tiep tuc. */
            }
        }

        if (retry >= 3) {
            stats.i2c_error_flag = 1;
            GPIO_SetBits(relay_port, relay_pin); /* active-low -> OFF */
            return stats;
        }

        if (current_now > limit_mA) {
            GPIO_SetBits(relay_port, relay_pin);
            stats.overcurrent_flag = 1;
            stats.I_Max = current_now;
            return stats;
        }

        if (current_now > stats.I_Max) {
            stats.I_Max = current_now;
        }
        sum += current_now;

        /* 2 ms giua cac lan doc, tranh doc qua sat khi relay/INA219 vua on. */
        delay_ms(2);
    }

    stats.I_Average = sum / (float)sample_count;
    GPIO_SetBits(relay_port, relay_pin);
    return stats;
}


float INA219_GetBusVoltage_V(void) {
    uint16_t raw_volt;
    if (!INA219_I2C_ReadSafe(INA219_REG_BUSVOLT, &raw_volt)) {
        return -1.0f;
    }
    /* Bus Voltage nam o bit 3 den bit 15. LSB = 4mV */
    return (float)((raw_volt >> 3) * 4) / 1000.0f;
}
