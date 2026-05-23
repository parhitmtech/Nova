import { useState, useRef, useEffect } from 'react'
import { Send, X, Trash2, Loader2, Check } from 'lucide-react'
import { sendAgentMessage, loadAgentHistory, clearAgentHistory, loadAgentMemory } from '../api/agent'
import type { ChatMessage, ToolEvent } from '../api/agent'

interface NovaBotProps {
  token: string
  currentCode: string
  currentFile: string
  recentOutput: string
  onFileWrite: (fileName: string) => void
}

function toolLabel(name: string, status: 'running' | 'done'): string {
  const map: Record<string, [string, string]> = {
    run_nova_code:   ['Running Nova code…',  'Ran Nova code'],
    read_user_file:  ['Reading file…',       'Read file'],
    write_user_file: ['Writing file…',       'Wrote file'],
    list_user_files: ['Listing files…',      'Listed files'],
    explain_error:   ['Analyzing error…',    'Analyzed error'],
  }
  return map[name]?.[status === 'running' ? 0 : 1] ?? name
}

const WELCOME: ChatMessage = {
  role: 'assistant',
  content: "Hi! I'm NovaBot. I can help you write Nova code, debug errors, and explain language features. What would you like to work on?",
  timestamp: Date.now(),
}

export default function NovaBot({ token, currentCode, currentFile, recentOutput, onFileWrite }: NovaBotProps) {
  const [messages,       setMessages]       = useState<ChatMessage[]>([WELCOME])
  const [input,          setInput]          = useState('')
  const [isLoading,      setIsLoading]      = useState(false)
  const [historyLoading, setHistoryLoading] = useState(true)
  const [toolEvents,     setToolEvents]     = useState<ToolEvent[]>([])
  const [memory,         setMemory]         = useState<{ preferences?: { experienceLevel?: string } } | null>(null)
  const bottomRef = useRef<HTMLDivElement>(null)
  const abortRef  = useRef<AbortController | null>(null)

  // Load persisted history and memory on mount
  useEffect(() => {
    loadAgentHistory(token).then(history => {
      if (history.length > 0) setMessages(history)
      setHistoryLoading(false)
    }).catch(() => setHistoryLoading(false))

    loadAgentMemory(token).then(m => {
      setMemory(m as { preferences?: { experienceLevel?: string } } | null)
    }).catch(() => {})
  }, [token])

  useEffect(() => {
    bottomRef.current?.scrollIntoView({ behavior: 'smooth' })
  }, [messages])

  const handleSend = () => {
    if (!input.trim() || isLoading) return

    const userMsg: ChatMessage = { role: 'user',      content: input.trim(), timestamp: Date.now() }
    const botMsg:  ChatMessage = { role: 'assistant', content: '',           timestamp: Date.now() }

    setMessages(prev => [...prev, userMsg, botMsg])
    setInput('')
    setIsLoading(true)
    setToolEvents([])

    abortRef.current = sendAgentMessage(
      token, userMsg.content, currentCode, currentFile, recentOutput,
      (text) => {
        setMessages(prev => {
          const next = [...prev]
          next[next.length - 1] = {
            ...next[next.length - 1],
            content: next[next.length - 1].content + text,
          }
          return next
        })
      },
      () => { setIsLoading(false); setToolEvents([]); abortRef.current = null },
      (err) => {
        setMessages(prev => {
          const next = [...prev]
          next[next.length - 1] = { ...next[next.length - 1], content: `Error: ${err}` }
          return next
        })
        setIsLoading(false)
        setToolEvents([])
        abortRef.current = null
      },
      (data) => {
        setToolEvents(prev => [...prev, { id: data.id, name: data.name, status: 'running' }])
      },
      (data) => {
        setToolEvents(prev => prev.map(ev =>
          ev.id === data.id ? { ...ev, status: 'done', result: data.result } : ev
        ))
      },
      (fileName) => { onFileWrite(fileName) },
    )
  }

  const handleKeyDown = (e: React.KeyboardEvent) => {
    if (e.key === 'Enter' && !e.shiftKey) { e.preventDefault(); handleSend() }
  }

  const handleStop  = () => { abortRef.current?.abort(); setIsLoading(false); setToolEvents([]) }
  const handleClear = () => {
    clearAgentHistory(token).catch(() => {})
    setMessages([{ ...WELCOME, timestamp: Date.now() }])
    setToolEvents([])
  }

  const renderContent = (content: string, streaming: boolean, isLast: boolean, events: ToolEvent[]) => {
    const parts = content.split(/```/)
    return (
      <>
        {/* Tool activity — shown while streaming the last message */}
        {isLast && (streaming || events.length > 0) && events.length > 0 && (
          <div style={{ marginBottom: 6 }}>
            {events.map(ev => (
              <div key={ev.id} style={{ display: 'flex', alignItems: 'center', gap: 4, marginBottom: 2 }}>
                {ev.status === 'running'
                  ? <Loader2 size={10} style={{ color: '#a855f7', animation: 'spin 1s linear infinite' }} />
                  : <Check size={10} style={{ color: '#39ff8f' }} />
                }
                <span style={{ fontSize: 10, color: '#8b949e' }}>{toolLabel(ev.name, ev.status)}</span>
              </div>
            ))}
          </div>
        )}
        {parts.map((part, idx) => {
          if (idx % 2 === 1) {
            const lines = part.split('\n')
            const code  = lines.length > 1 ? lines.slice(1).join('\n') : part
            return (
              <pre key={idx} style={{
                background: '#0d1117', border: '1px solid #30363d', borderRadius: 4,
                padding: '8px 10px', margin: '6px 0', overflowX: 'auto',
                color: '#39ff8f', fontSize: 11,
                fontFamily: 'ui-monospace, Consolas, monospace',
              }}>
                <code>{code}</code>
              </pre>
            )
          }
          return <span key={idx} style={{ whiteSpace: 'pre-wrap' }}>{part}</span>
        })}
        {streaming && isLast && (
          <span style={{
            display: 'inline-block', width: 6, height: 13, background: '#a855f7',
            marginLeft: 2, verticalAlign: 'middle', opacity: 1,
            animation: 'novaBlink 1s step-end infinite',
          }} />
        )}
      </>
    )
  }

  return (
    <div className="flex flex-col" style={{ height: '100%', background: '#0d1117' }}>

      {/* Header */}
      <div className="flex items-center justify-between px-3 py-2 shrink-0"
        style={{ background: '#161b22', borderBottom: '1px solid #30363d' }}>
        <div className="flex items-center gap-2">
          <div style={{
            width: 20, height: 20, background: '#a855f7', borderRadius: 4,
            display: 'flex', alignItems: 'center', justifyContent: 'center',
            fontSize: 11, fontWeight: 900, color: '#fff', fontFamily: 'monospace',
          }}>N</div>
          <span className="text-xs font-semibold" style={{ color: '#c9d1d9' }}>NovaBot</span>
          {memory?.preferences?.experienceLevel && (
            <span style={{ fontSize: 11, opacity: 0.7 }} title={`Adapted for ${memory.preferences.experienceLevel} level`}>
              {memory.preferences.experienceLevel === 'beginner' ? '📚' : '⚡'}
            </span>
          )}
          {(isLoading || historyLoading) && (
            <div style={{
              width: 6, height: 6, background: '#a855f7', borderRadius: '50%',
              animation: 'pulse 1s ease-in-out infinite',
            }} />
          )}
        </div>
        <button onClick={handleClear} title="Clear chat" className="p-0.5 rounded"
          style={{ color: '#8b949e', background: 'none', border: 'none', cursor: 'pointer' }}>
          <Trash2 size={12} />
        </button>
      </div>

      {/* Messages */}
      <div className="flex-1 overflow-y-auto p-3" style={{ display: 'flex', flexDirection: 'column', gap: 12 }}>
        {historyLoading && (
          <div className="flex items-center justify-center gap-2 py-4">
            <Loader2 size={12} style={{ color: '#a855f7', animation: 'spin 1s linear infinite' }} />
            <span style={{ fontSize: 11, color: '#8b949e' }}>Loading history…</span>
          </div>
        )}
        {messages.map((msg, i) => (
          <div key={i} style={{
            display: 'flex', gap: 8,
            justifyContent: msg.role === 'user' ? 'flex-end' : 'flex-start',
          }}>
            {msg.role === 'assistant' && (
              <div style={{
                width: 22, height: 22, background: '#a855f7', borderRadius: 4,
                flexShrink: 0, display: 'flex', alignItems: 'center',
                justifyContent: 'center', fontSize: 10, fontWeight: 900,
                color: '#fff', marginTop: 2,
              }}>N</div>
            )}
            <div style={{
              maxWidth: '82%', borderRadius: 8, padding: '8px 12px',
              fontSize: 12, lineHeight: 1.6,
              background: msg.role === 'user' ? '#1f6feb' : '#161b22',
              border: `1px solid ${msg.role === 'user' ? '#1f6feb' : '#30363d'}`,
              color: '#c9d1d9',
            }}>
              {renderContent(msg.content, isLoading, i === messages.length - 1, i === messages.length - 1 ? toolEvents : [])}
            </div>
            {msg.role === 'user' && (
              <div style={{
                width: 22, height: 22, background: '#1f6feb', borderRadius: '50%',
                flexShrink: 0, display: 'flex', alignItems: 'center',
                justifyContent: 'center', fontSize: 10, color: '#fff',
                fontWeight: 700, marginTop: 2,
              }}>U</div>
            )}
          </div>
        ))}
        <div ref={bottomRef} />
      </div>

      {/* Input */}
      <div className="shrink-0 p-2" style={{ borderTop: '1px solid #30363d', display: 'flex', gap: 8, alignItems: 'flex-end' }}>
        <textarea
          value={input}
          onChange={e => setInput(e.target.value)}
          onKeyDown={handleKeyDown}
          placeholder="Ask NovaBot… (Enter to send, Shift+Enter for newline)"
          rows={2}
          style={{
            flex: 1, background: '#161b22', color: '#c9d1d9', fontSize: 12,
            border: '1px solid #30363d', borderRadius: 6, padding: '8px 10px',
            resize: 'none', outline: 'none', fontFamily: 'inherit', maxHeight: 80,
          }}
          onFocus={e => { e.currentTarget.style.borderColor = '#a855f7' }}
          onBlur={e  => { e.currentTarget.style.borderColor = '#30363d' }}
        />
        {isLoading ? (
          <button onClick={handleStop} title="Stop"
            style={{ padding: 8, background: '#f85149', border: 'none', borderRadius: 6, cursor: 'pointer', color: '#fff', flexShrink: 0 }}>
            <X size={14} />
          </button>
        ) : (
          <button onClick={handleSend} disabled={!input.trim()} title="Send"
            style={{
              padding: 8, border: 'none', borderRadius: 6, flexShrink: 0,
              cursor: input.trim() ? 'pointer' : 'not-allowed',
              background: input.trim() ? '#a855f7' : '#21262d',
              color: '#fff',
            }}>
            <Send size={14} />
          </button>
        )}
      </div>
    </div>
  )
}
