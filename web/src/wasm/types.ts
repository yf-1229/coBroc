/**
 * coBroc Web — 型定義・API契約 (web/API_CONTRACT.md 準拠)
 */

// スナップショット JSON スキーマ v1
export interface SnapshotBlock {
  type: string;      // "NONE"|"MOVE"|"DRAW"|"IF"|"REPEAT"|"END"|"ELSE"
  param: number;
  from_ai: boolean;
  depth: number;
}

export interface SnapshotCircle {
  x: number;        // 0..239 (240x240 仮想空間)
  y: number;
  color: number;    // 1..8 (パラメータ→色)
}

export interface Snapshot {
  v: number;
  game_id: number;
  turn: 'player' | 'ai' | 'color_select' | 'run' | 'finished';
  move_count: number;
  max_moves: number;
  syntax_depth: number;
  selected_block: string;
  selected_param: number;
  param_min: number;
  param_max: number;
  run_input_color: number;
  block: SnapshotBlock[];
  circles: SnapshotCircle[];
}

// フロントエンドが使うコア API(実WASMとモックで同一シグネチャ)
export interface CoBrocCore {
  newGame(seed?: number): number;
  free(ptr: number): void;

  cycleType(ptr: number): void;
  cycleParam(ptr: number): void;
  addBlock(ptr: number): void;
  setInputColor(ptr: number, color: number): void;
  selectInputColor(ptr: number): void; // PlayerTurn -> color_select (Y key)
  aiTurn(ptr: number): void;
  run(ptr: number): void;

  getTurn(ptr: number): number;
  getMoveCount(ptr: number): number;
  getSelectedParam(ptr: number): number;

  snapshot(ptr: number): Snapshot;
  serialize(ptr: number): string;       // base64
  deserialize(data: string): number | null;
}

// リプレイ用の操作ログ
export type ReplayAction =
  | { op: 'cycle_type' }
  | { op: 'cycle_param' }
  | { op: 'add' }
  | { op: 'select_input_color' }
  | { op: 'set_color'; color: number }
  | { op: 'run' };

export interface ReplayRecord {
  seed: number;
  actions: ReplayAction[];
}

// 色パレット(param → 表示色) 契約セクション3
export const PARAM_COLORS: string[] = [
  '#000000', // 0: 未使用
  '#FFFFFF', // 1: 白
  '#FF0000', // 2: 赤
  '#FF823A', // 3: 橙 (RGB565 0xFC07)
  '#FFFF00', // 4: 黄
  '#00FF00', // 5: 緑
  '#0000FF', // 6: 青
  '#FF00FF', // 7: マゼンタ
  '#000000', // 8: 黒
];

export const BLOCK_NAMES: Record<string, string> = {
  NONE: 'NONE',
  MOVE: 'MOVE',
  DRAW: 'DRAW',
  IF: 'IF',
  REPEAT: 'REPEAT',
  END: 'END',
  ELSE: 'ELSE',
};

export const BLOCK_COLORS: Record<string, string> = {
  Run: '#16A34A',
  MOVE: '#2F80ED',
  DRAW: '#14A44D',
  IF: '#A95DF5',
  ELSE: '#E056FD',
  REPEAT: '#F39C12',
  END: '#6C757D',
  NONE: '#343A40',
};

export function paramColor(param: number): string {
  const i = Math.max(1, Math.min(8, param));
  return PARAM_COLORS[i] ?? '#000000';
}
