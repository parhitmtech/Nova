# NovaComp frontend

The browser IDE for Nova: Monaco editor, file explorer, run/stream output, terminal, sharing and the NovaBot assistant. React 19 + TypeScript + Vite + Tailwind.

## Develop

```sh
npm ci
npm run dev        # http://localhost:5173
```

The dev server proxies API and WebSocket routes to the backend on `localhost:3001` (see `vite.config.ts`), so start `server/` first (`npm start` there). Leave `VITE_API_URL` empty to use the same origin; see `.env.example`.

## Scripts

| Command | What it does |
|---|---|
| `npm run dev` | Vite dev server with hot reload |
| `npm run build` | type-check (`tsc -b`) and build to `dist/`, which the backend serves |
| `npm run lint` | ESLint |
| `npm run test:e2e` | Playwright browser tests in `tests/` (API responses are mocked; run `npx playwright install chromium` once) |

## Layout

```
src/
  pages/        IDEPage (main IDE), LoginPage, SharePage
  components/   editor, bottom panel, terminal, NovaBot, command palette, modal
  api/          typed clients for the backend (auth, files, compiler, share, agent)
  contexts/     AuthContext (session + token)
  lib/          Nova syntax highlighting for Monaco, workspace helpers
tests/          Playwright specs
```
