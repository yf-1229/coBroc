// wasm_api.cpp — C ABI implementation for the WebAssembly build.
//
// Implements the frozen contract documented in web/API_CONTRACT.md:
//   - cobroc_* C ABI (same signatures as core/wasm_api.h)
//   - snapshot() returns the JSON schema v1
//   - serialize() returns a base64 string of the binary layout
//   - deserialize() accepts that base64 string

#include "core/wasm_api.h"
#include "core/coBroc_core.h"

#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <random>
#include <string>
#include <vector>

namespace {

using namespace coBroc::core;

// ── Snapshot JSON helpers ──────────────────────────────────────────────

void jsonAppend(std::string& out, const char* fmt, ...) {
    char buf[256];
    va_list args;
    va_start(args, fmt);
    std::vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);
    out += buf;
}

const char* turnNameJson(TurnState t) {
    switch (t) {
        case TurnState::PlayerTurn:       return "player";
        case TurnState::AITurn:           return "ai";
        case TurnState::SelectInputColor: return "color_select";
        case TurnState::RunProgram:       return "run";
        case TurnState::Finished:         return "finished";
        default:                          return "unknown";
    }
}

std::string buildSnapshot(const ProgramState& s) {
    std::string j;
    j.reserve(2048);
    jsonAppend(j, "{\"v\":1,");
    jsonAppend(j, "\"game_id\":%u,", s.game_id);
    jsonAppend(j, "\"turn\":\"%s\",", turnNameJson(s.turn));
    jsonAppend(j, "\"move_count\":%u,", s.move_count);
    jsonAppend(j, "\"max_moves\":%u,", MAX_MOVES);
    jsonAppend(j, "\"syntax_depth\":%u,", s.syntax_depth);
    jsonAppend(j, "\"selected_block\":\"%s\",", blockName(s.selected_block));
    jsonAppend(j, "\"selected_param\":%u,", s.selected_param);
    jsonAppend(j, "\"param_min\":%u,", minParamForBlock(s.selected_block));
    jsonAppend(j, "\"param_max\":%u,", maxParamForBlock(s.selected_block));
    jsonAppend(j, "\"run_input_color\":%u,", s.run_input_color);

    j += "\"block\":[";
    for (uint8_t i = 0; i < s.move_count; i++) {
        const auto& step = s.program[i];
        if (i > 0) j += ",";
        jsonAppend(j, "{\"type\":\"%s\",\"param\":%u,\"from_ai\":%s,\"depth\":%u}",
                   blockName(step.type), step.param,
                   step.from_ai ? "true" : "false",
                   s.view_depths[i]);
    }
    j += "],";

    j += "\"circles\":[";
    for (uint16_t i = 0; i < s.runtime.circle_count; i++) {
        const auto& c = s.runtime.circles[i];
        if (i > 0) j += ",";
        jsonAppend(j, "{\"x\":%u,\"y\":%u,\"color\":%u}", c.x, c.y, c.color);
    }
    j += "]}";
    return j;
}

// ── Binary serialize helpers ───────────────────────────────────────────

struct ByteWriter {
    std::vector<uint8_t> buf;

    void u8(uint8_t v) { buf.push_back(v); }
    void u16(uint16_t v) {
        buf.push_back(static_cast<uint8_t>(v & 0xFF));
        buf.push_back(static_cast<uint8_t>((v >> 8) & 0xFF));
    }
    void u32(uint32_t v) {
        buf.push_back(static_cast<uint8_t>(v & 0xFF));
        buf.push_back(static_cast<uint8_t>((v >> 8) & 0xFF));
        buf.push_back(static_cast<uint8_t>((v >> 16) & 0xFF));
        buf.push_back(static_cast<uint8_t>((v >> 24) & 0xFF));
    }
    void bytes(const void* data, size_t n) {
        const auto* p = static_cast<const uint8_t*>(data);
        buf.insert(buf.end(), p, p + n);
    }
};

struct ByteReader {
    const uint8_t* p;
    size_t size;
    size_t pos = 0;
    bool ok = true;

    explicit ByteReader(const uint8_t* data, size_t n) : p(data), size(n) {}

    uint8_t u8() {
        if (pos + 1 > size) { ok = false; return 0; }
        return p[pos++];
    }
    uint16_t u16() {
        if (pos + 2 > size) { ok = false; return 0; }
        uint16_t v = static_cast<uint16_t>(p[pos] | (p[pos + 1] << 8));
        pos += 2;
        return v;
    }
    uint32_t u32() {
        if (pos + 4 > size) { ok = false; return 0; }
        uint32_t v = static_cast<uint32_t>(p[pos]) |
                     (static_cast<uint32_t>(p[pos + 1]) << 8) |
                     (static_cast<uint32_t>(p[pos + 2]) << 16) |
                     (static_cast<uint32_t>(p[pos + 3]) << 24);
        pos += 4;
        return v;
    }
    void bytes(void* data, size_t n) {
        if (pos + n > size) { ok = false; return; }
        std::memcpy(data, p + pos, n);
        pos += n;
    }
};

// ── Base64 ─────────────────────────────────────────────────────────────

const char kB64Table[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

std::string b64Encode(const uint8_t* data, size_t len) {
    std::string out;
    out.reserve(((len + 2) / 3) * 4);
    for (size_t i = 0; i < len; i += 3) {
        const uint32_t b0 = data[i];
        const uint32_t b1 = (i + 1 < len) ? data[i + 1] : 0;
        const uint32_t b2 = (i + 2 < len) ? data[i + 2] : 0;
        const uint32_t n = (b0 << 16) | (b1 << 8) | b2;
        out += kB64Table[(n >> 18) & 0x3F];
        out += kB64Table[(n >> 12) & 0x3F];
        out += (i + 1 < len) ? kB64Table[(n >> 6) & 0x3F] : '=';
        out += (i + 2 < len) ? kB64Table[n & 0x3F] : '=';
    }
    return out;
}

int b64Value(char c) {
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+') return 62;
    if (c == '/') return 63;
    return -1;
}

bool b64Decode(const char* in, std::vector<uint8_t>& out) {
    const size_t len = std::strlen(in);
    if (len % 4 != 0) return false;
    out.clear();
    out.reserve((len / 4) * 3);
    for (size_t i = 0; i < len; i += 4) {
        int v[4];
        for (int k = 0; k < 4; k++) {
            const char c = in[i + k];
            if (c == '=') v[k] = -2;
            else v[k] = b64Value(c);
            if (v[k] < 0 && !(c == '=' && k >= 2)) return false;
        }
        const uint32_t n = static_cast<uint32_t>(
            (v[0] & 0x3F) << 18 | (v[1] & 0x3F) << 12 |
            (v[2] > 0 ? v[2] : 0) << 6 | (v[3] > 0 ? v[3] : 0));
        out.push_back(static_cast<uint8_t>((n >> 16) & 0xFF));
        if (v[2] >= 0) out.push_back(static_cast<uint8_t>((n >> 8) & 0xFF));
        if (v[3] >= 0) out.push_back(static_cast<uint8_t>(n & 0xFF));
    }
    return true;
}

// ── Serialize / deserialize ────────────────────────────────────────────

std::string serializeState(const ProgramState& s) {
    ByteWriter w;
    w.bytes("CB", 2);
    w.u8(1);  // version
    w.u16(s.game_id);
    w.u32(s.rng_seed);
    w.u8(s.move_count);
    w.u8(s.syntax_depth);
    w.u8(s.selected_line);
    w.u8(s.scroll_top);
    w.u8(static_cast<uint8_t>(s.selected_block));
    w.u8(s.selected_param);
    w.u8(s.run_input_color);
    w.u8(static_cast<uint8_t>(s.turn));
    w.u8(s.history_size);
    w.bytes(s.history.data(), s.history.size() * sizeof(BlockType));
    w.bytes(s.block_frequency.data(), s.block_frequency.size() * sizeof(uint8_t));
    w.bytes(s.transitions.data(), s.transitions.size() * sizeof(std::array<uint16_t, 8>));
    for (uint8_t i = 0; i < s.move_count; i++) {
        const auto& step = s.program[i];
        w.u8(static_cast<uint8_t>(step.type));
        w.u8(step.param);
        w.u8(step.from_ai ? 1 : 0);
        w.u8(0);  // pad
    }
    for (uint8_t i = 0; i < s.move_count; i++) {
        w.u8(s.view_depths[i]);
    }
    w.u16(s.runtime.circle_count);
    for (uint16_t i = 0; i < s.runtime.circle_count; i++) {
        const auto& c = s.runtime.circles[i];
        w.u8(c.x);
        w.u8(c.y);
        w.u8(c.color);
    }
    return b64Encode(w.buf.data(), w.buf.size());
}

bool deserializeState(const char* data, ProgramState& s) {
    if (!data) return false;

    std::vector<uint8_t> bin;
    if (!b64Decode(data, bin)) return false;

    ByteReader r(bin.data(), bin.size());
    char magic[2];
    r.bytes(magic, 2);
    if (!r.ok || magic[0] != 'C' || magic[1] != 'B') return false;
    if (r.u8() != 1) return false;  // version

    ProgramState out{};
    out.game_id = r.u16();
    out.rng_seed = r.u32();
    out.move_count = r.u8();
    out.syntax_depth = r.u8();
    out.selected_line = r.u8();
    out.scroll_top = r.u8();
    out.selected_block = static_cast<BlockType>(r.u8());
    out.selected_param = r.u8();
    out.run_input_color = r.u8();
    out.turn = static_cast<TurnState>(r.u8());
    out.history_size = r.u8();

    if (!r.ok) return false;
    if (out.move_count > MAX_MOVES) return false;
    if (out.history_size > MAX_HISTORY) return false;
    if (out.selected_block > BlockType::Else) return false;
    if (out.turn > TurnState::Finished) return false;
    if (out.syntax_depth > MAX_NEST_DEPTH) return false;

    r.bytes(out.history.data(), out.history.size() * sizeof(BlockType));
    r.bytes(out.block_frequency.data(), out.block_frequency.size() * sizeof(uint8_t));
    r.bytes(out.transitions.data(), out.transitions.size() * sizeof(std::array<uint16_t, 8>));

    for (uint8_t i = 0; i < out.move_count; i++) {
        const uint8_t type = r.u8();
        const uint8_t param = r.u8();
        const uint8_t from_ai = r.u8();
        r.u8();  // pad
        if (type > static_cast<uint8_t>(BlockType::Else)) return false;
        out.program[i] = {static_cast<BlockType>(type), param, from_ai != 0};
    }
    // 旧セーブで MAX_NEST_DEPTH を超えるネストが含まれないことを検証する
    // (ランタイムの move/repeat スタックは MAX_NEST_DEPTH サイズのため、
    //  超過した状態を復元すると実行時に失敗する)。
    {
        uint8_t depth = 0;
        for (uint8_t i = 0; i < out.move_count; i++) {
            const BlockType t = out.program[i].type;
            if (t == BlockType::Move || t == BlockType::If || t == BlockType::Repeat) {
                depth++;
                if (depth > MAX_NEST_DEPTH) return false;
            } else if (t == BlockType::End) {
                if (depth > 0) depth--;
            }
        }
    }
    // view_depths は現在の MAX_NEST_DEPTH に合わせて再計算する
    // (旧セーブの値は 3..5 の可能性があるため)
    for (uint8_t i = 0; i < out.move_count; i++) {
        out.view_depths[i] = r.u8();
    }
    coBroc::core::recalcViewDepths(out);

    out.runtime.circle_count = r.u16();
    if (out.runtime.circle_count > MAX_DRAW_EVENTS) return false;
    for (uint16_t i = 0; i < out.runtime.circle_count; i++) {
        auto& c = out.runtime.circles[i];
        c.x = r.u8();
        c.y = r.u8();
        c.color = r.u8();
    }

    if (!r.ok) return false;

    s = out;
    return true;
}

} // anonymous namespace

// ── State lifecycle ─────────────────────────────────────────────────────

void* cobroc_new_game(unsigned int seed) {
    auto* game = new coBroc::core::ProgramState();
    coBroc::core::initProgramState(*game);
    game->rng_seed = seed ? seed : static_cast<unsigned int>(std::random_device{}());
    return game;
}

void cobroc_free(void* state) {
    delete static_cast<coBroc::core::ProgramState*>(state);
}

// ── Player actions ──────────────────────────────────────────────────────

void cobroc_cycle_type(void* state) {
    auto& s = *static_cast<coBroc::core::ProgramState*>(state);
    coBroc::core::cycleBlockType(s);
}

void cobroc_cycle_param(void* state) {
    auto& s = *static_cast<coBroc::core::ProgramState*>(state);
    coBroc::core::cycleParam(s);
}

void cobroc_add_block(void* state) {
    auto& s = *static_cast<coBroc::core::ProgramState*>(state);
    const auto t = s.selected_block;
    uint8_t param = coBroc::core::blockHasParam(t) ? s.selected_param : 0;
    if (coBroc::core::blockHasParam(t)) {
        const uint8_t minp = coBroc::core::minParamForBlock(t);
        const uint8_t maxp = coBroc::core::maxParamForBlock(t);
        if (param < minp || param > maxp) param = minp;
    }
    if (coBroc::core::addStepToProgram(s, t, param, false)) {
        // 盤面満杯、または END で最後のスコープを閉じてプログラム完成
        // (トップレベルでの DRAW などは完成ではないため depth==0 のみでは判定しない)
        const bool completed = (t == coBroc::core::BlockType::End && s.syntax_depth == 0);
        if (s.move_count >= coBroc::core::MAX_MOVES || completed) {
            s.turn = coBroc::core::TurnState::SelectInputColor;
        } else {
            s.turn = coBroc::core::TurnState::AITurn;
        }
    }
}

void cobroc_set_input_color(void* state, int color) {
    auto& s = *static_cast<coBroc::core::ProgramState*>(state);
    if (color >= coBroc::core::COLOR_PARAM_MIN && color <= coBroc::core::COLOR_PARAM_MAX) {
        s.run_input_color = static_cast<uint8_t>(color);
    }
}

void cobroc_select_input_color(void* state) {
    auto& s = *static_cast<coBroc::core::ProgramState*>(state);
    if (s.turn == coBroc::core::TurnState::PlayerTurn) {
        s.turn = coBroc::core::TurnState::SelectInputColor;
    }
}

void cobroc_run(void* state) {
    auto& s = *static_cast<coBroc::core::ProgramState*>(state);
    std::mt19937 rng(s.rng_seed);
    coBroc::core::compileAndRun(s, rng);
    s.turn = coBroc::core::TurnState::Finished;
}

void cobroc_undo(void* state) {
    auto& s = *static_cast<coBroc::core::ProgramState*>(state);
    coBroc::core::undoLastStep(s);
}

// ── AI turn ─────────────────────────────────────────────────────────────

void cobroc_ai_turn(void* state) {
    auto& s = *static_cast<coBroc::core::ProgramState*>(state);
    coBroc::core::performAITurn(s);
}

// ── State query ─────────────────────────────────────────────────────────

int cobroc_get_turn(void* state) {
    return static_cast<int>(static_cast<coBroc::core::ProgramState*>(state)->turn);
}

int cobroc_get_move_count(void* state) {
    return static_cast<int>(static_cast<coBroc::core::ProgramState*>(state)->move_count);
}

int cobroc_get_selected_param(void* state) {
    return static_cast<int>(static_cast<coBroc::core::ProgramState*>(state)->selected_param);
}

// ── JSON snapshot ───────────────────────────────────────────────────────

const char* cobroc_snapshot(void* state) {
    const auto& s = *static_cast<coBroc::core::ProgramState*>(state);
    const std::string json = buildSnapshot(s);
    char* buf = static_cast<char*>(std::malloc(json.size() + 1));
    if (!buf) return nullptr;
    std::memcpy(buf, json.c_str(), json.size() + 1);
    return buf;
}

void cobroc_free_snapshot(const char* json) {
    std::free(const_cast<char*>(json));
}

// ── Save/load (base64) ──────────────────────────────────────────────────

const char* cobroc_serialize(void* state) {
    const auto& s = *static_cast<coBroc::core::ProgramState*>(state);
    const std::string data = serializeState(s);
    char* buf = static_cast<char*>(std::malloc(data.size() + 1));
    if (!buf) return nullptr;
    std::memcpy(buf, data.c_str(), data.size() + 1);
    return buf;
}

void* cobroc_deserialize(const char* data) {
    if (!data) return nullptr;
    auto* game = new coBroc::core::ProgramState();
    if (!deserializeState(data, *game)) {
        delete game;
        return nullptr;
    }
    return game;
}
