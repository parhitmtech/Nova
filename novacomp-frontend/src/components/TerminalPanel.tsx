import { useEffect, useRef } from 'react'
import { Terminal } from '@xterm/xterm'
import { FitAddon } from '@xterm/addon-fit'
import { X } from 'lucide-react'
import '@xterm/xterm/css/xterm.css'

// Convert the HTTP API base URL to a WebSocket URL so we can connect to the
// terminal endpoint. Replaces "http" with "ws" (and "https" with "wss").
const WS_URL = import.meta.env.VITE_API_URL.replace(/^http/, 'ws')

interface Props {
  onClose: () => void
  // Exposes the live WebSocket to the parent so IDEPage can send file-write
  // commands to the server-side shell after a Save action.
  onWsReady?: (ws: WebSocket) => void
}

export default function TerminalPanel({ onClose, onWsReady }: Props) {
  const containerRef = useRef<HTMLDivElement>(null)

  useEffect(() => {
    if (!containerRef.current) return

    // Initialise xterm.js with the NovaComp dark theme palette.
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

    // FitAddon resizes the terminal's column/row count to match the DOM element.
    const fitAddon = new FitAddon()
    term.loadAddon(fitAddon)
    term.open(containerRef.current)

    // Defer the initial fit by one animation frame — the container needs one
    // layout pass before its dimensions are stable enough for FitAddon to read.
    requestAnimationFrame(() => fitAddon.fit())

    // Attach the JWT so the server can authorise the terminal WebSocket session.
    const token = localStorage.getItem('novacomp_token') ?? ''
    const ws = new WebSocket(`${WS_URL}/terminal?token=${encodeURIComponent(token)}`)

    ws.onopen = () => {
      fitAddon.fit()       // refit now that the connection is live
      onWsReady?.(ws)      // hand the socket to IDEPage for file-write commands
    }

    // Server sends raw PTY output — write it directly to the terminal.
    ws.onmessage = (e) => term.write(e.data)

    // Show a dim notice when the server closes the shell (e.g. idle timeout).
    ws.onclose = () =>
      term.write('\r\n\x1b[2m[connection closed]\x1b[0m\r\n')

    ws.onerror = () =>
      term.write('\r\n\x1b[31m[connection error]\x1b[0m\r\n')

    // Forward every keystroke to the server's PTY process as a JSON message.
    term.onKey(({ key }) => {
      if (ws.readyState === WebSocket.OPEN)
        ws.send(JSON.stringify({ type: 'input', data: key }))
    })

    // Refit the terminal whenever the panel is resized (e.g. window resize,
    // split-pane drag) so columns/rows stay in sync with the visible area.
    const observer = new ResizeObserver(() => fitAddon.fit())
    observer.observe(containerRef.current)

    // Cleanup on unmount: stop observing, dispose xterm, close the WebSocket.
    return () => {
      observer.disconnect()
      term.dispose()
      ws.close()
    }
  }, []) // run once on mount — terminal is tied to this component instance

  return (
    <div className="flex flex-col" style={{ height: '100%', background: '#0d1117' }}>
      {/* Panel header with close button */}
      <div
        className="flex items-center justify-between px-3 py-1.5 shrink-0"
        style={{ background: '#161b22', borderBottom: '1px solid #30363d' }}
      >
        <span className="text-xs font-semibold uppercase tracking-wider" style={{ color: '#8b949e' }}>
          Nova REPL
        </span>
        <button onClick={onClose} className="p-0.5 rounded" style={{ color: '#8b949e' }} title="Close terminal">
          <X size={14} />
        </button>
      </div>

      {/* xterm.js mounts into this div — padding keeps the text off the edges */}
      <div ref={containerRef} className="flex-1 px-2 pt-1" style={{ minHeight: 0 }} />
    </div>
  )
}
