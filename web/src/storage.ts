/**
 * coBroc Web — セーブ/ロード
 *
 * - localStorage に自動セーブ(毎ターン) + 手動セーブ
 * - エクスポート/インポート(JSONファイル)
 * 保存形式: { version: 1, seed, data(base64), savedAt }
 */

import type { CoBrocCore } from './wasm/types';

const AUTO_KEY = 'cobroc_autosave';
const SAVE_VERSION = 1;

export interface SaveFile {
  version: number;
  seed: number;
  data: string; // cobroc_serialize の base64
  savedAt: string;
}

export function buildSaveFile(core: CoBrocCore, ptr: number, seed: number): SaveFile {
  return {
    version: SAVE_VERSION,
    seed,
    data: core.serialize(ptr),
    savedAt: new Date().toISOString(),
  };
}

export function autoSave(save: SaveFile | null): void {
  if (!save) {
    localStorage.removeItem(AUTO_KEY);
    return;
  }
  try {
    localStorage.setItem(AUTO_KEY, JSON.stringify(save));
  } catch (e) {
    console.warn('[coBroc] autosave failed:', e);
  }
}

export function loadAutoSave(): SaveFile | null {
  try {
    const raw = localStorage.getItem(AUTO_KEY);
    if (!raw) return null;
    const parsed = JSON.parse(raw) as SaveFile;
    if (parsed.version !== SAVE_VERSION || typeof parsed.data !== 'string') return null;
    return parsed;
  } catch {
    return null;
  }
}

/** SaveFile を復元し、新しい状態ポインタを返す(失敗時 null) */
export function restoreSave(core: CoBrocCore, save: SaveFile): number | null {
  return core.deserialize(save.data);
}

export function downloadSave(save: SaveFile): void {
  const blob = new Blob([JSON.stringify(save, null, 2)], { type: 'application/json' });
  const url = URL.createObjectURL(blob);
  const a = document.createElement('a');
  a.href = url;
  a.download = `cobroc_save_${save.seed}_${save.savedAt.replace(/[:.]/g, '-')}.json`;
  a.click();
  URL.revokeObjectURL(url);
}

export function uploadSave(file: File): Promise<SaveFile | null> {
  return new Promise((resolve) => {
    const reader = new FileReader();
    reader.onload = () => {
      try {
        const parsed = JSON.parse(String(reader.result)) as SaveFile;
        if (parsed.version !== SAVE_VERSION || typeof parsed.data !== 'string') {
          resolve(null);
          return;
        }
        resolve(parsed);
      } catch {
        resolve(null);
      }
    };
    reader.onerror = () => resolve(null);
    reader.readAsText(file);
  });
}
