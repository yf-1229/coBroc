/**
 * coBroc Web — E2E テストランナー
 *
 * dist/ を静的サーバーで配信し、ヘッドレス Chrome(puppeteer-core)で
 * e2e_test.mjs を実行する。
 *
 * 実行方法(web/ ディレクトリで):
 *   npm run e2e          # dist をビルドしてから実行
 *   npm run e2e:serve    # ビルドせずに実行(dist が既にある場合)
 *
 * 事前準備: npm i -D puppeteer-core(システムの Chrome を使用)
 */

import { createServer } from 'http';
import { readFileSync, existsSync } from 'fs';
import { join, extname, dirname } from 'path';
import { spawn } from 'child_process';
import { fileURLToPath } from 'url';

const __dirname = dirname(fileURLToPath(import.meta.url));
const PORT = 17890;
const DIST = join(__dirname, '..', 'dist');

const mime = { '.html':'text/html','.js':'text/javascript','.wasm':'application/wasm','.json':'application/json','.svg':'image/svg+xml' };

const server = createServer((req, res) => {
  let p = join(DIST, req.url === '/' ? '/index.html' : req.url.split('?')[0]);
  if (!existsSync(p)) { res.writeHead(404); res.end(); return; }
  const ext = extname(p);
  res.writeHead(200, { 'Content-Type': mime[ext] || 'application/octet-stream' });
  res.end(readFileSync(p));
});

server.listen(PORT, async () => {
  console.log(`static server on :${PORT}`);
  const proc = spawn('node', ['e2e_test.mjs'], {
    cwd: __dirname,
    stdio: 'inherit',
    env: { ...process.env, E2E_URL: `http://localhost:${PORT}/` },
  });
  proc.on('close', (code) => {
    server.close();
    process.exit(code ?? 1);
  });
});
