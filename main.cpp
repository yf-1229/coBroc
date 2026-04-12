#include "main.h"

#include <random>

extern "C" {
#include "Infrared.h"
#include "LCD_1in3.h"
#include "DEV_Config.h"
#include "lvgl.h"
#include <stdio.h>
}

#include <array>
#include <algorithm>
#include <cstdio>
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

    constexpr uint16_t LCD_WIDTH = 240;
    constexpr uint16_t LCD_HEIGHT = 240;

    constexpr uint8_t COLOR_COUNT = 8;
    constexpr uint8_t COLOR_PARAM_MIN = 1;
    constexpr uint8_t COLOR_PARAM_MAX = COLOR_COUNT;   // 1..8
    constexpr uint8_t MOVE_PARAM_MIN = 1;
    constexpr uint8_t MOVE_PARAM_MAX = 19;             // cell index range 1..19
    constexpr uint8_t MAX_REPEAT = 7;                  // 1..7
    constexpr uint16_t RESULT_WIDTH = LCD_WIDTH;
    constexpr uint16_t RESULT_HEIGHT = LCD_HEIGHT;
    constexpr uint8_t RESULT_RADIUS = 6;
    constexpr uint8_t RESULT_MIN_COORD = RESULT_RADIUS;
    constexpr uint8_t RESULT_MAX_X = static_cast<uint8_t>(RESULT_WIDTH - RESULT_RADIUS - 1);
    constexpr uint8_t RESULT_MAX_Y = static_cast<uint8_t>(RESULT_HEIGHT - RESULT_RADIUS - 1);
    constexpr uint16_t MAX_DRAW_EVENTS = 128;
    constexpr uint8_t RANDOM_STEP_MIN = 0;
    constexpr uint8_t RANDOM_STEP_MAX = 2;
    constexpr int8_t RANDOM_STEP_OFFSET = 1;
    constexpr int16_t RANDOM_STEP_PIXELS = static_cast<int16_t>(RESULT_RADIUS + 2);
    constexpr size_t AI_CANDIDATE_SLOTS = MOVE_PARAM_MAX - MOVE_PARAM_MIN + 1 +
                                          (COLOR_PARAM_MAX - COLOR_PARAM_MIN + 1) * 2 +
                                          MAX_REPEAT + 2;
    constexpr uint8_t AI_MAX_PREDICT_CANDIDATES = 20;
    constexpr uint8_t YDF_WORKER_BATCH_CAPACITY = AI_MAX_PREDICT_CANDIDATES;

    std::mt19937 g_rng(std::random_device{}());
    std::uniform_int_distribution<uint8_t> g_dist_result_x(RESULT_MIN_COORD, RESULT_MAX_X);
    std::uniform_int_distribution<uint8_t> g_dist_result_y(RESULT_MIN_COORD, RESULT_MAX_Y);
    std::uniform_int_distribution<uint8_t> g_dist_step_unit(RANDOM_STEP_MIN, RANDOM_STEP_MAX);

    enum class BlockType : uint8_t {
        None = 0,
        Move = 1,
        Draw = 2,
        If = 3,
        Repeat = 4,
        End = 5,
        Else = 6
    };

    constexpr uint8_t blockIndex(BlockType t) {
        return static_cast<uint8_t>(t);
    }

    constexpr std::array<BlockType, 6> kPlayableBlocks = {
        BlockType::Move, BlockType::Draw, BlockType::If, BlockType::Else, BlockType::Repeat, BlockType::End
    };

    constexpr BlockType nextPlayableBlock(BlockType t) {
        switch (t) {
            case BlockType::Move:
                return BlockType::Draw;
            case BlockType::Draw:
                return BlockType::If;
            case BlockType::If:
                return BlockType::Else;
            case BlockType::Else:
                return BlockType::Repeat;
            case BlockType::Repeat:
                return BlockType::End;
            case BlockType::End:
            case BlockType::None:
            default:
                return BlockType::Move;
        }
    }

    constexpr std::array<const char*, 7> kBlockNames = {
        "NONE",
        "MOVE",
        "DRAW",
        "IF",
        "REPEAT",
        "END",
        "ELSE",
    };

    constexpr const char* blockName(BlockType t) {
        return kBlockNames[blockIndex(t)];
    }

    enum class TurnState : uint8_t {
        PlayerTurn = 0,
        AITurn = 1,
        SelectInputColor = 2,
        RunProgram = 3,
        Finished = 4
    };

    constexpr uint16_t RGB565_WHITE = 0xFFFF;
    constexpr uint16_t RGB565_BLACK = 0x0000;
    constexpr uint16_t RGB565_RED = 0xF800;
    constexpr uint16_t RGB565_ORANGE = 0xFC07;
    constexpr uint16_t RGB565_YELLOW = 0xFFE0;
    constexpr uint16_t RGB565_GREEN = 0x07E0;
    constexpr uint16_t RGB565_BLUE = 0x001F;
    constexpr uint16_t RGB565_MAGENTA = 0xF81F;

    constexpr std::array<uint16_t, COLOR_COUNT> kPaintColors = {
        RGB565_WHITE, RGB565_RED, RGB565_ORANGE, RGB565_YELLOW, RGB565_GREEN, RGB565_BLUE, RGB565_MAGENTA, RGB565_BLACK
    };

    constexpr uint8_t colorIndexFromParam(uint8_t param) {
        if (param < COLOR_PARAM_MIN) {
            return 0;
        }
        return static_cast<uint8_t>((param - COLOR_PARAM_MIN) % COLOR_COUNT);
    }

    constexpr uint16_t paintColorByParam(uint8_t param) {
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
            case BlockType::Else:
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
            case BlockType::Else:
            case BlockType::End:
            case BlockType::None:
            default:
                return 0;
        }
    }

    constexpr bool blockHasParam(BlockType t) {
        return t == BlockType::Move || t == BlockType::Draw || t == BlockType::If || t == BlockType::Repeat;
    }

    struct ProgramStep {
        BlockType type = BlockType::None;
        uint8_t param = 0;
        bool from_ai = false;
    };

    struct RuntimeState {
        struct DrawCircle {
            uint8_t x = 0;
            uint8_t y = 0;
            uint8_t color = 0;
        };
        std::array<DrawCircle, MAX_DRAW_EVENTS> circles{};
        uint16_t circle_count = 0;
    };

    struct RepeatRuntimeFrame {
        uint8_t start_pc = 0;
        uint8_t remaining = 0;
    };

    enum class VmOp : uint8_t {
        Nop = 0,
        EnterMove = 1,
        ExitMove = 2,
        Draw = 3,
        IfColorMismatchJump = 4,
        Jump = 5,
        RepeatBegin = 6,
        RepeatEnd = 7,
    };

    struct VmInstruction {
        VmOp op = VmOp::Nop;
        uint8_t param = 0;
        uint8_t jump_pc = 0;
    };

    struct ControlFlowGraph {
        std::array<int8_t, MAX_MOVES> block_end{};
        std::array<int8_t, MAX_MOVES> end_start{};
        std::array<int8_t, MAX_MOVES> if_else{};
        std::array<int8_t, MAX_MOVES> else_if{};
    };

    struct CompiledProgram {
        std::array<VmInstruction, MAX_MOVES> instructions{};
        uint8_t instruction_count = 0;
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
        std::array<BlockType, MAX_HISTORY> history{};
        std::array<uint8_t, 8> block_frequency{};
        std::array<std::array<uint16_t, 8>, 8> transitions{};
        std::array<uint8_t, MAX_MOVES> view_depths{};

        uint16_t game_id = 1;
        uint8_t history_size = 0;
        uint8_t move_count = 0;
        uint8_t selected_line = 0;
        uint8_t scroll_top = 0;
        BlockType selected_block = BlockType::Move;
        uint8_t selected_param = MOVE_PARAM_MIN;
        uint8_t syntax_depth = 0;
        uint8_t run_input_color = COLOR_PARAM_MIN;

        TurnState turn = TurnState::PlayerTurn;
        RuntimeState runtime{};
    };

    enum class YdfWorkerCommand : uint8_t {
        PredictBatch = 1,
        Shutdown = 2,
    };

    struct YdfWorkerRequest {
        YdfWorkerCommand command = YdfWorkerCommand::PredictBatch;
        uint8_t batch_count = 0;
        std::array<coBroc::ydf::CandidateFeatures, YDF_WORKER_BATCH_CAPACITY> features{};
    };

    struct YdfWorkerResponse {
        uint8_t batch_count = 0;
        std::array<coBroc::ydf::Prediction, YDF_WORKER_BATCH_CAPACITY> predictions{};
    };

    queue_t g_ydf_request_queue{};
    queue_t g_ydf_response_queue{};
    bool g_ydf_worker_running = false;

    void normalizeSelectedBlockType(ProgramState& s);

    void initProgramState(ProgramState& s) {
        const uint16_t next_game = static_cast<uint16_t>(s.game_id + 1);
        s = ProgramState{};
        s.game_id = next_game;
        s.selected_block = BlockType::Move;
        s.selected_param = minParamForBlock(BlockType::Move);
        s.run_input_color = COLOR_PARAM_MIN;
        normalizeSelectedBlockType(s);
    }

    bool isPlayableBlock(BlockType t) {
        return t == BlockType::Move || t == BlockType::Draw || t == BlockType::If ||
               t == BlockType::Else || t == BlockType::Repeat || t == BlockType::End;
    }

    bool insideMoveScope(const ProgramState& s) {
        std::array<BlockType, MAX_MOVES> open_stack{};
        uint8_t open_top = 0;
        for (uint8_t i = 0; i < s.move_count; i++) {
            const auto t = s.program[i].type;
            if (t == BlockType::End) {
                if (open_top > 0) {
                    open_top--;
                }
                continue;
            }
            if (t == BlockType::Move || t == BlockType::If || t == BlockType::Repeat) {
                if (open_top < MAX_MOVES) {
                    open_stack[open_top++] = t;
                }
                continue;
            }
            if (t == BlockType::Else && open_top > 0 && open_stack[open_top - 1] == BlockType::If) {
                open_stack[open_top - 1] = BlockType::Else;
            }
        }
        for (uint8_t i = 0; i < open_top; i++) {
            if (open_stack[i] == BlockType::Move) {
                return true;
            }
        }
        return false;
    }

    bool canOpenElseBranch(const ProgramState& s) {
        std::array<BlockType, MAX_MOVES> open_types{};
        std::array<bool, MAX_MOVES> if_has_else{};
        uint8_t open_top = 0;
        for (uint8_t i = 0; i < s.move_count; i++) {
            const BlockType t = s.program[i].type;
            if (t == BlockType::Move || t == BlockType::If || t == BlockType::Repeat) {
                if (open_top >= MAX_MOVES) {
                    return false;
                }
                open_types[open_top] = t;
                if_has_else[open_top] = false;
                open_top++;
                continue;
            }
            if (t == BlockType::Else) {
                if (open_top == 0 || open_types[open_top - 1] != BlockType::If || if_has_else[open_top - 1]) {
                    return false;
                }
                if_has_else[open_top - 1] = true;
                continue;
            }
            if (t == BlockType::End) {
                if (open_top == 0) {
                    return false;
                }
                open_top--;
            }
        }
        return open_top > 0 && open_types[open_top - 1] == BlockType::If && !if_has_else[open_top - 1];
    }

    bool blockAllowedByDepth(const ProgramState& s, BlockType t) {
        if (t == BlockType::Else) {
            return canOpenElseBranch(s);
        }
        if (t == BlockType::End) {
            return s.syntax_depth > 0;
        }
        if ((t == BlockType::Move || t == BlockType::If || t == BlockType::Repeat) && s.syntax_depth >= MAX_NEST_DEPTH) {
            return false;
        }
        if (insideMoveScope(s) && (t == BlockType::If || t == BlockType::Else || t == BlockType::Repeat)) {
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
        }
        if (t == BlockType::Else) {
            return param == 0;
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
        const uint8_t idx = blockIndex(t);
        if (s.history_size > 0) {
            const uint8_t prev = blockIndex(s.history[(s.history_size - 1) % MAX_HISTORY]);
            s.transitions[prev][idx]++;
        }
        s.history[s.history_size % MAX_HISTORY] = t;
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

    coBroc::ydf::CandidateFeatures makeYdfFeatures(const ProgramState& s, const AICandidate& c) {
        coBroc::ydf::CandidateFeatures f{};
        f.game_id = s.game_id;
        f.turn = static_cast<uint32_t>(s.move_count + 1);
        f.candidate_type = static_cast<uint32_t>(c.type);
        f.candidate_param = c.param;
        f.depth = s.syntax_depth;
        f.remaining_moves = static_cast<uint32_t>(MAX_MOVES - s.move_count);
        f.last_type = s.history_size == 0 ? 0u : static_cast<uint32_t>(blockIndex(s.history[(s.history_size - 1) % MAX_HISTORY]));
        f.freq_move = s.block_frequency[blockIndex(BlockType::Move)];
        f.freq_draw = s.block_frequency[blockIndex(BlockType::Draw)];
        f.freq_if = s.block_frequency[blockIndex(BlockType::If)];
        f.freq_repeat = s.block_frequency[blockIndex(BlockType::Repeat)];
        f.freq_end = s.block_frequency[blockIndex(BlockType::End)];
        f.transition_prev_to_candidate =
            s.history_size == 0 ? 0u : static_cast<uint32_t>(s.transitions[f.last_type][blockIndex(c.type)]);
        f.legal = c.legal ? 1u : 0u;
        f.actor = 1u; // current pipeline evaluates AI decisions only
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
            res.batch_count = req.batch_count;
            for (uint8_t i = 0; i < req.batch_count; i++) {
                res.predictions[i] = coBroc::ydf::Model::Predict(req.features[i]);
            }
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
        YdfWorkerRequest shutdown_req{};
        shutdown_req.command = YdfWorkerCommand::Shutdown;
        queue_add_blocking(&g_ydf_request_queue, &shutdown_req);
        multicore_reset_core1();
        queue_free(&g_ydf_request_queue);
        queue_free(&g_ydf_response_queue);
        g_ydf_worker_running = false;
    }

    void ydfPredictSuitabilityBatch(
        const ProgramState& s,
        std::array<AICandidate, AI_CANDIDATE_SLOTS>& candidates,
        const std::array<uint8_t, AI_CANDIDATE_SLOTS>& indices,
        uint8_t index_count
    ) {
        if (index_count == 0) {
            return;
        }
        if (!g_ydf_worker_running) {
            for (uint8_t i = 0; i < index_count; i++) {
                auto& cand = candidates[indices[i]];
                const auto pred = coBroc::ydf::Model::Predict(makeYdfFeatures(s, cand));
                cand.suitability = pred.ok ? pred.suitability_score : -1.0f;
            }
            return;
        }

        YdfWorkerRequest req{};
        req.command = YdfWorkerCommand::PredictBatch;
        req.batch_count = index_count;
        for (uint8_t i = 0; i < index_count; i++) {
            req.features[i] = makeYdfFeatures(s, candidates[indices[i]]);
        }
        queue_add_blocking(&g_ydf_request_queue, &req);

        YdfWorkerResponse res{};
        queue_remove_blocking(&g_ydf_response_queue, &res);
        const uint8_t out_count = std::min(req.batch_count, res.batch_count);
        for (uint8_t i = 0; i < out_count; i++) {
            candidates[indices[i]].suitability = res.predictions[i].ok ? res.predictions[i].suitability_score : -1.0f;
        }
        for (uint8_t i = out_count; i < req.batch_count; i++) {
            candidates[indices[i]].suitability = -1.0f;
        }
    }

    std::array<AICandidate, AI_CANDIDATE_SLOTS> buildCandidates(const ProgramState& s, uint8_t& out_count) {
        std::array<AICandidate, AI_CANDIDATE_SLOTS> cands{};
        out_count = 0;
        for (BlockType t : kPlayableBlocks) {
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
            } else if (t == BlockType::Else) {
                min_param = 0;
                max_param = 0;
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
                c.suitability = -1.0f;
                cands[out_count++] = c;
            }
        }
        return cands;
    }

    void pruneCandidatesForPrediction(
        const ProgramState& s,
        std::array<AICandidate, AI_CANDIDATE_SLOTS>& candidates,
        uint8_t count
    ) {
        if (count <= AI_MAX_PREDICT_CANDIDATES) {
            return;
        }

        std::array<uint8_t, AI_CANDIDATE_SLOTS> legal_indices{};
        uint8_t legal_count = 0;
        for (uint8_t i = 0; i < count; i++) {
            if (!candidates[i].legal) {
                continue;
            }
            legal_indices[legal_count++] = i;
        }
        if (legal_count <= AI_MAX_PREDICT_CANDIDATES) {
            return;
        }

        std::array<float, AI_CANDIDATE_SLOTS> heuristics{};
        for (uint8_t i = 0; i < legal_count; i++) {
            auto& c = candidates[legal_indices[i]];
            float h = 0.0f;
            if (s.history_size > 0) {
                const uint8_t last_type = blockIndex(s.history[(s.history_size - 1) % MAX_HISTORY]);
                h += static_cast<float>(s.transitions[last_type][blockIndex(c.type)]) * 0.05f;
            }
            h -= static_cast<float>(s.block_frequency[blockIndex(c.type)]) * 0.02f;
            if (c.type == BlockType::Draw) {
                h += 0.03f;
            } else if (c.type == BlockType::End && s.syntax_depth > 0) {
                h += 0.04f;
            } else {
                h += 0.02f;
            }
            heuristics[legal_indices[i]] = h;
        }

        std::array<bool, AI_CANDIDATE_SLOTS> keep{};
        for (uint8_t pick = 0; pick < AI_MAX_PREDICT_CANDIDATES; pick++) {
            int best = -1;
            for (uint8_t i = 0; i < legal_count; i++) {
                const uint8_t idx = legal_indices[i];
                if (keep[idx]) {
                    continue;
                }
                if (best < 0 || heuristics[idx] > heuristics[static_cast<size_t>(best)]) {
                    best = static_cast<int>(idx);
                }
            }
            if (best < 0) {
                break;
            }
            keep[static_cast<uint8_t>(best)] = true;
        }
        for (uint8_t i = 0; i < count; i++) {
            if (candidates[i].legal && !keep[i]) {
                candidates[i].legal = false;
                candidates[i].suitability = -1.0f;
            }
        }
    }

    void predictAllLegalCandidates(const ProgramState& s, std::array<AICandidate, AI_CANDIDATE_SLOTS>& candidates, uint8_t count) {
        std::array<uint8_t, AI_CANDIDATE_SLOTS> predict_indices{};
        uint8_t predict_count = 0;
        for (uint8_t i = 0; i < count; i++) {
            if (!candidates[i].legal) {
                candidates[i].suitability = -1.0f;
                continue;
            }
            predict_indices[predict_count++] = i;
        }
        ydfPredictSuitabilityBatch(s, candidates, predict_indices, predict_count);
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

    void applyRuleFeedbackAndRescore(const ProgramState& s, std::array<AICandidate, AI_CANDIDATE_SLOTS>& cands, uint8_t count) {
        std::array<uint8_t, AI_CANDIDATE_SLOTS> rescore_indices{};
        uint8_t rescore_count = 0;
        for (uint8_t i = 0; i < count; i++) {
            auto& c = cands[i];
            if (!c.legal) {
                continue;
            }
            if (!violatesAIMoveRule(s, c)) {
                continue;
            }
            c.feedback_penalty = static_cast<uint8_t>(std::min<uint16_t>(255, c.feedback_penalty + 1));
            rescore_indices[rescore_count++] = i;
        }
        if (rescore_count > 0) {
            ydfPredictSuitabilityBatch(s, cands, rescore_indices, rescore_count);
        }

        const uint8_t draw_streak = tailTypeStreak(s, BlockType::Draw);
        for (uint8_t i = 0; i < count; i++) {
            auto& c = cands[i];
            if (!c.legal) {
                continue;
            }

            if (c.type == BlockType::Draw) {
                // Prevent a Draw-only loop unless Draw is structurally required.
                c.suitability -= 0.12f * static_cast<float>(draw_streak);
                if (s.move_count > 0 && s.program[s.move_count - 1].type == BlockType::Draw &&
                    s.program[s.move_count - 1].param == c.param) {
                    c.suitability -= 0.10f;
                }
            } else {
                const uint8_t type_freq = s.block_frequency[blockIndex(c.type)];
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

    void addDrawCircle(RuntimeState& runtime, uint8_t color, uint8_t move_steps) {
        if (runtime.circle_count >= MAX_DRAW_EVENTS) {
            return;
        }
        auto& circle = runtime.circles[runtime.circle_count++];
        circle.color = color;
        circle.x = g_dist_result_x(g_rng);
        circle.y = g_dist_result_y(g_rng);

        for (uint8_t i = 0; i < move_steps; i++) {
            const int8_t dx = static_cast<int8_t>(g_dist_step_unit(g_rng)) - RANDOM_STEP_OFFSET;
            const int8_t dy = static_cast<int8_t>(g_dist_step_unit(g_rng)) - RANDOM_STEP_OFFSET;
            const int16_t nx = static_cast<int16_t>(circle.x) + dx * RANDOM_STEP_PIXELS;
            const int16_t ny = static_cast<int16_t>(circle.y) + dy * RANDOM_STEP_PIXELS;
            circle.x = static_cast<uint8_t>(std::clamp<int16_t>(nx, RESULT_MIN_COORD, RESULT_MAX_X));
            circle.y = static_cast<uint8_t>(std::clamp<int16_t>(ny, RESULT_MIN_COORD, RESULT_MAX_Y));
        }
    }

    bool chooseBestExcludingType(const std::array<AICandidate, AI_CANDIDATE_SLOTS>& candidates, uint8_t count, BlockType excluded, uint8_t& chosen) {
        chosen = 255;
        for (uint8_t i = 0; i < count; i++) {
            const auto& c = candidates[i];
            if (!c.legal || c.type == excluded) {
                continue;
            }
            if (chosen == 255 || c.suitability > candidates[chosen].suitability) {
                chosen = i;
            }
        }
        return chosen != 255;
    }

    void performAITurn(ProgramState& s) {
        uint8_t count = 0;
        auto candidates = buildCandidates(s, count);
        pruneCandidatesForPrediction(s, candidates, count);
        predictAllLegalCandidates(s, candidates, count);
        applyRuleFeedbackAndRescore(s, candidates, count);
        uint8_t chosen = 255;
        if (!chooseBestCandidate(candidates, count, chosen)) {
            s.turn = TurnState::RunProgram;
            return;
        }
        if (candidates[chosen].type == BlockType::Draw) {
            uint8_t non_draw = 255;
            if (chooseBestExcludingType(candidates, count, BlockType::Draw, non_draw)) {
                const uint8_t draw_streak = tailTypeStreak(s, BlockType::Draw);
                const float gap = candidates[chosen].suitability - candidates[non_draw].suitability;
                if (draw_streak >= 2 || gap <= 0.08f) {
                    chosen = non_draw;
                }
            }
        }
        // Avoid immediate closure after IF/REPEAT unless it is clearly better.
        if (candidates[chosen].type == BlockType::End && s.move_count > 0) {
            const auto prev = s.program[s.move_count - 1].type;
            if (prev == BlockType::If || prev == BlockType::Repeat) {
                uint8_t non_end = 255;
                if (chooseBestExcludingType(candidates, count, BlockType::End, non_end)) {
                    const float end_gap = candidates[chosen].suitability - candidates[non_end].suitability;
                    if (end_gap <= 0.15f) {
                        chosen = non_end;
                    }
                }
            }
        }
        if (!addStepToProgram(s, candidates[chosen].type, candidates[chosen].param, true)) {
            s.turn = TurnState::RunProgram;
            return;
        }
        if (s.move_count >= MAX_MOVES) {
            s.turn = TurnState::RunProgram;
        } else {
            s.turn = TurnState::PlayerTurn;
        }
    }

    // --- Run ---
    void finalizeSyntax(ProgramState& s) {
        while (s.syntax_depth > 0 && s.move_count < MAX_MOVES) {
            addStepToProgram(s, BlockType::End, 0, true);
        }
    }

    bool buildControlFlowGraph(const ProgramState& s, ControlFlowGraph& cfg) {
        cfg = ControlFlowGraph{};
        cfg.block_end.fill(-1);
        cfg.end_start.fill(-1);
        cfg.if_else.fill(-1);
        cfg.else_if.fill(-1);

        std::array<uint8_t, MAX_MOVES> stack{};
        uint8_t top = 0;
        for (uint8_t pc = 0; pc < s.move_count; pc++) {
            const BlockType t = s.program[pc].type;
            if (t == BlockType::Move || t == BlockType::If || t == BlockType::Repeat) {
                if (top >= MAX_MOVES) {
                    return false;
                }
                stack[top++] = pc;
                continue;
            }

            if (t == BlockType::Else) {
                if (top == 0) {
                    return false;
                }
                const uint8_t if_pc = stack[top - 1];
                if (s.program[if_pc].type != BlockType::If || cfg.if_else[if_pc] >= 0) {
                    return false;
                }
                cfg.if_else[if_pc] = static_cast<int8_t>(pc);
                cfg.else_if[pc] = static_cast<int8_t>(if_pc);
                continue;
            }

            if (t == BlockType::End) {
                if (top == 0) {
                    return false;
                }
                const uint8_t start = stack[--top];
                cfg.block_end[start] = static_cast<int8_t>(pc);
                cfg.end_start[pc] = static_cast<int8_t>(start);
                continue;
            }
        }

        return top == 0;
    }

    bool compileProgram(const ProgramState& s, const ControlFlowGraph& cfg, CompiledProgram& out) {
        out = CompiledProgram{};
        out.instruction_count = s.move_count;
        for (uint8_t pc = 0; pc < s.move_count; pc++) {
            const ProgramStep step = s.program[pc];
            auto& ins = out.instructions[pc];
            ins = VmInstruction{};

            switch (step.type) {
                case BlockType::Move:
                    if (cfg.block_end[pc] < 0) {
                        return false;
                    }
                    ins.op = VmOp::EnterMove;
                    ins.param = step.param;
                    break;
                case BlockType::Draw:
                    ins.op = VmOp::Draw;
                    ins.param = step.param;
                    break;
                case BlockType::If:
                    if (cfg.block_end[pc] < 0) {
                        return false;
                    }
                    ins.op = VmOp::IfColorMismatchJump;
                    ins.param = step.param;
                    if (cfg.if_else[pc] >= 0) {
                        ins.jump_pc = static_cast<uint8_t>(cfg.if_else[pc] + 1);
                    } else {
                        ins.jump_pc = static_cast<uint8_t>(cfg.block_end[pc] + 1);
                    }
                    break;
                case BlockType::Else: {
                    const int8_t if_pc = cfg.else_if[pc];
                    if (if_pc < 0 || cfg.block_end[static_cast<uint8_t>(if_pc)] < 0) {
                        return false;
                    }
                    ins.op = VmOp::Jump;
                    ins.jump_pc = static_cast<uint8_t>(cfg.block_end[static_cast<uint8_t>(if_pc)] + 1);
                    break;
                }
                case BlockType::Repeat:
                    if (cfg.block_end[pc] < 0) {
                        return false;
                    }
                    ins.op = VmOp::RepeatBegin;
                    ins.param = step.param;
                    break;
                case BlockType::End: {
                    const int8_t start = cfg.end_start[pc];
                    if (start < 0) {
                        return false;
                    }
                    const BlockType open = s.program[static_cast<uint8_t>(start)].type;
                    if (open == BlockType::Move) {
                        ins.op = VmOp::ExitMove;
                    } else if (open == BlockType::If) {
                        ins.op = VmOp::Nop;
                    } else if (open == BlockType::Repeat) {
                        ins.op = VmOp::RepeatEnd;
                        ins.jump_pc = static_cast<uint8_t>(start + 1);
                    } else {
                        return false;
                    }
                    break;
                }
                case BlockType::None:
                default:
                    return false;
            }
        }
        return true;
    }

    bool executeProgram(const ProgramState& s, const CompiledProgram& program, RuntimeState& runtime) {
        runtime = RuntimeState{};
        std::array<uint8_t, MAX_NEST_DEPTH> move_step_stack{};
        uint8_t move_anchor_top = 0;
        std::array<RepeatRuntimeFrame, MAX_NEST_DEPTH> repeat_stack{};
        uint8_t repeat_top = 0;

        uint8_t pc = 0;
        while (pc < program.instruction_count) {
            const VmInstruction& ins = program.instructions[pc];
            switch (ins.op) {
                case VmOp::Nop:
                    pc++;
                    break;
                case VmOp::EnterMove:
                    if (move_anchor_top >= MAX_NEST_DEPTH) {
                        return false;
                    }
                    move_step_stack[move_anchor_top++] = ins.param;
                    pc++;
                    break;
                case VmOp::ExitMove:
                    if (move_anchor_top == 0) {
                        return false;
                    }
                    move_anchor_top--;
                    pc++;
                    break;
                case VmOp::Draw:
                    addDrawCircle(
                        runtime,
                        colorIndexFromParam(ins.param),
                        move_anchor_top > 0 ? move_step_stack[move_anchor_top - 1] : 0
                    );
                    pc++;
                    break;
                case VmOp::IfColorMismatchJump: {
                    const bool cond = colorIndexFromParam(s.run_input_color) == colorIndexFromParam(ins.param);
                    pc = cond ? static_cast<uint8_t>(pc + 1) : ins.jump_pc;
                    break;
                }
                case VmOp::Jump:
                    pc = ins.jump_pc;
                    break;
                case VmOp::RepeatBegin:
                    if (repeat_top >= MAX_NEST_DEPTH) {
                        return false;
                    }
                    repeat_stack[repeat_top++] = {pc, ins.param};
                    pc++;
                    break;
                case VmOp::RepeatEnd:
                    if (repeat_top == 0) {
                        return false;
                    }
                    if (repeat_stack[repeat_top - 1].remaining > 1) {
                        repeat_stack[repeat_top - 1].remaining--;
                        pc = ins.jump_pc;
                    } else {
                        repeat_top--;
                        pc++;
                    }
                    break;
                default:
                    return false;
            }
        }

        return repeat_top == 0 && move_anchor_top == 0;
    }

    bool compileAndRun(ProgramState& s) {
        finalizeSyntax(s);

        ControlFlowGraph cfg{};
        if (!buildControlFlowGraph(s, cfg)) {
            return false;
        }

        CompiledProgram compiled{};
        if (!compileProgram(s, cfg, compiled)) {
            return false;
        }

        RuntimeState runtime{};
        if (!executeProgram(s, compiled, runtime)) {
            return false;
        }

        s.runtime = runtime;
        return true;
    }

    void cycleParam(ProgramState& s) {
        if (!blockHasParam(s.selected_block)) {
            return;
        }
        const uint8_t min_param = minParamForBlock(s.selected_block);
        const uint8_t max_param = maxParamForBlock(s.selected_block);
        const uint8_t base = s.selected_param < min_param ? min_param : s.selected_param;
        s.selected_param = base >= max_param ? min_param : static_cast<uint8_t>(base + 1);
    }

    void normalizeSelectedBlockType(ProgramState& s) {
        const BlockType current = s.selected_block;
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

        for (const BlockType t : kPlayableBlocks) {
            const uint8_t candidate_param = blockHasParam(t) ? minParamForBlock(t) : 0;
            if (!isLegalCandidate(s, t, candidate_param)) {
                continue;
            }
            s.selected_block = t;
            s.selected_param = candidate_param;
            return;
        }
    }

    void cycleBlockType(ProgramState& s) {
        BlockType next = s.selected_block;
        bool found = false;
        for (size_t i = 0; i < kPlayableBlocks.size(); i++) {
            next = nextPlayableBlock(next);
            const uint8_t candidate_param = blockHasParam(next) ? minParamForBlock(next) : 0;
            if (!isLegalCandidate(s, next, candidate_param)) {
                continue;
            }
            found = true;
            break;
        }
        if (!found) {
            return;
        }

        s.selected_block = next;
        const uint8_t min_param = minParamForBlock(next);
        const uint8_t max_param = maxParamForBlock(next);
        if (blockHasParam(next) && (s.selected_param < min_param || s.selected_param > max_param)) {
            s.selected_param = min_param;
        }
        if (!blockHasParam(next)) {
            s.selected_param = 0;
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
            const BlockType t = s.selected_block;
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

    namespace ui {
        constexpr uint16_t LVGL_DRAW_BUFFER_LINES = 24;
        constexpr size_t LVGL_DRAW_BUFFER_PIXEL_COUNT = static_cast<size_t>(LCD_WIDTH) * LVGL_DRAW_BUFFER_LINES;
        constexpr size_t LCD_TX_LINE_BUFFER_SIZE = static_cast<size_t>(LCD_WIDTH) * sizeof(lv_color_t);

        struct LvglUiContext {
            std::array<lv_color_t, LVGL_DRAW_BUFFER_PIXEL_COUNT> lvgl_draw_pixels{};
            std::array<uint8_t, LCD_TX_LINE_BUFFER_SIZE> lcd_tx_line_buffer{};
            std::array<lv_point_t, (MAX_MOVES + 2) * 8> flow_line_points{};
            size_t flow_line_point_count = 0;
            lv_disp_draw_buf_t lvgl_draw_buf{};
            lv_disp_drv_t lvgl_disp_drv{};
        };

        LvglUiContext g_lvgl_ui{};

        constexpr lv_coord_t UI_MARGIN = 6;
        constexpr lv_coord_t UI_CARD_WIDTH = static_cast<lv_coord_t>(LCD_WIDTH - UI_MARGIN * 2);
        constexpr lv_coord_t UI_HEADER_HEIGHT = 58;
        constexpr lv_coord_t UI_LIST_Y = static_cast<lv_coord_t>(UI_MARGIN + UI_HEADER_HEIGHT + 4);
        constexpr lv_coord_t UI_LIST_HEIGHT = static_cast<lv_coord_t>(LCD_HEIGHT - UI_LIST_Y - UI_MARGIN);
        constexpr lv_coord_t FLOW_NODE_HEIGHT = 34;
        constexpr lv_coord_t FLOW_NODE_WIDTH = static_cast<lv_coord_t>(UI_CARD_WIDTH - 28);
        constexpr lv_coord_t FLOW_NODE_X = static_cast<lv_coord_t>((UI_CARD_WIDTH - FLOW_NODE_WIDTH) / 2);
        constexpr lv_coord_t FLOW_NODE_GAP = 12;
        constexpr lv_coord_t FLOW_TOP_PADDING = 8;

        lv_color_t lvColorFromRgb565Fast(uint16_t rgb565) {
            const uint8_t r = static_cast<uint8_t>(((rgb565 >> 11) & 0x1F) * 255 / 31);
            const uint8_t g = static_cast<uint8_t>(((rgb565 >> 5) & 0x3F) * 255 / 63);
            const uint8_t b = static_cast<uint8_t>((rgb565 & 0x1F) * 255 / 31);
            return lv_color_make(r, g, b);
        }

        lv_color_t blockAccentColor(BlockType t) {
            switch (t) {
                case BlockType::Move:
                    return lv_color_hex(0x2F80ED);
                case BlockType::Draw:
                    return lv_color_hex(0x14A44D);
                case BlockType::If:
                    return lv_color_hex(0xA95DF5);
                case BlockType::Else:
                    return lv_color_hex(0xE056FD);
                case BlockType::Repeat:
                    return lv_color_hex(0xF39C12);
                case BlockType::End:
                    return lv_color_hex(0x6C757D);
                case BlockType::None:
                default:
                    return lv_color_hex(0x343A40);
            }
        }

        const char* turnName(TurnState t) {
            switch (t) {
                case TurnState::PlayerTurn:
                    return "PLAYER";
                case TurnState::AITurn:
                    return "AI";
                case TurnState::SelectInputColor:
                    return "INPUT COLOR";
                case TurnState::RunProgram:
                    return "RUN";
                case TurnState::Finished:
                    return "DONE";
                default:
                    return "-";
            }
        }

        void styleCard(lv_obj_t* obj, lv_color_t bg, lv_color_t border) {
            lv_obj_set_style_bg_color(obj, bg, 0);
            lv_obj_set_style_bg_opa(obj, LV_OPA_COVER, 0);
            lv_obj_set_style_border_color(obj, border, 0);
            lv_obj_set_style_border_width(obj, 1, 0);
            lv_obj_set_style_radius(obj, 6, 0);
            lv_obj_set_style_pad_all(obj, 4, 0);
        }

        void createColorSwatch(lv_obj_t* parent, lv_coord_t x, lv_coord_t y, uint8_t param, lv_coord_t size) {
            auto* swatch = lv_obj_create(parent);
            lv_obj_set_size(swatch, size, size);
            lv_obj_set_pos(swatch, x, y);
            lv_obj_set_style_radius(swatch, LV_RADIUS_CIRCLE, 0);
            lv_obj_set_style_bg_color(swatch, lvColorFromRgb565Fast(paintColorByParam(param)), 0);
            lv_obj_set_style_bg_opa(swatch, LV_OPA_COVER, 0);
            lv_obj_set_style_border_width(swatch, 1, 0);
            lv_obj_set_style_border_color(swatch, lv_color_hex(0x222222), 0);
            lv_obj_set_style_pad_all(swatch, 0, 0);
            lv_obj_clear_flag(swatch, LV_OBJ_FLAG_SCROLLABLE);
        }

        lv_point_t* reserveLinePoints(size_t count) {
            if (g_lvgl_ui.flow_line_point_count + count > g_lvgl_ui.flow_line_points.size()) {
                return nullptr;
            }
            auto* ptr = &g_lvgl_ui.flow_line_points[g_lvgl_ui.flow_line_point_count];
            g_lvgl_ui.flow_line_point_count += count;
            return ptr;
        }

        void styleFlowLine(lv_obj_t* line, lv_color_t color, uint8_t width) {
            lv_obj_set_style_line_color(line, color, 0);
            lv_obj_set_style_line_width(line, width, 0);
            lv_obj_set_style_line_opa(line, LV_OPA_COVER, 0);
            lv_obj_set_style_line_rounded(line, true, 0);
        }

        void drawFlowConnector(lv_obj_t* parent, lv_coord_t x, lv_coord_t top, lv_coord_t bottom) {
            auto* pts = reserveLinePoints(2);
            if (pts == nullptr) {
                return;
            }
            pts[0] = {x, top};
            pts[1] = {x, bottom};
            auto* line = lv_line_create(parent);
            lv_line_set_points(line, pts, 2);
            styleFlowLine(line, lv_color_hex(0x7A8795), 2);
            lv_obj_clear_flag(line, LV_OBJ_FLAG_SCROLLABLE);
        }

        uint8_t flowItemCount(const ProgramState& s) {
            return static_cast<uint8_t>(std::min<uint8_t>(MAX_MOVES + 1, static_cast<uint8_t>(s.move_count + 1)));
        }

        uint8_t focusFlowIndex(const ProgramState& s) {
            if (s.move_count == 0) {
                return 0;
            }
            return static_cast<uint8_t>(std::min<uint8_t>(s.move_count, static_cast<uint8_t>(s.selected_line + 1)));
        }

        uint8_t visibleFlowCount() {
            const lv_coord_t row_space = static_cast<lv_coord_t>(FLOW_NODE_HEIGHT + FLOW_NODE_GAP);
            const lv_coord_t available = static_cast<lv_coord_t>(UI_LIST_HEIGHT - FLOW_TOP_PADDING + FLOW_NODE_GAP);
            const lv_coord_t rows = available / row_space;
            const lv_coord_t safe_rows = std::max<lv_coord_t>(1, rows);
            return static_cast<uint8_t>(std::min<lv_coord_t>(safe_rows, MAX_MOVES + 1));
        }

        uint8_t flowTopIndex(const ProgramState& s) {
            const uint8_t total = flowItemCount(s);
            const uint8_t visible = visibleFlowCount();
            if (total <= visible) {
                return 0;
            }
            const uint8_t focus = focusFlowIndex(s);
            if (focus + 1 <= visible) {
                return 0;
            }
            const uint8_t max_top = static_cast<uint8_t>(total - visible);
            const uint8_t wanted = static_cast<uint8_t>(focus + 1 - visible);
            return std::min<uint8_t>(wanted, max_top);
        }

        ProgramStep flowStep(const ProgramState& s, uint8_t flow_index) {
            if (flow_index == 0) {
                ProgramStep start{};
                start.type = BlockType::None;
                start.param = 0;
                start.from_ai = false;
                return start;
            }
            return s.program[flow_index - 1];
        }

        uint8_t shownParam(const ProgramState& s, const ProgramStep& step, uint8_t flow_index) {
            if (flow_index == 0 || !blockHasParam(step.type)) {
                return 0;
            }
            if (flow_index != static_cast<uint8_t>(s.selected_line + 1)) {
                return step.param;
            }
            const uint8_t min_param = minParamForBlock(step.type);
            const uint8_t max_param = maxParamForBlock(step.type);
            if (s.selected_param < min_param || s.selected_param > max_param) {
                return min_param;
            }
            return s.selected_param;
        }

        lv_color_t flowColor(const ProgramStep& step, uint8_t flow_index) {
            if (flow_index == 0) {
                return lv_color_hex(0x16A34A);
            }
            return blockAccentColor(step.type);
        }

        void addTriangle(lv_obj_t* parent, lv_coord_t x, lv_coord_t y, lv_coord_t size, lv_color_t color, uint8_t width) {
            auto* pts = reserveLinePoints(4);
            if (pts == nullptr) {
                return;
            }
            const lv_coord_t max = static_cast<lv_coord_t>(size - 2);
            const lv_coord_t half = static_cast<lv_coord_t>(size / 2);
            pts[0] = {1, max};
            pts[1] = {half, 1};
            pts[2] = {max, max};
            pts[3] = {1, max};
            auto* line = lv_line_create(parent);
            lv_line_set_points(line, pts, 4);
            lv_obj_set_pos(line, x, y);
            styleFlowLine(line, color, width);
            lv_obj_clear_flag(line, LV_OBJ_FLAG_SCROLLABLE);
        }

        void addDiamond(lv_obj_t* parent, lv_coord_t x, lv_coord_t y, lv_coord_t size, lv_color_t color, uint8_t width) {
            auto* pts = reserveLinePoints(5);
            if (pts == nullptr) {
                return;
            }
            const lv_coord_t max = static_cast<lv_coord_t>(size - 2);
            const lv_coord_t half = static_cast<lv_coord_t>(size / 2);
            pts[0] = {half, 1};
            pts[1] = {max, half};
            pts[2] = {half, max};
            pts[3] = {1, half};
            pts[4] = {half, 1};
            auto* line = lv_line_create(parent);
            lv_line_set_points(line, pts, 5);
            lv_obj_set_pos(line, x, y);
            styleFlowLine(line, color, width);
            lv_obj_clear_flag(line, LV_OBJ_FLAG_SCROLLABLE);
        }

        void styleShape(lv_obj_t* obj, lv_coord_t x, lv_coord_t y, lv_coord_t size, lv_color_t color, bool selected, lv_coord_t radius) {
            lv_obj_set_pos(obj, x, y);
            lv_obj_set_size(obj, size, size);
            lv_obj_set_style_radius(obj, radius, 0);
            lv_obj_set_style_bg_color(obj, color, 0);
            lv_obj_set_style_bg_opa(obj, selected ? LV_OPA_30 : LV_OPA_20, 0);
            lv_obj_set_style_border_color(obj, color, 0);
            lv_obj_set_style_border_width(obj, selected ? 3 : 2, 0);
            lv_obj_set_style_pad_all(obj, 0, 0);
            lv_obj_clear_flag(obj, LV_OBJ_FLAG_SCROLLABLE);
        }

        void addNodeSymbol(
            lv_obj_t* parent,
            const ProgramStep& step,
            uint8_t flow_index,
            lv_color_t color,
            lv_coord_t x,
            lv_coord_t y,
            lv_coord_t size,
            bool selected
        ) {
            const uint8_t line_width = selected ? 3 : 2;
            if (flow_index == 0) {
                auto* circle = lv_obj_create(parent);
                styleShape(circle, x, y, size, color, selected, LV_RADIUS_CIRCLE);
                return;
            }

            switch (step.type) {
                case BlockType::Move: {
                    auto* rect = lv_obj_create(parent);
                    styleShape(rect, x, y, size, color, selected, 2);
                    break;
                }
                case BlockType::Draw:
                    addTriangle(parent, x, y, size, color, line_width);
                    break;
                case BlockType::If:
                    addDiamond(parent, x, y, size, color, line_width);
                    break;
                case BlockType::Else: {
                    auto* alt = lv_obj_create(parent);
                    styleShape(alt, x, y, size, color, selected, 8);
                    auto* mark = lv_label_create(alt);
                    lv_label_set_text(mark, "E");
                    lv_obj_set_style_text_color(mark, color, 0);
                    lv_obj_center(mark);
                    break;
                }
                case BlockType::Repeat: {
                    auto* loop = lv_obj_create(parent);
                    styleShape(loop, x, y, size, color, selected, 6);
                    auto* loop_mark = lv_label_create(loop);
                    lv_label_set_text(loop_mark, "R");
                    lv_obj_set_style_text_color(loop_mark, color, 0);
                    lv_obj_center(loop_mark);
                    break;
                }
                case BlockType::End:
                case BlockType::None:
                default: {
                    auto* end = lv_obj_create(parent);
                    styleShape(end, x, y, size, color, selected, LV_RADIUS_CIRCLE);
                    break;
                }
            }
        }

        void drawMain(const ProgramState& s) {
            g_lvgl_ui.flow_line_point_count = 0;

            auto* scr = lv_scr_act();
            lv_obj_clean(scr);
            lv_obj_set_style_bg_color(scr, lv_color_hex(0xEEF3F9), 0);
            lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);

            const BlockType t = s.selected_block;
            const uint8_t shown_param = blockHasParam(t) ? std::max<uint8_t>(minParamForBlock(t), s.selected_param) : 0;
            const uint8_t param_max = maxParamForBlock(t);

            auto* header = lv_obj_create(scr);
            lv_obj_set_pos(header, UI_MARGIN, UI_MARGIN);
            lv_obj_set_size(header, UI_CARD_WIDTH, UI_HEADER_HEIGHT);
            styleCard(header, lv_color_hex(0xFFFFFF), blockAccentColor(t));
            lv_obj_clear_flag(header, LV_OBJ_FLAG_SCROLLABLE);

            auto* title = lv_label_create(header);
            lv_label_set_text_fmt(title, "SEL:%s  P:%u/%u", blockName(t), shown_param, param_max);
            lv_obj_set_pos(title, 6, 6);

            auto* subtitle = lv_label_create(header);
            lv_label_set_text_fmt(subtitle, "TURN:%s", turnName(s.turn));
            lv_obj_set_pos(subtitle, 6, 24);

            auto* controls = lv_label_create(header);
            lv_label_set_text(controls, "A:add  B:type  X:param  Y:run");
            lv_obj_set_pos(controls, 6, 40);

            if (t == BlockType::Draw || t == BlockType::If) {
                createColorSwatch(header, UI_CARD_WIDTH - 28, 18, shown_param, 18);
            }

            auto* flow = lv_obj_create(scr);
            lv_obj_set_pos(flow, UI_MARGIN, UI_LIST_Y);
            lv_obj_set_size(flow, UI_CARD_WIDTH, UI_LIST_HEIGHT);
            styleCard(flow, lv_color_hex(0xFFFFFF), lv_color_hex(0xC5D1DE));
            lv_obj_set_scrollbar_mode(flow, LV_SCROLLBAR_MODE_OFF);
            lv_obj_clear_flag(flow, LV_OBJ_FLAG_SCROLLABLE);

            constexpr lv_coord_t COL_GAP = 8;
            constexpr lv_coord_t INNER_X = 4;
            const lv_coord_t inner_w = static_cast<lv_coord_t>(UI_CARD_WIDTH - INNER_X * 2);
            const lv_coord_t left_w = static_cast<lv_coord_t>((inner_w - COL_GAP) / 2);
            const lv_coord_t right_w = static_cast<lv_coord_t>(inner_w - left_w - COL_GAP);
            const lv_coord_t left_x = INNER_X;
            const lv_coord_t right_x = static_cast<lv_coord_t>(left_x + left_w + COL_GAP);
            const lv_coord_t shape_size = 24;
            const lv_coord_t center_x = static_cast<lv_coord_t>(left_x + left_w / 2);

            auto* divider_pts = reserveLinePoints(2);
            if (divider_pts != nullptr) {
                divider_pts[0] = {static_cast<lv_coord_t>(left_x + left_w + COL_GAP / 2), 4};
                divider_pts[1] = {static_cast<lv_coord_t>(left_x + left_w + COL_GAP / 2), static_cast<lv_coord_t>(UI_LIST_HEIGHT - 6)};
                auto* divider = lv_line_create(flow);
                lv_line_set_points(divider, divider_pts, 2);
                styleFlowLine(divider, lv_color_hex(0xD8E0EA), 1);
                lv_obj_clear_flag(divider, LV_OBJ_FLAG_SCROLLABLE);
            }

            const uint8_t total = flowItemCount(s);
            const uint8_t visible = visibleFlowCount();
            const uint8_t top = flowTopIndex(s);
            const uint8_t end = static_cast<uint8_t>(std::min<uint16_t>(total, static_cast<uint16_t>(top + visible)));
            const uint8_t focus = focusFlowIndex(s);

            bool has_prev = false;
            lv_coord_t prev_bottom = 0;
            for (uint8_t flow_idx = top; flow_idx < end; flow_idx++) {
                const uint8_t row = static_cast<uint8_t>(flow_idx - top);
                const lv_coord_t row_y = static_cast<lv_coord_t>(FLOW_TOP_PADDING + row * (FLOW_NODE_HEIGHT + FLOW_NODE_GAP));
                const lv_coord_t symbol_y = static_cast<lv_coord_t>(row_y + (FLOW_NODE_HEIGHT - shape_size) / 2);
                const lv_coord_t symbol_x = static_cast<lv_coord_t>(center_x - shape_size / 2);

                if (has_prev) {
                    drawFlowConnector(flow, center_x, prev_bottom, symbol_y);
                }

                const ProgramStep step = flowStep(s, flow_idx);
                const lv_color_t accent = flowColor(step, flow_idx);
                const bool selected = (flow_idx == focus);
                addNodeSymbol(flow, step, flow_idx, accent, symbol_x, symbol_y, shape_size, selected);

                const lv_coord_t info_y = row_y;
                if (selected) {
                    auto* focus_box = lv_obj_create(flow);
                    lv_obj_set_pos(focus_box, right_x, info_y);
                    lv_obj_set_size(focus_box, right_w, FLOW_NODE_HEIGHT);
                    lv_obj_set_style_radius(focus_box, 4, 0);
                    lv_obj_set_style_pad_all(focus_box, 0, 0);
                    lv_obj_set_style_bg_color(focus_box, lv_color_hex(0xEAF3FF), 0);
                    lv_obj_set_style_bg_opa(focus_box, LV_OPA_COVER, 0);
                    lv_obj_set_style_border_color(focus_box, accent, 0);
                    lv_obj_set_style_border_width(focus_box, 1, 0);
                    lv_obj_clear_flag(focus_box, LV_OBJ_FLAG_SCROLLABLE);
                }

                auto* info_title = lv_label_create(flow);
                if (flow_idx == 0) {
                    lv_label_set_text_fmt(info_title, "%sRun", selected ? "> " : "");
                } else {
                    lv_label_set_text_fmt(info_title, "%s%s", selected ? "> " : "", blockName(step.type));
                }
                lv_obj_set_pos(info_title, static_cast<lv_coord_t>(right_x + 4), static_cast<lv_coord_t>(info_y + 2));

                auto* info_detail = lv_label_create(flow);
                if (flow_idx == 0) {
                    lv_label_set_text(info_detail, "start node");
                } else {
                    const uint8_t param = shownParam(s, step, flow_idx);
                    if (blockHasParam(step.type)) {
                        lv_label_set_text_fmt(info_detail, "param:%u/%u%s", param, maxParamForBlock(step.type), step.from_ai ? " [AI]" : "");
                    } else {
                        lv_label_set_text_fmt(info_detail, "%s", step.from_ai ? "[AI]" : "no param");
                    }
                    if (step.type == BlockType::Draw || step.type == BlockType::If) {
                        createColorSwatch(flow, static_cast<lv_coord_t>(right_x + right_w - 12), static_cast<lv_coord_t>(info_y + 11), param, 10);
                    }
                }
                lv_obj_set_pos(info_detail, static_cast<lv_coord_t>(right_x + 4), static_cast<lv_coord_t>(info_y + 18));

                prev_bottom = static_cast<lv_coord_t>(symbol_y + shape_size);
                has_prev = true;
            }
        }

        void drawColorSelect(const ProgramState& s) {
            auto* scr = lv_scr_act();
            lv_obj_clean(scr);
            lv_obj_set_style_bg_color(scr, lv_color_hex(0xEEF3F9), 0);
            lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);

            auto* panel = lv_obj_create(scr);
            lv_obj_set_pos(panel, UI_MARGIN, UI_MARGIN);
            lv_obj_set_size(panel, UI_CARD_WIDTH, static_cast<lv_coord_t>(LCD_HEIGHT - UI_MARGIN * 2));
            styleCard(panel, lv_color_hex(0xFFFFFF), lv_color_hex(0xA95DF5));

            auto* title = lv_label_create(panel);
            lv_label_set_text(title, "Select IF input color");
            lv_obj_set_pos(title, 8, 10);

            auto* hint = lv_label_create(panel);
            lv_label_set_text(hint, "X:next  Y:start  A:back");
            lv_obj_set_pos(hint, 8, 32);

            auto* dot_holder = lv_obj_create(panel);
            lv_obj_set_size(dot_holder, 90, 90);
            lv_obj_set_pos(dot_holder, static_cast<lv_coord_t>((UI_CARD_WIDTH - 90) / 2), 62);
            lv_obj_set_style_radius(dot_holder, LV_RADIUS_CIRCLE, 0);
            lv_obj_set_style_bg_color(dot_holder, lv_color_hex(0xF5F7FA), 0);
            lv_obj_set_style_border_color(dot_holder, lv_color_hex(0xCED8E3), 0);
            lv_obj_set_style_border_width(dot_holder, 1, 0);
            lv_obj_set_style_pad_all(dot_holder, 0, 0);

            createColorSwatch(dot_holder, 25, 25, s.run_input_color, 40);

            auto* selected = lv_label_create(panel);
            lv_label_set_text_fmt(selected, "COLOR PARAM: %u", s.run_input_color);
            lv_obj_set_pos(selected, static_cast<lv_coord_t>((UI_CARD_WIDTH - 110) / 2), 164);

            auto* note = lv_label_create(panel);
            lv_label_set_text(note, "IF blocks compare this color.");
            lv_obj_set_pos(note, 8, 190);
        }

        void drawRunPreview(const ProgramState& s) {
            auto* scr = lv_scr_act();
            lv_obj_clean(scr);
            lv_obj_set_style_bg_color(scr, lv_color_hex(0xF3F6FA), 0);
            lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);

            auto* top = lv_obj_create(scr);
            lv_obj_set_pos(top, UI_MARGIN, UI_MARGIN);
            lv_obj_set_size(top, UI_CARD_WIDTH, 36);
            styleCard(top, lv_color_hex(0xFFFFFF), lv_color_hex(0x14A44D));

            auto* title = lv_label_create(top);
            lv_label_set_text_fmt(title, "Run Preview  circles:%u", s.runtime.circle_count);
            lv_obj_set_pos(title, 8, 10);

            constexpr lv_coord_t preview_top = 48;
            constexpr lv_coord_t preview_w = static_cast<lv_coord_t>(UI_CARD_WIDTH);
            constexpr lv_coord_t preview_h = 156;

            auto* preview = lv_obj_create(scr);
            lv_obj_set_pos(preview, UI_MARGIN, preview_top);
            lv_obj_set_size(preview, preview_w, preview_h);
            styleCard(preview, lv_color_hex(0xFFFFFF), lv_color_hex(0xC5D1DE));
            lv_obj_set_style_clip_corner(preview, true, 0);
            lv_obj_set_style_pad_all(preview, 2, 0);
            lv_obj_set_scrollbar_mode(preview, LV_SCROLLBAR_MODE_OFF);

            constexpr uint16_t x_span = RESULT_MAX_X - RESULT_MIN_COORD;
            constexpr uint16_t y_span = RESULT_MAX_Y - RESULT_MIN_COORD;
            constexpr lv_coord_t usable_w = preview_w - 2 * RESULT_RADIUS - 8;
            constexpr lv_coord_t usable_h = preview_h - 2 * RESULT_RADIUS - 8;

            for (uint16_t i = 0; i < s.runtime.circle_count; i++) {
                const auto& c = s.runtime.circles[i];
                const uint8_t color_idx = c.color % COLOR_COUNT;
                const uint16_t rel_x = static_cast<uint16_t>(c.x - RESULT_MIN_COORD);
                const uint16_t rel_y = static_cast<uint16_t>(c.y - RESULT_MIN_COORD);
                const lv_coord_t px = static_cast<lv_coord_t>(4 + (static_cast<uint32_t>(rel_x) * usable_w) / std::max<uint16_t>(1, x_span));
                const lv_coord_t py = static_cast<lv_coord_t>(4 + (static_cast<uint32_t>(rel_y) * usable_h) / std::max<uint16_t>(1, y_span));

                auto* dot = lv_obj_create(preview);
                lv_obj_set_size(dot, static_cast<lv_coord_t>(RESULT_RADIUS * 2), static_cast<lv_coord_t>(RESULT_RADIUS * 2));
                lv_obj_set_pos(dot, px, py);
                lv_obj_set_style_radius(dot, LV_RADIUS_CIRCLE, 0);
                lv_obj_set_style_bg_color(dot, lvColorFromRgb565Fast(kPaintColors[color_idx]), 0);
                lv_obj_set_style_bg_opa(dot, LV_OPA_COVER, 0);
                lv_obj_set_style_border_width(dot, 1, 0);
                lv_obj_set_style_border_color(dot, lv_color_hex(0x222222), 0);
                lv_obj_set_style_pad_all(dot, 0, 0);
            }

            auto* controls = lv_label_create(scr);
            lv_label_set_text(controls, "A:new game  X:exit");
            lv_obj_set_pos(controls, 12, 212);
        }

        void rebuildScreen(const ProgramState& s) {
            if (s.turn == TurnState::SelectInputColor) {
                drawColorSelect(s);
            } else if (s.turn == TurnState::Finished) {
                drawRunPreview(s);
            } else {
                drawMain(s);
            }
        }

        void lvglFlushCallback(lv_disp_drv_t* drv, const lv_area_t* area, lv_color_t* color_p) {
            auto* ctx = static_cast<LvglUiContext*>(drv->user_data);
            if (ctx == nullptr) {
                lv_disp_flush_ready(drv);
                return;
            }

            const int32_t x1 = std::max<int32_t>(0, area->x1);
            const int32_t y1 = std::max<int32_t>(0, area->y1);
            const int32_t x2 = std::min<int32_t>(LCD_WIDTH - 1, area->x2);
            const int32_t y2 = std::min<int32_t>(LCD_HEIGHT - 1, area->y2);

            if (x2 < x1 || y2 < y1) {
                lv_disp_flush_ready(drv);
                return;
            }

            const int32_t flush_w = x2 - x1 + 1;
            const int32_t flush_h = y2 - y1 + 1;
            const int32_t src_w = area->x2 - area->x1 + 1;
            const int32_t src_x0 = x1 - area->x1;
            const int32_t src_y0 = y1 - area->y1;

            LCD_1IN3_SetWindows(
                static_cast<UWORD>(x1),
                static_cast<UWORD>(y1),
                static_cast<UWORD>(x2 + 1),
                static_cast<UWORD>(y2 + 1)
            );
            DEV_Digital_Write(LCD_DC_PIN, 1);
            DEV_Digital_Write(LCD_CS_PIN, 0);

            for (int32_t row = 0; row < flush_h; row++) {
                const lv_color_t* src = color_p + (src_y0 + row) * src_w + src_x0;
                auto* tx = ctx->lcd_tx_line_buffer.data();
                for (int32_t col = 0; col < flush_w; col++) {
                    const uint16_t color = src[col].full;
                    tx[col * 2] = static_cast<uint8_t>(color >> 8);
                    tx[col * 2 + 1] = static_cast<uint8_t>(color & 0xFF);
                }
                DEV_SPI_Write_nByte(tx, static_cast<uint32_t>(flush_w * 2));
            }
            DEV_Digital_Write(LCD_CS_PIN, 1);

            lv_disp_flush_ready(drv);
        }

        void initLvglDisplay() {
            lv_init();

            lv_disp_draw_buf_init(
                &g_lvgl_ui.lvgl_draw_buf,
                g_lvgl_ui.lvgl_draw_pixels.data(),
                nullptr,
                static_cast<uint32_t>(g_lvgl_ui.lvgl_draw_pixels.size())
            );

            lv_disp_drv_init(&g_lvgl_ui.lvgl_disp_drv);
            g_lvgl_ui.lvgl_disp_drv.hor_res = LCD_WIDTH;
            g_lvgl_ui.lvgl_disp_drv.ver_res = LCD_HEIGHT;
            g_lvgl_ui.lvgl_disp_drv.flush_cb = lvglFlushCallback;
            g_lvgl_ui.lvgl_disp_drv.draw_buf = &g_lvgl_ui.lvgl_draw_buf;
            g_lvgl_ui.lvgl_disp_drv.user_data = &g_lvgl_ui;
            lv_disp_drv_register(&g_lvgl_ui.lvgl_disp_drv);
        }

        void updateLvglTick(uint32_t elapsed_ms) {
            if (elapsed_ms > 0) {
                lv_tick_inc(elapsed_ms);
            }
        }

        void runLvglTasks() {
            lv_timer_handler();
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
    LCD_1IN3_Clear(RGB565_WHITE);
    hardware::initInfraredPins();
    ui::initLvglDisplay();
    startYdfWorker();

    ProgramState state;
    initProgramState(state);

    bool running = true;
    bool needs_redraw = true;
    uint32_t last_tick_ms = to_ms_since_boot(get_absolute_time());
    while (running) {
        const uint32_t now_ms = to_ms_since_boot(get_absolute_time());
        ui::updateLvglTick(now_ms - last_tick_ms);
        last_tick_ms = now_ms;
        ui::runLvglTasks();

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
                ui::rebuildScreen(state);
                ui::runLvglTasks();
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
                ui::rebuildScreen(state);
                ui::runLvglTasks();
                needs_redraw = false;
            }
            sleep_ms(12);
            continue;
        }

        if (state.turn == TurnState::AITurn) {
            performAITurn(state);
            ui::rebuildScreen(state);
            ui::runLvglTasks();
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
            ui::rebuildScreen(state);
            ui::runLvglTasks();
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

    stopYdfWorker();
    DEV_Module_Exit();
    return 0;
}

int main() {
    stdio_init_all();
    return LCD();
}
