# coBroc Web — API 契約書 (FROZEN CONTRACT)

このドキュメントは WASM 基盤(Agent A)とフロントエンド(Agent B)の
並行開発を成立させるための**凍結契約**です。

- `core/wasm_api.h` の C ABI は変更しない
- スナップショット JSON スキーマは以下のみ(v=1)
- 変更が必要になった場合は、両エージェントの統合前に必ず本ファイルを更新する

## 1. WASM モジュール仕様

- ビルド: `web/CMakeLists.txt`(Emscripten, MODULARIZE)
- 出力: `web/public/coBrocWeb.js` + `web/public/coBrocWeb.wasm`
- EXPORT_NAME: `CoBrocModule`
- 生成JSはESM/グローバル問わず `CoBrocModule({ locateFile })` でロードできる

## 2. C ABI (`core/wasm_api.h` と同一)

```
void* cobroc_new_game(unsigned int seed);     // seed=0 → 自動ランダム
void  cobroc_free(void* state);
void  cobroc_cycle_type(void* state);
void  cobroc_cycle_param(void* state);
void  cobroc_add_block(void* state);          // 選択中ブロックを追加 → ターンがAIへ
void  cobroc_set_input_color(void* state, int color);
void  cobroc_select_input_color(void* state);  // PlayerTurn -> color_select (Y キー相当)
void  cobroc_run(void* state);                // VM実行 → turn=finished
void  cobroc_undo(void* state);               // 最後の1ブロックを取り消す → turn=player
void  cobroc_ai_turn(void* state);            // AIが1手置く
int   cobroc_get_turn(void* state);           // 0..4 (下記 TurnId)
int   cobroc_get_move_count(void* state);
int   cobroc_get_selected_param(void* state);
const char* cobroc_snapshot(void* state);     // JSON文字列(呼び出し側が free_snapshot)
void  cobroc_free_snapshot(const char* json);
const char* cobroc_serialize(void* state);    // base64 バイナリ(呼び出し側が free)
void* cobroc_deserialize(const char* data);   // 失敗時 null
```

TurnId: `0=player, 1=ai, 2=color_select, 3=run, 4=finished`

**注意(ターン遷移の規約)**:
- ゲーム開始時は `player`
- `add_block` 成功後は `ai` になる(JS は `ai_turn` を呼ぶ)
- 盤面満杯(`move_count == max_moves`)または**プログラム完成**になった場合:
  - プレイヤーの手で完成(END で最後のスコープを閉じた) → `color_select`
  - AI の手で完成 → 発生しない(**AI は END ブロックを一切設置しない**)
- **AI は END を候補に含めない**(`buildCandidates` で除外)。スコープを閉じるのは
  常にプレイヤーの役目。AI が開いたスコープはプレイヤーが END で閉じる。
- `player` 中の `select_input_color` で `color_select` へ遷移(実機の Y キー相当)
- `color_select` 中は `set_input_color` → `run` の順で進める
- END は最後のスコープを閉じた時点でゲーム終了となるため、それ以降の
  追加ブロック操作はできない

## 3. スナップショット JSON スキーマ (v=1)

```jsonc
{
  "v": 1,
  "game_id": 1,
  "turn": "player",                // "player"|"ai"|"color_select"|"run"|"finished"
  "move_count": 3,
  "max_moves": 16,
  "syntax_depth": 1,
  "selected_block": "MOVE",        // "NONE"|"MOVE"|"DRAW"|"IF"|"REPEAT"|"END"|"ELSE"
  "selected_param": 5,             // 選択中のパラメータ値
  "param_min": 1,                  // 選択中ブロックのパラメータ下限(0=パラメータなし)
  "param_max": 19,                 // 選択中ブロックのパラメータ上限
  "block": [                       // 配置済みブロック(フロー順)
    { "type": "MOVE", "param": 5, "from_ai": false, "depth": 0 }
  ],
  "run_input_color": 2,            // color_select で設定する色パラメータ (1..8)
  "circles": [                     // finished 時のみ配列が入る(それ以外は空)
    { "x": 12, "y": 200, "color": 3 }   // x/y: 0..239 (240x240 仮想空間)
  ]
}
```

- `param_min == 0 && param_max == 0` のブロック(END/ELSE)はパラメータ非表示
- 色スウォッチ表示対象: `DRAW`, `IF`(パラメータ → 色インデックス)
- 色パレット(パラメータ→表示色): 1=白, 2=赤, 3=橙, 4=黄, 5=緑, 6=青, 7=マゼンタ, 8=黒
  (厳密なRGB値は実機 RGB565 由来: WHITE=0xFFFF, RED=0xF800, ORANGE=0xFC07,
   YELLOW=0xFFE0, GREEN=0x07E0, BLUE=0x001F, MAGENTA=0xF81F, BLACK=0x0000)

## 4. シリアライズ形式 (セーブデータ, base64)

バイナリレイアウト(リトルエンディアン、`cobroc_serialize` の戻り値の元):

```
offset  size  field
0       2     magic "CB"
2       1     version = 1
3       2     game_id (u16)
5       4     rng_seed (u32)
9       1     move_count (u8)
10      1     syntax_depth (u8)
11      1     selected_line (u8)
12      1     scroll_top (u8)
13      1     selected_block (u8)  // BlockType 値
14      1     selected_param (u8)
15      1     run_input_color (u8)
16      1     turn (u8)
17      1     history_size (u8)
18      32    history[] (u8 × 32)
50      8     block_frequency[] (u8 × 8)
58      128   transitions[8][8] (u16 × 64)
186     move_count*4  program[] { type u8, param u8, from_ai u8, pad u8 }
        move_count   view_depths[] (u8)
2       circle_count (u16)
        circle_count*3 circles[] { x u8, y u8, color u8 }
```

JS 側は `cobroc_serialize` の戻り値をそのまま base64 文字列として localStorage に保存し、
復元時は `cobroc_deserialize(base64)` を呼ぶだけでよい(中身を解釈する必要はない)。

## 5. リプレイ形式 (JS側で完結)

```jsonc
{
  "seed": 12345,
  "actions": [
    { "op": "cycle_type" },
    { "op": "cycle_param" },
    { "op": "add" },
    { "op": "select_input_color" },
    { "op": "set_color", "color": 4 },
    { "op": "run" }
  ]
}
```

- `op` 一覧: `cycle_type` / `cycle_param` / `add` / `select_input_color`(Y相当) /
  `set_color`(color 1..8) / `run`
- 再生: `newGame(seed)` → actions を逐次適用(各 op の後に `ai` ターンなら `ai_turn` を自動実行)
- 決定論のため AI・乱数は seed だけで完全再現される(Phase 1 で検証済み)

## 6. ファイル所有権(並行開発時の編集禁止領域)

| パス | 所有者 |
|---|---|
| `core/`(wasm_api.h/cpp, coBroc_core.*, model.h) | **Agent A**(WASM基盤) |
| `web/CMakeLists.txt` | **Agent A** |
| `web/src/**`, `web/index.html`, `web/package.json`, `web/tsconfig.json`, `web/vite.config.ts` | **Agent B**(フロントエンド) |
| `.github/workflows/web.yml` | **Agent C**(デプロイ) |
| `core/test/**` | Agent A(パリティ検証で拡張) |

Agent B は WASM が未完成の間、`web/src/wasm/loader.ts` に
モック実装(本契約の関数シグネチャを満たすダミー)を置き、
`web/public/` に実 WASM が配置されたらモックを差し替えるだけでよい。
