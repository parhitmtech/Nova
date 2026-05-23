export interface ToolEvent {
    id: string
    name: string
    status: 'running' | 'done'
    result?: unknown
}

export interface ChatMessage {
    role: 'user' | 'assistant'
    content: string
    timestamp: number
    toolEvents?: ToolEvent[]
}

const API = import.meta.env.VITE_API_URL

export const loadAgentHistory = async (token: string): Promise<ChatMessage[]> => {
    const res = await fetch(`${API}/agent/history`, {
        headers: { Authorization: `Bearer ${token}` },
    })
    if (!res.ok) return []
    const { history } = await res.json()
    return (history || []).map((m: { role: 'user' | 'assistant'; content: string; timestamp: number }) => ({
        role: m.role, content: m.content, timestamp: m.timestamp,
    }))
}

export const clearAgentHistory = async (token: string): Promise<void> => {
    await fetch(`${API}/agent/history`, {
        method: 'DELETE',
        headers: { Authorization: `Bearer ${token}` },
    })
}

export const loadAgentMemory = async (token: string): Promise<unknown> => {
    const res = await fetch(`${API}/agent/memory`, {
        headers: { Authorization: `Bearer ${token}` },
    })
    if (!res.ok) return null
    const { memory } = await res.json()
    return memory
}

export const sendAgentMessage = (
    token: string,
    message: string,
    currentCode: string,
    currentFile: string,
    recentOutput: string,
    onText: (text: string) => void,
    onDone: () => void,
    onError: (err: string) => void,
    onToolStart?: (data: { name: string; id: string }) => void,
    onToolResult?: (data: { id: string; result: unknown }) => void,
    onFileWrite?: (fileName: string) => void,
): AbortController => {
    const controller = new AbortController()

    fetch(`${API}/agent/chat`, {
        method: 'POST',
        headers: { 'Content-Type': 'application/json', Authorization: `Bearer ${token}` },
        body: JSON.stringify({
            message,
            currentCode: currentCode.slice(0, 2000),
            currentFile,
            recentOutput: recentOutput.slice(0, 500),
        }),
        signal: controller.signal,
    }).then(async res => {
        if (!res.ok) {
            const err = await res.json().catch(() => ({ error: 'Request failed' }))
            onError(err.error || 'Request failed')
            return
        }

        const reader = res.body!.getReader()
        const decoder = new TextDecoder()
        let buffer = ''

        while (true) {
            const { done, value } = await reader.read()
            if (done) break

            buffer += decoder.decode(value, { stream: true })
            const lines = buffer.split('\n')
            buffer = lines.pop() ?? ''

            for (const line of lines) {
                if (!line.startsWith('data: ')) continue
                try {
                    const { type, data } = JSON.parse(line.slice(6))
                    if (type === 'text') onText(data)
                    else if (type === 'done') onDone()
                    else if (type === 'error') onError(data)
                    else if (type === 'tool_start') onToolStart?.(data)
                    else if (type === 'tool_result') onToolResult?.(data)
                    else if (type === 'file_written') onFileWrite?.(data.fileName)
                } catch { /* malformed SSE chunk — skip */ }
            }
        }
    }).catch(err => {
        if (err.name !== 'AbortError') onError('Connection failed')
    })

    return controller
}