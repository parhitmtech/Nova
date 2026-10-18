import { afterEach, describe, expect, it, vi } from 'vitest'
import { runCodeStream } from './compiler'

// A fetch Response whose body arrives in the given chunks (as the network might split it).
function streamResponse(chunks: string[], status = 200) {
  const encoder = new TextEncoder()
  const body = new ReadableStream<Uint8Array>({
    start(controller) {
      for (const chunk of chunks) controller.enqueue(encoder.encode(chunk))
      controller.close()
    },
  })
  return new Response(body, { status, headers: { 'Content-Type': 'text/event-stream' } })
}

const event = (type: string, text: string) => `data: ${JSON.stringify({ type, text })}\n\n`

afterEach(() => {
  vi.unstubAllGlobals()
})

describe('runCodeStream', () => {
  it('delivers events in order even when chunks split them anywhere', async () => {
    const wire = event('stdout', '\r[█░] 50%') + event('stdout', '\r[██] 100%\n') + event('done', '12')
    // 3-byte pieces cut through "data: ", the JSON and the multi-byte block characters
    const bytes = new TextEncoder().encode(wire)
    const fetchMock = vi.fn(async () => {
      const raw = new ReadableStream<Uint8Array>({
        start(controller) {
          for (let i = 0; i < bytes.length; i += 3) controller.enqueue(bytes.slice(i, i + 3))
          controller.close()
        },
      })
      return new Response(raw)
    })
    vi.stubGlobal('fetch', fetchMock)

    const seen: [string, string][] = []
    await runCodeStream('ignite {}', '', (type, text) => seen.push([type, text]), undefined, 'tok')

    expect(seen).toEqual([
      ['stdout', '\r[█░] 50%'],
      ['stdout', '\r[██] 100%\n'],
      ['done', '12'],
    ])
    expect(fetchMock).toHaveBeenCalledWith(
      expect.stringMatching(/\/run-stream$/), // base URL comes from VITE_API_URL
      expect.objectContaining({
        method: 'POST',
        headers: expect.objectContaining({ Authorization: 'Bearer tok' }),
      }),
    )
  })

  it('skips malformed and non-data lines', async () => {
    vi.stubGlobal(
      'fetch',
      vi.fn(async () =>
        streamResponse([
          ': keep-alive\n\n',
          'data: {broken\n\n',
          event('stdout', 'ok\n'),
          event('done', '1'),
        ]),
      ),
    )
    const seen: string[] = []
    await runCodeStream('x', '', (type, text) => seen.push(`${type}:${text}`), undefined, 'tok')
    expect(seen).toEqual(['stdout:ok\n', 'done:1'])
  })

  it('rejects with the server error for a failed request', async () => {
    vi.stubGlobal(
      'fetch',
      vi.fn(async () => new Response(JSON.stringify({ error: 'Server busy.' }), { status: 503 })),
    )
    await expect(runCodeStream('x', '', () => {}, undefined, 'tok')).rejects.toThrow('Server busy.')
  })

  it('signals an expired session on 401', async () => {
    const target = new EventTarget()
    const expired = vi.fn()
    target.addEventListener('novacomp:session-expired', expired)
    vi.stubGlobal('window', target)
    vi.stubGlobal(
      'fetch',
      vi.fn(async () => new Response('{}', { status: 401 })),
    )
    await expect(runCodeStream('x', '', () => {}, undefined, 'tok')).rejects.toThrow('HTTP 401')
    expect(expired).toHaveBeenCalledOnce()
  })
})
