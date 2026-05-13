import axios from 'axios'

const API = import.meta.env.VITE_API_URL

export const runCode = (source: string, inputs: string = '') =>
  axios.post(`${API}/run`, { source, inputs })

export const runCodeStream = (
  source: string,
  inputs: string = '',
  onChunk: (type: string, text: string) => void,
  signal?: AbortSignal,
  token?: string
): Promise<void> =>
  fetch(`${API}/run-stream`, {
    method: 'POST',
    headers: {
      'Content-Type': 'application/json',
      ...(token ? { Authorization: `Bearer ${token}` } : {}),
    },
    body: JSON.stringify({ source, inputs }),
    signal,
  }).then(async res => {
    if (!res.ok) {
      const data = await res.json().catch(() => ({}))
      throw new Error((data as { error?: string }).error ?? `HTTP ${res.status}`)
    }
    const reader = res.body!.getReader()
    const decoder = new TextDecoder()
    let buf = ''
    for (;;) {
      const { done, value } = await reader.read()
      if (done) break
      buf += decoder.decode(value, { stream: true })
      const parts = buf.split('\n')
      buf = parts.pop()!
      for (const line of parts) {
        if (line.startsWith('data: ')) {
          try {
            const parsed = JSON.parse(line.slice(6)) as { type: string; text: string }
            onChunk(parsed.type, parsed.text)
          } catch { /* skip malformed SSE line */ }
        }
      }
    }
  })

export const compareCode = (source: string, inputs: string = '') =>
  axios.post(`${API}/compare`, { source, inputs })

export const submitGpuJob = (token: string, source: string) =>
  axios.post(`${API}/gpu/submit`, { source }, {
    headers: { Authorization: `Bearer ${token}` },
  })

export const getGpuStatus = (token: string, jobId: string) =>
  axios.get(`${API}/gpu/status/${jobId}`, {
    headers: { Authorization: `Bearer ${token}` },
  })

export const getGpuResults = (token: string, jobId: string) =>
  axios.get(`${API}/gpu/results/${jobId}`, {
    headers: { Authorization: `Bearer ${token}` },
  })

export const compileBytecode = (source: string) =>
  axios.post(`${API}/compile-bytecode`, { source }, { responseType: 'blob' })
