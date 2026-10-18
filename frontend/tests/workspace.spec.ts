import { test, expect, type Page } from '@playwright/test'
import { Buffer } from 'node:buffer'

const modifier = process.platform === 'darwin' ? 'Meta' : 'Control'
const first = 'main() {\n    print(1) ;\n}\n'
const second = 'main() {\n    print(2) ;\n}\n'

async function workspace(page: Page) {
  const files: Record<string, string> = { 'first.nova': first, 'second.nova': second, 'model.nbc': 'binary' }
  const saves: { fileName: string; content: string }[] = []
  let failSave = false
  let runInputs = ''
  const payload = Buffer.from(
    JSON.stringify({
      userId: '12345678-1234-4234-9234-123456789abc',
      email: 'test@example.com',
      exp: Math.floor(Date.now() / 1000) + 3600,
    }),
  ).toString('base64url')
  const token = `eyJhbGciOiJIUzI1NiJ9.${payload}.test-fixture`
  await page.route(
    (url) => /^\/(auth|files|agent|gpu|run-stream)(\/|$)/.test(url.pathname),
    async (route) => {
      const url = new URL(route.request().url())
      const body = route.request().postDataJSON()
      const json = (data: unknown, status = 200) => route.fulfill({ status, json: data })
      if (url.pathname === '/auth/login' || url.pathname === '/auth/signup') return json({ token })
      if (url.pathname === '/files')
        return json({
          files: Object.keys(files).map((fileName) => ({ fileName, updatedAt: '2026-10-03T00:00:00Z' })),
        })
      if (url.pathname === '/files/load') return json({ content: files[url.searchParams.get('fileName')!] })
      if (url.pathname === '/files/save') {
        if (failSave) return json({ error: 'Temporary storage outage' }, 503)
        await new Promise((resolve) => setTimeout(resolve, 150))
        saves.push(body)
        files[body.fileName] = body.content
        return json({ ok: true })
      }
      if (url.pathname === '/files/delete') {
        delete files[url.searchParams.get('fileName')!]
        return json({ ok: true })
      }
      if (url.pathname === '/agent/history') return json({ history: [] })
      if (url.pathname === '/agent/memory') return json({ memory: {} })
      if (url.pathname === '/gpu/submit')
        return json({ error: 'GPU execution is disabled on this deployment' }, 503)
      if (url.pathname === '/run-stream') {
        expect(route.request().headers().authorization).toBe(`Bearer ${token}`)
        runInputs = body.inputs
        return route.fulfill({
          contentType: 'text/event-stream',
          body: 'data: {"type":"stdout","text":"12\\n"}\n\ndata: {"type":"done","text":"15"}\n\n',
        })
      }
      return json({})
    },
  )
  await page.goto('/login')
  await page.getByLabel('Email address').fill('test@example.com')
  await page.getByLabel('Password', { exact: true }).fill('test-password')
  await page.getByRole('button', { name: 'Open my workspace' }).click()
  await expect(page.getByRole('region', { name: 'Code editor' })).toBeVisible()
  await expect(page.locator('.monaco-editor .view-lines')).toBeVisible()
  return {
    files,
    saves,
    fail: (value: boolean) => {
      failSave = value
    },
    inputs: () => runInputs,
  }
}

async function edit(page: Page, code: string) {
  await page.getByRole('textbox', { name: 'Editor content' }).focus()
  await page.keyboard.press(`${modifier}+a`)
  await page.context().grantPermissions(['clipboard-read', 'clipboard-write'])
  await page.evaluate((text) => navigator.clipboard.writeText(text), code)
  await page.keyboard.press(`${modifier}+v`)
}

test('command palette keyboard navigation opens persistent editor settings and restores focus', async ({
  page,
}) => {
  await workspace(page)
  await page.keyboard.press(`${modifier}+k`)
  const search = page.getByRole('combobox', { name: 'Search commands and files' })
  await expect(search).toBeFocused()
  await search.fill('Editor settings')
  await search.press('Enter')
  await expect(page.getByRole('dialog', { name: 'Make yourself at home' })).toBeVisible()
  await page.getByLabel(/Font size/).selectOption('18')
  await page.getByLabel(/Minimap/).check()
  await page.getByRole('dialog').press('Escape')
  await page.reload()
  await page.getByRole('button', { name: 'Editor settings', exact: true }).click()
  await expect(page.getByLabel(/Font size/)).toHaveValue('18')
  await expect(page.getByLabel(/Minimap/)).toBeChecked()
  await page.getByRole('dialog').press('Escape')
  await expect(page.getByRole('button', { name: 'Editor settings', exact: true })).toBeFocused()
})

test('file switching saves the edited source before loading another file', async ({ page }) => {
  const state = await workspace(page)
  await page.getByRole('button', { name: 'first.nova', exact: true }).click()
  await expect(page.locator('.editor-tab')).toContainText('first.nova')
  const changed = 'main() {\n    print(42) ;\n}'
  await edit(page, changed)
  await page.getByRole('button', { name: 'second.nova', exact: true }).click()
  await expect(page.locator('.editor-tab')).toContainText('second.nova')
  expect(state.files['first.nova']).toBe(changed)
  expect(state.files['second.nova']).toBe(second)
  expect(state.saves.every((s) => s.fileName !== 'model.nbc')).toBe(true)
})

test('a failed save preserves the draft and blocks navigation until retry succeeds', async ({ page }) => {
  const state = await workspace(page)
  await page.getByRole('button', { name: 'first.nova', exact: true }).click()
  await expect(page.locator('.editor-tab')).toContainText('first.nova')
  const changed = 'main() { print(99) ; }'
  await edit(page, changed)
  state.fail(true)
  await page.getByRole('button', { name: 'second.nova', exact: true }).click()
  await expect(page.getByRole('alert').filter({ hasText: 'Temporary storage outage' })).toBeVisible()
  await expect(page.locator('.editor-tab')).toContainText('first.nova')
  await expect(page.locator('.view-lines')).toContainText('99')
  state.fail(false)
  await page.getByRole('button', { name: 'Save', exact: true }).click()
  await expect(page.locator('.save-indicator')).toContainText('All changes saved')
  expect(state.files['first.nova']).toBe(changed)
})

test('creating a file cannot overwrite the currently open source', async ({ page }) => {
  const state = await workspace(page)
  await page.getByRole('button', { name: 'first.nova', exact: true }).click()
  await expect(page.locator('.editor-tab')).toContainText('first.nova')
  await page.getByRole('button', { name: 'Create new file', exact: true }).click()
  await page.getByRole('dialog').getByRole('textbox').fill('first.nova')
  await page.getByRole('dialog').getByRole('button', { name: 'Create file' }).click()
  await expect(page.getByText('A file with this name already exists. Choose another name.')).toBeVisible()
  expect(state.files['first.nova']).toBe(first)
  expect(state.saves).toHaveLength(0)
})

test('runs with standard input and displays unavailable GPU capability explicitly', async ({ page }) => {
  const state = await workspace(page)
  await page.getByRole('button', { name: 'Standard input', exact: true }).click()
  await page.getByLabel('Input values, one per line').fill('3\n4')
  await page.getByRole('button', { name: /^Run/ }).click()
  await expect(page.locator('pre').filter({ hasText: '12' }).first()).toBeVisible()
  expect(state.inputs()).toBe('3\n4')
  await page.getByRole('button', { name: 'GPU', exact: true }).click()
  await expect(page.getByText('GPU execution is disabled on this deployment')).toBeVisible()
})

test('binary artifacts cannot be overwritten by autosave and delete confirmation is keyboard accessible', async ({
  page,
}) => {
  const state = await workspace(page)
  await page.getByRole('button', { name: 'model.nbc', exact: true }).click()
  await expect(page.getByRole('heading', { name: 'Model artifact' })).toBeVisible()
  await expect(page.getByRole('button', { name: 'Save', exact: true })).toBeDisabled()
  await page.getByRole('button', { name: 'Delete model.nbc' }).click()
  await expect(page.getByRole('dialog', { name: 'Delete file?' })).toBeVisible()
  await page.getByRole('button', { name: 'Keep file' }).click()
  expect(state.files['model.nbc']).toBe('binary')
  expect(state.saves.some((s) => s.fileName === 'model.nbc')).toBe(false)
})

test('mobile toolbar remains usable and explorer can be opened without horizontal overflow', async ({
  page,
}) => {
  await page.setViewportSize({ width: 390, height: 844 })
  await workspace(page)
  await expect(page.getByRole('button', { name: 'Compare optimization', exact: true })).toBeVisible()
  await expect(page.getByRole('button', { name: 'Share source code', exact: true })).toBeVisible()
  await expect(page.getByRole('complementary', { name: 'File explorer' })).toHaveCount(0)
  await page.getByRole('button', { name: 'Toggle file explorer' }).click()
  await expect(page.getByRole('complementary', { name: 'File explorer' })).toBeVisible()
  await page.getByLabel('Search files').fill('second')
  await expect(page.getByRole('button', { name: 'second.nova', exact: true })).toBeVisible()
  await expect(page.getByRole('button', { name: 'first.nova', exact: true })).toHaveCount(0)
  const width = await page.evaluate(() => document.documentElement.scrollWidth)
  expect(width).toBeLessThanOrEqual(390)
  await page.setViewportSize({ width: 320, height: 740 })
  await expect(page.getByRole('button', { name: 'Run', exact: true })).toBeInViewport()
  expect(await page.evaluate(() => document.documentElement.scrollWidth)).toBeLessThanOrEqual(320)
})

test('a failed editor chunk load shows the recovery screen instead of a blank page', async ({ page }) => {
  const payload = Buffer.from(
    JSON.stringify({
      userId: '12345678-1234-4234-9234-123456789abc',
      email: 'test@example.com',
      exp: 9999999999,
    }),
  ).toString('base64url')
  await page.route(
    (url) => /^\/(auth|files|agent)(\/|$)/.test(url.pathname),
    (route) => {
      const path = new URL(route.request().url()).pathname
      if (path.startsWith('/auth/'))
        return route.fulfill({ json: { token: `eyJhbGciOiJIUzI1NiJ9.${payload}.x` } })
      if (path === '/files') return route.fulfill({ json: { files: [] } })
      if (path === '/agent/history') return route.fulfill({ json: { history: [] } })
      return route.fulfill({ json: {} })
    },
  )
  // Simulate a network failure for the lazily loaded editor chunk.
  await page.route(/NovaEditor/, (route) => route.abort())
  await page.goto('/login')
  await page.getByLabel('Email address').fill('test@example.com')
  await page.getByLabel('Password', { exact: true }).fill('test-password')
  await page.getByRole('button', { name: 'Open my workspace' }).click()
  await expect(page.getByRole('alert').getByRole('heading', { name: 'Something went wrong' })).toBeVisible()
  await expect(page.getByRole('button', { name: 'Reload' })).toBeVisible()
})
