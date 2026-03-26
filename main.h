//
// Created by yuta on 2026/03/26.
//

#pragma once

#ifndef CODES_MAIN_H
#define CODES_MAIN_H

#include <iostream>
#include <random>
#include "pico/stdlib.h"
#include "pico/multicore.h"
#include "pico/mutex.h"


#define keyA 15
#define keyB 17
#define keyX 19
#define keyY 21
#define keyUp 2
#define keyDown 18
#define keyLeft 16
#define keyRight 20
#define keyCtrl 3

#endif //CODES_MAIN_H


constexpr uint16_t LCD_REFRESH_DELAY_MS = 50;