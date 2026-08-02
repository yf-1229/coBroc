// core_test.cpp — Host-side determinism test for the coBroc core module.
//
// Runs scripted games with a fixed seed and prints:
//   1. The actual action log (player moves + AI moves + run result)
//   2. A compact FNV-1a hash of the full final state
//
// The output must be identical across platforms (host g++, WASM, Pico).
// This gives us a parity baseline for the WASM build (Phase 2).
//
// Build (from repo root):
//   g++ -std=c++17 -O2 -I. core/test/core_test.cpp core/coBroc_core.cpp -o /tmp/core_test
//   /tmp/core_test

#include "core/coBroc_core.h"

#include <cstdio>
#include <cstdint>
#include <random>
#include <vector>

using namespace coBroc::core;

namespace {

// ── FNV-1a 32-bit hash ──────────────────────────────────────────────────

uint32_t fnv1a(uint32_t h, const void* data, size_t n) {
    const auto* p = static_cast<const uint8_t*>(data);
    for (size_t i = 0; i < n; i++) {
        h ^= p[i];
        h *= 16777619u;
    }
    return h;
}

uint32_t hashProgramState(const ProgramState& s) {
    uint32_t h = 2166136261u;
    h = fnv1a(h, &s.game_id, sizeof(s.game_id));
    h = fnv1a(h, &s.history_size, sizeof(s.history_size));
    h = fnv1a(h, &s.move_count, sizeof(s.move_count));
    h = fnv1a(h, &s.selected_line, sizeof(s.selected_line));
    h = fnv1a(h, &s.scroll_top, sizeof(s.scroll_top));
    h = fnv1a(h, &s.selected_block, sizeof(s.selected_block));
    h = fnv1a(h, &s.selected_param, sizeof(s.selected_param));
    h = fnv1a(h, &s.syntax_depth, sizeof(s.syntax_depth));
    h = fnv1a(h, &s.run_input_color, sizeof(s.run_input_color));
    h = fnv1a(h, &s.turn, sizeof(s.turn));
    h = fnv1a(h, &s.rng_seed, sizeof(s.rng_seed));
    h = fnv1a(h, s.program.data(), s.program.size() * sizeof(ProgramStep));
    h = fnv1a(h, s.history.data(), s.history.size() * sizeof(BlockType));
    h = fnv1a(h, s.block_frequency.data(), s.block_frequency.size() * sizeof(uint8_t));
    h = fnv1a(h, s.transitions.data(), s.transitions.size() * sizeof(std::array<uint16_t, 8>));
    h = fnv1a(h, s.view_depths.data(), s.view_depths.size() * sizeof(uint8_t));
    // Runtime result
    h = fnv1a(h, &s.runtime.circle_count, sizeof(s.runtime.circle_count));
    h = fnv1a(h, s.runtime.circles.data(), s.runtime.circles.size() * sizeof(RuntimeState::DrawCircle));
    return h;
}

// ── Player policy ───────────────────────────────────────────────────────
// Consumes the plan in order; if the intended block is illegal (e.g. AI
// closed a scope), falls back to the first legal candidate. Deterministic.

void playerPlaces(ProgramState& s, const std::vector<std::pair<BlockType, uint8_t>>& plan, size_t& step) {
    while (step < plan.size()) {
        const auto [type, param] = plan[step];
        step++;
        if (!isLegalCandidate(s, type, param)) {
            std::printf("  P: %s(%u) [skipped: illegal]\n", blockName(type), param);
            continue;
        }
        if (addStepToProgram(s, type, param, false)) {
            std::printf("  P: %s(%u)\n", blockName(type), param);
            return;
        }
        std::printf("  P: %s(%u) [skipped: add failed]\n", blockName(type), param);
    }
    for (BlockType t : kPlayableBlocks) {
        const uint8_t p = blockHasParam(t) ? minParamForBlock(t) : 0;
        if (!isLegalCandidate(s, t, p)) continue;
        if (addStepToProgram(s, t, p, false)) {
            std::printf("  P: %s(%u) [fallback]\n", blockName(t), p);
            return;
        }
    }
}

// ── Scripted game ───────────────────────────────────────────────────────

void playGame(const char* name, uint32_t seed,
              const std::vector<std::pair<BlockType, uint8_t>>& plan,
              uint8_t input_color) {
    ProgramState s;
    initProgramState(s);
    s.rng_seed = seed;

    std::printf("== %s (seed=%u) ==\n", name, seed);
    std::printf("  game_id=%u\n", s.game_id);

    size_t plan_step = 0;
    while (s.turn == TurnState::PlayerTurn && s.move_count < MAX_MOVES) {
        if (plan_step >= plan.size()) {
            // Player presses Y: run early (finalizeSyntax closes open scopes)
            std::printf("  P: Y (run early, count=%u depth=%u)\n", s.move_count, s.syntax_depth);
            break;
        }
        playerPlaces(s, plan, plan_step);
        std::printf("  count=%u depth=%u\n", s.move_count, s.syntax_depth);

        // プレイヤーの手でプログラム完成(END で最後のスコープを閉じた)
        // → 実ゲームでは color_select へ遷移し、それ以降は追加できない。
        // トップレベルでの DRAW など(最後のブロックが END でない)は完成ではない。
        if (s.move_count > 0 &&
            s.program[s.move_count - 1].type == BlockType::End &&
            s.syntax_depth == 0) {
            std::printf("  P: program complete (END closed depth=0) -> color_select\n");
            break;
        }
        if (s.move_count >= MAX_MOVES) break;
        if (s.turn != TurnState::PlayerTurn) break;

        performAITurn(s);
        const auto& last = s.program[s.move_count - 1];
        std::printf("  AI: %s(%u) count=%u turn=%d\n",
                    blockName(last.type), last.param, s.move_count, static_cast<int>(s.turn));
        // AI は END を置かない仕様(buildCandidates で除外)のため完成しない
        if (last.type == BlockType::End) {
            std::printf("  [unexpected] AI placed END\n");
            break;
        }
    }

    s.run_input_color = input_color;
    std::mt19937 rng(seed);
    const bool ok = compileAndRun(s, rng);
    std::printf("  run: ok=%d circles=%u\n", ok, s.runtime.circle_count);
    std::printf("  hash: %08X\n\n", hashProgramState(s));
}

} // namespace

int main() {
    // Scenario 1: structured program exercising Move/Repeat/If/Else
    playGame(
        "structured",
        12345,
        {
            {BlockType::Move,   5},
            {BlockType::Draw,   3},
            {BlockType::End,    0},
            {BlockType::Repeat, 2},
            {BlockType::Draw,   2},
            {BlockType::Move,   9},
            {BlockType::Draw,   4},
            {BlockType::End,    0},
            {BlockType::If,     4},
            {BlockType::Draw,   4},
            {BlockType::Else,   0},
            {BlockType::Draw,   6},
            {BlockType::End,    0},
            {BlockType::End,    0},
            {BlockType::Draw,   7},
        },
        4 // IF input color matches IF(4) -> then-branch executes
    );

    // Scenario 2: different seed, mostly Draw-heavy plan
    playGame(
        "draws",
        98765,
        {
            {BlockType::Draw, 1},
            {BlockType::Draw, 2},
            {BlockType::Draw, 3},
            {BlockType::Draw, 4},
            {BlockType::Draw, 5},
            {BlockType::Draw, 6},
            {BlockType::Draw, 7},
            {BlockType::Draw, 8},
        },
        1
    );

    // Scenario 3: Move-heavy with deep nesting
    playGame(
        "moves",
        424242,
        {
            {BlockType::Move,   19},
            {BlockType::Draw,   1},
            {BlockType::End,    0},
            {BlockType::Move,   3},
            {BlockType::Draw,   2},
            {BlockType::End,    0},
            {BlockType::Move,   7},
            {BlockType::Draw,   3},
            {BlockType::End,    0},
            {BlockType::Repeat, 3},
            {BlockType::Draw,   4},
            {BlockType::End,    0},
        },
        2
    );

    // Scenario 4: short plan, run early (finalizeSyntax closes scopes),
    // exercises the full VM with open scopes + IF/REPEAT bodies.
    playGame(
        "short-run",
        555,
        {
            {BlockType::Repeat, 2},
            {BlockType::Draw,   4},
            {BlockType::If,     6},
            {BlockType::Draw,   1},
        },
        6
    );

    // Scenario 5: player closes the final scope with END -> program complete
    // -> color_select (no more adds). Exercises the END-completion rule.
    // AI は END を一切置かない(buildCandidates で除外)ため、スコープを閉じるのは
    // 常にプレイヤーの役目。
    playGame(
        "end-complete",
        777,
        {
            {BlockType::Move,   5},
            {BlockType::End,    0},
            {BlockType::Draw,   7},   // 置けなくなるはず(完成後)
        },
        1
    );

    // Scenario 6: undo — player places a block, then undoes it; the AI's
    // response is also undone one step at a time. Exercises undoLastStep's
    // state rollback (history/frequency/transitions/depth) deterministically.
    {
        ProgramState s;
        initProgramState(s);
        s.rng_seed = 999;

        std::printf("== undo (seed=999) ==\n");
        std::printf("  game_id=%u\n", s.game_id);

        // P: MOVE(5) -> AI turn
        addStepToProgram(s, BlockType::Move, 5, false);
        std::printf("  P: MOVE(5) count=%u depth=%u\n", s.move_count, s.syntax_depth);
        performAITurn(s);
        std::printf("  AI: %s(%u) count=%u\n",
                    blockName(s.program[s.move_count - 1].type),
                    s.program[s.move_count - 1].param, s.move_count);

        // Undo 1: removes AI's block, back to player
        if (undoLastStep(s)) std::printf("  undo1: count=%u depth=%u\n", s.move_count, s.syntax_depth);
        // Undo 2: removes player's MOVE
        if (undoLastStep(s)) std::printf("  undo2: count=%u depth=%u\n", s.move_count, s.syntax_depth);
        // Undo 3: no-op (empty)
        if (!undoLastStep(s)) std::printf("  undo3: no-op\n");

        std::printf("  hash: %08X\n\n", hashProgramState(s));
    }

    std::printf("core_test: done\n");
    return 0;
}
