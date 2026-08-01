/**
 * coBroc Web — リプレイ
 *
 * 操作ログ(ReplayRecord)を記録し、シード+操作の再適用で決定的に再生する。
 * AIターンは自動で aiTurn を呼ぶ(契約セクション5)。
 */

import type { CoBrocCore, ReplayAction, ReplayRecord } from './wasm/types';

export class ReplayRecorder {
  private actions: ReplayAction[] = [];
  private seed: number;

  constructor(seed: number) {
    this.seed = seed;
  }

  reset(seed: number): void {
    this.seed = seed;
    this.actions = [];
  }

  record(action: ReplayAction): void {
    this.actions.push(action);
  }

  toRecord(): ReplayRecord {
    return { seed: this.seed, actions: [...this.actions] };
  }
}

export interface ReplayCallbacks {
  onState(ptr: number): void;       // 各操作後に呼ばれる(描画用)
  onDone(): void;
  onError(message: string): void;
}

/**
 * リプレイ再生。delayMs 毎に1操作を適用する。
 * 状態ポインタ(ptr)の所有権は呼び出し側(main.ts)にあり、ここでは
 * 生成も解放もしない。戻り値は停止用の関数。
 */
export function startReplay(
  core: CoBrocCore,
  ptr: number,
  record: ReplayRecord,
  delayMs: number,
  cb: ReplayCallbacks,
): () => void {
  let stopped = false;

  const apply = async (action: ReplayAction): Promise<boolean> => {
    switch (action.op) {
      case 'cycle_type': core.cycleType(ptr); break;
      case 'cycle_param': core.cycleParam(ptr); break;
      case 'add': core.addBlock(ptr); break;
      case 'select_input_color': core.selectInputColor(ptr); break;
      case 'set_color': core.setInputColor(ptr, action.color); break;
      case 'run': core.run(ptr); break;
    }
    // AI ターンは自動で進める(満杯で turn=3(run) になった場合は自動 run)
    let guard = 0;
    while (core.getTurn(ptr) === 1 && guard < 32) {
      core.aiTurn(ptr);
      guard++;
      if (core.getTurn(ptr) === 3) {
        core.run(ptr);
      }
    }
    cb.onState(ptr);
    return true;
  };

  (async () => {
    try {
      for (const action of record.actions) {
        if (stopped) return;
        await apply(action);
        if (delayMs > 0) await new Promise((r) => setTimeout(r, delayMs));
      }
      if (!stopped) cb.onDone();
    } catch (e) {
      cb.onError(String(e));
    }
  })();

  return () => {
    stopped = true;
  };
}
