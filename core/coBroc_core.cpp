#include "core/coBroc_core.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <random>

namespace coBroc::core {

// ── Predictor function pointer ──────────────────────────────────────────
// Default = synchronous direct prediction. Platform can override externally.
PredictorFn g_predictor_fn = nullptr;

// ── Turn name helper ────────────────────────────────────────────────────

const char* turnName(TurnState t) {
    switch (t) {
        case TurnState::PlayerTurn:       return "PLAYER";
        case TurnState::AITurn:           return "AI";
        case TurnState::SelectInputColor:  return "INPUT COLOR";
        case TurnState::RunProgram:        return "RUN";
        case TurnState::Finished:          return "DONE";
        default:                           return "-";
    }
}

// ── State init ──────────────────────────────────────────────────────────

void initProgramState(ProgramState& s) {
    const uint16_t next_game = static_cast<uint16_t>(s.game_id + 1);
    uint32_t saved_seed = s.rng_seed;  // preserve seed across reset if set externally
    s = ProgramState{};
    s.game_id = next_game;
    s.rng_seed = saved_seed;
    s.selected_block = BlockType::Move;
    s.selected_param = minParamForBlock(BlockType::Move);
    s.run_input_color = COLOR_PARAM_MIN;
    normalizeSelectedBlockType(s);
}

// ── Legality ────────────────────────────────────────────────────────────

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
            if (open_top > 0) open_top--;
            continue;
        }
        if (t == BlockType::Move || t == BlockType::If || t == BlockType::Repeat) {
            if (open_top < MAX_MOVES) open_stack[open_top++] = t;
            continue;
        }
        if (t == BlockType::Else && open_top > 0 && open_stack[open_top - 1] == BlockType::If) {
            open_stack[open_top - 1] = BlockType::Else;
        }
    }
    for (uint8_t i = 0; i < open_top; i++) {
        if (open_stack[i] == BlockType::Move) return true;
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
            if (open_top >= MAX_MOVES) return false;
            open_types[open_top] = t;
            if_has_else[open_top] = false;
            open_top++;
            continue;
        }
        if (t == BlockType::Else) {
            if (open_top == 0 || open_types[open_top - 1] != BlockType::If || if_has_else[open_top - 1]) return false;
            if_has_else[open_top - 1] = true;
            continue;
        }
        if (t == BlockType::End) {
            if (open_top == 0) return false;
            open_top--;
        }
    }
    return open_top > 0 && open_types[open_top - 1] == BlockType::If && !if_has_else[open_top - 1];
}

bool blockAllowedByDepth(const ProgramState& s, BlockType t) {
    if (t == BlockType::Else) return canOpenElseBranch(s);
    if (t == BlockType::End) return s.syntax_depth > 0;
    if ((t == BlockType::Move || t == BlockType::If || t == BlockType::Repeat) && s.syntax_depth >= MAX_NEST_DEPTH)
        return false;
    if (insideMoveScope(s) && (t == BlockType::If || t == BlockType::Else || t == BlockType::Repeat))
        return false;
    return true;
}

bool isLegalCandidate(const ProgramState& s, BlockType t, uint8_t param) {
    if (!isPlayableBlock(t) || !blockAllowedByDepth(s, t)) return false;
    if (s.move_count > 0) {
        const BlockType last_type = s.program[s.move_count - 1].type;
        if ((last_type == BlockType::Repeat || last_type == BlockType::If) && t == last_type) return false;
    }
    if (t == BlockType::Else) return param == 0;
    if (t == BlockType::Move && (param < MOVE_PARAM_MIN || param > MOVE_PARAM_MAX)) return false;
    if ((t == BlockType::Draw || t == BlockType::If) && (param < COLOR_PARAM_MIN || param > COLOR_PARAM_MAX))
        return false;
    if (t == BlockType::Repeat && (param < 1 || param > MAX_REPEAT)) return false;
    if (t == BlockType::End && param != 0) return false;
    return true;
}

// ── Program manipulation ────────────────────────────────────────────────

void rememberHistory(ProgramState& s, BlockType t) {
    const uint8_t idx = blockIndex(t);
    if (s.history_size > 0) {
        const uint8_t prev = blockIndex(s.history[(s.history_size - 1) % MAX_HISTORY]);
        s.transitions[prev][idx]++;
    }
    s.history[s.history_size % MAX_HISTORY] = t;
    if (s.history_size < MAX_HISTORY) s.history_size++;
    s.block_frequency[idx]++;
}

void ensureSelectionVisible(ProgramState& s) {
    if (s.selected_line < s.scroll_top) {
        s.scroll_top = s.selected_line;
        return;
    }
    const uint8_t bottom = static_cast<uint8_t>(s.scroll_top + LIST_VISIBLE - 1);
    if (s.selected_line > bottom)
        s.scroll_top = static_cast<uint8_t>(s.selected_line - (LIST_VISIBLE - 1));
}

void recalcViewDepths(ProgramState& s) {
    uint8_t depth = 0;
    for (uint8_t i = 0; i < s.move_count; i++) {
        const BlockType t = s.program[i].type;
        if (t == BlockType::End) {
            if (depth > 0) depth--;
            s.view_depths[i] = depth;
            continue;
        }
        s.view_depths[i] = depth;
        if (t == BlockType::Move || t == BlockType::If || t == BlockType::Repeat)
            depth = static_cast<uint8_t>(std::min<uint8_t>(MAX_NEST_DEPTH, depth + 1));
    }
}

bool addStepToProgram(ProgramState& s, BlockType t, uint8_t param, bool from_ai) {
    if (s.move_count >= MAX_MOVES) return false;
    if (!isLegalCandidate(s, t, param)) return false;
    s.program[s.move_count] = {t, param, from_ai};
    s.move_count++;
    s.selected_line = static_cast<uint8_t>(s.move_count - 1);

    if (t == BlockType::Move || t == BlockType::If || t == BlockType::Repeat)
        s.syntax_depth++;
    else if (t == BlockType::End && s.syntax_depth > 0)
        s.syntax_depth--;

    rememberHistory(s, t);
    recalcViewDepths(s);
    ensureSelectionVisible(s);
    normalizeSelectedBlockType(s);
    return true;
}

// ── Selection ───────────────────────────────────────────────────────────

void normalizeSelectedBlockType(ProgramState& s) {
    const BlockType current = s.selected_block;
    uint8_t current_param = 0;
    if (blockHasParam(current)) {
        const uint8_t minp = minParamForBlock(current);
        const uint8_t maxp = maxParamForBlock(current);
        current_param = s.selected_param < minp ? minp : s.selected_param;
        if (current_param > maxp) current_param = minp;
    }
    if (isLegalCandidate(s, current, current_param)) {
        s.selected_param = blockHasParam(current) ? current_param : 0;
        return;
    }
    for (const BlockType t : kPlayableBlocks) {
        const uint8_t cand_param = blockHasParam(t) ? minParamForBlock(t) : 0;
        if (!isLegalCandidate(s, t, cand_param)) continue;
        s.selected_block = t;
        s.selected_param = cand_param;
        return;
    }
}

void cycleParam(ProgramState& s) {
    if (!blockHasParam(s.selected_block)) return;
    const uint8_t minp = minParamForBlock(s.selected_block);
    const uint8_t maxp = maxParamForBlock(s.selected_block);
    const uint8_t base = s.selected_param < minp ? minp : s.selected_param;
    s.selected_param = base >= maxp ? minp : static_cast<uint8_t>(base + 1);
}

void cycleBlockType(ProgramState& s) {
    BlockType next = s.selected_block;
    bool found = false;
    for (size_t i = 0; i < kPlayableBlocks.size(); i++) {
        next = nextPlayableBlock(next);
        const uint8_t cand_param = blockHasParam(next) ? minParamForBlock(next) : 0;
        if (!isLegalCandidate(s, next, cand_param)) continue;
        found = true;
        break;
    }
    if (!found) return;
    s.selected_block = next;
    const uint8_t minp = minParamForBlock(next);
    const uint8_t maxp = maxParamForBlock(next);
    if (blockHasParam(next) && (s.selected_param < minp || s.selected_param > maxp))
        s.selected_param = minp;
    if (!blockHasParam(next)) s.selected_param = 0;
}

// ── AI helpers ──────────────────────────────────────────────────────────

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
    f.actor = 1u;
    f.feedback_penalty = c.feedback_penalty;
    return f;
}

std::array<AICandidate, AI_CANDIDATE_SLOTS> buildCandidates(const ProgramState& s, uint8_t& out_count) {
    std::array<AICandidate, AI_CANDIDATE_SLOTS> cands{};
    out_count = 0;
    for (BlockType t : kPlayableBlocks) {
        // AI は END ブロックを設置しない(スコープを閉じるのはプレイヤーの役目)
        if (t == BlockType::End) continue;

        uint8_t minp = 0, maxp = 0;
        if (t == BlockType::Move)       { minp = MOVE_PARAM_MIN;   maxp = MOVE_PARAM_MAX;   }
        else if (t == BlockType::Draw || t == BlockType::If) { minp = COLOR_PARAM_MIN; maxp = COLOR_PARAM_MAX; }
        else if (t == BlockType::Repeat){ minp = 1;                maxp = MAX_REPEAT;        }
        else if (t == BlockType::Else)  { minp = 0;                maxp = 0;                }
        else                             { minp = 0;                maxp = 0;                }

        for (uint8_t p = minp; p <= maxp; p++) {
            if (out_count >= cands.size()) break;
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

void pruneCandidatesForPrediction(const ProgramState& s,
                                  std::array<AICandidate, AI_CANDIDATE_SLOTS>& candidates, uint8_t count) {
    if (count <= AI_MAX_PREDICT_CANDIDATES) return;

    std::array<uint8_t, AI_CANDIDATE_SLOTS> legal_indices{};
    uint8_t legal_count = 0;
    for (uint8_t i = 0; i < count; i++) {
        if (!candidates[i].legal) continue;
        legal_indices[legal_count++] = i;
    }
    if (legal_count <= AI_MAX_PREDICT_CANDIDATES) return;

    std::array<float, AI_CANDIDATE_SLOTS> heuristics{};
    for (uint8_t i = 0; i < legal_count; i++) {
        auto& c = candidates[legal_indices[i]];
        float h = 0.0f;
        if (s.history_size > 0) {
            const uint8_t last_type = blockIndex(s.history[(s.history_size - 1) % MAX_HISTORY]);
            h += static_cast<float>(s.transitions[last_type][blockIndex(c.type)]) * 0.05f;
        }
        h -= static_cast<float>(s.block_frequency[blockIndex(c.type)]) * 0.02f;
        if (c.type == BlockType::Draw)       h += 0.03f;
        else if (c.type == BlockType::End && s.syntax_depth > 0) h += 0.04f;
        else                                  h += 0.02f;
        heuristics[legal_indices[i]] = h;
    }

    std::array<bool, AI_CANDIDATE_SLOTS> keep{};
    for (uint8_t pick = 0; pick < AI_MAX_PREDICT_CANDIDATES; pick++) {
        int best = -1;
        for (uint8_t i = 0; i < legal_count; i++) {
            const uint8_t idx = legal_indices[i];
            if (keep[idx]) continue;
            if (best < 0 || heuristics[idx] > heuristics[static_cast<size_t>(best)])
                best = static_cast<int>(idx);
        }
        if (best < 0) break;
        keep[static_cast<uint8_t>(best)] = true;
    }
    for (uint8_t i = 0; i < count; i++) {
        if (candidates[i].legal && !keep[i]) {
            candidates[i].legal = false;
            candidates[i].suitability = -1.0f;
        }
    }
}

// ── Synchronous prediction (default / fallback) ─────────────────────────

void predictCandidatesSync(const ProgramState& s,
                            std::array<AICandidate, AI_CANDIDATE_SLOTS>& candidates,
                            const std::array<uint8_t, AI_CANDIDATE_SLOTS>& indices, uint8_t index_count) {
    for (uint8_t i = 0; i < index_count; i++) {
        auto& cand = candidates[indices[i]];
        const auto pred = coBroc::ydf::Model::Predict(makeYdfFeatures(s, cand));
        cand.suitability = pred.ok ? pred.suitability_score : -1.0f;
    }
}

static void dispatchPredict(const ProgramState& s,
                            std::array<AICandidate, AI_CANDIDATE_SLOTS>& candidates,
                            const std::array<uint8_t, AI_CANDIDATE_SLOTS>& indices, uint8_t index_count) {
    if (g_predictor_fn)
        g_predictor_fn(s, candidates, indices, index_count);
    else
        predictCandidatesSync(s, candidates, indices, index_count);
}

void predictAllLegalCandidates(const ProgramState& s,
                               std::array<AICandidate, AI_CANDIDATE_SLOTS>& candidates, uint8_t count) {
    std::array<uint8_t, AI_CANDIDATE_SLOTS> predict_indices{};
    uint8_t predict_count = 0;
    for (uint8_t i = 0; i < count; i++) {
        if (!candidates[i].legal) {
            candidates[i].suitability = -1.0f;
            continue;
        }
        predict_indices[predict_count++] = i;
    }
    dispatchPredict(s, candidates, predict_indices, predict_count);
}

bool chooseBestCandidate(const std::array<AICandidate, AI_CANDIDATE_SLOTS>& cands, uint8_t count, uint8_t& chosen) {
    chosen = 255;
    for (uint8_t i = 0; i < count; i++) {
        const auto& c = cands[i];
        if (!c.legal) continue;
        if (chosen == 255 || c.suitability > cands[chosen].suitability) chosen = i;
    }
    return chosen != 255;
}

bool chooseBestExcludingType(const std::array<AICandidate, AI_CANDIDATE_SLOTS>& candidates, uint8_t count,
                             BlockType excluded, uint8_t& chosen) {
    chosen = 255;
    for (uint8_t i = 0; i < count; i++) {
        const auto& c = candidates[i];
        if (!c.legal || c.type == excluded) continue;
        if (chosen == 255 || c.suitability > candidates[chosen].suitability) chosen = i;
    }
    return chosen != 255;
}

uint8_t tailTypeStreak(const ProgramState& s, BlockType t) {
    uint8_t streak = 0;
    for (int i = static_cast<int>(s.move_count) - 1; i >= 0; --i) {
        if (s.program[static_cast<size_t>(i)].type != t) break;
        streak++;
    }
    return streak;
}

bool violatesAIMoveRule(const ProgramState& s, const AICandidate& c) {
    if (c.type != BlockType::Draw || s.move_count == 0) return false;
    for (int i = static_cast<int>(s.move_count) - 1; i >= 0; --i) {
        const BlockType t = s.program[static_cast<size_t>(i)].type;
        if (t == BlockType::End) continue;
        if (t == BlockType::Move) return false;
        break;
    }
    if (s.move_count > 0) {
        const auto& last = s.program[s.move_count - 1];
        if (last.type == BlockType::Draw && last.param == c.param) return true;
    }
    return false;
}

void applyRuleFeedbackAndRescore(const ProgramState& s,
                                 std::array<AICandidate, AI_CANDIDATE_SLOTS>& cands, uint8_t count) {
    std::array<uint8_t, AI_CANDIDATE_SLOTS> rescore_indices{};
    uint8_t rescore_count = 0;
    for (uint8_t i = 0; i < count; i++) {
        auto& c = cands[i];
        if (!c.legal) continue;
        if (!violatesAIMoveRule(s, c)) continue;
        c.feedback_penalty = static_cast<uint8_t>(std::min<uint16_t>(255, c.feedback_penalty + 1));
        rescore_indices[rescore_count++] = i;
    }
    if (rescore_count > 0)
        dispatchPredict(s, cands, rescore_indices, rescore_count);

    const uint8_t draw_streak = tailTypeStreak(s, BlockType::Draw);
    for (uint8_t i = 0; i < count; i++) {
        auto& c = cands[i];
        if (!c.legal) continue;

        if (c.type == BlockType::Draw) {
            c.suitability -= 0.12f * static_cast<float>(draw_streak);
            if (s.move_count > 0 && s.program[s.move_count - 1].type == BlockType::Draw &&
                s.program[s.move_count - 1].param == c.param)
                c.suitability -= 0.10f;
        } else {
            const uint8_t type_freq = s.block_frequency[blockIndex(c.type)];
            if (type_freq == 0)       c.suitability += 0.08f;
            else if (type_freq == 1)  c.suitability += 0.04f;
            if (c.type == BlockType::End && s.syntax_depth > 0) c.suitability += 0.10f;
        }
    }
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
            if (draw_streak >= 2 || gap <= 0.08f) chosen = non_draw;
        }
    }
    // AI は END を候補に含めない(buildCandidates で除外)ため、
    // プログラムを閉じる最終手は必ずプレイヤーが行う。

    if (!addStepToProgram(s, candidates[chosen].type, candidates[chosen].param, true)) {
        s.turn = TurnState::RunProgram;
        return;
    }
    // 盤面満杯でゲーム終了(AI は END を置かないので syntax_depth は減らない)
    if (s.move_count >= MAX_MOVES)
        s.turn = TurnState::RunProgram;
    else
        s.turn = TurnState::PlayerTurn;
}

// ── Drawing (RNG passed explicitly) ─────────────────────────────────────
// NOTE: We avoid std::uniform_int_distribution here because libstdc++
// (native) and libc++ (emscripten) consume different numbers of underlying
// RNG draws for the same distribution, which would break cross-platform
// replay determinism. mt19937::operator() is specified by the standard and
// is identical on all platforms, so a modulo-based range is used instead.

static uint32_t rngNext(std::mt19937& rng) {
    return rng();
}

static uint8_t rngInRange(std::mt19937& rng, uint8_t lo, uint8_t hi) {
    const uint32_t range = static_cast<uint32_t>(hi) - static_cast<uint32_t>(lo) + 1u;
    return static_cast<uint8_t>(lo + rngNext(rng) % range);
}

void addDrawCircle(RuntimeState& runtime, uint8_t color, uint8_t move_steps, std::mt19937& rng) {
    if (runtime.circle_count >= MAX_DRAW_EVENTS) return;

    auto& circle = runtime.circles[runtime.circle_count++];
    circle.color = color;
    circle.x = rngInRange(rng, RESULT_MIN_COORD, RESULT_MAX_X);
    circle.y = rngInRange(rng, RESULT_MIN_COORD, RESULT_MAX_Y);

    for (uint8_t i = 0; i < move_steps; i++) {
        const int8_t dx = static_cast<int8_t>(rngInRange(rng, RANDOM_STEP_MIN, RANDOM_STEP_MAX)) - RANDOM_STEP_OFFSET;
        const int8_t dy = static_cast<int8_t>(rngInRange(rng, RANDOM_STEP_MIN, RANDOM_STEP_MAX)) - RANDOM_STEP_OFFSET;
        int16_t nx = static_cast<int16_t>(circle.x) + dx * RANDOM_STEP_PIXELS;
        int16_t ny = static_cast<int16_t>(circle.y) + dy * RANDOM_STEP_PIXELS;
        circle.x = static_cast<uint8_t>(std::clamp<int16_t>(nx, RESULT_MIN_COORD, RESULT_MAX_X));
        circle.y = static_cast<uint8_t>(std::clamp<int16_t>(ny, RESULT_MIN_COORD, RESULT_MAX_Y));
    }
}

// ── Compile & Run ───────────────────────────────────────────────────────

void finalizeSyntax(ProgramState& s) {
    while (s.syntax_depth > 0 && s.move_count < MAX_MOVES)
        addStepToProgram(s, BlockType::End, 0, true);
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
            if (top >= MAX_MOVES) return false;
            stack[top++] = pc;
            continue;
        }
        if (t == BlockType::Else) {
            if (top == 0) return false;
            const uint8_t if_pc = stack[top - 1];
            if (s.program[if_pc].type != BlockType::If || cfg.if_else[if_pc] >= 0) return false;
            cfg.if_else[if_pc] = static_cast<int8_t>(pc);
            cfg.else_if[pc] = static_cast<int8_t>(if_pc);
            continue;
        }
        if (t == BlockType::End) {
            if (top == 0) return false;
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
                if (cfg.block_end[pc] < 0) return false;
                ins.op = VmOp::EnterMove;
                ins.param = step.param;
                break;
            case BlockType::Draw:
                ins.op = VmOp::Draw;
                ins.param = step.param;
                break;
            case BlockType::If:
                if (cfg.block_end[pc] < 0) return false;
                ins.op = VmOp::IfColorMismatchJump;
                ins.param = step.param;
                if (cfg.if_else[pc] >= 0)
                    ins.jump_pc = static_cast<uint8_t>(cfg.if_else[pc] + 1);
                else
                    ins.jump_pc = static_cast<uint8_t>(cfg.block_end[pc] + 1);
                break;
            case BlockType::Else: {
                const int8_t if_pc = cfg.else_if[pc];
                if (if_pc < 0 || cfg.block_end[static_cast<uint8_t>(if_pc)] < 0) return false;
                ins.op = VmOp::Jump;
                ins.jump_pc = static_cast<uint8_t>(cfg.block_end[static_cast<uint8_t>(if_pc)] + 1);
                break;
            }
            case BlockType::Repeat:
                if (cfg.block_end[pc] < 0) return false;
                ins.op = VmOp::RepeatBegin;
                ins.param = step.param;
                break;
            case BlockType::End: {
                const int8_t start = cfg.end_start[pc];
                if (start < 0) return false;
                const BlockType open = s.program[static_cast<uint8_t>(start)].type;
                if (open == BlockType::Move)        ins.op = VmOp::ExitMove;
                else if (open == BlockType::If)     ins.op = VmOp::Nop;
                else if (open == BlockType::Repeat) {
                    ins.op = VmOp::RepeatEnd;
                    ins.jump_pc = static_cast<uint8_t>(start + 1);
                } else return false;
                break;
            }
            default: return false;
        }
    }
    return true;
}

bool executeProgram(const ProgramState& s, const CompiledProgram& program, RuntimeState& runtime,
                    std::mt19937& rng) {
    runtime = RuntimeState{};
    std::array<uint8_t, MAX_NEST_DEPTH> move_step_stack{};
    uint8_t move_anchor_top = 0;
    std::array<RepeatRuntimeFrame, MAX_NEST_DEPTH> repeat_stack{};
    uint8_t repeat_top = 0;

    uint8_t pc = 0;
    while (pc < program.instruction_count) {
        const VmInstruction& ins = program.instructions[pc];
        switch (ins.op) {
            case VmOp::Nop: pc++; break;
            case VmOp::EnterMove:
                if (move_anchor_top >= MAX_NEST_DEPTH) return false;
                move_step_stack[move_anchor_top++] = ins.param;
                pc++;
                break;
            case VmOp::ExitMove:
                if (move_anchor_top == 0) return false;
                move_anchor_top--;
                pc++;
                break;
            case VmOp::Draw:
                addDrawCircle(runtime, colorIndexFromParam(ins.param),
                              move_anchor_top > 0 ? move_step_stack[move_anchor_top - 1] : 0, rng);
                pc++;
                break;
            case VmOp::IfColorMismatchJump: {
                const bool cond = colorIndexFromParam(s.run_input_color) == colorIndexFromParam(ins.param);
                pc = cond ? static_cast<uint8_t>(pc + 1) : ins.jump_pc;
                break;
            }
            case VmOp::Jump: pc = ins.jump_pc; break;
            case VmOp::RepeatBegin:
                if (repeat_top >= MAX_NEST_DEPTH) return false;
                repeat_stack[repeat_top++] = {pc, ins.param};
                pc++;
                break;
            case VmOp::RepeatEnd:
                if (repeat_top == 0) return false;
                if (repeat_stack[repeat_top - 1].remaining > 1) {
                    repeat_stack[repeat_top - 1].remaining--;
                    pc = ins.jump_pc;
                } else {
                    repeat_top--;
                    pc++;
                }
                break;
            default: return false;
        }
    }
    return repeat_top == 0 && move_anchor_top == 0;
}

bool compileAndRun(ProgramState& s, std::mt19937& rng) {
    finalizeSyntax(s);

    ControlFlowGraph cfg{};
    if (!buildControlFlowGraph(s, cfg)) return false;

    CompiledProgram compiled{};
    if (!compileProgram(s, cfg, compiled)) return false;

    RuntimeState runtime{};
    if (!executeProgram(s, compiled, runtime, rng)) return false;

    s.runtime = runtime;
    return true;
}

// ── Flow helpers (shared between LVGL and WASM renderers) ───────────────

uint8_t flowItemCount(const ProgramState& s) {
    return static_cast<uint8_t>(std::min<uint8_t>(MAX_MOVES + 1, static_cast<uint8_t>(s.move_count + 1)));
}

uint8_t focusFlowIndex(const ProgramState& s) {
    if (s.move_count == 0) return 0;
    return static_cast<uint8_t>(std::min<uint8_t>(s.move_count, static_cast<uint8_t>(s.selected_line + 1)));
}

uint8_t visibleFlowCount() {
    const int row_space = 34 + 12;  // FLOW_NODE_HEIGHT + FLOW_NODE_GAP
    const int available = 186 - 8 + 12;  // UI_LIST_HEIGHT - FLOW_TOP_PADDING + GAP (approximate)
    const int rows = available / row_space;
    return static_cast<uint8_t>(std::max<int>(1, std::min<int>(rows, MAX_MOVES + 1)));
}

uint8_t flowTopIndex(const ProgramState& s) {
    const uint8_t total = flowItemCount(s);
    const uint8_t visible = visibleFlowCount();
    if (total <= visible) return 0;
    const uint8_t focus = focusFlowIndex(s);
    if (focus + 1 <= visible) return 0;
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
    if (flow_index == 0 || !blockHasParam(step.type)) return 0;
    if (flow_index != static_cast<uint8_t>(s.selected_line + 1)) return step.param;
    const uint8_t minp = minParamForBlock(step.type);
    const uint8_t maxp = maxParamForBlock(step.type);
    if (s.selected_param < minp || s.selected_param > maxp) return minp;
    return s.selected_param;
}

} // namespace coBroc::core
