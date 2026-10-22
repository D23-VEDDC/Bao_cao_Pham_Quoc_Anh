#include "st7735.h"
#include "ina219.h"

// Các chân di?u khi?n trên Port B
#define TFT_PORT    GPIOB
#define TFT_DC      GPIO_Pin_10
#define TFT_RST     GPIO_Pin_11
#define TFT_CS      GPIO_Pin_12

const uint8_t Font5x7[] = {
    0x00, 0x00, 0x00, 0x00, 0x00, // (space) - 32
    0x00, 0x00, 0x5f, 0x00, 0x00, // !
    0x00, 0x07, 0x00, 0x07, 0x00, // "
    0x14, 0x7f, 0x14, 0x7f, 0x14, // #
    0x24, 0x2a, 0x7f, 0x2a, 0x12, // $
    0x23, 0x13, 0x08, 0x64, 0x62, // %
    0x36, 0x49, 0x55, 0x22, 0x50, // &
    0x00, 0x05, 0x03, 0x00, 0x00, // '
    0x00, 0x1c, 0x22, 0x41, 0x00, // (
    0x00, 0x41, 0x22, 0x1c, 0x00, // )
    0x14, 0x08, 0x3e, 0x08, 0x14, // *
    0x08, 0x08, 0x3e, 0x08, 0x08, // +
    0x00, 0x50, 0x30, 0x00, 0x00, // ,
    0x08, 0x08, 0x08, 0x08, 0x08, // -
    0x00, 0x60, 0x60, 0x00, 0x00, // .
    0x20, 0x10, 0x08, 0x04, 0x02, // /
    0x3e, 0x51, 0x49, 0x45, 0x3e, // 0 - 48
    0x00, 0x42, 0x7f, 0x40, 0x00, // 1
    0x42, 0x61, 0x51, 0x49, 0x46, // 2
    0x21, 0x41, 0x45, 0x4b, 0x31, // 3
    0x18, 0x14, 0x12, 0x7f, 0x10, // 4
    0x27, 0x45, 0x45, 0x45, 0x39, // 5
    0x3c, 0x4a, 0x49, 0x49, 0x30, // 6
    0x01, 0x71, 0x09, 0x05, 0x03, // 7
    0x36, 0x49, 0x49, 0x49, 0x36, // 8
    0x06, 0x49, 0x49, 0x29, 0x1e, // 9
    0x00, 0x36, 0x36, 0x00, 0x00, // :
    0x00, 0x56, 0x36, 0x00, 0x00, // ;
    0x08, 0x14, 0x22, 0x41, 0x00, // <
    0x14, 0x14, 0x14, 0x14, 0x14, // =
    0x00, 0x41, 0x22, 0x14, 0x08, // >
    0x02, 0x01, 0x51, 0x09, 0x06, // ?
    0x32, 0x49, 0x79, 0x41, 0x3e, // @ - 64
    0x7e, 0x11, 0x11, 0x11, 0x7e, // A - 65
    0x7f, 0x49, 0x49, 0x49, 0x36, // B
    0x3e, 0x41, 0x41, 0x41, 0x22, // C
    0x7f, 0x41, 0x41, 0x22, 0x1c, // D
    0x7f, 0x49, 0x49, 0x49, 0x41, // E
    0x7f, 0x09, 0x09, 0x09, 0x01, // F
    0x3e, 0x41, 0x49, 0x49, 0x7a, // G
    0x7f, 0x08, 0x08, 0x08, 0x7f, // H
    0x00, 0x41, 0x7f, 0x41, 0x00, // I
    0x20, 0x40, 0x41, 0x3f, 0x01, // J
    0x7f, 0x08, 0x14, 0x22, 0x41, // K
    0x7f, 0x40, 0x40, 0x40, 0x40, // L
    0x7f, 0x02, 0x0c, 0x02, 0x7f, // M
    0x7f, 0x04, 0x08, 0x10, 0x7f, // N
    0x3e, 0x41, 0x41, 0x41, 0x3e, // O
    0x7f, 0x09, 0x09, 0x09, 0x06, // P
    0x3e, 0x41, 0x51, 0x21, 0x5e, // Q
    0x7f, 0x09, 0x19, 0x29, 0x46, // R
    0x46, 0x49, 0x49, 0x49, 0x31, // S
    0x01, 0x01, 0x7f, 0x01, 0x01, // T
    0x3f, 0x40, 0x40, 0x40, 0x3f, // U
    0x1f, 0x20, 0x40, 0x20, 0x1f, // V
    0x3f, 0x40, 0x38, 0x40, 0x3f, // W
    0x63, 0x14, 0x08, 0x14, 0x63, // X
    0x07, 0x08, 0x70, 0x08, 0x07, // Y
    0x61, 0x51, 0x49, 0x45, 0x43, // Z - 90
    0x00, 0x7f, 0x41, 0x41, 0x00, // [ - 91
    0x02, 0x04, 0x08, 0x10, 0x20, // ASCII 92
    0x00, 0x41, 0x41, 0x7f, 0x00, // ]
    0x04, 0x02, 0x01, 0x02, 0x04, // ^
    0x40, 0x40, 0x40, 0x40, 0x40, // _
    0x00, 0x01, 0x02, 0x04, 0x00, // `
    0x20, 0x54, 0x54, 0x54, 0x78, // a - 97
    0x7f, 0x48, 0x44, 0x44, 0x38, // b
    0x38, 0x44, 0x44, 0x44, 0x20, // c
    0x38, 0x44, 0x44, 0x48, 0x7f, // d
    0x38, 0x54, 0x54, 0x54, 0x18, // e
    0x08, 0x7e, 0x09, 0x01, 0x02, // f
    0x0c, 0x52, 0x52, 0x52, 0x3e, // g
    0x7f, 0x08, 0x04, 0x04, 0x78, // h
    0x00, 0x44, 0x7d, 0x40, 0x00, // i
    0x20, 0x40, 0x44, 0x3d, 0x00, // j
    0x7f, 0x10, 0x28, 0x44, 0x00, // k
    0x00, 0x41, 0x7f, 0x40, 0x00, // l
    0x7c, 0x04, 0x18, 0x04, 0x78, // m
    0x7c, 0x08, 0x04, 0x04, 0x78, // n
    0x38, 0x44, 0x44, 0x44, 0x38, // o
    0x7c, 0x14, 0x14, 0x14, 0x08, // p
    0x08, 0x14, 0x14, 0x18, 0x7c, // q
    0x7c, 0x08, 0x04, 0x04, 0x08, // r
    0x48, 0x54, 0x54, 0x54, 0x20, // s
    0x04, 0x3f, 0x44, 0x40, 0x20, // t
    0x3c, 0x40, 0x40, 0x20, 0x7c, // u
    0x1c, 0x20, 0x40, 0x20, 0x1c, // v
    0x3c, 0x40, 0x38, 0x40, 0x3c, // w
    0x44, 0x28, 0x10, 0x28, 0x44, // x
    0x0c, 0x50, 0x50, 0x50, 0x3c, // y
    0x44, 0x64, 0x54, 0x4c, 0x44  // z - 122
};

// G?i 1 byte qua SPI2
/* CHANGELOG (fix treo TFT khi do dong dien):
 *
 * 1) SPI_Send() truoc day co 2 vong lap while() KHONG TIMEOUT cho co
 *    TXE/BSY cua SPI2. Them timeout + tu phuc hoi (soft-reset SPI2) phong
 *    truong hop ban than SPI2 cua STM32 bi ket co (hiem nhung neu xay ra
 *    se treo CA HE THONG vi du an nay chi co 1 task FreeRTOS).
 *
 * 2) NGUYEN NHAN CHINH gay "treo TFT ma STM32 van chay binh thuong" la o
 *    cho chan CS (PB12) TRUOC DAY chi duoc keo xuong 1 LAN DUY NHAT luc
 *    ST7735_Init() roi khong bao gio doi nua. CS la tin hieu de chip
 *    ST7735 TU RESYNC bo dem lenh/du lieu noi bo moi khi CS chuyen tu cao
 *    xuong thap. Giu CS co dinh suot phien nghia la chi can 1 bit tren
 *    SCK/MOSI bi nhieu dien (vd luc relay dong/cat trong buoc do dong) la
 *    chip ST7735 lech dong bo VINH VIEN - trong khi co TXE/BSY cua STM32
 *    van bao "da gui xong" binh thuong (do la trang thai cua STM32, khong
 *    phai cua chip man hinh), nen code STM32 van chay tiep binh thuong
 *    (UART, relay, dong co...) nhung man hinh thi treo/meo vinh vien vi
 *    chip ST7735 tu do hieu sai moi lenh gui toi.
 *
 *    FIX: xung CS (cao -> thap) TRUOC MOI lenh (WriteCmd) de chip ST7735
 *    co co hoi resync lai tai moi diem bat dau lenh moi. Neu nhieu xay ra
 *    giua 1 chuoi du lieu dai (vd dang FillScreen), chi lan ve hien tai bi
 *    anh huong thoang qua - lenh tiep theo se duoc resync lai ngay, KHONG
 *    con treo vinh vien nhu truoc.
 */
#define SPI_WAIT_TIMEOUT  60000UL

static void SPI2_SoftReset(void) {
    RCC_APB1PeriphResetCmd(RCC_APB1Periph_SPI2, ENABLE);
    RCC_APB1PeriphResetCmd(RCC_APB1Periph_SPI2, DISABLE);

    SPI_InitTypeDef SPI_InitStructure;
    SPI_InitStructure.SPI_Direction = SPI_Direction_2Lines_FullDuplex;
    SPI_InitStructure.SPI_Mode = SPI_Mode_Master;
    SPI_InitStructure.SPI_DataSize = SPI_DataSize_8b;
    SPI_InitStructure.SPI_CPOL = SPI_CPOL_Low;
    SPI_InitStructure.SPI_CPHA = SPI_CPHA_1Edge;
    SPI_InitStructure.SPI_NSS = SPI_NSS_Soft;
    SPI_InitStructure.SPI_BaudRatePrescaler = SPI_BaudRatePrescaler_2;
    SPI_InitStructure.SPI_FirstBit = SPI_FirstBit_MSB;
    SPI_InitStructure.SPI_CRCPolynomial = 7;
    SPI_Init(SPI2, &SPI_InitStructure);
    SPI_Cmd(SPI2, ENABLE);
}

void SPI_Send(uint8_t data) {
    uint32_t guard = SPI_WAIT_TIMEOUT;
    while (SPI_I2S_GetFlagStatus(SPI2, SPI_I2S_FLAG_TXE) == RESET) {
        if (--guard == 0) { SPI2_SoftReset(); return; }
    }
    SPI_I2S_SendData(SPI2, data);

    guard = SPI_WAIT_TIMEOUT;
    while (SPI_I2S_GetFlagStatus(SPI2, SPI_I2S_FLAG_BSY) == SET) {
        if (--guard == 0) { SPI2_SoftReset(); return; }
    }
}

void WriteCmd(uint8_t cmd) {
    /* Xung CS: dong giao dich truoc (neu co) roi mo giao dich moi.
       Day la diem RESYNC quan trong nhat, dat truoc MOI lenh. */
    GPIO_SetBits(TFT_PORT, TFT_CS);
    GPIO_ResetBits(TFT_PORT, TFT_DC); // DC = 0 (Command)
    GPIO_ResetBits(TFT_PORT, TFT_CS); // CS = 0, bat dau giao dich moi
    SPI_Send(cmd);
}

void WriteData(uint8_t data) {
    GPIO_SetBits(TFT_PORT, TFT_DC); // DC = 1 (Data) - van trong giao dich hien tai (CS dang thap)
    SPI_Send(data);
}

// Kh?i t?o SPI2 và chân GPIO trên Port B cho TFT
void ST7735_Init(void) {
    SPI_InitTypeDef SPI_InitStructure;
    GPIO_InitTypeDef GPIO_InitStructure;
    
    // B?t Clock cho GPIOB và SPI2
    // Luu ý: SPI2 n?m trên bus APB1
    RCC_APB2PeriphClockCmd(RCC_APB2Periph_GPIOB | RCC_APB2Periph_AFIO, ENABLE);
    RCC_APB1PeriphClockCmd(RCC_APB1Periph_SPI2, ENABLE);

    // C?u hình chân SPI2: SCK (PB13), MOSI (PB15)
    GPIO_InitStructure.GPIO_Pin = GPIO_Pin_13 | GPIO_Pin_15;
    GPIO_InitStructure.GPIO_Mode = GPIO_Mode_AF_PP;
    GPIO_InitStructure.GPIO_Speed = GPIO_Speed_50MHz;
    GPIO_Init(GPIOB, &GPIO_InitStructure);

    // C?u hình chân di?u khi?n: CS (PB12), RST (PB11), DC/A0 (PB10)
    GPIO_InitStructure.GPIO_Pin = TFT_CS | TFT_RST | TFT_DC;
    GPIO_InitStructure.GPIO_Mode = GPIO_Mode_Out_PP;
    GPIO_Init(GPIOB, &GPIO_InitStructure);

    // C?u hình SPI2
    SPI_InitStructure.SPI_Direction = SPI_Direction_2Lines_FullDuplex;
    SPI_InitStructure.SPI_Mode = SPI_Mode_Master;
    SPI_InitStructure.SPI_DataSize = SPI_DataSize_8b;
    SPI_InitStructure.SPI_CPOL = SPI_CPOL_Low;
    SPI_InitStructure.SPI_CPHA = SPI_CPHA_1Edge;
    SPI_InitStructure.SPI_NSS = SPI_NSS_Soft;
    
    // SPI2 ch?y trên APB1 (36MHz max). Prescaler 2 -> 18MHz
    SPI_InitStructure.SPI_BaudRatePrescaler = SPI_BaudRatePrescaler_2; 
    SPI_InitStructure.SPI_FirstBit = SPI_FirstBit_MSB;
    SPI_InitStructure.SPI_CRCPolynomial = 7;
    SPI_Init(SPI2, &SPI_InitStructure);
    SPI_Cmd(SPI2, ENABLE);

    // --- B?T Ð?U KH?I T?O MÀN HÌNH ---
    GPIO_SetBits(TFT_PORT, TFT_CS); // CS = 1 (Ngh?)
    
    // Reset c?ng màn hình
    GPIO_ResetBits(TFT_PORT, TFT_RST); delay_ms(50);
    GPIO_SetBits(TFT_PORT, TFT_RST);   delay_ms(50);

    GPIO_ResetBits(TFT_PORT, TFT_CS); // CS = 0 (B?t d?u giao ti?p)

    WriteCmd(0x01); // Software Reset
    delay_ms(150);
    
    WriteCmd(0x11); // Sleep Out
    delay_ms(255);

    // C?u hình các tham s? hi?n th? (gi? nguyên logic cu)
    WriteCmd(0xB1); WriteData(0x01); WriteData(0x2C); WriteData(0x2D);
    WriteCmd(0xB2); WriteData(0x01); WriteData(0x2C); WriteData(0x2D);
    WriteCmd(0xB3); WriteData(0x01); WriteData(0x2C); WriteData(0x2D); 
                    WriteData(0x01); WriteData(0x2C); WriteData(0x2D);

    WriteCmd(0x36); WriteData(0xC8); // Ð?nh hu?ng hi?n th?
    WriteCmd(0x3A); WriteData(0x05); // 16-bit color

    WriteCmd(0x2A); // C?a s? X
    WriteData(0x00); WriteData(0x00); WriteData(0x00); WriteData(0x7F);

    WriteCmd(0x2B); // C?a s? Y
    WriteData(0x00); WriteData(0x00); WriteData(0x00); WriteData(0x9F);

    WriteCmd(0x29); // B?t hi?n th?
    delay_ms(100);
}

void ST7735_SetWindow(uint8_t x0, uint8_t y0, uint8_t x1, uint8_t y1) {
    WriteCmd(0x2A); WriteData(0x00); WriteData(x0); WriteData(0x00); WriteData(x1);
    WriteCmd(0x2B); WriteData(0x00); WriteData(y0); WriteData(0x00); WriteData(y1);
    WriteCmd(0x2C);
}

void ST7735_FillScreen(uint16_t color) {
	 int i = 0;
    ST7735_SetWindow(0, 0, 127, 159);
    for ( i = 0; i < 128 * 160; i++) {
        WriteData(color >> 8);
        WriteData(color & 0xFF);
    }
}


/**
 * @brief  V? m?t ký t? lên màn hình
 */
/**
 * @brief  V? m?t ký t? lên màn hình (Ðã fix l?i thu?t toán quét)
 */
void ST7735_FillRect(uint8_t x, uint8_t y, uint8_t width, uint8_t height, uint16_t color) {
    uint32_t pixels;
    if (x >= 128U || y >= 160U || width == 0U || height == 0U) return;
    if ((uint16_t)x + width > 128U) width = (uint8_t)(128U - x);
    if ((uint16_t)y + height > 160U) height = (uint8_t)(160U - y);
    pixels = (uint32_t)width * height;
    ST7735_SetWindow(x, y, (uint8_t)(x + width - 1U),
                     (uint8_t)(y + height - 1U));
    while (pixels-- != 0U) {
        WriteData((uint8_t)(color >> 8));
        WriteData((uint8_t)color);
    }
}

void ST7735_DrawChar(uint16_t x, uint16_t y, char ch, uint16_t color, uint16_t bg) {
    int i, j;
    if (ch < 32 || ch > 126) return;

    // Thi?t l?p vùng v? cho 1 ký t? (R?ng 6 x Cao 8)
    ST7735_SetWindow(x, y, x + 5, y + 7);

    // B?T BU?C: Duy?t Hàng (j) ? ngoài, C?t (i) ? trong
    for (j = 0; j < 8; j++) {
        for (i = 0; i < 6; i++) {
            // L?y d? li?u c?a c?t th? i
            uint8_t line = (i == 5) ? 0x00 : Font5x7[(ch - 32) * 5 + i];
            
            // Ki?m tra bit th? j c?a c?t dó
            if (line & (1 << j)) {
                WriteData(color >> 8);
                WriteData(color & 0xFF);
            } else {
                WriteData(bg >> 8);
                WriteData(bg & 0xFF);
            }
        }
    }
}
/**
 * @brief  V? m?t chu?i ký t? (String)
 */
void ST7735_DrawString(uint16_t x, uint16_t y, char* str, uint16_t color, uint16_t bg) {//cot hang 
    while (*str) {
        ST7735_DrawChar(x, y, *str, color, bg);
        x += 6; // Nh?y sang v? trí ti?p theo (5 pixel ch? + 1 pixel kho?ng cách)
        
        // Xu?ng dòng n?u ch?m biên ph?i màn hình
        if (x > 122) {
            x = 0;
            y += 8;
        }
        str++;
    }
}