#include "main.h"

extern "C" {
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
    constexpr uint8_t MAX_MOVES = 16;
    constexpr uint8_t MAX_HISTORY = 32;
    constexpr uint8_t MAX_REPEAT_DEPTH = 4;

    constexpr uint8_t LIST_VISIBLE = 9;
    constexpr uint16_t HEADER_TOP_Y = 2;
    constexpr uint16_t HEADER_HEIGHT = 42;
    constexpr uint16_t LIST_TOP_Y = HEADER_TOP_Y + HEADER_HEIGHT + 4;
    constexpr uint16_t LIST_START_X = 16;
    constexpr uint16_t LIST_ROW_H = 20;
    constexpr uint16_t INDENT_STEP = 16;

    constexpr uint8_t MAX_PARAM = 7; // 0..7
    constexpr uint8_t COLOR_COUNT = 8;
    constexpr uint8_t MAX_REPEAT = MAX_PARAM;
    constexpr uint8_t BOARD_LIMIT = 8;
    constexpr size_t AI_CANDIDATE_SLOTS = static_cast<size_t>((MAX_PARAM + 1) * 2 + MAX_REPEAT + 1);

    constexpr bool ENABLE_AI_LOG = true;
    constexpr const char* AI_LOG_PATH = "/tmp/maincpp_ai_ranking.csv";

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
        SelectInputColor = 2,
        RunProgram = 3,
        Finished = 4
    };

    constexpr std::array<UWORD, COLOR_COUNT> kPaintColors = {
        WHITE, RED, BRRED, YELLOW, GREEN, BLUE, MAGENTA, BLACK
    };

    constexpr std::array<const char*, 6> kBlockNames = {
        "NONE",
        "MOVE",
        "DRAW",
        "IF",
        "REPEAT",
        "END",
    };

    constexpr const char* blockName(BlockType t) {
        return kBlockNames[static_cast<uint8_t>(t)];
    }

    constexpr uint8_t colorIndex(uint8_t idx) {
        return static_cast<uint8_t>(idx % COLOR_COUNT);
    }

    constexpr UWORD paintColorByIndex(uint8_t idx) {
        return kPaintColors[colorIndex(idx)];
    }

    struct ProgramStep {
        BlockType type = BlockType::None;
        uint8_t param = 0;
        bool from_ai = false;
    };

    struct RuntimeState {
        uint8_t x = 0;
        uint8_t y = 0;
        uint8_t draw_color = 0;
    };

    struct LoopFrame {
        uint8_t start_pc = 0;
        uint8_t remaining = 0;
    };

    struct AICandidate {
        BlockType type = BlockType::None;
        uint8_t param = 0;
        bool legal = false;
        uint16_t transition_score = 0;
        uint16_t structure_score = 0;
        uint16_t progress_score = 0;
        uint16_t score = 0;
    };

    struct ProgramState {
        std::array<ProgramStep, MAX_MOVES> program{};
        std::array<uint8_t, MAX_HISTORY> history{};
        std::array<uint8_t, 8> block_frequency{};
        std::array<std::array<uint16_t, 8>, 8> transitions{};
        std::array<uint8_t, MAX_MOVES> view_depths{};

        uint16_t game_id = 1;
        uint8_t history_size = 0;
        uint8_t move_count = 0;
        uint8_t selected_line = 0;
        uint8_t scroll_top = 0;
        uint8_t selected_block_idx = static_cast<uint8_t>(BlockType::Draw);
        uint8_t selected_param = 0;
        uint8_t syntax_depth = 0;
        uint8_t run_input_color = 0;

        TurnState turn = TurnState::PlayerTurn;
        bool compiled_ok = false;
        RuntimeState runtime{};
    };

    bool g_ai_log_header_written = false;

    void writeAiLogHeader() {
        if (!ENABLE_AI_LOG || g_ai_log_header_written) {
            return;
        }
        FILE* fp = std::fopen(AI_LOG_PATH, "w");
        if (fp == nullptr) {
            return;
        }
        std::fprintf(
            fp,
            "query_id,game_id,turn,candidate_type,candidate_param,depth,remaining_moves,last_type,freq_move,freq_draw,freq_if,freq_repeat,freq_end,transition_prev_to_candidate,legal,chosen,label\n"
        );
        std::fclose(fp);
        g_ai_log_header_written = true;
    }

    void appendAiLogRow(
        const ProgramState& s,
        uint8_t turn_index,
        const AICandidate& c,
        uint8_t last_type,
        uint16_t transition_prev_to_candidate,
        bool chosen,
        uint8_t label
    ) {
        if (!ENABLE_AI_LOG) {
            return;
        }
        writeAiLogHeader();
        FILE* fp = std::fopen(AI_LOG_PATH, "a");
        if (fp == nullptr) {
            return;
        }
        const uint32_t query_id = static_cast<uint32_t>(s.game_id) * 100u + turn_index;
        std::fprintf(
            fp,
            "%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u\n",
            query_id,
            s.game_id,
            turn_index,
            static_cast<uint8_t>(c.type),
            c.param,
            s.syntax_depth,
            static_cast<uint8_t>(MAX_MOVES - s.move_count),
            last_type,
            0,
            s.block_frequency[static_cast<uint8_t>(BlockType::Draw)],
            s.block_frequency[static_cast<uint8_t>(BlockType::If)],
            s.block_frequency[static_cast<uint8_t>(BlockType::Repeat)],
            s.block_frequency[static_cast<uint8_t>(BlockType::End)],
            transition_prev_to_candidate,
            c.legal ? 1 : 0,
            chosen ? 1 : 0,
            label
        );
        std::fclose(fp);
    }

    void initProgramState(ProgramState& s) {
        const uint16_t next_game = static_cast<uint16_t>(s.game_id + 1);
        s = ProgramState{};
        s.game_id = next_game;
        s.selected_param = 0;
        s.selected_block_idx = static_cast<uint8_t>(BlockType::Draw);
    }

    bool isPlayableBlock(BlockType t) {
        return t == BlockType::Draw || t == BlockType::If || t == BlockType::Repeat || t == BlockType::End;
    }

    bool blockAllowedByDepth(const ProgramState& s, BlockType t) {
        if (t == BlockType::End) {
            return s.syntax_depth > 0;
        }
        if ((t == BlockType::If || t == BlockType::Repeat) && s.syntax_depth >= MAX_REPEAT_DEPTH) {
            return false;
        }
        return true;
    }

    bool isLegalCandidate(const ProgramState& s, BlockType t, uint8_t param) {
        if (!isPlayableBlock(t) || !blockAllowedByDepth(s, t)) {
            return false;
        }
        if (t == BlockType::Draw && param > MAX_PARAM) {
            return false;
        }
        if (t == BlockType::If && param > MAX_PARAM) {
            return false;
        }
        if (t == BlockType::Repeat && (param < 1 || param > MAX_REPEAT)) {
            return false;
        }
        if (t == BlockType::End && param != 0) {
            return false;
        }
        return true;
    }

    void rememberHistory(ProgramState& s, BlockType t) {
        const uint8_t idx = static_cast<uint8_t>(t);
        if (s.history_size > 0) {
            const uint8_t prev = s.history[(s.history_size - 1) % MAX_HISTORY];
            s.transitions[prev][idx]++;
        }
        s.history[s.history_size % MAX_HISTORY] = idx;
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
            if (t == BlockType::If || t == BlockType::Repeat) {
                depth = static_cast<uint8_t>(std::min<uint8_t>(MAX_REPEAT_DEPTH, depth + 1));
            }
        }
    }

    bool addStepToProgram(ProgramState& s, BlockType t, uint8_t param, bool from_ai) {
        if (s.move_count >= MAX_MOVES) {
            return false;
        }
        if (!isLegalCandidate(s, t, param)) {
            return false;
        }
        s.program[s.move_count] = {t, param, from_ai};
        s.move_count++;
        s.selected_line = static_cast<uint8_t>(s.move_count - 1);

        if (t == BlockType::If || t == BlockType::Repeat) {
            s.syntax_depth++;
        } else if (t == BlockType::End && s.syntax_depth > 0) {
            s.syntax_depth--;
        }

        rememberHistory(s, t);
        recalcViewDepths(s);
        ensureSelectionVisible(s);
        return true;
    }

    uint16_t transitionScore(const ProgramState& s, BlockType t) {
        if (s.history_size == 0) {
            return 1;
        }
        const uint8_t prev = s.history[(s.history_size - 1) % MAX_HISTORY];
        const uint8_t next = static_cast<uint8_t>(t);
        return static_cast<uint16_t>(1 + s.transitions[prev][next] * 2 + s.block_frequency[next]);
    }

    uint16_t structureScore(const ProgramState& s, BlockType t, uint8_t param) {
        uint16_t score = 1;
        if (t == BlockType::End) {
            score += static_cast<uint16_t>(s.syntax_depth > 0 ? 6 : 0);
        }
        if (t == BlockType::Repeat) {
            if (s.syntax_depth < MAX_REPEAT_DEPTH - 1) {
                score += 4;
            }
            score += static_cast<uint16_t>(param <= 3 ? 2 : 0);
        }
        if (t == BlockType::If) {
            score += static_cast<uint16_t>(param >= 2 && param <= 5 ? 3 : 1);
        }
        if (t == BlockType::Draw) {
            score += 2;
        }
        return score;
    }

    uint16_t progressScore(const ProgramState& s, BlockType t) {
        const uint8_t remaining = static_cast<uint8_t>(MAX_MOVES - s.move_count);
        uint16_t score = 1;
        if (remaining <= 2 && t == BlockType::End) {
            score += 5;
        }
        if (remaining >= 6 && (t == BlockType::If || t == BlockType::Repeat)) {
            score += 2;
        }
        return score;
    }

    std::array<AICandidate, AI_CANDIDATE_SLOTS> buildCandidates(const ProgramState& s, uint8_t& out_count) {
        std::array<AICandidate, AI_CANDIDATE_SLOTS> cands{};
        out_count = 0;
        const std::array<BlockType, 4> blocks = {
            BlockType::Draw, BlockType::If, BlockType::Repeat, BlockType::End
        };

        for (BlockType t : blocks) {
            uint8_t min_param = 0;
            uint8_t max_param = 0;
            if (t == BlockType::Draw || t == BlockType::If) {
                min_param = 0;
                max_param = MAX_PARAM;
            } else if (t == BlockType::Repeat) {
                min_param = 1;
                max_param = MAX_REPEAT;
            } else {
                min_param = 0;
                max_param = 0;
            }

            for (uint8_t p = min_param; p <= max_param; p++) {
                if (out_count >= cands.size()) {
                    break;
                }
                AICandidate c{};
                c.type = t;
                c.param = p;
                c.legal = isLegalCandidate(s, t, p);
                c.transition_score = transitionScore(s, t);
                c.structure_score = structureScore(s, t, p);
                c.progress_score = progressScore(s, t);
                c.score = c.legal ? static_cast<uint16_t>(c.transition_score + c.structure_score + c.progress_score) : 0;
                cands[out_count++] = c;
            }
        }
        return cands;
    }

    bool chooseBestCandidate(const std::array<AICandidate, AI_CANDIDATE_SLOTS>& cands, uint8_t count, uint8_t& chosen) {
        chosen = 255;
        for (uint8_t i = 0; i < count; i++) {
            const auto& c = cands[i];
            if (!c.legal) {
                continue;
            }
            if (chosen == 255 || c.score > cands[chosen].score) {
                chosen = i;
            }
        }
        return chosen != 255;
    }

    void logCandidates(const ProgramState& s, const std::array<AICandidate, AI_CANDIDATE_SLOTS>& cands, uint8_t count, uint8_t chosen_index) {
        if (!ENABLE_AI_LOG) {
            return;
        }
        const uint8_t last_type = s.history_size == 0 ? 0 : s.history[(s.history_size - 1) % MAX_HISTORY];
        const uint8_t turn_index = static_cast<uint8_t>(s.move_count + 1);

        for (uint8_t i = 0; i < count; i++) {
            const auto& c = cands[i];
            const uint16_t trans = s.history_size == 0 ? 0 : s.transitions[last_type][static_cast<uint8_t>(c.type)];
            const bool chosen = (i == chosen_index);
            const uint8_t label = chosen ? 3 : (c.legal && c.score + 2 >= cands[chosen_index].score ? 2 : 0);
            appendAiLogRow(s, turn_index, c, last_type, trans, chosen, label);
        }
    }

    void performAITurn(ProgramState& s) {
        uint8_t count = 0;
        const auto cands = buildCandidates(s, count);
        uint8_t chosen = 255;
        if (!chooseBestCandidate(cands, count, chosen)) {
            s.turn = TurnState::RunProgram;
            return;
        }
        logCandidates(s, cands, count, chosen);
        if (!addStepToProgram(s, cands[chosen].type, cands[chosen].param, true)) {
            s.turn = TurnState::RunProgram;
            return;
        }
        if (s.move_count >= MAX_MOVES) {
            s.turn = TurnState::RunProgram;
        } else {
            s.turn = TurnState::PlayerTurn;
        }
    }

    void finalizeSyntax(ProgramState& s) {
        while (s.syntax_depth > 0 && s.move_count < MAX_MOVES) {
            addStepToProgram(s, BlockType::End, 0, true);
        }
    }

    bool compileAndRun(ProgramState& s) {
        s.compiled_ok = false;
        s.runtime = RuntimeState{};
        s.runtime.draw_color = colorIndex(s.run_input_color);
        finalizeSyntax(s);

        std::array<int8_t, MAX_MOVES> if_end{};
        std::array<int8_t, MAX_MOVES> repeat_end{};
        std::array<int8_t, MAX_MOVES> end_repeat_start{};
        if_end.fill(-1);
        repeat_end.fill(-1);
        end_repeat_start.fill(-1);

        std::array<uint8_t, MAX_MOVES> stack{};
        uint8_t top = 0;
        for (uint8_t pc = 0; pc < s.move_count; pc++) {
            const BlockType t = s.program[pc].type;
            if (t == BlockType::If || t == BlockType::Repeat) {
                if (top >= MAX_MOVES) {
                    return false;
                }
                stack[top++] = pc;
            } else if (t == BlockType::End) {
                if (top == 0) {
                    return false;
                }
                const uint8_t start = stack[--top];
                if (s.program[start].type == BlockType::If) {
                    if_end[start] = static_cast<int8_t>(pc);
                } else {
                    repeat_end[start] = static_cast<int8_t>(pc);
                    end_repeat_start[pc] = static_cast<int8_t>(start);
                }
            }
        }
        if (top != 0) {
            return false;
        }

        std::array<LoopFrame, MAX_REPEAT_DEPTH> loops{};
        uint8_t loop_top = 0;
        uint8_t pc = 0;

        while (pc < s.move_count) {
            const ProgramStep step = s.program[pc];
            switch (step.type) {
                case BlockType::Draw:
                    s.runtime.draw_color = colorIndex(step.param);
                    pc++;
                    break;
                case BlockType::If: {
                    const bool cond = (colorIndex(s.run_input_color) == colorIndex(step.param));
                    if (!cond) {
                        const int8_t jump = if_end[pc];
                        if (jump < 0) {
                            return false;
                        }
                        pc = static_cast<uint8_t>(jump + 1);
                    } else {
                        pc++;
                    }
                    break;
                }
                case BlockType::Repeat: {
                    if (repeat_end[pc] < 0 || loop_top >= MAX_REPEAT_DEPTH) {
                        return false;
                    }
                    loops[loop_top++] = {pc, step.param};
                    pc++;
                    break;
                }
                case BlockType::End: {
                    const int8_t start = end_repeat_start[pc];
                    if (start < 0) {
                        pc++;
                        break;
                    }
                    if (loop_top == 0) {
                        return false;
                    }
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
                default:
                    return false;
            }
        }

        s.compiled_ok = true;
        return true;
    }

    void drawHeader(const ProgramState& s) {
        const BlockType t = static_cast<BlockType>(s.selected_block_idx);
        const uint8_t shown_param = (t == BlockType::Repeat && s.selected_param == 0) ? 1 : s.selected_param;
        const uint8_t param_max = (t == BlockType::Repeat) ? MAX_REPEAT : MAX_PARAM;
        const uint8_t legal_param = (t == BlockType::End) ? 0 : shown_param;

        Paint_DrawRectangle(4, HEADER_TOP_Y, 236, HEADER_TOP_Y + HEADER_HEIGHT, BLUE, DOT_PIXEL_1X1, DRAW_FILL_EMPTY);

        char line1[96];
        std::snprintf(
            line1,
            sizeof(line1),
            "SEL:%s  P:%u/%u  LINE:%u/%u",
            blockName(t),
            shown_param,
            param_max,
            static_cast<uint8_t>(s.selected_line + 1),
            MAX_MOVES
        );
        Paint_DrawString_EN(8, HEADER_TOP_Y + 4, line1, &Font12, BLACK, WHITE);

        char line2[96];
        std::snprintf(line2, sizeof(line2), "A:add B:type X:param Y:run  depth:%u", s.syntax_depth);
        Paint_DrawString_EN(8, HEADER_TOP_Y + 20, line2, &Font12, BLACK, WHITE);

        if (t == BlockType::Draw || t == BlockType::If) {
            const UWORD dot_color = paintColorByIndex(shown_param);
            Paint_DrawRectangle(206, HEADER_TOP_Y + 10, 230, HEADER_TOP_Y + 34, BLACK, DOT_PIXEL_1X1, DRAW_FILL_EMPTY);
            Paint_DrawCircle(218, HEADER_TOP_Y + 22, 8, dot_color, DOT_PIXEL_1X1, DRAW_FILL_FULL);
        }
    }

    void drawProgramList(const ProgramState& s) {
        
        for (uint8_t row = 0; row < LIST_VISIBLE; row++) {
            const uint8_t idx = static_cast<uint8_t>(s.scroll_top + row);
            const uint16_t y = static_cast<uint16_t>(LIST_TOP_Y + row * LIST_ROW_H);
            if (idx >= MAX_MOVES) {
                break;
            }

            const bool is_selected = (idx == s.selected_line);
            Paint_DrawRectangle(8, y - 1, 232, y + LIST_ROW_H - 3, is_selected ? GREEN : GRAY, DOT_PIXEL_1X1, DRAW_FILL_EMPTY);

            if (idx >= s.move_count) {
                Paint_DrawString_EN(12, y + 2, "[empty]", &Font12, BLACK, WHITE);
                continue;
            }

            const auto& step = s.program[idx];
            const uint16_t indent_x = static_cast<uint16_t>(LIST_START_X + s.view_depths[idx] * INDENT_STEP);
            char line[72];
            std::snprintf(
                line,
                sizeof(line),
                "%s(%u)%s",
                blockName(step.type),
                step.param,
                step.from_ai ? " [AI]" : ""
            );
            Paint_DrawString_EN(indent_x, y + 2, line, &Font12, BLACK, WHITE);
        }
    }

    void drawRunPreview(const ProgramState& s) {
        Paint_Clear(WHITE);
        Paint_DrawString_EN(4, 4, "Run Preview", &Font16, BLACK, WHITE);
        const uint16_t origin_x = 20;
        const uint16_t origin_y = 40;
        const uint16_t cell = 20;

        for (uint8_t y = 0; y < BOARD_LIMIT; y++) {
            for (uint8_t x = 0; x < BOARD_LIMIT; x++) {
                const uint16_t x0 = static_cast<uint16_t>(origin_x + x * cell);
                const uint16_t y0 = static_cast<uint16_t>(origin_y + y * cell);
                Paint_DrawRectangle(x0, y0, x0 + cell - 2, y0 + cell - 2, GRAY, DOT_PIXEL_1X1, DRAW_FILL_EMPTY);
            }
        }

        const uint16_t px = static_cast<uint16_t>(origin_x + s.runtime.x * cell + (cell / 2));
        const uint16_t py = static_cast<uint16_t>(origin_y + s.runtime.y * cell + (cell / 2));
        Paint_DrawCircle(px, py, 6, paintColorByIndex(s.runtime.draw_color), DOT_PIXEL_1X1, DRAW_FILL_FULL);
        Paint_DrawCircle(202, 22, 8, paintColorByIndex(s.runtime.draw_color), DOT_PIXEL_1X1, DRAW_FILL_FULL);

        char line[64];
        std::snprintf(line, sizeof(line), "x:%u y:%u color:%u", s.runtime.x, s.runtime.y, s.runtime.draw_color);
        Paint_DrawString_EN(4, 220, line, &Font12, BLACK, WHITE);
    }

    void drawMainScene(const ProgramState& s) {
        if (s.turn == TurnState::Finished) {
            drawRunPreview(s);
            return;
        }
        Paint_Clear(WHITE);
        drawHeader(s);
        drawProgramList(s);
    }

    void cycleBlockType(ProgramState& s) {
        s.selected_block_idx++;
        if (s.selected_block_idx > static_cast<uint8_t>(BlockType::End)) {
            s.selected_block_idx = static_cast<uint8_t>(BlockType::Draw);
        }
    }

    void cycleParam(ProgramState& s) {
        const BlockType t = static_cast<BlockType>(s.selected_block_idx);
        if (t == BlockType::Repeat) {
            const uint8_t base = s.selected_param == 0 ? 1 : s.selected_param;
            s.selected_param = static_cast<uint8_t>((base % MAX_REPEAT) + 1); // 1..MAX_REPEAT
        } else {
            s.selected_param = static_cast<uint8_t>((s.selected_param + 1) % (MAX_PARAM + 1));
        }
    }

    bool handlePlayerInput(ProgramState& s) {
        if (hardware::keyPressed(keyB)) {
            cycleBlockType(s);
            sleep_ms(140);
            return true;
        }
        if (hardware::keyPressed(keyX)) {
            cycleParam(s);
            sleep_ms(140);
            return true;
        }
        if (hardware::keyPressed(keyA)) {
            const BlockType t = static_cast<BlockType>(s.selected_block_idx);
            uint8_t param = s.selected_param;
            if (t == BlockType::End) {
                param = 0;
            } else if (t == BlockType::Repeat) {
                const uint8_t base = param == 0 ? 1 : param;
                param = static_cast<uint8_t>(((base - 1) % MAX_REPEAT) + 1);
            }
            if (addStepToProgram(s, t, param, false)) {
                s.turn = TurnState::AITurn;
            }
            sleep_ms(160);
            return true;
        }
        if (hardware::keyPressed(keyY)) {
            s.turn = TurnState::SelectInputColor;
            sleep_ms(180);
            return true;
        }
        return false;
    }

    bool handleColorSelectInput(ProgramState& s) {
        if (hardware::keyPressed(keyX)) {
            s.run_input_color = static_cast<uint8_t>((s.run_input_color + 1) % COLOR_COUNT);
            sleep_ms(140);
            return true;
        }
        if (hardware::keyPressed(keyY)) {
            s.turn = TurnState::RunProgram;
            sleep_ms(180);
            return true;
        }
        if (hardware::keyPressed(keyA)) {
            s.turn = TurnState::PlayerTurn;
            sleep_ms(180);
            return true;
        }
        return false;
    }

    void drawColorSelectScene(const ProgramState& s) {
        Paint_Clear(WHITE);
        Paint_DrawString_EN(4, 4, "Select Input Color", &Font16, BLACK, WHITE);
        Paint_DrawString_EN(8, 34, "X:next color  Y:start  A:back", &Font12, BLACK, WHITE);
        Paint_DrawString_EN(8, 56, "IF compares this color", &Font12, BLACK, WHITE);

        const UWORD c = paintColorByIndex(s.run_input_color);
        Paint_DrawRectangle(84, 86, 156, 158, BLACK, DOT_PIXEL_1X1, DRAW_FILL_EMPTY);
        Paint_DrawCircle(120, 122, 24, c, DOT_PIXEL_1X1, DRAW_FILL_FULL);

        char line[64];
        std::snprintf(line, sizeof(line), "selected color idx: %u", s.run_input_color);
        Paint_DrawString_EN(8, 176, line, &Font12, BLACK, WHITE);
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
    writeAiLogHeader();

    ProgramState state;
    initProgramState(state);

    bool running = true;
    bool needs_redraw = true;
    while (running) {
        if (state.turn == TurnState::PlayerTurn) {
            const bool changed = handlePlayerInput(state);
            if (state.move_count >= MAX_MOVES && state.turn == TurnState::PlayerTurn) {
                state.turn = TurnState::SelectInputColor;
                needs_redraw = true;
            }

            if (state.turn != TurnState::PlayerTurn) {
                needs_redraw = true;
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

        if (state.turn == TurnState::SelectInputColor) {
            const bool changed = handleColorSelectInput(state);
            if (state.turn != TurnState::SelectInputColor) {
                needs_redraw = true;
                sleep_ms(20);
                continue;
            }
            if (changed || needs_redraw) {
                drawColorSelectScene(state);
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
            drawMainScene(state);
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
