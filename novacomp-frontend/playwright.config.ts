import { defineConfig } from '@playwright/test'
import { existsSync } from 'node:fs'

const chrome = '/Applications/Google Chrome.app/Contents/MacOS/Google Chrome'
export default defineConfig({
  testDir: './tests',
  fullyParallel: true,
  use: {
    baseURL: 'http://127.0.0.1:5173',
    viewport: { width: 1440, height: 900 },
    launchOptions: existsSync(chrome) ? { executablePath: chrome } : {},
    screenshot: 'only-on-failure',
    trace: 'retain-on-failure',
  },
  webServer: {
    command: 'npm run dev -- --host 127.0.0.1 --port 5173 --strictPort',
    url: 'http://127.0.0.1:5173',
    env: { VITE_API_URL: '' },
    reuseExistingServer: false,
  },
})
