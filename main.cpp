#include "main.h"

extern "C" {
#include "Infrared.h"
#include "LCD_1in3.h"
#include "DEV_Config.h"
#include "GUI_Paint.h"
#include <stdio.h>
}

namespace hardware {
    bool keyPressed(uint8_t key) {
        return DEV_Digital_Read(key) == 0;
    }

    void initInfraredPins() {
        SET_Infrared_PIN(keyA);
        SET_Infrared_PIN(keyB);
        SET_Infrared_PIN(keyX);
        SET_Infrared_PIN(keyY);
        SET_Infrared_PIN(keyLeft);
        SET_Infrared_PIN(keyRight);
        SET_Infrared_PIN(keyUp);
        SET_Infrared_PIN(keyDown);
        SET_Infrared_PIN(keyCtrl);
    }
}

namespace {
    // general
    constexpr uint8_t MAX_MOVES = 16;
    constexpr uint8_t MAX_HISTORY = 32;
    constexpr uint8_t MAX_REPEAT = 9;
    constexpr uint8_t MAX_REPEAT_DEPTH = 4;
    // block list
    constexpr uint8_t LIST_VISIBLE = 8;
    constexpr uint16_t LIST_TOP_Y = 36;
    constexpr uint16_t LIST_START_X = 40;
    constexpr uint16_t LIST_ROW_H = 20;
    constexpr uint16_t INDENT_STEP = 16;
    constexpr uint8_t COLOR_SLOTS = 6;
    constexpr uint8_t MAX_PARAM = 7; // 0..7
    constexpr uint8_t COLOR_COUNT = 8;

    enum class BlockType : uint8_t {
        None = 0,
        Move = 1,
        Draw = 2,
        If = 3,
        Repeat = 4,
        End = 5
    };

    enum class TurnState : uint8_t {
        PlayerTurn = 0,
        AITurn = 1,
        RunProgram = 2,
        Finished = 3
    };

    // GUI_Paint.h の色defineを「番号付きリスト」で扱う                                                                                                                                                               │
    constexpr std::array<UWORD, COLOR_COUNT> kPaintColors = {
        WHITE, RED, BRRED, YELLOW, GREEN, BLUE, MAGENTA, BLACK
    };

    constexpr uint8_t colorIndex(const uint8_t idx) {
        return static_cast<uint8_t>(idx % COLOR_COUNT);
    }

    constexpr UWORD paintColorByIndex(const uint8_t idx) {
        return kPaintColors[colorIndex(idx)];
    }


    struct ProgramStep {
        BlockType type = BlockType::None;
        uint8_t param = 0; // Move-> move length, Draw-> Color, Repeat-> repeat times
        bool from_ai = false;
    };

    struct ProgramState {
        std::array<ProgramStep, MAX_MOVES> program{};
        std::array<uint8_t, MAX_HISTORY> player_history{};
        std::array<uint8_t, 8> block_frequency{};
        std::array<std::array<uint16_t, 8>, 8> transitions{};
        std::array<uint8_t, MAX_MOVES> view_depths{};

        uint8_t history_size = 0;
        uint8_t move_count = 0;
        uint8_t selected_line = 0;
        uint8_t scroll_top = 0;
        uint8_t selected_block_idx = 1;
        uint8_t selected_param = 0;
        uint8_t syntax_depth = 0; // 現在開いている IF/REPEAT のネスト数

        TurnState turn = TurnState::PlayerTurn;
        bool compiled_ok = false;
        char status_line[64]{};
    };

    constexpr std::array<const char*, 6> kBlockNames = {
        "NONE",
        "MOVE",
        "DRAW",
        "IF",
        "REPEAT",
        "END",
    };

    void initProgramState(ProgramState& s) {
        s = ProgramState{};
        std::strncpy(s.status_line, "A:add B:val Y:type L/R:var X:run", sizeof(s.status_line) - 1);
    }

    bool isPlayableBlock(const BlockType t) {
        return t == BlockType::Move || t == BlockType::Draw || t == BlockType::If || t == BlockType::Repeat || t == BlockType::End;
    }

    bool blockAllowedByDepth(const ProgramState& s, const BlockType t) {
        if (t == BlockType::End) {
            return s.syntax_depth > 0;
        }
        if ((t == BlockType::If || t == BlockType::Repeat) && s.syntax_depth >= MAX_REPEAT_DEPTH) {
            return false;
        }
        return true;
    }

    void rememberHistory(ProgramState& s, BlockType block) {
        const auto idx = static_cast<uint8_t>(block);
        if (s.history_size > 0) {
            const uint8_t prev = s.player_history[(s.history_size - 1) % MAX_HISTORY];
            s.transitions[prev][idx]++;
        }
        s.player_history[s.history_size % MAX_HISTORY] = idx;
        if (s.history_size < MAX_HISTORY) {
            s.history_size++;
        }
        s.block_frequency[idx]++;
    }

    void ensureSelectionVisible(ProgramState& s) {
        if (s.selected_line < s.scroll_top) {
            s.scroll_top = s.selected_line;
            return;
        }
        const auto bottom = static_cast<uint8_t>(s.scroll_top + LIST_VISIBLE - 1);
        if (s.selected_line > bottom) {
            s.scroll_top = static_cast<uint8_t>(s.selected_line - (LIST_VISIBLE - 1));
        }
    }

    void recalcViewDepths(ProgramState& s) {
        uint8_t depth = 0;
        for (uint8_t i = 0; i < s.move_count; i++) {
            const BlockType t = s.program[i].type;
            if (t == BlockType::End) {
                if (depth > 0) {
                    depth--;
                }
                s.view_depths[i] = depth;
                continue;
            }
            s.view_depths[i] = depth;
            if (t == BlockType::If || t == BlockType::Repeat) {
                depth = static_cast<uint8_t>(std::min<uint8_t>(MAX_REPEAT_DEPTH, depth + 1));
            }
        }
    }

    bool addStepToProgram(ProgramState& s, BlockType type, const uint8_t param, const bool from_ai) {
        if (s.move_count >= MAX_MOVES) {
            std::strncpy(s.status_line, "Move limit reached", sizeof(s.status_line) - 1);
            return false;
        }
        if (!isPlayableBlock(type) || !blockAllowedByDepth(s, type)) {
            std::strncpy(s.status_line, "Block not allowed now", sizeof(s.status_line) - 1);
            return false;
        }

        s.program[s.move_count] = {type, param,  from_ai};
        s.move_count++;

        if (type == BlockType::If || type == BlockType::Repeat) {
            s.syntax_depth++;
        } else if (type == BlockType::End && s.syntax_depth > 0) {
            s.syntax_depth--;
        }

        if (!from_ai) {
            rememberHistory(s, type);
        }

        recalcViewDepths(s);
        if (type == BlockType::If || type == BlockType::Draw) {
            std::snprintf(
            s.status_line,
            sizeof(s.status_line),
            "%s add %s -> %u",
            from_ai ? "AI" : "You",
            kBlockNames[static_cast<uint8_t>(type)],
            param);
        } else {
            std::snprintf(
            s.status_line,
            sizeof(s.status_line),
            "%s add %s -> %u",
            from_ai ? "AI" : "You",
            kBlockNames[static_cast<uint8_t>(type)],
            param);
        }

        return true;
    }


    // --- TODO: AI will come in these lines ---


    void drawProgramList(const ProgramState& s) {
        Paint_DrawString_EN(4, 4, "Blockode", &Font16, BLACK, WHITE);
        Paint_DrawString_EN(4, 20, s.status_line, &Font12, BLACK, WHITE);

        for (uint8_t row = 0; row < LIST_VISIBLE; row++) {
            const auto idx = static_cast<uint8_t>(s.scroll_top + row);
            const auto y = static_cast<uint16_t>(LIST_TOP_Y + row * LIST_ROW_H);
            if (idx >= MAX_MOVES) {
                break; // Finish coding
            }

            const bool is_selected = (idx == s.selected_line);
            Paint_DrawRectangle(40, y - 2, 200, y + 16, is_selected ? GREEN : GRAY, DOT_PIXEL_1X1, DRAW_FILL_EMPTY);

            if (idx >= s.move_count) {
                char empty_line[32];
                std::snprintf(empty_line, sizeof(empty_line), "%02u: [empty]", idx + 1);
                Paint_DrawString_EN(8, y, empty_line, &Font12, BLACK, WHITE);
                continue;
            }

            char line[72];
            const auto& step = s.program[idx];
            const auto indent_x = static_cast<uint16_t>(LIST_START_X + s.view_depths[idx] * INDENT_STEP);

            if (step.type == BlockType::If) {
                std::snprintf(line, sizeof(line), "%02u:IF v%u>=%s", idx + 1, step.param, step.from_ai ? " [AI]" : "");
            } else if (step.type == BlockType::Repeat) {
                std::snprintf(line, sizeof(line), "%02u:REPEAT(%u)%s", idx + 1, step.param, step.from_ai ? " [AI]" : "");
            } else if (step.type == BlockType::Draw) {
                std::snprintf(line, sizeof(line), "%02u:%s(%u)%s", idx + 1, kBlockNames[static_cast<uint8_t>(step.type)], step.param, step.from_ai ? " [AI]" : "");
            } else {
                std::snprintf(line, sizeof(line), "%02u:%s(%u)%s", idx + 1, kBlockNames[static_cast<uint8_t>(step.type)], step.param, step.from_ai ? " [AI]" : "");
            }
            Paint_DrawString_EN(indent_x, y, line, &Font12, BLACK, paintColorByIndex(s.selected_param));
        }
    }

    void drawHUD(const ProgramState& s) {
        char line[64];
        std::snprintf(line, sizeof(line), "Turn:%s Mv:%u/%u Type:%s",
                      s.turn == TurnState::PlayerTurn ? "You" :
                      s.turn == TurnState::AITurn ? "AI" :
                      s.turn == TurnState::RunProgram ? "RUN" : "END",
                      s.move_count, MAX_MOVES,
                      kBlockNames[s.selected_block_idx]);
        Paint_DrawString_EN(4, 232, line, &Font12, BLACK, WHITE);
    }

    void drawMainScene(const ProgramState& s) {
        Paint_Clear(WHITE);
        drawProgramList(s);
        drawHUD(s);
    }

    void cycleBlockType(ProgramState& s) {
        s.selected_block_idx++;
        if (s.selected_block_idx > static_cast<uint8_t>(BlockType::End)) {
            s.selected_block_idx = static_cast<uint8_t>(BlockType::Move);
        }
        std::snprintf(s.status_line, sizeof(s.status_line), "Select type:%s", kBlockNames[s.selected_block_idx]);
    }

    void cycleColor(ProgramState& s) { // TODO: change to color
        s.selected_param = static_cast<uint8_t>((s.selected_param + 1) % (MAX_PARAM + 1)); // 0..MAX_COLORS
    }

// --------------------------------------------
    bool handlePlayerInput(ProgramState& s) {
        if (hardware::keyPressed(keyUp)) {
            if (s.selected_line > 0) {
                s.selected_line--;
                ensureSelectionVisible(s);
            }
            sleep_ms(120);
            return true; // update screen
        }
        if (hardware::keyPressed(keyDown)) {
            if (s.selected_line + 1 < MAX_MOVES) {
                s.selected_line++;
                ensureSelectionVisible(s);
            }
            sleep_ms(120);
            return true;
        }
        if (hardware::keyPressed(keyLeft)) {
            const auto t = static_cast<BlockType>(s.selected_block_idx);
            if (t == BlockType::Move || t == BlockType::If) {
                s.selected_param = static_cast<uint8_t>((s.selected_param + COLOR_SLOTS - 1) % COLOR_SLOTS);
                std::snprintf(s.status_line, sizeof(s.status_line), "Select color:%u", s.selected_param);
            } else if (t == BlockType::Repeat) {
                s.selected_param = static_cast<uint8_t>((s.selected_param + MAX_REPEAT - 1) % MAX_REPEAT);
                std::snprintf(s.status_line, sizeof(s.status_line), "Select repeat:%u", s.selected_param++);
            }
            sleep_ms(120);
            return true;
        }
        if (hardware::keyPressed(keyRight)) {
            const auto t = static_cast<BlockType>(s.selected_block_idx);
            if (t == BlockType::Move || t == BlockType::If) {
                s.selected_param = static_cast<uint8_t>((s.selected_param + 1) % COLOR_SLOTS);
                std::snprintf(s.status_line, sizeof(s.status_line), "Select color:%u", s.selected_param);
            } else if (t == BlockType::Repeat) {
                s.selected_param = static_cast<uint8_t>((s.selected_param + 1) % MAX_REPEAT);
                std::snprintf(s.status_line, sizeof(s.status_line), "Select repeat:%u", s.selected_param++);
            }
            sleep_ms(120);
            return true;
        }
        if (hardware::keyPressed(keyY)) {
            cycleBlockType(s);
            sleep_ms(140);
            return true;
        }
        if (hardware::keyPressed(keyB)) {
            cycleColor(s);
            sleep_ms(140);
            return true;
        }
        if (hardware::keyPressed(keyA)) {
            const auto t = static_cast<BlockType>(s.selected_block_idx);
            uint8_t param = s.selected_param;

            if (addStepToProgram(s, t, param, false)) {
                s.turn = TurnState::AITurn;
            }
            sleep_ms(160);
            return true;
        }
        if (hardware::keyPressed(keyX)) {
            s.turn = TurnState::RunProgram;
            sleep_ms(180);
            return true;
        }
        return false;
    }
}






int LCD() {
    if (DEV_Module_Init() != 0) {
        return -1;
    }
    DEV_SET_PWM(50);
    printf("1.3inch LCD init...\r\n");
    LCD_1IN3_Init(HORIZONTAL);
    LCD_1IN3_Clear(WHITE);

    UDOUBLE imagesize = LCD_1IN3_HEIGHT * LCD_1IN3_WIDTH * 2;
    auto* black_image = static_cast<UWORD*>(malloc(imagesize));
    if (black_image == nullptr) {
        printf("Failed to allocate LCD memory\r\n");
        DEV_Module_Exit();
        return -1;
    }

    Paint_NewImage(reinterpret_cast<UBYTE*>(black_image), LCD_1IN3.WIDTH, LCD_1IN3.HEIGHT, 0, WHITE);
    Paint_SetScale(65);
    Paint_Clear(WHITE);
    hardware::initInfraredPins();

    ProgramState state;
    initProgramState(state);

    bool running = true;
    bool needs_redraw = true;
    while (running) {
        if (state.turn == TurnState::PlayerTurn) {
            const bool changed = handlePlayerInput(state);
            if (state.move_count >= MAX_MOVES && state.turn == TurnState::PlayerTurn) {
                state.turn = TurnState::RunProgram;
                needs_redraw = true;
            }

            if (state.turn != TurnState::PlayerTurn) {
                sleep_ms(20);
                continue;
            }

            if (changed || needs_redraw) {
                drawMainScene(state);
                LCD_1IN3_Display(black_image);
                needs_redraw = false;
            }
            sleep_ms(12);
            continue;
        }

        if (state.turn == TurnState::AITurn) {
            // performAITurn(state);
            drawMainScene(state);
            LCD_1IN3_Display(black_image);
            needs_redraw = false;
            sleep_ms(180);
            continue;
        }

        if (state.turn == TurnState::RunProgram) {
            // compileAndRun(state);
            state.turn = TurnState::Finished;
            needs_redraw = true;
        }

        if (needs_redraw) {
            // drawExecutionResult(state);
            LCD_1IN3_Display(black_image);
            needs_redraw = false;
        }

        if (hardware::keyPressed(keyX)) {
            running = false;
            sleep_ms(180);
        } else if (hardware::keyPressed(keyA)) {
            initProgramState(state);
            needs_redraw = true;
            sleep_ms(180);
        } else {
            sleep_ms(30);
        }
    }

    free(black_image);
    DEV_Module_Exit();
    return 0;
}

int main() {
    stdio_init_all();
    return LCD();
}