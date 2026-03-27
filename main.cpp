#include "main.h"

extern "C" {
#include "Debug.h"
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
    constexpr uint8_t MAX_MOVES = 12;
    constexpr uint8_t MAX_HISTORY = 32;
    constexpr uint8_t MAX_FOR_REPEAT = 9;
    constexpr uint8_t MAX_FOR_DEPTH = 4;
    // block list
    constexpr uint8_t LIST_VISIBLE = 8;
    constexpr uint16_t LIST_TOP_Y = 36;
    constexpr uint16_t LIST_START_X = 8;
    constexpr uint16_t LIST_ROW_H = 20;
    constexpr uint16_t INDENT_STEP = 16;
    // var
    constexpr uint8_t COLOR_SLOTS = 6;
    constexpr uint8_t MAX_COLORS = 9;

    enum class BlockType : uint8_t {
        None = 0,
        Move = 1,
        Draw = 2,
        Val = 3,
        If = 4,
        For = 5,
        End = 6
    };

    enum class Colors : uint8_t {
        White = 0,
        Red = 1,
        Orange = 2,
        Yellow = 3,
        Green = 4,
        Blue = 5,
        Purple = 6,
        Black = 7,
    };

    enum class TurnState : uint8_t {
        PlayerTurn = 0,
        AITurn = 1,
        RunProgram = 2,
        Finished = 3
    };

    struct ProgramStep {
        BlockType type = BlockType::None;
        uint8_t param = 0;
        uint8_t val_id = 0; // TODO: change to color
        bool from_ai = false;
    };

    struct ProgramState {
        std::array<ProgramStep, MAX_MOVES> program{};
        std::array<uint8_t, MAX_HISTORY> player_history{};
        std::array<uint8_t, 8> block_frequency{};
        std::array<std::array<uint16_t, 8>, 8> transitions{};
        std::array<uint8_t, MAX_MOVES> view_depths{};
        std::array<uint8_t, COLOR_SLOTS> live_colors{};
        std::array<uint8_t, COLOR_SLOTS> exec_colors{};

        uint8_t history_size = 0;
        uint8_t move_count = 0;
        uint8_t selected_block_idx = 1;
        uint8_t selected_repeat = 1;
        uint8_t selected_color = 0;
        uint8_t if_edit_threshold = 0;
        uint8_t syntax_depth = 0;

        TurnState turn = TurnState::PlayerTurn;
        bool compiled_ok = false;
        char status_line[64]{};
    };

    constexpr std::array<const char*, 8> kBlockNames = {
        "NONE",
        "MOVE",
        "DRAW",
        "VAL",
        "IF",
        "FOR",
        "END",
    };

    void initProgramState(ProgramState& s) {
        s = ProgramState{};
        std::strncpy(s.status_line, "A:add B:val Y:type L/R:var X:run", sizeof(s.status_line) - 1);
    }

    bool isPlayableBlock(BlockType t) { // TODO: add rule
        return t == BlockType::Move || t == BlockType::Draw || t == BlockType::Val || t == BlockType::If || t == BlockType::For || t == BlockType::End;
    }

    bool blockAllowedByDepth(const ProgramState& s, BlockType t) {
        if (t == BlockType::End) {
            return s.syntax_depth > 0;
        }
        if ((t == BlockType::If || t == BlockType::For) && s.syntax_depth >= MAX_FOR_DEPTH) {
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
            if (t == BlockType::If || t == BlockType::For) {
                depth = static_cast<uint8_t>(std::min<uint8_t>(MAX_FOR_DEPTH, depth + 1));
            }
        }
    }




    void cycleBlockType(ProgramState& s) {
        s.selected_block_idx++;
        if (s.selected_block_idx > static_cast<uint8_t>(BlockType::End)) {
            s.selected_block_idx = static_cast<uint8_t>(BlockType::Move);
        }
        std::snprintf(s.status_line, sizeof(s.status_line), "Select type:%s", kBlockNames[s.selected_block_idx]);
    }

    void cycleColor(ProgramState& s) { // TODO: change to color
        s.selected_color = static_cast<uint8_t>((s.selected_color + 1) % (MAX_COLORS + 1)); // 0..MAX_COLORS
        std::snprintf(s.status_line, sizeof(s.status_line), "Select val:%u", s.selected_color);
    }

// --------------------------------------------
    bool handlePlayerInput(ProgramState& s) {
        if (hardware::keyPressed(keyUp)) {
            // TODO: select block
            sleep_ms(120);
            return true; // update screen
        }
        if (hardware::keyPressed(keyDown)) {

            sleep_ms(120);
            return true;
        }
        if (hardware::keyPressed(keyLeft)) {
            const auto t = static_cast<BlockType>(s.selected_block_idx);
            if (t == BlockType::Move || t == BlockType::If) {
                s.selected_color %= MAX_COLORS;
            } else if (t == BlockType::For) {
                s.selected_repeat = static_cast<uint8_t>((s.selected_repeat % MAX_FOR_REPEAT) + 1);
            }


            s.selected_repeat = static_cast<uint8_t>((s.selected_var_id + COLOR_SLOTS - 1) % COLOR_SLOTS);
            std::snprintf(s.status_line, sizeof(s.status_line), "Select var:v%u", s.selected_var_id);
            sleep_ms(120);
            return true;
        }
        if (hardware::keyPressed(keyRight)) {
            s.selected_var_id = static_cast<uint8_t>((s.selected_var_id + 1) % COLOR_SLOTS);
            std::snprintf(s.status_line, sizeof(s.status_line), "Select var:v%u", s.selected_var_id);
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
            // uint8_t param = s.selected_color;
            // if (t == BlockType::Move) {
            //     param %= 4;
            //     var_id = 0;
            // } else if (t == BlockType::For) {
            //     param = static_cast<uint8_t>((param % MAX_FOR_REPEAT) + 1);
            //     var_id = 0;
            // } else if (t == BlockType::End) {
            //     param = 0;
            //     var_id = 0;
            // }

            if (addStepToProgram(s, t, param, var_id, false)) {
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
            performAITurn(state);
            drawMainScene(state);
            LCD_1IN3_Display(black_image);
            needs_redraw = false;
            sleep_ms(180);
            continue;
        }

        if (state.turn == TurnState::RunProgram) {
            compileAndRun(state);
            state.turn = TurnState::Finished;
            needs_redraw = true;
        }

        if (needs_redraw) {
            drawExecutionResult(state);
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