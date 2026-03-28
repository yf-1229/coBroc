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
