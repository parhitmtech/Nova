import { useEffect, useRef } from 'react'
import { Terminal } from '@xterm/xterm'
import { FitAddon } from '@xterm/addon-fit'
import { X } from 'lucide-react'
import '@xterm/xterm/css/xterm.css'

const WS_URL = import.meta.env.VITE_API_URL.replace(/^http/, 'ws')

interface Props {
  onClose: () => void
  onWsReady?: (ws: WebSocket) => void
}

export default function TerminalPanel({ onClose, onWsReady }: Props) {
  const containerRef = useRef<HTMLDivElement>(null)

  useEffect(() => {
    if (!containerRef.current) return

    const term = new Terminal({
      theme: {
        background:  '#0d1117',
        foreground:  '#c9d1d9',
        cursor:      '#00d4ff',
        black:       '#0d1117',
        brightBlack: '#3d4259',
        white:       '#c9d1d9',
        brightWhite: '#e8ecf5',
        cyan:        '#00d4ff',
        green:       '#39ff8f',
        yellow:      '#ffb347',
        red:         '#ff4757',
      },
      fontSize: 13,
      fontFamily: 'ui-monospace, Consolas, "Courier New", monospace',
      cursorStyle: 'bar',
      cursorBlink: true,
      scrollback: 2000,
    })

    const fitAddon = new FitAddon()
    term.loadAddon(fitAddon)
    term.open(containerRef.current)

    // Fit after a tick so the DOM has settled
    requestAnimationFrame(() => fitAddon.fit())

    const token = localStorage.getItem('novacomp_token') ?? ''
    const ws = new WebSocket(`${WS_URL}/terminal?token=${encodeURIComponent(token)}`)

    ws.onopen = () => { fitAddon.fit(); onWsReady?.(ws) }

    ws.onmessage = (e) => term.write(e.data)

    ws.onclose = () =>
      term.write('\r\n\x1b[2m[connection closed]\x1b[0m\r\n')

    ws.onerror = () =>
      term.write('\r\n\x1b[31m[connection error]\x1b[0m\r\n')

    // Forward all keystrokes to the REPL
    term.onKey(({ key }) => {
      if (ws.readyState === WebSocket.OPEN)
        ws.send(JSON.stringify({ type: 'input', data: key }))
    })

    const observer = new ResizeObserver(() => fitAddon.fit())
    observer.observe(containerRef.current)

    return () => {
      observer.disconnect()
      term.dispose()
      ws.close()
    }
  }, [])

  return (
    <div className="flex flex-col" style={{ height: '100%', background: '#0d1117' }}>
      <div
        className="flex items-center justify-between px-3 py-1.5 shrink-0"
        style={{ background: '#161b22', borderBottom: '1px solid #30363d' }}
      >
        <span className="text-xs font-semibold uppercase tracking-wider" style={{ color: '#8b949e' }}>
          Nova REPL
        </span>
        <button
          onClick={onClose}
          className="p-0.5 rounded"
          style={{ color: '#8b949e' }}
          title="Close terminal"
        >
          <X size={14} />
        </button>
      </div>
      <div ref={containerRef} className="flex-1 px-2 pt-1" style={{ minHeight: 0 }} />
    </div>
  )
}
