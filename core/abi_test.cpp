// abi_test.c — Host-side C ABI test for the WASM interface.
// Exercises snapshot/serialize/deserialize round-trip + turn transitions.
//
// Build (native, for logic checks):
//   g++ -std=c++17 -I. core/abi_test.cpp core/coBroc_core.cpp core/wasm_api.cpp -o /tmp/abi_test
// Build (wasm, for node):
//   emcc -O2 -std=c++17 -I. core/abi_test.cpp core/coBroc_core.cpp core/wasm_api.cpp
//     -s EXPORTED_FUNCTIONS="['_cobroc_new_game','_cobroc_free','_cobroc_cycle_type',
//       '_cobroc_cycle_param','_cobroc_add_block','_cobroc_set_input_color','_cobroc_ai_turn',
//       '_cobroc_run','_cobroc_get_turn','_cobroc_snapshot','_cobroc_free_snapshot',
//       '_cobroc_serialize','_cobroc_deserialize','_malloc','_free']" -o /tmp/abi_test_wasm.js
//   node /tmp/abi_test_wasm.js

#include "core/wasm_api.h"
#include <cstdio>
#include <cstring>
#include <string>

static int g_failures = 0;

#define CHECK(cond, msg) \
    do { if (cond) { std::printf("  OK: %s\n", msg); } \
         else { std::printf("  FAIL: %s\n", msg); g_failures++; } } while (0)

int main() {
    std::printf("== ABI test ==\n");

    // 1. new game
    void* s = cobroc_new_game(12345);
    CHECK(s != nullptr, "cobroc_new_game(12345)");
    CHECK(cobroc_get_turn(s) == 0, "initial turn == player(0)");
    CHECK(cobroc_get_move_count(s) == 0, "initial move_count == 0");

    // 2. snapshot shape (v1)
    const char* snap0 = cobroc_snapshot(s);
    CHECK(snap0 != nullptr, "snapshot not null");
    CHECK(std::strstr(snap0, "\"v\":1") != nullptr, "snapshot has v:1");
    CHECK(std::strstr(snap0, "\"turn\":\"player\"") != nullptr, "snapshot turn=player");
    CHECK(std::strstr(snap0, "\"block\":[]") != nullptr, "snapshot empty block list");
    cobroc_free_snapshot(snap0);

    // 3. cycle param (Move selected by default, 1..19)
    for (int i = 0; i < 20; i++) cobroc_cycle_param(s);
    const char* snap1 = cobroc_snapshot(s);
    CHECK(std::strstr(snap1, "\"selected_param\":2") != nullptr,
          "cycle_param wraps 19 -> 1 -> 2 after 20 cycles");
    cobroc_free_snapshot(snap1);

    // 4. add block -> turn becomes ai
    cobroc_cycle_param(s);  // selected_param 2 -> 3
    cobroc_add_block(s);
    CHECK(cobroc_get_move_count(s) == 1, "move_count == 1 after add");
    CHECK(cobroc_get_turn(s) == 1, "turn == ai(1) after add");

    // 5. ai turn -> back to player
    cobroc_ai_turn(s);
    CHECK(cobroc_get_move_count(s) == 2, "move_count == 2 after ai");
    CHECK(cobroc_get_turn(s) == 0, "turn == player(0) after ai");

    // 6. serialize -> deserialize round-trip
    const char* bin = cobroc_serialize(s);
    CHECK(bin != nullptr, "serialize not null");
    void* s2 = cobroc_deserialize(bin);
    CHECK(s2 != nullptr, "deserialize ok");
    CHECK(cobroc_get_move_count(s2) == cobroc_get_move_count(s), "round-trip move_count");
    CHECK(cobroc_get_turn(s2) == cobroc_get_turn(s), "round-trip turn");
    const char* snapA = cobroc_snapshot(s);
    const char* snapB = cobroc_snapshot(s2);
    CHECK(std::strcmp(snapA, snapB) == 0, "round-trip snapshot identical");
    cobroc_free_snapshot(snapA);
    cobroc_free_snapshot(snapB);
    cobroc_free(s2);

    // 7. corrupt base64 -> null
    CHECK(cobroc_deserialize("!!!not-base64!!!") == nullptr, "corrupt base64 -> null");

    // 8. color select + run
    cobroc_set_input_color(s, 4);
    cobroc_run(s);
    CHECK(cobroc_get_turn(s) == 4, "turn == finished(4) after run");
    const char* snapC = cobroc_snapshot(s);
    CHECK(std::strstr(snapC, "\"run_input_color\":4") != nullptr, "run_input_color == 4");
    CHECK(std::strstr(snapC, "\"circles\":[") != nullptr, "snapshot has circles array");
    cobroc_free_snapshot(snapC);

    cobroc_free(s);

    std::printf("== ABI test %s (%d failures) ==\n", g_failures == 0 ? "PASSED" : "FAILED", g_failures);
    return g_failures == 0 ? 0 : 1;
}
