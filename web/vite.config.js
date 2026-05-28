import { defineConfig } from 'vite';
import { resolve } from 'path';

export default defineConfig({
  base: '/',
  build: {
    outDir: 'dist',
    emptyOutDir: true,
  },
  publicDir: resolve(__dirname, '../firmware/data'),
  root: resolve(__dirname, '../firmware/data'),
});
