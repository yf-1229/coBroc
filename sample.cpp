#include "sample.h"

extern "C" {
#include "Debug.h"
#include "Infrared.h"
#include "LCD_1in3.h"
#include "DEV_Config.h"
#include "GUI_Paint.h"
#include <stdio.h>
}

#include <array>
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <cstdlib>

namespace {

constexpr uint8_t MAX_MOVES = 20;
constexpr uint8_t MAX_HISTORY = 32;
constexpr uint8_t MAX_FOR_DEPTH = 4;
constexpr uint8_t MAX_FOR_REPEAT = 4;
constexpr uint8_t MAX_VARIABLE = 9;
constexpr uint8_t VAR_SLOTS = 4;
constexpr uint8_t LIST_VISIBLE = 8;
constexpr uint16_t LIST_TOP_Y = 36;
constexpr uint16_t LIST_ROW_H = 20;
constexpr uint16_t LIST_START_X = 8;
constexpr uint16_t INDENT_STEP = 16;

enum class BlockType : uint8_t {
    None = 0,
    Move = 1,
    Draw = 2,
    DefineVar = 3,
    If = 4,
    For = 5,
    End = 6
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
    uint8_t var_id = 0;
    bool from_ai = false;
};

struct ProgramState {
    std::array<ProgramStep, MAX_MOVES> program{};
    std::array<uint8_t, MAX_HISTORY> player_history{};
    std::array<uint8_t, 8> block_frequency{};
    std::array<std::array<uint16_t, 8>, 8> transitions{};
    std::array<uint8_t, MAX_MOVES> view_depths{};
    std::array<uint8_t, VAR_SLOTS> live_vars{};
    std::array<uint8_t, VAR_SLOTS> exec_vars{};

    uint8_t history_size = 0;
    uint8_t move_count = 0;
    uint8_t selected_line = 0;
    uint8_t scroll_top = 0;
    uint8_t selected_block_idx = 1;
    uint8_t selected_param = 1;
    uint8_t selected_var_id = 0;
    uint8_t syntax_depth = 0;

    uint8_t exec_x = 0;
    uint8_t exec_y = 0;
    uint8_t exec_draw_count = 0;

    TurnState turn = TurnState::PlayerTurn;
    bool compiled_ok = false;
    char status_line[64]{};
};

const std::array<const char*, 8> kBlockNames = {
    "NONE",
    "MOVE",
    "DRAW",
    "DEFV",
    "IF",
    "FOR",
    "END",
    "RESV"
};

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

void initProgramState(ProgramState& s) {
    s = ProgramState{};
    std::strncpy(s.status_line, "A:add B:val Y:type L/R:var X:run", sizeof(s.status_line) - 1);
}

bool isPlayableBlock(BlockType t) {
    return t == BlockType::Move || t == BlockType::Draw || t == BlockType::DefineVar || t == BlockType::If || t == BlockType::For || t == BlockType::End;
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
    const uint8_t idx = static_cast<uint8_t>(block);
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
    const uint8_t bottom = static_cast<uint8_t>(s.scroll_top + LIST_VISIBLE - 1);
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
        if (t == BlockType::If || t == BlockType::For) {
            depth = static_cast<uint8_t>(std::min<uint8_t>(MAX_FOR_DEPTH, depth + 1));
        }
    }
}

bool addStepToProgram(ProgramState& s, BlockType type, uint8_t param, uint8_t var_id, bool from_ai) {
    if (s.move_count >= MAX_MOVES) {
        std::strncpy(s.status_line, "Move limit reached", sizeof(s.status_line) - 1);
        return false;
    }
    if (!isPlayableBlock(type) || !blockAllowedByDepth(s, type)) {
        std::strncpy(s.status_line, "Block not allowed now", sizeof(s.status_line) - 1);
        return false;
    }

    s.program[s.move_count] = {type, param, static_cast<uint8_t>(var_id % VAR_SLOTS), from_ai};
    s.move_count++;
    s.selected_line = static_cast<uint8_t>(s.move_count == 0 ? 0 : s.move_count - 1);

    if (type == BlockType::If || type == BlockType::For) {
        s.syntax_depth++;
    } else if (type == BlockType::End && s.syntax_depth > 0) {
        s.syntax_depth--;
    }

    if (type == BlockType::DefineVar) {
        s.live_vars[var_id % VAR_SLOTS] = param;
    }

    if (!from_ai) {
        rememberHistory(s, type);
    }

    recalcViewDepths(s);
    ensureSelectionVisible(s);
    std::snprintf(
        s.status_line,
        sizeof(s.status_line),
        "%s add %s v%u=%u",
        from_ai ? "AI" : "P",
        kBlockNames[static_cast<uint8_t>(type)],
        var_id % VAR_SLOTS,
        param
    );
    return true;
}

uint8_t findDominantVar(const ProgramState& s) {
    uint8_t best = 0;
    for (uint8_t i = 1; i < VAR_SLOTS; i++) {
        if (s.live_vars[i] > s.live_vars[best]) {
            best = i;
        }
    }
    return best;
}

uint16_t scoreAIBlock(const ProgramState& s, BlockType candidate) {
    const uint8_t idx = static_cast<uint8_t>(candidate);
    uint16_t score = 4;
    if (s.history_size > 0) {
        const uint8_t prev = s.player_history[(s.history_size - 1) % MAX_HISTORY];
        score += static_cast<uint16_t>(s.transitions[prev][idx] * 2);
    }
    score += s.block_frequency[idx];
    if ((candidate == BlockType::If || candidate == BlockType::For) && s.syntax_depth >= MAX_FOR_DEPTH - 1) {
        score = 0;
    }
    if (candidate == BlockType::End) {
        score = s.syntax_depth == 0 ? 0 : static_cast<uint16_t>(score + 3);
    }
    if (candidate == BlockType::If) {
        score = static_cast<uint16_t>(score + s.live_vars[findDominantVar(s)]);
    }
    return score;
}

BlockType selectAIBlock(const ProgramState& s) {
    const std::array<BlockType, 6> candidates = {
        BlockType::Move, BlockType::Draw, BlockType::DefineVar, BlockType::If, BlockType::For, BlockType::End
    };
    BlockType best = BlockType::Move;
    uint16_t best_score = 0;
    for (BlockType c : candidates) {
        if (!blockAllowedByDepth(s, c)) {
            continue;
        }
        const uint16_t score = scoreAIBlock(s, c);
        if (score > best_score) {
            best = c;
            best_score = score;
        }
    }
    return best;
}

uint8_t chooseAIVarId(const ProgramState& s, BlockType type) {
    if (type == BlockType::If) {
        return findDominantVar(s);
    }
    if (type == BlockType::DefineVar) {
        return static_cast<uint8_t>((s.move_count + s.history_size) % VAR_SLOTS);
    }
    return 0;
}

uint8_t chooseAIParam(const ProgramState& s, BlockType type, uint8_t var_id) {
    switch (type) {
        case BlockType::Move:
            return static_cast<uint8_t>((s.move_count + s.live_vars[var_id % VAR_SLOTS]) % 4);
        case BlockType::Draw:
            return static_cast<uint8_t>((s.move_count % 4) + 1);
        case BlockType::DefineVar:
            return static_cast<uint8_t>((s.live_vars[var_id % VAR_SLOTS] + 2) % (MAX_VARIABLE + 1));
        case BlockType::If:
            return static_cast<uint8_t>(s.live_vars[var_id % VAR_SLOTS] / 2);
        case BlockType::For:
            return static_cast<uint8_t>((s.move_count % MAX_FOR_REPEAT) + 1);
        case BlockType::End:
        case BlockType::None:
        default:
            return 0;
    }
}

void performAITurn(ProgramState& s) {
    const BlockType block = selectAIBlock(s);
    const uint8_t var_id = chooseAIVarId(s, block);
    const uint8_t param = chooseAIParam(s, block, var_id);

    if (!addStepToProgram(s, block, param, var_id, true)) {
        std::strncpy(s.status_line, "AI fallback MOVE", sizeof(s.status_line) - 1);
        addStepToProgram(s, BlockType::Move, 0, 0, true);
    }

    if (s.move_count >= MAX_MOVES) {
        s.turn = TurnState::RunProgram;
    } else {
        s.turn = TurnState::PlayerTurn;
    }
}

bool finalizeSyntax(ProgramState& s) {
    while (s.syntax_depth > 0 && s.move_count < MAX_MOVES) {
        addStepToProgram(s, BlockType::End, 0, 0, true);
    }
    return s.syntax_depth == 0;
}

struct LoopFrame {
    uint8_t start_pc;
    uint8_t remaining;
    uint8_t end_pc;
};

bool compileAndRun(ProgramState& s) {
    s.compiled_ok = false;
    s.exec_x = 0;
    s.exec_y = 0;
    s.exec_draw_count = 0;
    s.exec_vars.fill(0);

    if (!finalizeSyntax(s)) {
        std::strncpy(s.status_line, "Compile failed: unclosed blocks", sizeof(s.status_line) - 1);
        return false;
    }

    std::array<int8_t, MAX_MOVES> if_end{};
    std::array<int8_t, MAX_MOVES> for_end{};
    std::array<int8_t, MAX_MOVES> end_for_start{};
    for (auto& v : if_end) v = -1;
    for (auto& v : for_end) v = -1;
    for (auto& v : end_for_start) v = -1;

    std::array<uint8_t, MAX_MOVES> stack{};
    uint8_t top = 0;
    for (uint8_t pc = 0; pc < s.move_count; pc++) {
        const BlockType t = s.program[pc].type;
        if (t == BlockType::If || t == BlockType::For) {
            if (top >= MAX_MOVES) {
                std::strncpy(s.status_line, "Compile stack overflow", sizeof(s.status_line) - 1);
                return false;
            }
            stack[top++] = pc;
        } else if (t == BlockType::End) {
            if (top == 0) {
                std::strncpy(s.status_line, "Compile error: END mismatch", sizeof(s.status_line) - 1);
                return false;
            }
            const uint8_t start = stack[--top];
            if (s.program[start].type == BlockType::If) {
                if_end[start] = static_cast<int8_t>(pc);
            } else {
                for_end[start] = static_cast<int8_t>(pc);
                end_for_start[pc] = static_cast<int8_t>(start);
            }
        }
    }
    if (top != 0) {
        std::strncpy(s.status_line, "Compile error: missing END", sizeof(s.status_line) - 1);
        return false;
    }

    std::array<LoopFrame, MAX_FOR_DEPTH> loops{};
    uint8_t loop_top = 0;
    uint8_t pc = 0;
    while (pc < s.move_count) {
        const ProgramStep step = s.program[pc];
        switch (step.type) {
            case BlockType::Move:
                switch (step.param % 4) {
                    case 0: if (s.exec_x + 1 < 8) s.exec_x++; break;
                    case 1: if (s.exec_x > 0) s.exec_x--; break;
                    case 2: if (s.exec_y + 1 < 8) s.exec_y++; break;
                    default: if (s.exec_y > 0) s.exec_y--; break;
                }
                pc++;
                break;
            case BlockType::Draw:
                s.exec_draw_count = static_cast<uint8_t>(std::min<int>(255, s.exec_draw_count + step.param));
                pc++;
                break;
            case BlockType::DefineVar:
                s.exec_vars[step.var_id % VAR_SLOTS] = step.param;
                pc++;
                break;
            case BlockType::If: {
                const uint8_t lhs = s.exec_vars[step.var_id % VAR_SLOTS];
                const bool cond = lhs >= step.param;
                if (!cond) {
                    const int8_t jump = if_end[pc];
                    if (jump < 0) return false;
                    pc = static_cast<uint8_t>(jump + 1);
                } else {
                    pc++;
                }
                break;
            }
            case BlockType::For: {
                const int8_t end_pc = for_end[pc];
                if (end_pc < 0) return false;
                if (loop_top >= MAX_FOR_DEPTH) {
                    std::strncpy(s.status_line, "Runtime loop overflow", sizeof(s.status_line) - 1);
                    return false;
                }
                loops[loop_top++] = {pc, static_cast<uint8_t>(std::max<uint8_t>(1, step.param)), static_cast<uint8_t>(end_pc)};
                pc++;
                break;
            }
            case BlockType::End: {
                const int8_t start = end_for_start[pc];
                if (start < 0) {
                    pc++;
                    break;
                }
                if (loop_top == 0) return false;
                auto& frame = loops[loop_top - 1];
                if (frame.start_pc != static_cast<uint8_t>(start)) {
                    return false;
                }
                if (frame.remaining > 1) {
                    frame.remaining--;
                    pc = static_cast<uint8_t>(frame.start_pc + 1);
                } else {
                    loop_top--;
                    pc++;
                }
                break;
            }
            case BlockType::None:
            default:
                pc++;
                break;
        }
    }

    s.compiled_ok = true;
    std::snprintf(
        s.status_line,
        sizeof(s.status_line),
        "Run ok x:%u y:%u d:%u v0:%u",
        s.exec_x,
        s.exec_y,
        s.exec_draw_count,
        s.exec_vars[0]
    );
    return true;
}

void drawBlockIcon(const BlockType type, const uint16_t center_x, const uint16_t center_y, const bool from_ai) {
    const UWORD color = from_ai ? BLUE : BLACK;
    switch (type) {
        case BlockType::Move:
            Paint_DrawLine(
                static_cast<uint16_t>(center_x - 3),
                center_y,
                static_cast<uint16_t>(center_x + 3),
                center_y,
                color,
                DOT_PIXEL_1X1,
                LINE_STYLE_SOLID
            );
            Paint_DrawLine(
                static_cast<uint16_t>(center_x + 3),
                center_y,
                static_cast<uint16_t>(center_x + 1),
                static_cast<uint16_t>(center_y - 2),
                color,
                DOT_PIXEL_1X1,
                LINE_STYLE_SOLID
            );
            Paint_DrawLine(
                static_cast<uint16_t>(center_x + 3),
                center_y,
                static_cast<uint16_t>(center_x + 1),
                static_cast<uint16_t>(center_y + 2),
                color,
                DOT_PIXEL_1X1,
                LINE_STYLE_SOLID
            );
            break;
        case BlockType::Draw:
            Paint_DrawCircle(center_x, center_y, 3, color, DOT_PIXEL_1X1, DRAW_FILL_FULL);
            break;
        case BlockType::DefineVar:
            Paint_DrawRectangle(
                static_cast<uint16_t>(center_x - 3),
                static_cast<uint16_t>(center_y - 3),
                static_cast<uint16_t>(center_x + 3),
                static_cast<uint16_t>(center_y + 3),
                color,
                DOT_PIXEL_1X1,
                DRAW_FILL_EMPTY
            );
            break;
        case BlockType::If:
            Paint_DrawLine(
                center_x,
                static_cast<uint16_t>(center_y - 3),
                static_cast<uint16_t>(center_x + 3),
                center_y,
                color,
                DOT_PIXEL_1X1,
                LINE_STYLE_SOLID
            );
            Paint_DrawLine(
                static_cast<uint16_t>(center_x + 3),
                center_y,
                center_x,
                static_cast<uint16_t>(center_y + 3),
                color,
                DOT_PIXEL_1X1,
                LINE_STYLE_SOLID
            );
            Paint_DrawLine(
                center_x,
                static_cast<uint16_t>(center_y + 3),
                static_cast<uint16_t>(center_x - 3),
                center_y,
                color,
                DOT_PIXEL_1X1,
                LINE_STYLE_SOLID
            );
            Paint_DrawLine(
                static_cast<uint16_t>(center_x - 3),
                center_y,
                center_x,
                static_cast<uint16_t>(center_y - 3),
                color,
                DOT_PIXEL_1X1,
                LINE_STYLE_SOLID
            );
            break;
        case BlockType::For:
            Paint_DrawCircle(center_x, center_y, 3, color, DOT_PIXEL_1X1, DRAW_FILL_EMPTY);
            Paint_DrawLine(
                static_cast<uint16_t>(center_x + 1),
                static_cast<uint16_t>(center_y - 3),
                static_cast<uint16_t>(center_x + 3),
                static_cast<uint16_t>(center_y - 3),
                color,
                DOT_PIXEL_1X1,
                LINE_STYLE_SOLID
            );
            Paint_DrawLine(
                static_cast<uint16_t>(center_x + 3),
                static_cast<uint16_t>(center_y - 3),
                static_cast<uint16_t>(center_x + 2),
                static_cast<uint16_t>(center_y - 4),
                color,
                DOT_PIXEL_1X1,
                LINE_STYLE_SOLID
            );
            break;
        case BlockType::End:
            Paint_DrawLine(
                static_cast<uint16_t>(center_x - 3),
                static_cast<uint16_t>(center_y - 3),
                static_cast<uint16_t>(center_x + 3),
                static_cast<uint16_t>(center_y + 3),
                color,
                DOT_PIXEL_1X1,
                LINE_STYLE_SOLID
            );
            Paint_DrawLine(
                static_cast<uint16_t>(center_x - 3),
                static_cast<uint16_t>(center_y + 3),
                static_cast<uint16_t>(center_x + 3),
                static_cast<uint16_t>(center_y - 3),
                color,
                DOT_PIXEL_1X1,
                LINE_STYLE_SOLID
            );
            break;
        case BlockType::None:
        default:
            Paint_DrawPoint(center_x, center_y, color, DOT_PIXEL_2X2, DOT_FILL_AROUND);
            break;
    }
}

void drawProgramList(const ProgramState& s) {
    Paint_DrawString_EN(4, 4, "Program (List + Indent)", &Font16, BLACK, WHITE);
    Paint_DrawString_EN(4, 20, s.status_line, &Font12, BLACK, WHITE);

    for (uint8_t row = 0; row < LIST_VISIBLE; row++) {
        const uint8_t idx = static_cast<uint8_t>(s.scroll_top + row);
        const uint16_t y = static_cast<uint16_t>(LIST_TOP_Y + row * LIST_ROW_H);
        if (idx >= MAX_MOVES) {
            break;
        }

        const bool is_selected = (idx == s.selected_line);
        Paint_DrawRectangle(2, y - 2, 238, y + 16, is_selected ? GREEN : GRAY, DOT_PIXEL_1X1, DRAW_FILL_EMPTY);

        if (idx >= s.move_count) {
            char empty_line[32];
            std::snprintf(empty_line, sizeof(empty_line), "%02u: [empty]", idx + 1);
            Paint_DrawString_EN(8, y, empty_line, &Font12, BLACK, WHITE);
            continue;
        }

        const auto& step = s.program[idx];
        const uint16_t indent_x = static_cast<uint16_t>(LIST_START_X + s.view_depths[idx] * INDENT_STEP);
        const uint16_t icon_center_x = static_cast<uint16_t>(indent_x + 4);
        const uint16_t icon_center_y = static_cast<uint16_t>(y + 6);
        const uint16_t text_x = static_cast<uint16_t>(indent_x + 12);
        drawBlockIcon(step.type, icon_center_x, icon_center_y, step.from_ai);

        char line[72];
        if (step.type == BlockType::DefineVar) {
            std::snprintf(line, sizeof(line), "%02u:DEFV v%u=%u%s", idx + 1, step.var_id, step.param, step.from_ai ? " [AI]" : "");
        } else if (step.type == BlockType::If) {
            std::snprintf(line, sizeof(line), "%02u:IF v%u>=%u%s", idx + 1, step.var_id, step.param, step.from_ai ? " [AI]" : "");
        } else if (step.type == BlockType::For) {
            std::snprintf(line, sizeof(line), "%02u:FOR(%u)%s", idx + 1, step.param, step.from_ai ? " [AI]" : "");
        } else {
            std::snprintf(line, sizeof(line), "%02u:%s(%u)%s", idx + 1, kBlockNames[static_cast<uint8_t>(step.type)], step.param, step.from_ai ? " [AI]" : "");
        }
        Paint_DrawString_EN(text_x, y, line, &Font12, BLACK, WHITE);
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
    Paint_DrawString_EN(4, 202, line, &Font12, BLACK, WHITE);



    std::snprintf(line, sizeof(line), "Depth:%u  keys:A add B val Y type", s.syntax_depth);
    Paint_DrawString_EN(4, 232, line, &Font12, BLACK, WHITE);
}

void drawExecutionResult(const ProgramState& s) {
    Paint_Clear(WHITE);
    Paint_DrawString_EN(4, 8, "Execution Result", &Font20, BLACK, WHITE);

    char line[64];
    std::snprintf(line, sizeof(line), "Program steps: %u", s.move_count);
    Paint_DrawString_EN(4, 40, line, &Font16, BLACK, WHITE);
    std::snprintf(line, sizeof(line), "Compiled: %s", s.compiled_ok ? "YES" : "NO");
    Paint_DrawString_EN(4, 64, line, &Font16, BLACK, WHITE);
    std::snprintf(line, sizeof(line), "Cursor x:%u y:%u", s.exec_x, s.exec_y);
    Paint_DrawString_EN(4, 88, line, &Font16, BLACK, WHITE);
    std::snprintf(line, sizeof(line), "Draw count:%u", s.exec_draw_count);
    Paint_DrawString_EN(4, 112, line, &Font16, BLACK, WHITE);
    std::snprintf(
        line,
        sizeof(line),
        "Vars v0:%u v1:%u v2:%u v3:%u",
        s.exec_vars[0], s.exec_vars[1], s.exec_vars[2], s.exec_vars[3]
    );
    Paint_DrawString_EN(4, 136, line, &Font16, BLACK, WHITE);
    Paint_DrawString_EN(4, 180, "X:exit  A:new game", &Font16, BLACK, WHITE);
    Paint_DrawString_EN(4, 206, s.status_line, &Font12, BLACK, WHITE);
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

void cycleParam(ProgramState& s) {
    s.selected_param = static_cast<uint8_t>((s.selected_param + 1) % (MAX_VARIABLE + 1));
    std::snprintf(s.status_line, sizeof(s.status_line), "Select val:%u", s.selected_param);
}

bool handlePlayerInput(ProgramState& s) {
    if (keyPressed(keyUp)) {
        if (s.selected_line > 0) {
            s.selected_line--;
            ensureSelectionVisible(s);
        }
        sleep_ms(120);
        return true;
    }
    if (keyPressed(keyDown)) {
        if (s.selected_line + 1 < MAX_MOVES) {
            s.selected_line++;
            ensureSelectionVisible(s);
        }
        sleep_ms(120);
        return true;
    }
    if (keyPressed(keyLeft)) {
        s.selected_var_id = static_cast<uint8_t>((s.selected_var_id + VAR_SLOTS - 1) % VAR_SLOTS);
        std::snprintf(s.status_line, sizeof(s.status_line), "Select var:v%u", s.selected_var_id);
        sleep_ms(120);
        return true;
    }
    if (keyPressed(keyRight)) {
        s.selected_var_id = static_cast<uint8_t>((s.selected_var_id + 1) % VAR_SLOTS);
        std::snprintf(s.status_line, sizeof(s.status_line), "Select var:v%u", s.selected_var_id);
        sleep_ms(120);
        return true;
    }
    if (keyPressed(keyY)) {
        cycleBlockType(s);
        sleep_ms(140);
        return true;
    }
    if (keyPressed(keyB)) {
        cycleParam(s);
        sleep_ms(140);
        return true;
    }
    if (keyPressed(keyA)) {
        const BlockType t = static_cast<BlockType>(s.selected_block_idx);
        uint8_t param = s.selected_param;
        uint8_t var_id = s.selected_var_id;
        if (t == BlockType::Move) {
            param %= 4;
            var_id = 0;
        } else if (t == BlockType::For) {
            param = static_cast<uint8_t>((param % MAX_FOR_REPEAT) + 1);
            var_id = 0;
        } else if (t == BlockType::End) {
            param = 0;
            var_id = 0;
        }

        if (addStepToProgram(s, t, param, var_id, false)) {
            s.turn = TurnState::AITurn;
        }
        sleep_ms(160);
        return true;
    }
    if (keyPressed(keyX)) {
        s.turn = TurnState::RunProgram;
        sleep_ms(180);
        return true;
    }
    return false;
}

} // namespace

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
    initInfraredPins();

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

        if (keyPressed(keyX)) {
            running = false;
            sleep_ms(180);
        } else if (keyPressed(keyA)) {
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
