/**
 * coBroc Web — モック実装 (実WASMが無い間のフォールバック)
 *
 * 契約(web/API_CONTRACT.md)の関数シグネチャ・スナップショットスキーマに
 * 準拠した簡易ゲームエンジン。UI開発・テスト用。
 * 実WASM (web/public/coBrocWeb.js) が存在する場合は loader.ts が
 * こちらを使わず実モジュールを優先する。
 */

import type { CoBrocCore, Snapshot, SnapshotBlock } from './types';

interface MockState {
  seed: number;
  blocks: SnapshotBlock[];
  turn: number; // 0 player, 1 ai, 2 color_select, 3 run, 4 finished
  selected_block: string;
  selected_param: number;
  run_input_color: number;
  circles: { x: number; y: number; color: number }[];
}

const PLAYABLE = ['MOVE', 'DRAW', 'IF', 'ELSE', 'REPEAT', 'END'];
// AI は END ブロックを設置しない(スコープを閉じるのはプレイヤーの役目)
const AI_PLAYABLE = ['MOVE', 'DRAW', 'IF', 'ELSE', 'REPEAT'];
const PARAM_RANGE: Record<string, [number, number]> = {
  MOVE: [1, 19],
  DRAW: [1, 8],
  IF: [1, 8],
  REPEAT: [1, 7],
  ELSE: [0, 0],
  END: [0, 0],
};

function mkState(seed: number): MockState {
  return {
    seed: seed || Math.floor(Math.random() * 0xffffffff),
    blocks: [],
    turn: 0,
    selected_block: 'MOVE',
    selected_param: 1,
    run_input_color: 1,
    circles: [],
  };
}

function snapshot(s: MockState): Snapshot {
  const [pmin, pmax] = PARAM_RANGE[s.selected_block] ?? [0, 0];
  return {
    v: 1,
    game_id: 1,
    turn: (['player', 'ai', 'color_select', 'run', 'finished'] as const)[s.turn],
    move_count: s.blocks.length,
    max_moves: 16,
    syntax_depth: 0,
    selected_block: s.selected_block,
    selected_param: s.selected_param,
    param_min: pmin,
    param_max: pmax,
    run_input_color: s.run_input_color,
    block: s.blocks,
    circles: s.circles,
  };
}

export function createMockCore(): CoBrocCore {
  let state: MockState | null = null;

  const ptrOf = (ptr: number): MockState => {
    if (ptr !== 1) throw new Error('mock: invalid state ptr');
    return state!;
  };

  return {
    newGame(seed?: number): number {
      state = mkState(seed ?? 0);
      return 1;
    },
    free(_ptr: number): void {
      state = null;
    },
    cycleType(ptr: number): void {
      const s = ptrOf(ptr);
      const idx = PLAYABLE.indexOf(s.selected_block);
      s.selected_block = PLAYABLE[(idx + 1) % PLAYABLE.length];
      const [min] = PARAM_RANGE[s.selected_block] ?? [0, 0];
      s.selected_param = min;
    },
    cycleParam(ptr: number): void {
      const s = ptrOf(ptr);
      const [min, max] = PARAM_RANGE[s.selected_block] ?? [0, 0];
      if (max > min) s.selected_param = s.selected_param >= max ? min : s.selected_param + 1;
    },
    addBlock(ptr: number): void {
      const s = ptrOf(ptr);
      s.blocks.push({ type: s.selected_block, param: s.selected_param, from_ai: false, depth: 0 });
      s.turn = s.blocks.length >= 16 ? 2 : 1;
    },
    setInputColor(ptr: number, color: number): void {
      ptrOf(ptr).run_input_color = Math.max(1, Math.min(8, color));
    },
    selectInputColor(ptr: number): void {
      const s = ptrOf(ptr);
      if (s.turn === 0) s.turn = 2;
    },
    aiTurn(ptr: number): void {
      const s = ptrOf(ptr);
      if (s.turn !== 1) return;
      const t = AI_PLAYABLE[Math.floor(Math.random() * AI_PLAYABLE.length)];
      const [min, max] = PARAM_RANGE[t] ?? [0, 0];
      const param = max > min ? min + Math.floor(Math.random() * (max - min + 1)) : 0;
      s.blocks.push({ type: t, param, from_ai: true, depth: 0 });
      s.turn = s.blocks.length >= 16 ? 3 : 0;
    },
    run(ptr: number): void {
      const s = ptrOf(ptr);
      s.circles = [];
      for (let i = 0; i < Math.min(16, s.blocks.length); i++) {
        s.circles.push({
          x: Math.floor(Math.random() * 233) + 6,
          y: Math.floor(Math.random() * 233) + 6,
          color: 1 + Math.floor(Math.random() * 8),
        });
      }
      s.turn = 4;
    },
    getTurn(ptr: number): number {
      return ptrOf(ptr).turn;
    },
    getMoveCount(ptr: number): number {
      return ptrOf(ptr).blocks.length;
    },
    getSelectedParam(ptr: number): number {
      return ptrOf(ptr).selected_param;
    },
    snapshot(ptr: number): Snapshot {
      return snapshot(ptrOf(ptr));
    },
    serialize(ptr: number): string {
      return btoa(JSON.stringify(snapshot(ptrOf(ptr))));
    },
    deserialize(data: string): number | null {
      try {
        const obj = JSON.parse(atob(data));
        if (!obj || obj.v !== 1) return null;
        state = mkState(obj.seed ?? 0);
        state.blocks = obj.block ?? [];
        state.turn = (['player', 'ai', 'color_select', 'run', 'finished'] as const).indexOf(obj.turn);
        state.run_input_color = obj.run_input_color ?? 1;
        state.circles = obj.circles ?? [];
        return 1;
      } catch {
        return null;
      }
    },
  };
}
