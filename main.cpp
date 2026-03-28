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

#include "pico/multicore.h"
#include "pico/util/queue.h"

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
    constexpr uint8_t MAX_NEST_DEPTH = 6;

    constexpr uint8_t LIST_VISIBLE = 9;
    constexpr uint16_t HEADER_TOP_Y = 2;
    constexpr uint16_t HEADER_HEIGHT = 42;
    constexpr uint16_t LIST_TOP_Y = HEADER_TOP_Y + HEADER_HEIGHT + 4;
    constexpr uint16_t LIST_START_X = 16;
    constexpr uint16_t LIST_ROW_H = 20;
    constexpr uint16_t INDENT_STEP = 16;

    constexpr uint8_t COLOR_COUNT = 8;
    constexpr uint8_t COLOR_PARAM_MIN = 1;
    constexpr uint8_t COLOR_PARAM_MAX = COLOR_COUNT;   // 1..8
    constexpr uint8_t MOVE_PARAM_MIN = 1;
    constexpr uint8_t MOVE_PARAM_MAX = 19;             // cell index range 1..19
    constexpr uint8_t MAX_REPEAT = 7;                  // 1..7
    constexpr uint8_t RESULT_COLS = 5;
    constexpr uint8_t RESULT_ROWS = 4;
    constexpr size_t AI_CANDIDATE_SLOTS = MOVE_PARAM_MAX - MOVE_PARAM_MIN + 1 +
                                          (COLOR_PARAM_MAX - COLOR_PARAM_MIN + 1) * 2 +
                                          MAX_REPEAT + 1;

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

    enum class ActorType : uint8_t {
        Player = 0,
        AI = 1,
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

    constexpr uint8_t colorIndexFromParam(uint8_t param) {
        if (param < COLOR_PARAM_MIN) {
            return 0;
        }
        return static_cast<uint8_t>((param - COLOR_PARAM_MIN) % COLOR_COUNT);
    }

    constexpr UWORD paintColorByParam(uint8_t param) {
        return kPaintColors[colorIndexFromParam(param)];
    }

    constexpr uint8_t minParamForBlock(BlockType t) {
        switch (t) {
            case BlockType::Move:
                return MOVE_PARAM_MIN;
            case BlockType::Draw:
            case BlockType::If:
                return COLOR_PARAM_MIN;
            case BlockType::Repeat:
                return 1;
            case BlockType::End:
            case BlockType::None:
            default:
                return 0;
        }
    }

    constexpr uint8_t maxParamForBlock(BlockType t) {
        switch (t) {
            case BlockType::Move:
                return MOVE_PARAM_MAX;
            case BlockType::Draw:
            case BlockType::If:
                return COLOR_PARAM_MAX;
            case BlockType::Repeat:
                return MAX_REPEAT;
            case BlockType::End:
            case BlockType::None:
            default:
                return 0;
        }
    }

    constexpr bool blockHasParam(BlockType t) {
        return t == BlockType::Move || t == BlockType::Draw || t == BlockType::If || t == BlockType::Repeat;
    }

    constexpr uint8_t FIRST_PLAYABLE_BLOCK = static_cast<uint8_t>(BlockType::Move);
    constexpr uint8_t LAST_PLAYABLE_BLOCK = static_cast<uint8_t>(BlockType::End);
    constexpr uint8_t PLAYABLE_BLOCK_COUNT = static_cast<uint8_t>(LAST_PLAYABLE_BLOCK - FIRST_PLAYABLE_BLOCK + 1);

    constexpr uint8_t nextPlayableBlockIndex(uint8_t idx) {
        return idx >= LAST_PLAYABLE_BLOCK ? FIRST_PLAYABLE_BLOCK : static_cast<uint8_t>(idx + 1);
    }

    struct ProgramStep {
        BlockType type = BlockType::None;
        uint8_t param = 0;
        bool from_ai = false;
    };

    struct RuntimeState {
        uint8_t x = 0;
        uint8_t y = 0;
        uint8_t anchor_cell = 0;
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
        float suitability = -1.0f;
        uint8_t feedback_penalty = 0;
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
        uint8_t selected_block_idx = static_cast<uint8_t>(BlockType::Move);
        uint8_t selected_param = MOVE_PARAM_MIN;
        uint8_t syntax_depth = 0;
        uint8_t run_input_color = COLOR_PARAM_MIN;

        TurnState turn = TurnState::PlayerTurn;
        bool compiled_ok = false;
        RuntimeState runtime{};
    };

    enum class YdfWorkerCommand : uint8_t {
        Predict = 1,
        Shutdown = 2,
    };

    struct YdfWorkerRequest {
        YdfWorkerCommand command = YdfWorkerCommand::Predict;
        blockode::ydf::CandidateFeatures features{};
    };

    struct YdfWorkerResponse {
        blockode::ydf::Prediction prediction{};
    };

    queue_t g_ydf_request_queue{};
    queue_t g_ydf_response_queue{};
    bool g_ydf_worker_running = false;

    void normalizeSelectedBlockType(ProgramState& s);

    void initProgramState(ProgramState& s) {
        const uint16_t next_game = static_cast<uint16_t>(s.game_id + 1);
        s = ProgramState{};
        s.game_id = next_game;
        s.selected_block_idx = static_cast<uint8_t>(BlockType::Move);
        s.selected_param = minParamForBlock(BlockType::Move);
        s.run_input_color = COLOR_PARAM_MIN;
        normalizeSelectedBlockType(s);
    }

    bool isPlayableBlock(BlockType t) {
        return t == BlockType::Move || t == BlockType::Draw || t == BlockType::If || t == BlockType::Repeat || t == BlockType::End;
    }

    bool blockAllowedByDepth(const ProgramState& s, BlockType t) {
        if (t == BlockType::End) {
            return s.syntax_depth > 0;
        }
        if ((t == BlockType::Move || t == BlockType::If || t == BlockType::Repeat) && s.syntax_depth >= MAX_NEST_DEPTH) {
            return false;
        }
        if (s.syntax_depth > 0 && t == BlockType::Move) {
            return false;
        }
        return true;
    }

    bool isLegalCandidate(const ProgramState& s, BlockType t, uint8_t param) {
        if (!isPlayableBlock(t) || !blockAllowedByDepth(s, t)) {
            return false;
        }
        if (s.move_count > 0) {
            const BlockType last_type = s.program[s.move_count - 1].type;
            if ((last_type == BlockType::Repeat || last_type == BlockType::If) && t == last_type) {
                return false;
            }
            if (last_type == BlockType::Move && t != BlockType::Draw) {
                return false;
            }
        }
        if (t == BlockType::Move && (param < MOVE_PARAM_MIN || param > MOVE_PARAM_MAX)) {
            return false;
        }
        if ((t == BlockType::Draw || t == BlockType::If) && (param < COLOR_PARAM_MIN || param > COLOR_PARAM_MAX)) {
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
            if (t == BlockType::Move || t == BlockType::If || t == BlockType::Repeat) {
                depth = static_cast<uint8_t>(std::min<uint8_t>(MAX_NEST_DEPTH, depth + 1));
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

        if (t == BlockType::Move || t == BlockType::If || t == BlockType::Repeat) {
            s.syntax_depth++;
        } else if (t == BlockType::End && s.syntax_depth > 0) {
            s.syntax_depth--;
        }

        rememberHistory(s, t);
        recalcViewDepths(s);
        ensureSelectionVisible(s);
        normalizeSelectedBlockType(s);
        return true;
    }

    blockode::ydf::CandidateFeatures makeYdfFeatures(const ProgramState& s, const AICandidate& c, ActorType actor) {
        blockode::ydf::CandidateFeatures f{};
        f.game_id = s.game_id;
        f.turn = static_cast<uint32_t>(s.move_count + 1);
        f.candidate_type = static_cast<uint32_t>(c.type);
        f.candidate_param = c.param;
        f.depth = s.syntax_depth;
        f.remaining_moves = static_cast<uint32_t>(MAX_MOVES - s.move_count);
        f.last_type = s.history_size == 0 ? 0 : s.history[(s.history_size - 1) % MAX_HISTORY];
        f.freq_move = s.block_frequency[static_cast<uint8_t>(BlockType::Move)];
        f.freq_draw = s.block_frequency[static_cast<uint8_t>(BlockType::Draw)];
        f.freq_if = s.block_frequency[static_cast<uint8_t>(BlockType::If)];
        f.freq_repeat = s.block_frequency[static_cast<uint8_t>(BlockType::Repeat)];
        f.freq_end = s.block_frequency[static_cast<uint8_t>(BlockType::End)];
        f.transition_prev_to_candidate =
            s.history_size == 0 ? 0 : s.transitions[f.last_type][static_cast<uint8_t>(c.type)];
        f.legal = c.legal ? 1u : 0u;
        f.actor = actor == ActorType::AI ? 1u : 0u;
        f.feedback_penalty = c.feedback_penalty;
        return f;
    }

    void ydfWorkerCore1Main() {
        while (true) {
            YdfWorkerRequest req{};
            queue_remove_blocking(&g_ydf_request_queue, &req);
            if (req.command == YdfWorkerCommand::Shutdown) {
                break;
            }
            YdfWorkerResponse res{};
            res.prediction = blockode::ydf::Model::Predict(req.features);
            queue_add_blocking(&g_ydf_response_queue, &res);
        }
    }

    void startYdfWorker() {
        if (g_ydf_worker_running) {
            return;
        }
        // RP2350 has a shallower intercore FIFO than RP2040; use SDK queue_t for transport.
        queue_init(&g_ydf_request_queue, sizeof(YdfWorkerRequest), 1);
        queue_init(&g_ydf_response_queue, sizeof(YdfWorkerResponse), 1);
        multicore_reset_core1();
        multicore_launch_core1(ydfWorkerCore1Main);
        g_ydf_worker_running = true;
    }

    void stopYdfWorker() {
        if (!g_ydf_worker_running) {
            return;
        }
        constexpr YdfWorkerRequest shutdown_req{YdfWorkerCommand::Shutdown, {}};
        queue_add_blocking(&g_ydf_request_queue, &shutdown_req);
        multicore_reset_core1();
        queue_free(&g_ydf_request_queue);
        queue_free(&g_ydf_response_queue);
        g_ydf_worker_running = false;
    }

    float ydfPredictSuitability(const ProgramState& s, const AICandidate& c, ActorType actor) {
        const auto features = makeYdfFeatures(s, c, actor);
        if (!g_ydf_worker_running) {
            const auto pred = blockode::ydf::Model::Predict(features);
            if (!pred.ok) {
                return -1.0f;
            }
            return pred.suitability_score;
        }

        const YdfWorkerRequest req{YdfWorkerCommand::Predict, features};
        queue_add_blocking(&g_ydf_request_queue, &req);

        YdfWorkerResponse res{};
        queue_remove_blocking(&g_ydf_response_queue, &res);
        if (!res.prediction.ok) {
            return -1.0f;
        }
        return res.prediction.suitability_score;
    }

    std::array<AICandidate, AI_CANDIDATE_SLOTS> buildCandidates(const ProgramState& s, uint8_t& out_count) {
        std::array<AICandidate, AI_CANDIDATE_SLOTS> cands{};
        out_count = 0;
        constexpr std::array<BlockType, 5> blocks = {
            BlockType::Move, BlockType::Draw, BlockType::If, BlockType::Repeat, BlockType::End
        };

        for (BlockType t : blocks) {
            uint8_t min_param = 0;
            uint8_t max_param = 0;
            if (t == BlockType::Move) {
                min_param = MOVE_PARAM_MIN;
                max_param = MOVE_PARAM_MAX;
            } else if (t == BlockType::Draw || t == BlockType::If) {
                min_param = COLOR_PARAM_MIN;
                max_param = COLOR_PARAM_MAX;
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
                c.suitability = c.legal ? ydfPredictSuitability(s, c, ActorType::AI) : -1.0f;
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
            if (chosen == 255 || c.suitability > cands[chosen].suitability) {
                chosen = i;
            }
        }
        return chosen != 255;
    }

    uint8_t tailTypeStreak(const ProgramState& s, BlockType t) {
        uint8_t streak = 0;
        for (int i = static_cast<int>(s.move_count) - 1; i >= 0; --i) {
            if (s.program[static_cast<size_t>(i)].type != t) {
                break;
            }
            streak++;
        }
        return streak;
    }

    bool violatesAIMoveRule(const ProgramState& s, const AICandidate& c) {
        if (c.type != BlockType::Draw || s.move_count == 0) {
            return false;
        }
        // Rule feedback example: If the nearest open block is Move, only Draw is allowed directly under it.
        // Also penalize repeated same Draw color under same depth to encourage alternative proposals.
        for (int i = static_cast<int>(s.move_count) - 1; i >= 0; --i) {
            const BlockType t = s.program[static_cast<size_t>(i)].type;
            if (t == BlockType::End) {
                continue;
            }
            if (t == BlockType::Move) {
                return false;
            }
            break;
        }
        if (s.move_count > 0) {
            const auto& last = s.program[s.move_count - 1];
            if (last.type == BlockType::Draw && last.param == c.param) {
                return true;
            }
        }
        return false;
    }

    void applyRuleFeedbackAndRescore(const ProgramState& s, std::array<AICandidate, AI_CANDIDATE_SLOTS>& cands, uint8_t count, ActorType actor) {
        const uint8_t draw_streak = tailTypeStreak(s, BlockType::Draw);
        for (uint8_t i = 0; i < count; i++) {
            auto& c = cands[i];
            if (!c.legal) {
                continue;
            }
            if (violatesAIMoveRule(s, c)) {
                c.feedback_penalty = static_cast<uint8_t>(std::min<uint16_t>(255, c.feedback_penalty + 1));
            }
            c.suitability = ydfPredictSuitability(s, c, actor);

            if (c.type == BlockType::Draw) {
                // Prevent a Draw-only loop unless Draw is structurally required.
                c.suitability -= 0.12f * static_cast<float>(draw_streak);
                if (s.move_count > 0 && s.program[s.move_count - 1].type == BlockType::Draw &&
                    s.program[s.move_count - 1].param == c.param) {
                    c.suitability -= 0.10f;
                }
            } else {
                const uint8_t type_freq = s.block_frequency[static_cast<uint8_t>(c.type)];
                if (type_freq == 0) {
                    c.suitability += 0.08f;
                } else if (type_freq == 1) {
                    c.suitability += 0.04f;
                }
                if (c.type == BlockType::End && s.syntax_depth > 0) {
                    c.suitability += 0.10f;
                }
            }
        }
    }

    bool chooseBestNonDrawCandidate(const std::array<AICandidate, AI_CANDIDATE_SLOTS>& cands, uint8_t count, uint8_t& chosen) {
        chosen = 255;
        for (uint8_t i = 0; i < count; i++) {
            const auto& c = cands[i];
            if (!c.legal || c.type == BlockType::Draw) {
                continue;
            }
            if (chosen == 255 || c.suitability > cands[chosen].suitability) {
                chosen = i;
            }
        }
        return chosen != 255;
    }

    void performAITurn(ProgramState& s) {
        uint8_t count = 0;
        auto cands = buildCandidates(s, count);
        applyRuleFeedbackAndRescore(s, cands, count, ActorType::AI);
        uint8_t chosen = 255;
        if (!chooseBestCandidate(cands, count, chosen)) {
            s.turn = TurnState::RunProgram;
            return;
        }
        if (cands[chosen].feedback_penalty > 0) {
            cands[chosen].feedback_penalty = static_cast<uint8_t>(std::min<uint16_t>(255, cands[chosen].feedback_penalty + 1));
            cands[chosen].suitability = ydfPredictSuitability(s, cands[chosen], ActorType::AI);
            uint8_t retry = 255;
            if (chooseBestCandidate(cands, count, retry) && retry != chosen) {
                chosen = retry;
            }
        }
        if (cands[chosen].type == BlockType::Draw) {
            uint8_t non_draw = 255;
            if (chooseBestNonDrawCandidate(cands, count, non_draw)) {
                const uint8_t draw_streak = tailTypeStreak(s, BlockType::Draw);
                const float gap = cands[chosen].suitability - cands[non_draw].suitability;
                if (draw_streak >= 2 || gap <= 0.08f) {
                    chosen = non_draw;
                }
            }
        }
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

    void drawHeader(const ProgramState& s) {
        const auto t = static_cast<BlockType>(s.selected_block_idx);
        const uint8_t shown_param = blockHasParam(t) ? std::max<uint8_t>(minParamForBlock(t), s.selected_param) : 0;
        const uint8_t param_max = maxParamForBlock(t);

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
        std::snprintf(
            line2,
            sizeof(line2),
            "A:add B:type X:param Y:run depth:%u ydf:%s(%s)",
            s.syntax_depth,
            blockode::ydf::kModelAvailable ? "on" : "off",
            blockode::ydf::kBackendName
        );
        Paint_DrawString_EN(8, HEADER_TOP_Y + 20, line2, &Font12, BLACK, WHITE);

        if (t == BlockType::Draw || t == BlockType::If) {
            const UWORD dot_color = paintColorByParam(shown_param);
            Paint_DrawRectangle(206, HEADER_TOP_Y + 10, 230, HEADER_TOP_Y + 34, BLACK, DOT_PIXEL_1X1, DRAW_FILL_EMPTY);
            Paint_DrawCircle(218, HEADER_TOP_Y + 22, 8, dot_color, DOT_PIXEL_1X1, DRAW_FILL_FULL);
        }
    }

    void drawProgramList(const ProgramState& s) {
        for (uint8_t row = 0; row < LIST_VISIBLE; row++) {
            const auto idx = static_cast<uint8_t>(s.scroll_top + row);
            const auto y = static_cast<uint16_t>(LIST_TOP_Y + row * LIST_ROW_H);
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
            const auto indent_x = static_cast<uint16_t>(LIST_START_X + s.view_depths[idx] * INDENT_STEP);
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

            if (step.type == BlockType::Draw || step.type == BlockType::If) {
                const UWORD dot_color = paintColorByParam(step.param);
                constexpr uint16_t swatch_left = 210;
                const auto swatch_top = static_cast<uint16_t>(y + 2);
                constexpr uint16_t dot_x = 220;
                const auto dot_y = static_cast<uint16_t>(y + 8);
                Paint_DrawRectangle(swatch_left, swatch_top, 230, static_cast<uint16_t>(y + 14), BLACK, DOT_PIXEL_1X1, DRAW_FILL_EMPTY);
                Paint_DrawCircle(dot_x, dot_y, 4, dot_color, DOT_PIXEL_1X1, DRAW_FILL_FULL);
            }
        }
    }

    // --- Run ---
    void finalizeSyntax(ProgramState& s) {
        while (s.syntax_depth > 0 && s.move_count < MAX_MOVES) {
            addStepToProgram(s, BlockType::End, 0, true);
        }
    }

    bool compileAndRun(ProgramState& s) {
        s.compiled_ok = false;
        s.runtime = RuntimeState{};
        s.runtime.draw_color = colorIndexFromParam(s.run_input_color);
        finalizeSyntax(s);

        std::array<int8_t, MAX_MOVES> block_end{};
        std::array<int8_t, MAX_MOVES> end_start{};
        block_end.fill(-1);
        end_start.fill(-1);

        std::array<uint8_t, MAX_MOVES> stack{};
        uint8_t top = 0;
        for (uint8_t pc = 0; pc < s.move_count; pc++) {
            const auto t = s.program[pc].type;
            if (t == BlockType::Move || t == BlockType::If || t == BlockType::Repeat) {
                if (top >= MAX_MOVES) {
                    return false;
                }
                stack[top++] = pc;
            } else if (t == BlockType::End) {
                if (top == 0) {
                    return false;
                }
                const uint8_t start = stack[--top];
                block_end[start] = static_cast<int8_t>(pc);
                end_start[pc] = static_cast<int8_t>(start);
            }
        }
        if (top != 0) {
            return false;
        }

        std::array<LoopFrame, MAX_NEST_DEPTH> loops{};
        uint8_t loop_top = 0;
        uint8_t pc = 0;
        std::array<uint8_t, MAX_NEST_DEPTH> move_anchor_stack{};
        uint8_t move_anchor_top = 0;

        while (pc < s.move_count) {
            const ProgramStep step = s.program[pc];
            switch (step.type) {
                case BlockType::Move: {
                    if (block_end[pc] < 0 || move_anchor_top >= MAX_NEST_DEPTH) {
                        return false;
                    }
                    move_anchor_stack[move_anchor_top++] = static_cast<uint8_t>(step.param - MOVE_PARAM_MIN);
                    s.runtime.anchor_cell = static_cast<uint8_t>(step.param - MOVE_PARAM_MIN);
                    s.runtime.x = static_cast<uint8_t>(s.runtime.anchor_cell % RESULT_COLS);
                    s.runtime.y = static_cast<uint8_t>(s.runtime.anchor_cell / RESULT_COLS);
                    pc++;
                    break;
                }
                case BlockType::Draw:
                    s.runtime.draw_color = colorIndexFromParam(step.param);
                    if (move_anchor_top > 0) {
                        s.runtime.anchor_cell = move_anchor_stack[move_anchor_top - 1];
                    } else {
                        s.runtime.anchor_cell = 0;
                    }
                    s.runtime.x = static_cast<uint8_t>(s.runtime.anchor_cell % RESULT_COLS);
                    s.runtime.y = static_cast<uint8_t>(s.runtime.anchor_cell / RESULT_COLS);
                    pc++;
                    break;
                case BlockType::If: {
                    if (block_end[pc] < 0) {
                        return false;
                    }
                    const bool cond = (colorIndexFromParam(s.run_input_color) == colorIndexFromParam(step.param));
                    if (!cond) {
                        pc = static_cast<uint8_t>(block_end[pc] + 1);
                    } else {
                        pc++;
                    }
                    break;
                }
                case BlockType::Repeat: {
                    if (block_end[pc] < 0 || loop_top >= MAX_NEST_DEPTH) {
                        return false;
                    }
                    loops[loop_top++] = {pc, step.param};
                    pc++;
                    break;
                }
                case BlockType::End: {
                    const int8_t start = end_start[pc];
                    if (start < 0) {
                        return false;
                    }
                    const auto open_type = s.program[static_cast<uint8_t>(start)].type;
                    if (open_type == BlockType::Repeat) {
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
                    if (open_type == BlockType::Move) {
                        if (move_anchor_top == 0) {
                            return false;
                        }
                        move_anchor_top--;
                        if (move_anchor_top > 0) {
                            s.runtime.anchor_cell = move_anchor_stack[move_anchor_top - 1];
                        } else {
                            s.runtime.anchor_cell = 0;
                        }
                        s.runtime.x = static_cast<uint8_t>(s.runtime.anchor_cell % RESULT_COLS);
                        s.runtime.y = static_cast<uint8_t>(s.runtime.anchor_cell / RESULT_COLS);
                        pc++;
                        break;
                    }
                    if (open_type == BlockType::If) {
                        pc++;
                        break;
                    }
                    return false;
                }
                default:
                    return false;
            }
        }

        s.compiled_ok = true;
        return true;
    }

    void drawRunPreview(const ProgramState& s) {
        Paint_Clear(WHITE);
        Paint_DrawString_EN(4, 4, "Run Preview", &Font16, BLACK, WHITE);
        constexpr uint16_t origin_x = 20;
        constexpr uint16_t origin_y = 40;
        constexpr uint16_t cell = 20;

        for (uint8_t y = 0; y < RESULT_ROWS; y++) {
            for (uint8_t x = 0; x < RESULT_COLS; x++) {
                const auto x0 = static_cast<uint16_t>(origin_x + x * cell);
                const auto y0 = static_cast<uint16_t>(origin_y + y * cell);
                Paint_DrawRectangle(x0, y0, x0 + cell - 2, y0 + cell - 2, GRAY, DOT_PIXEL_1X1, DRAW_FILL_EMPTY);
                const uint8_t idx = static_cast<uint8_t>(y * RESULT_COLS + x);
                char id[4];
                std::snprintf(id, sizeof(id), "%u", idx);
                Paint_DrawString_EN(
                    static_cast<UWORD>(x0 + 3),
                    static_cast<UWORD>(y0 + 3),
                    id,
                    &Font12,
                    BLACK,
                    WHITE
                );
            }
        }

        const auto px = static_cast<uint16_t>(origin_x + s.runtime.x * cell + (cell / 2));
        const auto py = static_cast<uint16_t>(origin_y + s.runtime.y * cell + (cell / 2));
        Paint_DrawCircle(px, py, 6, kPaintColors[s.runtime.draw_color], DOT_PIXEL_1X1, DRAW_FILL_FULL);
        Paint_DrawCircle(202, 22, 8, kPaintColors[s.runtime.draw_color], DOT_PIXEL_1X1, DRAW_FILL_FULL);

        char line[64];
        std::snprintf(line, sizeof(line), "cell:%u x:%u y:%u color:%u", s.runtime.anchor_cell, s.runtime.x, s.runtime.y, s.runtime.draw_color);
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
        uint8_t next_idx = s.selected_block_idx;
        bool found = false;
        for (uint8_t i = 0; i < PLAYABLE_BLOCK_COUNT; i++) {
            next_idx = nextPlayableBlockIndex(next_idx);
            const auto t = static_cast<BlockType>(next_idx);
            const uint8_t candidate_param = blockHasParam(t) ? minParamForBlock(t) : 0;
            if (!isLegalCandidate(s, t, candidate_param)) {
                continue;
            }
            found = true;
            break;
        }
        if (!found) {
            return;
        }

        s.selected_block_idx = next_idx;
        const auto selected_t = static_cast<BlockType>(s.selected_block_idx);
        const uint8_t min_param = minParamForBlock(selected_t);
        const uint8_t max_param = maxParamForBlock(selected_t);
        if (blockHasParam(selected_t) && (s.selected_param < min_param || s.selected_param > max_param)) {
            s.selected_param = min_param;
        }
        if (!blockHasParam(selected_t)) {
            s.selected_param = 0;
        }
    }

    void normalizeSelectedBlockType(ProgramState& s) {
        const auto current = static_cast<BlockType>(s.selected_block_idx);
        uint8_t current_param = 0;
        if (blockHasParam(current)) {
            const uint8_t min_param = minParamForBlock(current);
            const uint8_t max_param = maxParamForBlock(current);
            current_param = s.selected_param < min_param ? min_param : s.selected_param;
            if (current_param > max_param) {
                current_param = min_param;
            }
        }
        if (isLegalCandidate(s, current, current_param)) {
            s.selected_param = blockHasParam(current) ? current_param : 0;
            return;
        }

        for (uint8_t offset = 0; offset < PLAYABLE_BLOCK_COUNT; offset++) {
            const auto idx = static_cast<uint8_t>(FIRST_PLAYABLE_BLOCK + offset);
            const auto t = static_cast<BlockType>(idx);
            const uint8_t candidate_param = blockHasParam(t) ? minParamForBlock(t) : 0;
            if (!isLegalCandidate(s, t, candidate_param)) {
                continue;
            }
            s.selected_block_idx = idx;
            s.selected_param = candidate_param;
            return;
        }
    }

    void cycleParam(ProgramState& s) {
        const auto t = static_cast<BlockType>(s.selected_block_idx);
        if (!blockHasParam(t)) {
            return;
        }
        const uint8_t min_param = minParamForBlock(t);
        const uint8_t max_param = maxParamForBlock(t);
        const uint8_t base = s.selected_param < min_param ? min_param : s.selected_param;
        s.selected_param = base >= max_param ? min_param : static_cast<uint8_t>(base + 1);
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
            const auto t = static_cast<BlockType>(s.selected_block_idx);
            uint8_t param = blockHasParam(t) ? s.selected_param : 0;
            if (blockHasParam(t)) {
                const uint8_t min_param = minParamForBlock(t);
                const uint8_t max_param = maxParamForBlock(t);
                if (param < min_param || param > max_param) {
                    param = min_param;
                }
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
            s.run_input_color = s.run_input_color >= COLOR_PARAM_MAX ? COLOR_PARAM_MIN : static_cast<uint8_t>(s.run_input_color + 1);
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

        const UWORD c = paintColorByParam(s.run_input_color);
        Paint_DrawRectangle(84, 86, 156, 158, BLACK, DOT_PIXEL_1X1, DRAW_FILL_EMPTY);
        Paint_DrawCircle(120, 122, 24, c, DOT_PIXEL_1X1, DRAW_FILL_FULL);

        char line[64];
        std::snprintf(line, sizeof(line), "selected color param: %u", s.run_input_color);
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
    startYdfWorker();

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
    stopYdfWorker();
    DEV_Module_Exit();
    return 0;
}

int main() {
    stdio_init_all();
    return LCD();
}
