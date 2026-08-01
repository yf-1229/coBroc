#pragma once

#ifndef COBROC_CORE_H
#define COBROC_CORE_H

#include <array>
#include <cstdint>
#include <random>
#include "core/model.h"

namespace coBroc::core {

// ── Constants ───────────────────────────────────────────────────────────

constexpr uint8_t MAX_MOVES = 16;
constexpr uint8_t MAX_HISTORY = 32;
constexpr uint8_t MAX_NEST_DEPTH = 6;
constexpr uint8_t LIST_VISIBLE = 9;

constexpr uint16_t LCD_WIDTH = 240;
constexpr uint16_t LCD_HEIGHT = 240;

constexpr uint8_t COLOR_COUNT = 8;
constexpr uint8_t COLOR_PARAM_MIN = 1;
constexpr uint8_t COLOR_PARAM_MAX = COLOR_COUNT;
constexpr uint8_t MOVE_PARAM_MIN = 1;
constexpr uint8_t MOVE_PARAM_MAX = 19;
constexpr uint8_t MAX_REPEAT = 7;

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

// ── Enums ───────────────────────────────────────────────────────────────

enum class BlockType : uint8_t {
    None = 0,
    Move = 1,
    Draw = 2,
    If = 3,
    Repeat = 4,
    End = 5,
    Else = 6
};

enum class TurnState : uint8_t {
    PlayerTurn = 0,
    AITurn = 1,
    SelectInputColor = 2,
    RunProgram = 3,
    Finished = 4
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

// ── Block helpers ───────────────────────────────────────────────────────

constexpr uint8_t blockIndex(BlockType t) {
    return static_cast<uint8_t>(t);
}

constexpr std::array<BlockType, 6> kPlayableBlocks = {
    BlockType::Move, BlockType::Draw, BlockType::If, BlockType::Else, BlockType::Repeat, BlockType::End
};

constexpr BlockType nextPlayableBlock(BlockType t) {
    switch (t) {
        case BlockType::Move:  return BlockType::Draw;
        case BlockType::Draw:  return BlockType::If;
        case BlockType::If:    return BlockType::Else;
        case BlockType::Else:  return BlockType::Repeat;
        case BlockType::Repeat:return BlockType::End;
        case BlockType::End:
        case BlockType::None:
        default:               return BlockType::Move;
    }
}

constexpr std::array<const char*, 7> kBlockNames = {
    "NONE", "MOVE", "DRAW", "IF", "REPEAT", "END", "ELSE"
};

constexpr const char* blockName(BlockType t) {
    return kBlockNames[blockIndex(t)];
}

constexpr uint8_t minParamForBlock(BlockType t) {
    switch (t) {
        case BlockType::Move:   return MOVE_PARAM_MIN;
        case BlockType::Draw:
        case BlockType::If:     return COLOR_PARAM_MIN;
        case BlockType::Repeat: return 1;
        default:                return 0;
    }
}

constexpr uint8_t maxParamForBlock(BlockType t) {
    switch (t) {
        case BlockType::Move:   return MOVE_PARAM_MAX;
        case BlockType::Draw:
        case BlockType::If:     return COLOR_PARAM_MAX;
        case BlockType::Repeat: return MAX_REPEAT;
        default:                return 0;
    }
}

constexpr bool blockHasParam(BlockType t) {
    return t == BlockType::Move || t == BlockType::Draw || t == BlockType::If || t == BlockType::Repeat;
}

constexpr uint8_t colorIndexFromParam(uint8_t param) {
    if (param < COLOR_PARAM_MIN) return 0;
    return static_cast<uint8_t>((param - COLOR_PARAM_MIN) % COLOR_COUNT);
}

constexpr uint16_t paintColorByParam(uint8_t param) {
    return kPaintColors[colorIndexFromParam(param)];
}

const char* turnName(TurnState t);

// ── Data structures ─────────────────────────────────────────────────────

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

    // Per-game RNG seed (for deterministic replay)
    uint32_t rng_seed = 0;
};

// ── Function declarations ───────────────────────────────────────────────

// State init
void initProgramState(ProgramState& s);

// Legality checks
bool isPlayableBlock(BlockType t);
bool insideMoveScope(const ProgramState& s);
bool canOpenElseBranch(const ProgramState& s);
bool blockAllowedByDepth(const ProgramState& s, BlockType t);
bool isLegalCandidate(const ProgramState& s, BlockType t, uint8_t param);

// Program manipulation
void rememberHistory(ProgramState& s, BlockType t);
void ensureSelectionVisible(ProgramState& s);
void recalcViewDepths(ProgramState& s);
bool addStepToProgram(ProgramState& s, BlockType t, uint8_t param, bool from_ai);

// Selection
void normalizeSelectedBlockType(ProgramState& s);
void cycleParam(ProgramState& s);
void cycleBlockType(ProgramState& s);

// AI
coBroc::ydf::CandidateFeatures makeYdfFeatures(const ProgramState& s, const AICandidate& c);
std::array<AICandidate, AI_CANDIDATE_SLOTS> buildCandidates(const ProgramState& s, uint8_t& out_count);
void pruneCandidatesForPrediction(const ProgramState& s,
                                  std::array<AICandidate, AI_CANDIDATE_SLOTS>& candidates, uint8_t count);

// Predictor interface: platform sets g_predictor_fn before AI turn.
// Default (nullptr) == synchronous direct Predict.
using PredictorFn = void (*)(const ProgramState& s, std::array<AICandidate, AI_CANDIDATE_SLOTS>& candidates,
                              const std::array<uint8_t, AI_CANDIDATE_SLOTS>& indices, uint8_t index_count);
extern PredictorFn g_predictor_fn;

void predictCandidatesSync(const ProgramState& s,
                            std::array<AICandidate, AI_CANDIDATE_SLOTS>& candidates,
                            const std::array<uint8_t, AI_CANDIDATE_SLOTS>& indices, uint8_t index_count);
void predictAllLegalCandidates(const ProgramState& s,
                               std::array<AICandidate, AI_CANDIDATE_SLOTS>& candidates, uint8_t count);

bool chooseBestCandidate(const std::array<AICandidate, AI_CANDIDATE_SLOTS>& cands, uint8_t count, uint8_t& chosen);
bool chooseBestExcludingType(const std::array<AICandidate, AI_CANDIDATE_SLOTS>& candidates, uint8_t count,
                             BlockType excluded, uint8_t& chosen);
uint8_t tailTypeStreak(const ProgramState& s, BlockType t);
bool violatesAIMoveRule(const ProgramState& s, const AICandidate& c);
void applyRuleFeedbackAndRescore(const ProgramState& s,
                                 std::array<AICandidate, AI_CANDIDATE_SLOTS>& cands, uint8_t count);

void performAITurn(ProgramState& s);

// Drawing
void addDrawCircle(RuntimeState& runtime, uint8_t color, uint8_t move_steps,
                   std::mt19937& rng);

// Compile & Run
void finalizeSyntax(ProgramState& s);
bool buildControlFlowGraph(const ProgramState& s, ControlFlowGraph& cfg);
bool compileProgram(const ProgramState& s, const ControlFlowGraph& cfg, CompiledProgram& out);
bool executeProgram(const ProgramState& s, const CompiledProgram& program, RuntimeState& runtime,
                    std::mt19937& rng);
bool compileAndRun(ProgramState& s, std::mt19937& rng);

// Flow helpers (for UI)
uint8_t flowItemCount(const ProgramState& s);
uint8_t focusFlowIndex(const ProgramState& s);
uint8_t flowTopIndex(const ProgramState& s);
uint8_t visibleFlowCount();
ProgramStep flowStep(const ProgramState& s, uint8_t flow_index);
uint8_t shownParam(const ProgramState& s, const ProgramStep& step, uint8_t flow_index);

} // namespace coBroc::core

#endif // COBROC_CORE_H
