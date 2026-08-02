/**
 * coBroc Web — Canvas 2D 描画
 *
 * - メイン: 左半分にフローチャート図形(縦接続線)、右半分にブロック情報
 * - color_select: 大きな色ドット
 * - finished: 円(circles)を 240x240 仮想座標に描画
 * devicePixelRatio / レスポンシブ対応。
 */

import type { Snapshot, SnapshotBlock } from '../wasm/types';
import { BLOCK_COLORS, paramColor } from '../wasm/types';

export interface RenderTheme {
  bg: string;
  card: string;
  border: string;
  text: string;
  sub: string;
  accent: string;
}

const LIGHT: RenderTheme = {
  bg: '#EEF3F9',
  card: '#FFFFFF',
  border: '#C5D1DE',
  text: '#1F2937',
  sub: '#6B7280',
  accent: '#2F80ED',
};

export function blockShape(type: string): 'circle' | 'rect' | 'triangle' | 'diamond' | 'round' {
  switch (type) {
    case 'MOVE': return 'rect';
    case 'DRAW': return 'triangle';
    case 'IF': return 'diamond';
    case 'REPEAT': return 'round';
    case 'ELSE': return 'round';
    case 'END': return 'circle';
    default: return 'circle';
  }
}

interface DrawCtx {
  ctx: CanvasRenderingContext2D;
  W: number; // CSS px
  H: number;
  dpr: number;
}

function setupCanvas(canvas: HTMLCanvasElement): DrawCtx | null {
  const rect = canvas.getBoundingClientRect();
  if (rect.width === 0 || rect.height === 0) return null;
  const dpr = window.devicePixelRatio || 1;
  canvas.width = Math.round(rect.width * dpr);
  canvas.height = Math.round(rect.height * dpr);
  const ctx = canvas.getContext('2d');
  if (!ctx) return null;
  ctx.setTransform(dpr, 0, 0, dpr, 0, 0);
  return { ctx, W: rect.width, H: rect.height, dpr };
}

function clear(d: DrawCtx, bg: string) {
  d.ctx.clearRect(0, 0, d.W, d.H);
  d.ctx.fillStyle = bg;
  d.ctx.fillRect(0, 0, d.W, d.H);
}

function roundRect(ctx: CanvasRenderingContext2D, x: number, y: number, w: number, h: number, r: number) {
  ctx.beginPath();
  ctx.moveTo(x + r, y);
  ctx.lineTo(x + w - r, y);
  ctx.quadraticCurveTo(x + w, y, x + w, y + r);
  ctx.lineTo(x + w, y + h - r);
  ctx.quadraticCurveTo(x + w, y + h, x + w - r, y + h);
  ctx.lineTo(x + r, y + h);
  ctx.quadraticCurveTo(x, y + h, x, y + h - r);
  ctx.lineTo(x, y + r);
  ctx.quadraticCurveTo(x, y, x + r, y);
  ctx.closePath();
}

/** 図形を (cx, cy) 中心・size サイズで描画(塗り + 枠線) */
function drawShape(d: DrawCtx, shape: 'circle' | 'rect' | 'triangle' | 'diamond' | 'round',
                   cx: number, cy: number, size: number, color: string, selected: boolean) {
  const ctx = d.ctx;
  const half = size / 2;
  const lineW = selected ? 3 : 2;
  ctx.fillStyle = color;
  ctx.globalAlpha = selected ? 0.35 : 0.22;
  ctx.strokeStyle = color;
  ctx.lineWidth = lineW;
  ctx.lineJoin = 'round';

  ctx.beginPath();
  switch (shape) {
    case 'circle':
      ctx.arc(cx, cy, half, 0, Math.PI * 2);
      break;
    case 'rect':
      ctx.rect(cx - half, cy - half, size, size);
      break;
    case 'round':
      roundRect(ctx, cx - half, cy - half, size, size, 6);
      break;
    case 'triangle':
      ctx.moveTo(cx, cy - half);
      ctx.lineTo(cx + half, cy + half);
      ctx.lineTo(cx - half, cy + half);
      ctx.closePath();
      break;
    case 'diamond':
      ctx.moveTo(cx, cy - half);
      ctx.lineTo(cx + half, cy);
      ctx.lineTo(cx, cy + half);
      ctx.lineTo(cx - half, cy);
      ctx.closePath();
      break;
  }
  ctx.fill();
  ctx.globalAlpha = 1;
  ctx.stroke();
}

function drawSwatch(d: DrawCtx, x: number, y: number, r: number, param: number) {
  const ctx = d.ctx;
  ctx.beginPath();
  ctx.arc(x, y, r, 0, Math.PI * 2);
  ctx.fillStyle = paramColor(param);
  ctx.fill();
  ctx.lineWidth = 1;
  ctx.strokeStyle = '#222222';
  ctx.stroke();
}

function drawText(d: DrawCtx, text: string, x: number, y: number, color: string,
                  size = 11, bold = false, align: CanvasTextAlign = 'left') {
  const ctx = d.ctx;
  ctx.font = `${bold ? '600 ' : ''}${size}px 'Segoe UI', -apple-system, sans-serif`;
  ctx.fillStyle = color;
  ctx.textAlign = align;
  ctx.textBaseline = 'middle';
  ctx.fillText(text, x, y);
}

// ── メイン(フローチャート) ──────────────────────────────────────────────

const ROW_H = 34;
const ROW_GAP = 12;

export function renderMain(canvas: HTMLCanvasElement, snap: Snapshot, focus: number) {
  const d = setupCanvas(canvas);
  if (!d) return;
  clear(d, LIGHT.bg);
  const ctx = d.ctx;

  const margin = 8;
  const innerW = d.W - margin * 2;
  const colGap = 8;
  const leftW = Math.floor((innerW - colGap) / 2);
  const rightW = innerW - leftW - colGap;
  const leftX = margin;
  const rightX = margin + leftW + colGap;

  // 中央の縦区切り線
  const dividerX = leftX + leftW + colGap / 2;
  ctx.strokeStyle = '#D8E0EA';
  ctx.lineWidth = 1;
  ctx.beginPath();
  ctx.moveTo(dividerX, 4);
  ctx.lineTo(dividerX, d.H - 4);
  ctx.stroke();

  const items: ({ kind: 'run' } | { kind: 'block'; b: SnapshotBlock; i: number })[] = [{ kind: 'run' }];
  for (let i = 0; i < snap.block.length; i++) items.push({ kind: 'block', b: snap.block[i], i });

  // 表示可能行数を計算してスクロール
  const rowSpace = ROW_H + ROW_GAP;
  const visible = Math.max(1, Math.floor((d.H - 8 + ROW_GAP) / rowSpace));
  const total = items.length;
  let top = 0;
  if (total > visible) {
    if (focus + 1 > visible) top = Math.min(focus + 1 - visible, total - visible);
  }

  const shapeSize = 24;
  const centerX = leftX + leftW / 2;
  const INDENT_STEP = 8;
  const MAX_INDENT_DEPTH = 5;
  const maxShapeRight = leftX + leftW - 4 - shapeSize / 2;
  let prevBottom = -1;
  let prevCenterX = centerX;

  const end = Math.min(total, top + visible);
  for (let idx = top; idx < end; idx++) {
    const row = idx - top;
    const y = 4 + row * rowSpace;
    const symbolY = y + (ROW_H - shapeSize) / 2;
    const selected = idx === focus;

    const item = items[idx];
    const depth = item.kind === 'run'
      ? 0
      : Math.min(item.b.depth, MAX_INDENT_DEPTH);
    const curCenterX = Math.min(centerX + depth * INDENT_STEP, maxShapeRight);

    // 接続線(L字: 前の行の中心 x → 現在の行の中心 x)
    if (prevBottom >= 0) {
      ctx.strokeStyle = '#7A8795';
      ctx.lineWidth = 2;
      ctx.beginPath();
      ctx.moveTo(prevCenterX, prevBottom);
      ctx.lineTo(curCenterX, prevBottom);
      ctx.lineTo(curCenterX, symbolY);
      ctx.stroke();
    }

    const color = item.kind === 'run' ? BLOCK_COLORS.Run : BLOCK_COLORS[item.b.type] ?? BLOCK_COLORS.NONE;

    // 左: 図形
    if (item.kind === 'run') {
      drawShape(d, 'circle', curCenterX, symbolY + shapeSize / 2, shapeSize, color, selected);
      drawText(d, 'Run', curCenterX, symbolY + shapeSize / 2, color, 9, true, 'center');
    } else {
      drawShape(d, blockShape(item.b.type), curCenterX, symbolY + shapeSize / 2, shapeSize, color, selected);
    }

    // 右: 情報カード
    if (selected) {
      ctx.fillStyle = '#EAF3FF';
      roundRect(ctx, rightX, y, rightW, ROW_H, 4);
      ctx.fill();
    }
    const name = item.kind === 'run' ? 'Run' : item.b.type;
    drawText(d, (selected ? '> ' : '') + name, rightX + 6, y + 9, LIGHT.text, 11, true);
    const swatchX = rightX + rightW - 14;
    if (item.kind === 'block' && (item.b.type === 'DRAW' || item.b.type === 'IF')) {
      drawSwatch(d, swatchX, y + 12, 5, item.b.param);
    }
    const detail = item.kind === 'run'
      ? 'start node'
      : item.b.type === 'DRAW' || item.b.type === 'IF' || item.b.type === 'MOVE' || item.b.type === 'REPEAT'
        ? `param:${item.b.param}${item.b.from_ai ? ' [AI]' : ''}`
        : item.b.from_ai ? '[AI]' : 'no param';
    drawText(d, detail, rightX + 6, y + 22, LIGHT.sub, 10);

    prevBottom = symbolY + shapeSize;
    prevCenterX = curCenterX;
  }

  // ヘッダー情報(キャンバス上部はDOM側に持たせるため、ここでは空)
}

// ── 色選択 ──────────────────────────────────────────────────────────────

export function renderColorSelect(canvas: HTMLCanvasElement, snap: Snapshot) {
  const d = setupCanvas(canvas);
  if (!d) return;
  clear(d, LIGHT.bg);
  const ctx = d.ctx;

  const cardX = 8;
  const cardY = 8;
  const cardW = d.W - 16;
  const cardH = d.H - 16;
  ctx.fillStyle = LIGHT.card;
  roundRect(ctx, cardX, cardY, cardW, cardH, 8);
  ctx.fill();
  ctx.strokeStyle = '#A95DF5';
  ctx.lineWidth = 1;
  ctx.stroke();

  drawText(d, 'Select IF input color', cardX + 12, cardY + 20, LIGHT.text, 13, true);
  drawText(d, 'X:next  Y:run', cardX + 12, cardY + 40, LIGHT.sub, 10);

  // 大きな色ドット
  const dotR = 36;
  const dotCx = cardX + cardW / 2;
  const dotCy = cardY + cardH / 2 - 14;
  ctx.beginPath();
  ctx.arc(dotCx, dotCy, dotR, 0, Math.PI * 2);
  ctx.fillStyle = '#F5F7FA';
  ctx.fill();
  ctx.strokeStyle = '#CED8E3';
  ctx.stroke();
  drawSwatch(d, dotCx, dotCy, dotR - 8, snap.run_input_color);

  drawText(d, `COLOR PARAM: ${snap.run_input_color}`, cardX + cardW / 2, dotCy + dotR + 22, LIGHT.text, 12, true, 'center');
  drawText(d, 'IF blocks compare this color.', cardX + cardW / 2, dotCy + dotR + 42, LIGHT.sub, 10, false, 'center');
}

// ── 実行結果 ────────────────────────────────────────────────────────────

export function renderFinished(canvas: HTMLCanvasElement, snap: Snapshot) {
  const d = setupCanvas(canvas);
  if (!d) return;
  clear(d, '#F3F6FA');
  const ctx = d.ctx;

  const topH = 32;
  ctx.fillStyle = LIGHT.card;
  roundRect(ctx, 8, 8, d.W - 16, topH, 6);
  ctx.fill();
  ctx.strokeStyle = '#14A44D';
  ctx.stroke();
  drawText(d, `Run Preview  circles:${snap.circles.length}`, 18, 8 + topH / 2, LIGHT.text, 12, true);

  // 240x240 仮想座標 → canvas へスケーリング
  const areaX = 10;
  const areaY = 8 + topH + 8;
  const areaW = d.W - 20;
  const areaH = d.H - areaY - 10;
  ctx.fillStyle = LIGHT.card;
  roundRect(ctx, areaX, areaY, areaW, areaH, 6);
  ctx.fill();
  ctx.strokeStyle = LIGHT.border;
  ctx.stroke();

  const VIRT = 240;
  for (const c of snap.circles) {
    const px = areaX + 8 + (c.x / VIRT) * (areaW - 16);
    const py = areaY + 8 + (c.y / VIRT) * (areaH - 16);
    const r = Math.max(2, Math.min(8, areaW / 60));
    ctx.beginPath();
    ctx.arc(px, py, r, 0, Math.PI * 2);
    ctx.fillStyle = paramColor(c.color);
    ctx.fill();
    ctx.lineWidth = 1;
    ctx.strokeStyle = '#222222';
    ctx.stroke();
  }

  drawText(d, 'A:new game', 18, d.H - 4, LIGHT.sub, 10);
}

// ── ディスパッチ ────────────────────────────────────────────────────────

export function render(canvas: HTMLCanvasElement, snap: Snapshot, focus: number) {
  switch (snap.turn) {
    case 'color_select':
      renderColorSelect(canvas, snap);
      break;
    case 'finished':
      renderFinished(canvas, snap);
      break;
    default:
      renderMain(canvas, snap, focus);
      break;
  }
}

export { LIGHT as theme };
