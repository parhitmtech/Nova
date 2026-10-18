import { defineConfig } from 'vite'
import react from '@vitejs/plugin-react'
import tailwindcss from '@tailwindcss/vite'

export default defineConfig({
  plugins: [react(), tailwindcss()],
  server: {
    proxy: Object.fromEntries(
      [
        '/auth',
        '/files',
        '/run',
        '/run-stream',
        '/compare',
        '/compile-bytecode',
        '/gpu',
        '/agent',
        '/api',
        '/terminal',
        '/repl',
      ].map((route) => [route, { target: 'http://localhost:3001', changeOrigin: true, ws: true }]),
    ),
  },
})
