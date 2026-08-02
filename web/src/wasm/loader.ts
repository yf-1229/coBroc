/**
 * coBroc Web — WASM ローダー
 *
 * 実WASM (web/public/coBrocWeb.js + coBrocWeb.wasm、Emscripten MODULARIZE,
 * EXPORT_NAME=CoBrocModule) が存在すればそれを使い、無ければモックに
 * フォールバックする。公開APIは CoBrocCore で実/モック同一。
 */

import type { CoBrocCore, Snapshot } from './types';
import { createMockCore } from './mock';

// Emscripten モジュールの実態 (MODULARIZE のファクトリが生成するインスタンス)
interface EmscriptenModule {
  _cobroc_new_game(seed: number): number;
  _cobroc_free(ptr: number): void;
  _cobroc_cycle_type(ptr: number): void;
  _cobroc_cycle_param(ptr: number): void;
  _cobroc_add_block(ptr: number): void;
  _cobroc_set_input_color(ptr: number, color: number): void;
  _cobroc_select_input_color(ptr: number): void;
  _cobroc_ai_turn(ptr: number): void;
  _cobroc_run(ptr: number): void;
  _cobroc_undo(ptr: number): void;
  _cobroc_get_turn(ptr: number): number;
  _cobroc_get_move_count(ptr: number): number;
  _cobroc_get_selected_param(ptr: number): number;
  _cobroc_snapshot(ptr: number): number; // returns malloc'd char*
  _cobroc_free_snapshot(ptr: number): void;
  _cobroc_serialize(ptr: number): number; // returns malloc'd char*
  _cobroc_deserialize(ptr: number): number | 0;
  _malloc(size: number): number;
  _free(ptr: number): void;
  UTF8ToString(ptr: number): string;
  stringToUTF8(str: string, ptr: number, maxBytes: number): void;
}

type ModuleFactory = (opts: { locateFile: (file: string) => string }) => Promise<EmscriptenModule>;

declare global {
  interface Window {
    CoBrocModule?: ModuleFactory;
  }
}

const WASM_JS = './coBrocWeb.js';

let cachedCore: CoBrocCore | null = null;
let cachedUsingMock = false;

function injectScript(src: string): Promise<void> {
  return new Promise((resolve, reject) => {
    const existing = document.querySelector(`script[data-cobroc-wasm]`);
    if (existing) {
      resolve();
      return;
    }
    const script = document.createElement('script');
    script.src = src;
    script.dataset.cobrocWasm = '1';
    script.onload = () => resolve();
    script.onerror = () => reject(new Error('failed to load ' + src));
    document.head.appendChild(script);
  });
}

/** JS文字列をWASMヒープに書き込み、そのポインタを返す(呼び出し側で _free すること) */
function toCString(mod: EmscriptenModule, str: string): number {
  const buf = mod._malloc(str.length + 1);
  mod.stringToUTF8(str, buf, str.length + 1);
  return buf;
}

/** WASM ラッパー */
function wrapWasm(mod: EmscriptenModule): CoBrocCore {
  return {
    newGame(seed?: number): number {
      return mod._cobroc_new_game(seed ?? 0);
    },
    free(ptr: number): void {
      mod._cobroc_free(ptr);
    },
    cycleType(ptr: number): void {
      mod._cobroc_cycle_type(ptr);
    },
    cycleParam(ptr: number): void {
      mod._cobroc_cycle_param(ptr);
    },
    addBlock(ptr: number): void {
      mod._cobroc_add_block(ptr);
    },
    setInputColor(ptr: number, color: number): void {
      mod._cobroc_set_input_color(ptr, color);
    },
    selectInputColor(ptr: number): void {
      mod._cobroc_select_input_color(ptr);
    },
    aiTurn(ptr: number): void {
      mod._cobroc_ai_turn(ptr);
    },
    run(ptr: number): void {
      mod._cobroc_run(ptr);
    },
    undo(ptr: number): void {
      mod._cobroc_undo(ptr);
    },
    getTurn(ptr: number): number {
      return mod._cobroc_get_turn(ptr);
    },
    getMoveCount(ptr: number): number {
      return mod._cobroc_get_move_count(ptr);
    },
    getSelectedParam(ptr: number): number {
      return mod._cobroc_get_selected_param(ptr);
    },
    snapshot(ptr: number): Snapshot | null {
      const p = mod._cobroc_snapshot(ptr);
      if (!p) return null;
      const json = mod.UTF8ToString(p);
      mod._cobroc_free_snapshot(p);
      return JSON.parse(json) as Snapshot;
    },
    serialize(ptr: number): string {
      const p = mod._cobroc_serialize(ptr);
      if (!p) return '';
      const s = mod.UTF8ToString(p);
      mod._free(p);
      return s;
    },
    deserialize(data: string): number | null {
      const p = toCString(mod, data);
      const out = mod._cobroc_deserialize(p);
      mod._free(p);
      return out ? out : null;
    },
  };
}

/**
 * コアを初期化して返す(シングルトン)。
 * 実WASMがロードできれば実モジュール、できなければモック。
 */
export async function initCore(): Promise<{ core: CoBrocCore; usingMock: boolean }> {
  if (cachedCore) return { core: cachedCore, usingMock: cachedUsingMock };

  try {
    await injectScript(WASM_JS);
    if (!window.CoBrocModule) throw new Error('CoBrocModule factory not found');
    const mod = await window.CoBrocModule({
      locateFile: (file: string) => './' + file,
    });
    cachedCore = wrapWasm(mod);
    cachedUsingMock = false;
    console.info('[coBroc] using real WASM module');
  } catch (err) {
    console.warn('[coBroc] WASM unavailable, falling back to mock:', err);
    cachedCore = createMockCore();
    cachedUsingMock = true;
  }
  return { core: cachedCore, usingMock: cachedUsingMock };
}
