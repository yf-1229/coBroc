#pragma once

#ifndef COBROC_MAIN_H
#define COBROC_MAIN_H

#include "pico/stdlib.h"
#include <cstdint>

#include "core/coBroc_core.h"

// ── Key mappings (Pico GPIO) ────────────────────────────────────────────

#define keyA 15
#define keyB 17
#define keyX 19
#define keyY 21
#define keyUp 2
#define keyDown 18
#define keyLeft 16
#define keyRight 20
#define keyCtrl 3

constexpr uint16_t LCD_REFRESH_DELAY_MS = 50;
int LCD();

#endif // COBROC_MAIN_H
