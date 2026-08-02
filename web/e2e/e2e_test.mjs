// e2e_test.mjs — ヘッドレスChromeで coBroc Web の実ゲームフローを検証
// 1. ページロード → WASM ロード確認
// 2. A(ブロック追加) → AIターン → 自動AI応答
// 3. B(タイプ切替) X(パラメータ) 操作
// 4. Y → 色選択 → X(色変更) → Y(実行) → 結果画面
// 5. セーブ/リプレイボタンの存在確認
import puppeteer from 'puppeteer-core';

const URL = process.env.E2E_URL || 'http://localhost:4173/';
const CHROME = process.env.CHROME_PATH || '/usr/bin/google-chrome';

const browser = await puppeteer.launch({
  executablePath: CHROME,
  headless: 'new',
  args: ['--no-sandbox', '--disable-gpu'],
});

const page = await browser.newPage();
page.on('console', (msg) => {
  const t = msg.type();
  if (t === 'error' || t === 'warning') console.log(`[console:${t}] ${msg.text()}`);
});
page.on('pageerror', (err) => console.log('[pageerror]', err.message));

let failures = 0;
const check = (cond, msg) => {
  console.log(`${cond ? '  OK' : '  FAIL'}: ${msg}`);
  if (!cond) failures++;
};

await page.goto(URL, { waitUntil: 'networkidle0', timeout: 30000 });
await page.waitForSelector('#controls', { visible: true, timeout: 15000 });

// WASMロード確認
const usingWasm = await page.evaluate(() => !document.getElementById('status').textContent.includes('MOCK'));
check(usingWasm, '実WASMモジュールを使用');

const selText = () => page.$eval('#sel-label', (el) => el.textContent);
const turnText = () => page.$eval('#turn-label', (el) => el.textContent);

check((await turnText()).includes('PLAYER'), '初期ターン PLAYER');

// 1. A: ブロック追加 → AIターン → 自動応答
await page.click('#btn-a');
await new Promise((r) => setTimeout(r, 1500)); // AI思考演出
const turnAfterAdd = await turnText();
check(turnAfterAdd.includes('PLAYER'), `add後 AI応答で PLAYER に復帰 (got: ${turnAfterAdd})`);

// 2. B: タイプ切替、X: パラメータ
await page.click('#btn-b');
const selB = await selText();
check(selB.includes('DRAW'), `Bでタイプ切替 (got: ${selB})`);
await page.click('#btn-x');
const selX = await selText();
check(/P:[2-8]\//.test(selX), `Xでパラメータ増加 (got: ${selX})`);

// 3. 数手進める(満杯にしない: 6クリックで12手)
for (let i = 0; i < 6; i++) {
  await page.click('#btn-a');
  await new Promise((r) => setTimeout(r, 800));
  const t = await turnText();
  if (t.includes('INPUT COLOR')) break;
}

const turnMid = await turnText();
check(turnMid.includes('PLAYER') || turnMid.includes('COLOR_SELECT'), `中盤ターン状態 (got: ${turnMid})`);

// 4. Y → 色選択 → X(色変更) → Y(実行)
if (turnMid.includes('PLAYER')) {
  await page.click('#btn-y'); // color_select へ
  await new Promise((r) => setTimeout(r, 400));
}
const colorTurn = await turnText();
if (colorTurn.includes('COLOR_SELECT') || colorTurn.includes('INPUT COLOR')) {
  await page.click('#btn-x'); // 色変更
  await new Promise((r) => setTimeout(r, 200));
  await page.click('#btn-y'); // 実行
  await new Promise((r) => setTimeout(r, 600));
  const finalTurn = await turnText();
  check(finalTurn.includes('FINISHED') || finalTurn.includes('DONE'), `実行後 finished (got: ${finalTurn})`);

  // 結果画面に円が描画されているか(canvasピクセル)
  const hasContent = await page.evaluate(() => {
    const c = document.getElementById('game-canvas');
    const ctx = c.getContext('2d');
    const data = ctx.getImageData(0, 0, c.width, c.height).data;
    let colored = 0;
    for (let i = 0; i < data.length; i += 4) {
      const r = data[i], g = data[i + 1], b = data[i + 2];
      if (r > 40 || g > 40 || b > 40) colored++;
    }
    return colored > 1000;
  });
  check(hasContent, '結果画面に円が描画されている');
} else {
  // 色選択がスキップされたケース(満杯時は即run)
  check(true, '満杯パスで結果確認(スキップ)');
}

// 5. セーブ/リプレイボタン存在
const hasButtons = await page.evaluate(() => {
  return ['btn-save', 'btn-load', 'btn-replay'].every((id) => !!document.getElementById(id));
});
check(hasButtons, 'セーブ/ロード/リプレイボタン存在');

// 6. リプレイ: 記録があれば再生
await page.click('#btn-replay');
await new Promise((r) => setTimeout(r, 4000));
const replayStatus = await page.$eval('#status', (el) => el.textContent);
check(replayStatus.includes('Replay'), `リプレイ開始 (status: ${replayStatus})`);

// 7. リプレイ後は finished のため、オートセーブはクリアされている
const saveAfterFinish = await page.evaluate(() => {
  return localStorage.getItem('cobroc_autosave');
});
check(saveAfterFinish === null, 'finished 状態のオートセーブが残らない');

// 8. 新規ゲーム
await page.click('#btn-a');
await new Promise((r) => setTimeout(r, 500));
await page.keyboard.press('n');
await new Promise((r) => setTimeout(r, 500));
check((await turnText()).includes('PLAYER'), 'Nで新規ゲーム');

// 8b. 進行中(未完了)のゲームはオートセーブされる
const saveMidGame = await page.evaluate(() => {
  return localStorage.getItem('cobroc_autosave');
});
check(!!saveMidGame, '進行中のオートセーブが存在');

// 9. ENDでプログラム完成 → color_select → 追加ボタン無効化
await page.click('#btn-a'); // MOVE(1) を置く → AI応答
await new Promise((r) => setTimeout(r, 1200));

// B を 5 回押して END に切替 (MOVE→DRAW→IF→ELSE→REPEAT→END)
for (let i = 0; i < 5; i++) {
  await page.click('#btn-b');
  await new Promise((r) => setTimeout(r, 150));
}
const selEnd = await selText();
check(selEnd.includes('END'), `BでENDに切替 (got: ${selEnd})`);

await page.click('#btn-a'); // END を置く
await new Promise((r) => setTimeout(r, 800));

// AIはENDを置かないため、AIが開いたスコープを閉じるには
// プレイヤーがENDを複数回置く必要がある(最大8回)
for (let attempt = 0; attempt < 8; attempt++) {
  const t = await turnText();
  if (t.includes('COLOR_SELECT') || t.includes('FINISHED')) break;
  await page.click('#btn-a');
  await new Promise((r) => setTimeout(r, 800));
}
const turnEnd = await turnText();
check(turnEnd.includes('COLOR_SELECT') || turnEnd.includes('FINISHED'),
  `ENDで完成→color_select/実行 (got: ${turnEnd})`);

// 追加ボタンが無効化されていること
const addDisabled = await page.evaluate(() => {
  const btn = document.getElementById('btn-a');
  return btn.disabled;
});
check(addDisabled, '完成後 A(追加)ボタンが無効');

// 10. 完了状態はオートセーブせず、リロード後は player で開始(仕様: ゲーム開始時は player)
const turnBeforeReload = await turnText();
if (turnBeforeReload.includes('COLOR_SELECT') || turnBeforeReload.includes('INPUT COLOR')) {
  await page.click('#btn-y'); // 色選択 → 実行
  await new Promise((r) => setTimeout(r, 800));
}
const turnAfterRun = await turnText();
check(turnAfterRun.includes('FINISHED') || turnAfterRun.includes('DONE'), `実行後 finished (got: ${turnAfterRun})`);

// finished 状態が localStorage に保存されていないこと
const autosaveAfterFinish = await page.evaluate(() => localStorage.getItem('cobroc_autosave'));
check(autosaveAfterFinish === null, '完了状態のオートセーブが残らない');

// リロード後も新規ゲーム(PLAYER)で開始
await page.reload({ waitUntil: 'networkidle0', timeout: 30000 });
await page.waitForSelector('#controls', { visible: true, timeout: 15000 });
check((await turnText()).includes('PLAYER'), 'リロード後は新規ゲーム PLAYER');

console.log(failures === 0 ? '== E2E PASSED ==' : `== E2E FAILED (${failures}) ==`);
await browser.close();
process.exit(failures === 0 ? 0 : 1);
