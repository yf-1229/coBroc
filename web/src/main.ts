/**
 * coBroc Web — メインエントリ・ゲームループ
 *
 * ターン遷移規約(web/API_CONTRACT.md セクション2):
 * - 開始: player
 * - add_block 成功 → ai(満杯なら color_select)
 * - ai ターン → ai_turn → player / run(満杯時)
 * - run → finished
 * - color_select: set_color(X) → run(Y)
 */

import { initCore } from './wasm/loader';
import type { CoBrocCore, ReplayRecord, Snapshot } from './wasm/types';
import { render } from './ui/render';
import { autoSave, buildSaveFile, downloadSave, loadAutoSave, restoreSave, uploadSave } from './storage';
import { ReplayRecorder, startReplay } from './replay';

// DOM refs
const $ = <T extends HTMLElement>(id: string): T => document.getElementById(id) as T;
const canvas = $<HTMLCanvasElement>('game-canvas');
const statusEl = $<HTMLDivElement>('status');
const turnEl = $<HTMLSpanElement>('turn-label');
const selEl = $<HTMLSpanElement>('sel-label');
const buttons: Record<string, HTMLButtonElement> = {
  a: $<HTMLButtonElement>('btn-a'),
  b: $<HTMLButtonElement>('btn-b'),
  x: $<HTMLButtonElement>('btn-x'),
  y: $<HTMLButtonElement>('btn-y'),
};

let core: CoBrocCore | null = null;
let ptr = 0;
let seed = 0;
let snap: Snapshot | null = null;
let recorder: ReplayRecorder | null = null;
let busy = false;          // 操作処理中(リプレイ/思考中)
let replayStop: (() => void) | null = null;

// ── 状態取得・描画 ──────────────────────────────────────────────────────

function updateStatus(msg: string): void {
  statusEl.textContent = msg;
}

function focusIndex(s: Snapshot): number {
  return s.block.length;
}

function refresh(): void {
  if (!core || !ptr) return;
  snap = core.snapshot(ptr);
  if (!snap) return;

  // ヘッダー
  turnEl.textContent = `TURN: ${snap.turn.toUpperCase()}`;
  selEl.textContent = `SEL: ${snap.selected_block}  P:${snap.selected_param}/${snap.param_max}`;

  // 操作ボタンの有効/無効
  // A(追加)は player ターンのみ。ENDでプログラム完成後は color_select へ
  // 遷移するため、以降は追加できない(実機のYキー相当の動線と同じ)。
  buttons.a.disabled = snap.turn !== 'player' || busy;
  buttons.b.disabled = snap.turn !== 'player' || busy;
  buttons.x.disabled = snap.turn !== 'player' && snap.turn !== 'color_select';
  buttons.y.disabled = snap.turn !== 'player' && snap.turn !== 'color_select' || busy;

  render(canvas, snap, focusIndex(snap));
}

function autoSaveCurrent(): void {
  if (!core || !ptr) return;
  // 完了/実行中状態はオートセーブしない(リロード時に TURN: FINISHED で
  // 始まってしまうのを防ぐ。仕様: ゲーム開始時は player)。
  const current = snap ?? core.snapshot(ptr);
  if (current && (current.turn === 'finished' || current.turn === 'run')) {
    autoSave(null);
    return;
  }
  autoSave(buildSaveFile(core, ptr, seed));
}

// ── 操作 ────────────────────────────────────────────────────────────────

function runAiIfNeeded(): void {
  // add_block 成功後は ai ターン
  if (!core || !ptr) return;
  let guard = 0;
  while (core.getTurn(ptr) === 1 && guard < 32) {
    core.aiTurn(ptr);
    guard++;
    if (core.getTurn(ptr) === 3) {
      // 満杯 → 自動 run(実機は色選択を挟むが、満杯時は契約どおり run へ)
      core.run(ptr);
    }
  }
}

async function doAction(action: 'cycle_type' | 'cycle_param' | 'add' | 'run' | 'next_color' | 'new_game'): Promise<void> {
  if (!core || !ptr || busy) return;
  busy = true;
  try {
    switch (action) {
      case 'cycle_type':
        if (core.getTurn(ptr) === 0) {
          core.cycleType(ptr);
          recorder?.record({ op: 'cycle_type' });
        }
        break;
      case 'cycle_param':
        if (core.getTurn(ptr) === 0) {
          core.cycleParam(ptr);
          recorder?.record({ op: 'cycle_param' });
        }
        break;
      case 'add':
        if (core.getTurn(ptr) === 0) {
          core.addBlock(ptr);
          recorder?.record({ op: 'add' });
          if (core.getTurn(ptr) === 1) {
            // AI ターン: 思考演出
            updateStatus('AI thinking...');
            await sleep(350);
            runAiIfNeeded();
            if (core.getTurn(ptr) === 0) updateStatus('');
          } else if (core.getTurn(ptr) === 2) {
            updateStatus('Program full. Select input color (X) then run (Y).');
          }
        }
        break;
      case 'next_color':
        if (core.getTurn(ptr) === 2) {
          const next = snap ? snap.run_input_color + 1 : 1;
          const color = next > 8 ? 1 : next;
          core.setInputColor(ptr, color);
          recorder?.record({ op: 'set_color', color });
        }
        break;
      case 'run':
        if (core.getTurn(ptr) === 0) {
          // PlayerTurn 中は色選択画面へ遷移(実機の Y キー相当)
          core.selectInputColor(ptr);
          recorder?.record({ op: 'select_input_color' });
          updateStatus('Select input color: X:next  Y:run');
        } else if (core.getTurn(ptr) === 2) {
          core.run(ptr);
          recorder?.record({ op: 'run' });
          updateStatus('Executing...');
        }
        break;
      case 'new_game':
        startNewGame();
        break;
    }
    refresh();
    autoSaveCurrent();
  } finally {
    busy = false;
    refresh(); // busy を戻した後に再描画してボタン状態を更新
  }
}

function sleep(ms: number): Promise<void> {
  return new Promise((r) => setTimeout(r, ms));
}

// ── 新規ゲーム / ロード ─────────────────────────────────────────────────

function startNewGame(seedOverride?: number): void {
  if (!core) return;
  if (ptr) core.free(ptr);
  seed = seedOverride ?? (Math.floor(Math.random() * 0xffffffff) || 1);
  ptr = core.newGame(seed);
  recorder = new ReplayRecorder(seed);
  snap = null;
  updateStatus('Player turn. A:add B:type X:param Y:run');
  refresh();
  autoSaveCurrent();
}

function loadGame(save: { seed: number; data: string }): void {
  if (!core) return;
  if (ptr) core.free(ptr);
  const newPtr = core.deserialize(save.data);
  if (!newPtr) {
    updateStatus('Load failed.');
    return;
  }
  ptr = newPtr;
  seed = save.seed;
  recorder = new ReplayRecorder(seed);
  refresh();
  autoSaveCurrent();
  updateStatus('Loaded.');
}

function stopReplayIfRunning(): void {
  if (replayStop) {
    replayStop();
    replayStop = null;
  }
  busy = false;
}

// ── リプレイ ────────────────────────────────────────────────────────────

function startReplayMode(record: ReplayRecord): void {
  if (!core) return;
  stopReplayIfRunning();
  if (ptr) core.free(ptr);
  busy = true;
  updateStatus('Replaying...');

  // 状態ポインタの所有権は main.ts 側にある(リプレイ中は新規ゲームで作成)
  ptr = core.newGame(record.seed);

  // ストッパーの世代管理: onDone/onError が古いリプレイの後始末で
  // 新しい replayStop を上書きしないよう、クロージャで stopper を捕捉する
  let stopper: (() => void) | null = null;
  stopper = startReplay(core, ptr, record, 120, {
    onState: (p) => {
      ptr = p;
      refresh();
      autoSaveCurrent();
    },
    onDone: () => {
      if (replayStop === stopper) replayStop = null;
      busy = false;
      updateStatus('Replay finished. A:new game');
    },
    onError: (msg) => {
      if (replayStop === stopper) replayStop = null;
      busy = false;
      updateStatus('Replay error: ' + msg);
    },
  });
  replayStop = stopper;
}

// ── イベント結線 ────────────────────────────────────────────────────────

function bindControls(): void {
  buttons.a.addEventListener('click', () => doAction('add'));
  buttons.b.addEventListener('click', () => doAction('cycle_type'));
  buttons.x.addEventListener('click', () => doAction(snap?.turn === 'color_select' ? 'next_color' : 'cycle_param'));
  buttons.y.addEventListener('click', () => doAction(snap?.turn === 'color_select' ? 'run' : 'run'));

  $<HTMLButtonElement>('btn-save').addEventListener('click', () => {
    if (core && ptr) downloadSave(buildSaveFile(core, ptr, seed));
  });

  const fileInput = $<HTMLInputElement>('file-input');
  $<HTMLButtonElement>('btn-load').addEventListener('click', () => fileInput.click());
  fileInput.addEventListener('change', async () => {
    const file = fileInput.files?.[0];
    if (!file) return;
    const save = await uploadSave(file);
    if (save) loadGame(save);
    else updateStatus('Import failed.');
    fileInput.value = '';
  });

  $<HTMLButtonElement>('btn-replay').addEventListener('click', () => {
    if (recorder && recorder.toRecord().actions.length > 0) {
      startReplayMode(recorder.toRecord());
    } else {
      updateStatus('No actions recorded yet.');
    }
  });

  document.addEventListener('keydown', (e) => {
    if (e.repeat) return;
    switch (e.key.toLowerCase()) {
      case 'a': doAction('add'); break;
      case 'b': doAction('cycle_type'); break;
      case 'x': doAction(snap?.turn === 'color_select' ? 'next_color' : 'cycle_param'); break;
      case 'y': doAction('run'); break;
      case 'n': doAction('new_game'); break;
      case 'r': $<HTMLButtonElement>('btn-replay').click(); break;
      case 's': $<HTMLButtonElement>('btn-save').click(); break;
    }
  });
}

// ── 起動 ────────────────────────────────────────────────────────────────

async function main(): Promise<void> {
  const { core: c, usingMock } = await initCore();
  core = c;

  bindControls();
  const loading = $<HTMLDivElement>('loading-msg');
  loading.style.display = 'none';
  $<HTMLDivElement>('controls').style.display = 'flex';
  updateStatus(usingMock ? 'MOCK mode (WASM not found)' : 'WASM ready');

  // 自動セーブがあれば復元
  const auto = loadAutoSave();
  if (auto) {
    const newPtr = core.deserialize(auto.data);
    if (newPtr) {
      // 完了/実行中状態のセーブは復元せず破棄し、新規ゲームを開始する
      // (仕様: ゲーム開始時は player。TURN: FINISHED で始まるのを防ぐ)。
      // snapshot が取得できない場合も復元不能として破棄する。
      const restored = core.snapshot(newPtr);
      if (!restored || restored.turn === 'finished' || restored.turn === 'run') {
        core.free(newPtr);
        autoSave(null);
      } else {
        ptr = newPtr;
        seed = auto.seed;
        recorder = new ReplayRecorder(seed);
        refresh();
        autoSaveCurrent();
        updateStatus('Restored autosave.');
        return;
      }
    }
  }
  startNewGame();
}

// リサイズ対応
window.addEventListener('resize', () => {
  if (snap) refresh();
});

window.addEventListener('DOMContentLoaded', () => {
  void main();
});
