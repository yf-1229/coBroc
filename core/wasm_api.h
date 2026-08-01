#pragma once

#ifndef COBROC_WASM_API_H
#define COBROC_WASM_API_H

// C ABI for WebAssembly interface.
// These functions are exported by the WASM module and called from JavaScript.
// All functions use opaque pointers (uintptr_t or void*) to ProgramState.

#ifdef __cplusplus
extern "C" {
#endif

// State lifecycle
void* cobroc_new_game(unsigned int seed);
void  cobroc_free(void* state);

// Player actions (all take the state pointer returned by cobroc_new_game)
void cobroc_cycle_type(void* state);
void cobroc_cycle_param(void* state);
void cobroc_add_block(void* state);
void cobroc_set_input_color(void* state, int color);
// Enter the IF input-color selection screen (PlayerTurn -> color_select),
// mirroring the Pico "Y" key behaviour.
void cobroc_select_input_color(void* state);
void cobroc_run(void* state);

// AI turn
void cobroc_ai_turn(void* state);

// State query
int  cobroc_get_turn(void* state);
int  cobroc_get_move_count(void* state);
int  cobroc_get_selected_param(void* state);

// JSON snapshot for rendering (caller must free with cobroc_free_snapshot)
const char* cobroc_snapshot(void* state);
void  cobroc_free_snapshot(const char* json);

// Save/load (binary, base64-encoded by JS)
const char* cobroc_serialize(void* state);
void* cobroc_deserialize(const char* data);

#ifdef __cplusplus
}
#endif

#endif // COBROC_WASM_API_H
