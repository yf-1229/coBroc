#include "main.h"

extern "C" {
#include "Debug.h"
#include "Infrared.h"
#include "LCD_1in3.h"
#include "DEV_Config.h"
#include "GUI_Paint.h"
#include <stdio.h>
}

bool game_status = false;

std::random_device dev;
std::mt19937 rng(dev());
// test

std::uniform_int_distribution<uint8_t> dist32(0,31);

int LCD() { // template for games

    if (DEV_Module_Init() != 0) {
        return -1;
    }
    DEV_SET_PWM(50);
    printf("1.3inch LCD init...\r\n");
    LCD_1IN3_Init(HORIZONTAL);
    LCD_1IN3_Clear(BLACK);

    UDOUBLE Imagesize = LCD_1IN3_HEIGHT * LCD_1IN3_WIDTH * 2;
    UWORD *BlackImage;
    if ((BlackImage = static_cast<uint16_t *>(malloc(Imagesize))) == nullptr) {
        printf("Failed to apply for black memory...\r\n");
        exit(0);
    }

    Paint_NewImage(reinterpret_cast<uint8_t *>(BlackImage), LCD_1IN3.WIDTH, LCD_1IN3.HEIGHT, 0, WHITE);
    Paint_SetScale(65);
    Paint_Clear(BLACK);

    // ボタンピンの初期化
    SET_Infrared_PIN(keyA);
    SET_Infrared_PIN(keyB);
    SET_Infrared_PIN(keyX);
    SET_Infrared_PIN(keyY);
    SET_Infrared_PIN(keyLeft);
    SET_Infrared_PIN(keyRight);
    SET_Infrared_PIN(keyUp);
    SET_Infrared_PIN(keyDown);
    SET_Infrared_PIN(keyCtrl);

    // 初期描画
    LCD_1IN3_Display(BlackImage);

    bool* const game_status_ptr = &game_status;

    while (true) {
        Paint_Clear(WHITE);

        // TODO : draw() is here!

        // User Action
        if (DEV_Digital_Read(keyA) == 0 ) {
            printf("KeyA Pressed!\r\n");
            *game_status_ptr = true;
            sleep_ms(LCD_REFRESH_DELAY_MS);
        }

        if (DEV_Digital_Read(keyX) == 0) {
            break;
        }

        if (DEV_Digital_Read(keyB) == 0 ) {
            printf("KeyB Pressed!\r\n");
            LCD_1IN3_Display(BlackImage);
            sleep_ms(LCD_REFRESH_DELAY_MS);
        }

        if (DEV_Digital_Read(keyUp) == 0) {
            printf("keyUp Pressed!\r\n"); // for Debug

            LCD_1IN3_Display(BlackImage);

        }

        if (DEV_Digital_Read(keyDown) == 0) {
            printf("keyDown Pressed!\r\n"); // for Debug

            LCD_1IN3_Display(BlackImage);
        }

        if (DEV_Digital_Read(keyLeft) == 0) {
            printf("keyLeft Pressed!\r\n"); // for Debug
            LCD_1IN3_Display(BlackImage);
        }

        if (DEV_Digital_Read(keyRight) == 0) {
            printf("KeyRight Pressed!\r\n"); // for Debug
            LCD_1IN3_Display(BlackImage);
        }
    }
    LCD_1IN3_Display(BlackImage);
    sleep_ms(16);  // 約60fps
    puts("Off");
    free(BlackImage);
    BlackImage = NULL;
    DEV_Module_Exit();

    return 0;
}

int main() {
    stdio_init_all();

    LCD();

    return 0;
}