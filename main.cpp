#include "main.h"

extern "C" {
#include "Debug.h"
#include "Infrared.h"
#include "LCD_1in3.h"
#include "DEV_Config.h"
#include "GUI_Paint.h"
#include <stdio.h>
}

namespace {
    constexpr uint8_t MAX_MOVES = 12;
    constexpr uint8_t MAX_VARIABLES = 9;
    constexpr uint8_t MAX_FOR_DEPTH = 4;

}