import axios from 'axios'

const API = import.meta.env.VITE_API_URL

// Buffered run — waits for the entire execution to finish before returning.
// Prefer runCodeStream for any long-running code (ML training, SOL jobs).
export const runCode = (source: string, inputs: string = '') =>
  axios.post(`${API}/run`, { source, inputs })

// Streaming run over Server-Sent Events (SSE).
// We use fetch instead of EventSource because EventSource only supports GET,
// but we need to POST the source code in the request body.
// Each SSE line is: data: {"type":"stdout"|"stderr"|"error"|"done","text":"..."}
// The "done" event text contains elapsed milliseconds as a string.
// Pass an AbortSignal to cancel mid-stream (e.g. user navigates away or re-runs).
// Pass the JWT token so the server can associate NOVA_SAVE_FILE/NOVA_S3_FILE
// uploads with the correct user's S3 bucket.
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
      // Only attach Authorization when a token is present —
      // anonymous runs are still allowed but won't trigger file uploads.
      ...(token ? { Authorization: `Bearer ${token}` } : {}),
    },
    body: JSON.stringify({ source, inputs }),
    signal,
  }).then(async res => {
    if (!res.ok) {
      const data = await res.json().catch(() => ({}))
      throw new Error((data as { error?: string }).error ?? `HTTP ${res.status}`)
    }

    // Read the response body as a raw byte stream and decode line by line.
    // A partial line at the end of a chunk is buffered until the next chunk arrives.
    const reader = res.body!.getReader()
    const decoder = new TextDecoder()
    let buf = ''

    for (;;) {
      const { done, value } = await reader.read()
      if (done) break
      buf += decoder.decode(value, { stream: true })
      const parts = buf.split('\n')
      buf = parts.pop()! // keep the incomplete trailing line in the buffer
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

// Run the same code twice (normal AST vs constant-folded/optimised AST) and
// return timing and node-count comparisons. Used by the Compare button.
export const compareCode = (source: string, inputs: string = '') =>
  axios.post(`${API}/compare`, { source, inputs })

// Submit a GPU job to the SOL HPC cluster via the server's SSH tunnel.
// The server detects the job type (CUDA / HuggingFace / SOL finetune) from the source.
export const submitGpuJob = (token: string, source: string) =>
  axios.post(`${API}/gpu/submit`, { source }, {
    headers: { Authorization: `Bearer ${token}` },
  })

// Poll the current state of a submitted GPU job (pending / running / completed / failed).
export const getGpuStatus = (token: string, jobId: string) =>
  axios.get(`${API}/gpu/status/${jobId}`, {
    headers: { Authorization: `Bearer ${token}` },
  })

// Fetch stdout output of a completed GPU job.
export const getGpuResults = (token: string, jobId: string) =>
  axios.get(`${API}/gpu/results/${jobId}`, {
    headers: { Authorization: `Bearer ${token}` },
  })

// Compile Nova source to bytecode and return the raw binary as a Blob
// so the browser can trigger a .nbc file download directly.
export const compileBytecode = (source: string) =>
  axios.post(`${API}/compile-bytecode`, { source }, { responseType: 'blob' })
