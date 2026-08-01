import { defineConfig } from 'vite';

export default defineConfig({
  root: '.',
  // GitHub Pages はサブパス(例: /coBroc/)で配信されるため、相対パス指定。
  // これによりビルド後のアセット参照(assets/*, coBrocWeb.js, coBrocWeb.wasm)が
  // ルート起点ではなくページ相対になり、どのサブパスでも動作する。
  base: './',
  build: {
    outDir: 'dist',
    target: 'es2022',
  },
  server: {
    port: 3000,
  },
});
