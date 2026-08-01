@main.cpp

# TODO 01 ← It's done.

- 行番号必要なし
- DrawとIfでParamを選んでいるときは、うえにParamごとに割り当てられている色(Colorより)も表示して。最大値はMAX_PARAMからとってきて。
- MoveでParamを選んでいるときは、数字をそのまま表示して。あとMoveの最大値はMAX_PARAMからとってきて。
- Forのときの数字はそのまま表示して。最大値もMAX_PARAMから
- 下のステータスメッセージみたいなやつはいらない。下いっぱいまで表示スペースを広げて。
- Result画面ではなく、Drawで指定された色を、Moveで指定されたぶんだけ動かすようにして。
- 定義された定数を変更したので、アルゴリズムに問題がないか再確認して。
- AI機能をydfのランキングで実施して、C++のStandAloneで出力したライブラリを使いたい。入力と出力する必要のあるデータをまとめて。

# TODO 02

- 上の文字が赤くするのをやめて。 ← done
- MOVEの削除 ← done
- 下のAキーとXキーの操作説明は不要。 ← done
- いわゆる実行画面が始まる前に、一つ色を選択できるようにしたい。 ← done
- その指定された色が、IF文で指定されたパラメータと同一か確認して、同一ならIF文の中身を表示。 ← done
- 実行画面を開きましたが、どこにも色が表示されません。 ← done
- ユーザースクロール機能の削除(画面が限界になったときの自動スクロールのみ) ← done
- 操作方法を変更してほしいです。(十字キーは一旦封印、Aでブロックを置く、Bでブロックの変更、Xでパラメータの変更(変更色を出す表示のタイミングはTODO01と同じ)、Yで実行。) ← done


# 質問とTODO

- csvが表すデータは人の1手？Aiの1手？
- 私の判断を元に、CSVを編集してデータを増やしても良い？
-  @/home/yuta/CLionProjects/codeS/main.cpp で実装してほしい。

# 質問とTODO-02

-  codeS内にヘッダーファイルがないのに、どうやって実装してるの？？？
  - 回答: これまではルールベース実装でした。今回 `#if defined(BLOCKODE_USE_YDF) && __has_include("blockode_ranking_model.h")` で、生成ヘッダーがある場合だけYDF分岐が有効になる実装を追加しました（ヘッダー未配置でもビルド可能）。
-  ydfを使って、実装して。
  - done: `main.cpp` にYDF接続用フック（`ydfScoreBonus` / `candidateTotalScore` / `BLOCKODE_HAS_YDF_MODEL`）を追加し、候補スコア計算経路に統合。
-  Moveを座標指定ブロックとして考えて、その下にネストを作る。その下に来るのはMove以外のブロック。
  - done: `Move` を復活し、`Move/If/Repeat` を開きブロックとして `End` で閉じる構造に変更。深さ>0で `Move` は配置不可にして「Move配下にMove不可」を実装。
-  最後のリザルト画面でMove下にあるDrawブロックを指定されたパラメータの数だけ動かして。例えば19なら、Cellの左上を0️、右下を19として、19に置く。(Moveの選択画面では1..19のパラメータを表示して。)
  - done: Moveパラメータを `1..19` に変更。実行時は `Move(param)` をセル `param-1` (0..19) のアンカーとし、配下の `Draw` はそのセルへ配置。リザルト画面は `5x4` グリッドで `0..19` を描画。
-  他のブロックでも、パラメータ選択ボタンで選べるのは、1以降の整数とする。
  - done: `Draw/If` は `1..8`、`Repeat` は `1..7`、`Move` は `1..19` の循環選択に統一（`End` はパラメータなし）。

# 質問とTODO-03

- `sample_ranking.csv` を100件作る
  - done: `/home/yuta/PycharmProjects/Blockode_ml/data/sample_ranking.csv` を100行に生成（ヘッダ除く）。
- `main.cpp` からルールベースを削除して、ヘッダーファイルからYDF予測へ完全切替
  - done: ルールベースのスコア関数（`transition/structure/progress` 系）を削除し、`external/ml/ydf.h` の `Model::Predict()` のみで候補スコアを決定する方式に変更。
- `external/ml/ydf.h` をYDFファイル想定で、特徴量入力と出力の枠組みを作る
  - done: `CandidateFeatures`（入力特徴）と `Prediction`（出力）と `Model::Predict()` インターフェースを `ydf.h` に実装し、`main.cpp` から接続。

# 質問とTODO-04

- AIに予測させたあと、ルール違反ならフィードバックして別候補を再予測したい
  - done: `main.cpp` に `violatesAIMoveRule` / `applyRuleFeedbackAndRescore` を追加。
  - done: 予測後にルール違反候補へ `feedback_penalty` を加算して再スコアリングし、必要なら別候補へ切替。
- Python側の編集も含めて対応
  - done: `Blockode_ml/main.py` に `actor` / `feedback_penalty` を列定義追加し、学習前に `label = max(0, label - feedback_penalty)` の補正を追加。
  - done: `sample_ranking.csv` に `feedback_penalty` 列を追加済み（100行）。

# TODO-05（standalone回帰への移行）

- `to_standalone_cc()` 前提で ranking から regression へ変更
  - done: `Blockode_ml/main.py` を回帰学習へ変更（`ydf.Task.REGRESSION`）。
  - done: ラベル列を `suitability_label` に変更し、`query_id` ごとに合計1になるよう正規化。
  - done: エクスポートを `to_standalone_cc(name="blockode_regression_model")` へ変更。
- `main.cpp` を適切度最大の候補を選ぶ方式へ変更
  - done: `score` を `suitability` に置換し、予測関数を `ydfPredictSuitability` に変更。
  - done: 候補ログも `suitability_label` を出力する形式に更新（`/tmp/maincpp_ai_regression.csv`）。
- standalone化で不要になった依存の見直し
  - done: runtime用 `external/ml/model/` を削除。
  - done: runtime前提のCMake定義 `BLOCKODE_YDF_MODEL_PATH` を削除。
  - done: ランキング生成ヘッダー `external/ml/blockode_ranking_model.h` を削除。

# TODO-06（ヘッダー整理）

- `external/ml/ydf.h` が冗長なら削除し、必要定義を `main.h` へ移す
  - done: `blockode::ydf` の定義（`CandidateFeatures`/`Prediction`/`Model::Predict`）を `main.h` へ移設。
  - done: `main.cpp` の `#include "external/ml/ydf.h"` を削除。
  - done: `CMakeLists.txt` の `external/ml/ydf.h` 参照を削除。
  - done: `external/ml/ydf.h` を削除。

# TODO-07

- 上部ヘッダーの表示で、色のボックスとdepthの文字が重なっている。depthとydfの表示はいらないので削除して。
  - done: ヘッダー2行目を `A:add B:type X:param Y:run` のみへ変更し、`depth` と `ydf` 表示を削除。
- REPEATやIFなどをおいたらAIが絶対にENDしてくるのですが、データセットを変えるべきでは？改善して。
  - done: `performAITurn` に「IF/REPEAT直後の即END抑制」を追加。ENDと最良非END候補の適切度差が小さい場合は非ENDを採用するよう改善。

# TODO-08 
- 最後のリザルト画面の大幅変更をしてほしい。
- グリッドは削除して、すべてのDrawの内容を円で表示して。
- 円はすべてランダムな座標に配置して(240*240)
- Moveがついている円については、パラメータの数だけランダムに移動して。(移動回数 = パラメータ)
- TODO-07とTODO-08の変更で不要になった定数や関数があれば削除して。
  - done: 実行結果を「グリッドなし」へ変更し、`Draw` 実行ごとに円を描画する方式へ変更。
  - done: すべての円を `240x240` 内のランダム座標へ配置。
  - done: `Move` ネスト下の `Draw` は `Move` のparam回数ぶんランダム移動を適用。
  - done: 旧グリッド描画で不要になった `RESULT_COLS/RESULT_ROWS` と座標アンカー依存処理を削除。


# TODO-09
- 240*240 のランダムな数字を選択するのに、67-69行に追加した乱数発生器を使って。マジックナンバーは避けてほしい。
  - done: `std::random_device` + `std::mt19937` + `std::uniform_int_distribution` を乱数生成に使用し、`240x240` 座標・移動量の生成を定数化（`RANDOM_STEP_*` など）してマジックナンバーを削減。
- IF,FORのネストの中では、MOVEを使うことをAllowして。
  - done: `isLegalCandidate` の「`last_type == Move` なら Draw 以外禁止」制約を削除し、IF/REPEAT配下でMoveを許可。
- MOVEのネストの中では、IF,FORの使用禁止
  - done: `insideMoveScope()` を追加し、Moveスコープ内では `If` / `Repeat` を `blockAllowedByDepth` で禁止。

# TODO-10（Pico軽量化の実装計画）

## 問題とアプローチ

- Pico実機で重くなりやすい箇所は、`Paint_Clear` を伴う全面再描画、AIターンでの候補全件予測、フレームごとの文字列整形、コア間キュー往復です。
- 軽量化は「描画回数を減らす」「推論回数を減らす」「1回あたりの処理コストを下げる」の順で進める。
- 見た目・ルールを維持しつつ、挙動差分が出る変更は段階導入（フラグ化）する。

## TODO一覧（提案）

- `perf-dirty-redraw`:
  - 全面 `Paint_Clear` を減らし、ヘッダー/変更行/結果画面を差分再描画する。
- `perf-ai-candidate-pruning`:
  - 予測前に候補を絞り込み、再スコア回数を削減（同一ターンでの重複推論を避ける）。
- `perf-ai-batch-core1`:
  - コア1への予測要求を候補ごとの往復ではなく、バッチ処理化して同期オーバーヘッドを減らす。
- `perf-ui-string-cache`:
  - `snprintf` を毎フレーム実行せず、状態変更時のみ再生成して文字列をキャッシュする。
- `perf-rng-fastpath`:
  - 結果画面の乱数使用を軽量化（必要なら分布生成の再利用範囲を拡大し、呼び出し回数を削減）。
- `perf-release-profile`:
  - Release用に `-Os` / `-flto` / `NDEBUG` 前提のビルドプロファイルを用意して実機計測する。

## 依存関係

- `perf-ai-batch-core1` depends on `perf-ai-candidate-pruning`
- `perf-release-profile` depends on `perf-dirty-redraw`
- `perf-release-profile` depends on `perf-ai-batch-core1`

## 実装結果（done）

- `perf-dirty-redraw`:
  - シーン描画を `clear_background` 制御に変更し、不要な全面 `Paint_Clear` を削減。
  - `drawHeader` / `drawProgramList` の内部クリアを追加して、差分更新時の残像を抑制。
- `perf-ai-candidate-pruning`:
  - `pruneCandidatesForPrediction` を追加し、合法候補が多いターンでは最大20候補へ絞り込み。
- `perf-ai-batch-core1`:
  - `YdfWorkerCommand::PredictBatch` を導入し、core1推論をバッチ要求/応答化。
  - `ydfPredictSuitabilityBatch` で一括予測結果を候補へ反映。
- `perf-ui-string-cache`:
  - `UiTextCache` を追加し、ヘッダー/行テキスト/結果サマリ/色選択文字列を状態変化時のみ再生成。
- `perf-rng-fastpath`:
  - 乱数分布を `g_dist_result_x/y` に分離し、結果円の初期座標生成の不要clampを削減。
  - 画面境界は `RESULT_MIN_COORD`, `RESULT_MAX_X`, `RESULT_MAX_Y` に定数化。
- `perf-release-profile`:
  - `CMakeLists.txt` にRelease向け `-Os -ffunction-sections -fdata-sections`, `-Wl,--gc-sections`, `NDEBUG` を追加。
  - `cmake-build-debug` / `cmake-build-release` の両方で `codeS` ビルド成功を確認。

## 変更調整（2026-03-28）

- ユーザー要望により、`perf-dirty-redraw` と `perf-ui-string-cache` 相当の変更を巻き戻し。
  - `UiTextCache` および文字列キャッシュ関数群を削除。
  - `draw*Scene` を従来の全面再描画（`Paint_Clear`）ベースへ戻し。
- それ以外の軽量化（候補絞り込み、core1バッチ推論、RNG最適化、Release最適化フラグ）は維持。
- 変更後も Debug/Release ビルド成功を確認。

### TODO-11 
- lvglを使った表示システムについての説明をPlan.mdに書いて
- からのリストを表示させるのをやめて、一番上に実行開始というブロックを初期で配置して。(図形は丸)
- lvglの図形を表示させる機能を使って、三角形や四角形などをBlockの種類ごとに、フローチャートのように表示してほしい。

#### 実装説明（LVGL表示システム）

- `main.cpp` のUI層は `namespace ui` に集約し、`lv_init()` と `lv_disp_drv_register()` を使って Pico-LCD-1.3 (240x240) へ接続。
- LVGLの `flush_cb` で `lv_color_t` バッファをLCDドライバ向けRGB565バイト順へ変換し、`LCD_1IN3_Display()` へ転送。
- 既存のブロックロジック（配置制約・AI候補選択・実行器）はそのまま維持し、表示だけをLVGLに置換。
- `render()` でターン状態ごと（通常UI / 色選択 / 実行結果）に画面を描画し、`lv_timer_handler()` と `lv_tick_inc()` で更新。
- 依存は `CMakeLists.txt` で `lvgl v8.3.11` を追加。`external/lvgl` があればそれを使い、なければFetchContentで取得。
- `lv_conf.h` はPico向け軽量構成を採用し、今回のフローチャート描画のため `LV_USE_LINE=1` を有効化。

#### TODO-11 実装結果

- done: 従来の「空行リスト」を廃止し、フロー先頭に固定ノード `実行開始` を表示（丸形）。
- done: ブロックをフローチャート風の縦連結で表示。
  - `実行開始`: 丸
  - `MOVE`: 四角
  - `DRAW`: 三角
  - `IF`: ひし形（ダイヤ）
  - `REPEAT`: 角丸四角（ループ記号付き）
  - `END`: 丸（終端）
- done: ノード間を `lv_line` で接続し、上から下へ流れる見た目に統一。
- done: `DRAW` / `IF` には色スウォッチを表示し、選択中ステップは現在選択中パラメータでプレビュー表示。
- done: `cmake-build-debug` で `coBroc` のビルド成功を確認。

### TODO-12
- 固定ノード `実行開始` は文字化けしています。"Run”にして。
- 四角いカードの上に図形や文字を表示させるのではなく、** 画面左半分に図形でフローチャート ** 、　** 画面右半分(図形と同じy座標に)に、パラメータなどの情報 ** を表示させて。

#### TODO-12 実装結果

- done: 固定開始ノードのラベルを `実行開始` から `Run` に変更。
- done: 1行カード内の重ね描画をやめ、フロー領域を左右2カラムに再設計。
  - 左半分: 図形ベースのフローチャート（縦接続線 + ブロック種別図形）
  - 右半分: 同一Y座標でブロック情報（名前/param/AIマーク/色スウォッチ）
- done: 左右分割の中央に縦の区切り線を追加し、視認性を改善。
- done: 選択中行は右カラムにハイライト表示を追加。
- done: `cmake-build-debug` で `coBroc` ビルド成功を確認。

# WEB移植計画

## 方針

- 技術方式: Emscripten/WASM で C++ コア + YDFモデルを再利用し、UI は JS(Canvas + DOM)で実装
- UI: レスポンシブな現代UI(Pico 240x240 の忠実再現ではなく Web 向け再設計)
- 追加機能: セーブ/ロード、リプレイ(ローカル完結、localStorage)
- デプロイ: GitHub Pages 等の静的ホスティング
- ディレクトリ: `core/`(Pico/Web 共通ロジック) + `web/`(Web 版一式)

## TODO-Web01(コア抽出)

- `core/` に Pico 非依存のゲームロジックを分離し、Pico と Web の両方から共有する。
  - `core/coBroc_core.h` / `core/coBroc_core.cpp`: 全型・定数・ゲームロジック(合法判定/VM/AI採点/フロー演算)
  - `core/model.h`: `coBroc::ydf` ラッパー(YDF standalone ヘッダ)を `main.h` から移設
  - `core/wasm_api.h` / `core/wasm_api.cpp`: Web 用 C ABI(スナップショット/シリアライズは Phase 2 で実装)
  - `core/test/core_test.cpp`: ホスト決定論テスト(固定シードで3+1シナリオを実行し FNV-1a ハッシュを出力)
- 乱数をグローバルから `ProgramState.rng_seed` へ状態化(リプレイの決定論化)。実行時は `mt19937(seed)` を生成して使用。
- AI 予測を `g_predictor_fn` 関数ポインタ経由に変更。Pico は core1 バッチ予測(既存動作維持)、WASM は同期直実行。
- `main.cpp` は LVGL UI・赤外線入力・YDF マルチコアワーカー等の Pico 依存のみを保持し、ロジックは `core::` を参照。

### TODO-Web01 実装結果

- done: `core/` 抽出完了。`main.cpp` は UI/ハードウェア/ワーカーのみに縮小。
- done: 決定論テスト実装。4シナリオ(盤面満杯エッジケース / Draw多め / Move多め / 途中Y実行+finalize)を2回実行し出力一致を確認。
  - ハッシュベースライン: structured=32C4A949, draws=88DF6A01, moves=2737E23F, short-run=A2D353D8
- done: `wasm_api.cpp` のホスト(g++)コンパイル/リンク検証。
- done: `web/` に Vite + TypeScript の雛形を作成(`npm run build` 成功)。
- done: Pico `cmake-build-debug` / `cmake-build-release` の両方でビルド成功。

## TODO-Web02(WASMビルド)

- `web/CMakeLists.txt` を Emscripten 用に構成し `cobroc_*` ABI をエクスポートする。
- `core/wasm_api.cpp` にスナップショットJSON / シリアライズ(base64)を実装する。
- ネイティブ(g++)と WASM で同一シードのゲームを実行し、ハッシュが一致することを確認(パリティ検証)。

### TODO-Web02 実装結果

- done: Emscripten SDK 3.1.69 を `~/emsdk` に導入し activate。
- done: `web/CMakeLists.txt` 完成。
  - emcc は `-s NAME=VALUE`(スペース)を単一引数で渡すと**無視**されるため `-sNAME=VALUE` 形式に統一。
  - `-s` フラグはリンク時のみ有効(clang++ コンパイル時は `-O3` のみ)。
  - 出力: `web/public/coBrocWeb.js` + `coBrocWeb.wasm`(Vite が dist/ へ自動コピー)。
- done: `core/wasm_api.cpp` にスナップショットJSON v1 / シリアライズ(base64, 契約書セクション4のバイナリ形式)を実装。
  - ABI 追加: `cobroc_select_input_color`(PlayerTurn→color_select、実機の Y キー相当)。
- done: RNG 移植性対応。`std::uniform_int_distribution` は libstdc++/libc++ で乱数消費量が異なり
  WASM とネイティブで結果が乖離したため、`rng() % range` の剰余法に置換。
  - ネイティブと WASM で**バイト単位一致**を確認(4シナリオ)。
  - 最終ハッシュ: structured=32C4A949, draws=7810D9F8, moves=2737E23F, short-run=1B6EECC7
- done: パリティ検証(`core/test/core_test.cpp` をネイティブ/WASM両方で実行し diff)。
- done: ABI スモークテスト(`core/abi_test.cpp` + Node 経由)21項目パス。
- done: 実 WASM モジュールを Node でロードし全 ABI 呼び出し検証(`/tmp` スモーク、16項目パス)。

## TODO-Web03(フロントエンド)

- Vite + TypeScript で UI を実装する。
- WASM ローダー(実モジュール優先、無ければモック)、Canvas 描画、操作UI、
  ゲームループ、セーブ/ロード(localStorage)、リプレイを実装する。

### TODO-Web03 実装結果

- done: `web/src/wasm/types.ts` — スナップショット/ABI/リプレイの型定義(契約書準拠)。
- done: `web/src/wasm/loader.ts` — script タグで `coBrocWeb.js` をロードし
  `window.CoBrocModule({locateFile})` から ABI をラップ。失敗時はモックへフォールバック。
- done: `web/src/wasm/mock.ts` — 契約準拠のモック(実 WASM 無しでも動作)。
- done: `web/src/ui/render.ts` — Canvas 2D 描画(フローチャート左図形/右情報、
  色選択、結果円、devicePixelRatio・レスポンシブ対応)。
- done: `web/src/ui/controls` 相当は `index.html` のボタン + `main.ts` で結線
  (A:add / B:type / X:param・色変更 / Y:run・色選択開始 / N:new / R:replay / S:save)。
- done: `web/src/main.ts` — ゲームループ。ターン遷移規約に従い AI ターンは自動実行、
  満杯時は自動 run、色選択画面の操作を実装。AI ターンに 350ms の思考演出。
- done: `web/src/storage.ts` — localStorage オートセーブ(毎ターン)、
  セーブファイルのエクスポート/インポート(JSON)。
- done: `web/src/replay.ts` — 操作ログ記録(seed + actions)と決定的リプレイ再生(速度制御)。
- done: E2E テスト(ヘッドレス Chrome + puppeteer-core)12項目パス:
  WASM ロード / ターン遷移 / ブロック操作 / 色選択→実行→結果描画 / リプレイ / オートセーブ / 新規ゲーム。

## TODO-Web04(デプロイ)

- GitHub Pages への自動デプロイワークフローを作成する。

### TODO-Web04 実装結果

- done: `.github/workflows/web.yml` 作成。
  - build: setup-emsdk → WASM ビルド → npm ci → vite build → upload-pages-artifact
  - deploy: configure-pages → deploy-pages(静的配信)
  - トリガー: push(main/master) + workflow_dispatch
- done: YAML 構文検証(js-yaml)。
- done: `web/CMakeLists.txt` の `cmake_minimum_required` を 3.20 に引き下げ
  (GitHub Actions の cmake 3.28 でも構成可能に)。

## 補足

- 契約書: `web/API_CONTRACT.md`(C ABI / スナップショットJSON / シリアライズ形式 / リプレイ形式)。
- 並行開発時のファイル所有権(契約書セクション6): core/・web/CMakeLists.txt=A(基盤)、
  web/src/**=B(フロントエンド)、.github/workflows/web.yml=C(デプロイ)。
- 検証コマンド:
  - パリティ: `g++ -std=c++17 -O2 -I. core/test/core_test.cpp core/coBroc_core.cpp` と
    `emcc -O2 -std=c++17 -I. ... -o /tmp/x.js; node /tmp/x.js` の diff。
  - WASM ビルド: `emcmake cmake -B build-emscripten -S web && emmake cmake --build build-emscripten`
  - フロント: `cd web && npm run build`

## TODO-Web05(レビュー修正)

- コードレビューで見つかった問題を修正した。
  - done: リプレイ中の状態ポインタ二重解放を修正。`replay.ts` はポインタを
    生成/解放せず、所有権を `main.ts` 側に一元化(`startReplay` のシグネチャ変更)。
  - done: リプレイ時に AI の手で盤面が満杯(turn=3)になった場合の自動 `run` が
    再現されない問題を修正。本体(`runAiIfNeeded`)と同じ挙動に統一。
  - done: `cobroc_select_input_color` を ABI に追加(PlayerTurn→color_select、
    実機の Y キー相当)。フロントエンドの Y ボタンは player 中に色選択画面へ遷移、
    color_select 中に実行。
  - done: 修正後、E2E テスト(ヘッドレス Chrome)再実行で全12項目パスを確認。

## TODO-Web06(END完了ルール)

- ユーザー指摘: 「ENDブロックを置いても追加ボタンが使用可能なのは元からの仕様か」
  - 回答: 元仕様ではゲーム終了条件は盤面満杯のみで、END を置いても追加可能だった。
  - 修正: END で最後のスコープを閉じた時点(`syntax_depth == 0`)でプログラム完成とし、
    以降の追加を不可にする。
- 実装:
  - done: プレイヤーの手で完成 → `SelectInputColor` へ遷移
    (`cobroc_add_block` / Pico `handlePlayerInput`)。色選択画面では追加ボタン無効。
  - done: AI の手で完成 → `RunProgram` へ遷移(`performAITurn`)。
  - done: **AI は「完成させる END」を置かない**仕様を追加(`syntax_depth == 1` のとき
    END 候補を回避。既存の IF/REPEAT 直後即END抑制を拡張)。プログラムを閉じる最終手は
    プレイヤーの役目とし、AI による即終了(1手目で終了など)を防止。
  - done: Web UI の追加ボタン(A)を player ターンのみ有効に変更。
  - done: `web/API_CONTRACT.md` のターン遷移規約に「プログラム完成時」の記載を追加。
- 検証:
  - done: `core_test` に `end-complete` シナリオを追加
    (P: MOVE → AI: DRAW(完成END回避の確認) → P: END → 完成 → 追加停止)。
  - done: ネイティブ/WASM パリティ一致を再確認(5シナリオ)。
    - 新ハッシュ: structured=BE80D8B9, draws=09C024B4, moves=47792542,
      short-run=1B6EECC7, end-complete=0CE81100
  - done: E2E テストに「END で完成 → color_select → 追加ボタン無効」の 3 項目を追加し、
    全 15 項目パス。
  - done: Pico Debug/Release ビルド成功を確認。

## TODO-Web07(AIのEND設置を全面禁止)

- ユーザー要望: 「そもそも、AIがENDブロックを設置することを禁止にしたい」
  - 前回(TODO-Web06)の「完成させるENDのみ抑制」をさらに強化し、AI は END を
    一切候補に含めない方式へ変更。
- 実装:
  - done: `buildCandidates` で `BlockType::End` をスキップ(AI の候補から完全除外)。
  - done: `performAITurn` の END 抑制ブロック(候補に END が無くなったため)を削除。
    - AI は END を置かないので `syntax_depth` は減らず、AI 起因のゲーム終了は
      盤面満杯のみ。
  - done: プレイヤー側の「完成判定」を「END で閉じて depth==0」に厳密化
    (`cobroc_add_block` / Pico `handlePlayerInput`)。
    トップレベルでの DRAW などは完成と誤判定しないよう修正。
  - done: モック(`mock.ts`)の AI も END を候補から除外(`AI_PLAYABLE`)。
  - done: `web/API_CONTRACT.md` に「AI は END を一切設置しない」を明記。
- 検証:
  - done: `core_test` に「AI が END を置いたら [unexpected]」チェックを追加し 0 件を確認。
  - done: ネイティブ/WASM パリティ一致を再確認(5シナリオ)。
    - 新ハッシュ: structured=3BF544D8, draws=F43A33F6, moves=E01971F1,
      short-run=045744D7, end-complete=8A0B3A3C
  - done: WASM スモークテスト全項目パス。
  - done: E2E 全 15 項目パス(END 完成テストは AI が開くスコープ分の END を
    複数回置くフローに更新)。
  - done: Pico Debug/Release ビルド成功。

## TODO-Web08(E2Eテストの正式配置)

- E2E テストをリポジトリ内に正式配置し、再現手順を整備した。
  - done: `web/e2e/e2e_test.mjs`(ヘッドレス Chrome + puppeteer-core、
    `E2E_URL` / `CHROME_PATH` 環境変数対応)。
  - done: `web/e2e/run_e2e.mjs`(dist/ を静的サーバーで配信するランナー)。
  - done: `package.json` に `npm run e2e`(ビルド→テスト) / `npm run e2e:serve`
    / `npm run wasm` を追加。`puppeteer-core` を devDependencies に追加。
  - done: リポジトリ内ランナーで E2E 全 15 項目パスを再確認。
