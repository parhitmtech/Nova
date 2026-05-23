import { useState, useRef, useEffect, lazy, Suspense } from 'react'
import { FileText, Terminal, Maximize2, Minimize2, Loader2, Check, Zap } from 'lucide-react'

const TerminalPanel = lazy(() => import('./TerminalPanel'))

type Tab = 'output' | 'terminal'

interface CompareResult {
  output: string
  outputMismatch: boolean
  normal:    { us: number; nodes: number; iters: number }
  optimized: { us: number; nodes: number; iters: number; folds?: number; removed?: number }
  speedup:   string | null
  nodesEliminated: number
  error?: string
}

export interface OutputState {
  mode: 'idle' | 'run' | 'compare' | 'error'
  content: string
  ms?: number
  compareData?: CompareResult
}

interface BottomPanelProps {
  output:      OutputState
  loading:     boolean
  gpuStatus:   'idle' | 'submitting' | 'pending' | 'running' | 'completed' | 'failed' | 'unavailable'
  gpuOutput:   string
  gpuError:    string
  gpuScenario: string
  gpuBenefit:  string
  onWsReady:   (ws: WebSocket) => void
}

const toMs = (us: number | undefined) =>
  us != null ? `${(us / 1000).toFixed(2)} ms` : '—'

export default function BottomPanel({
  output, loading, gpuStatus, gpuOutput, gpuError, gpuScenario, gpuBenefit, onWsReady,
}: BottomPanelProps) {
  const [activeTab,   setActiveTab]   = useState<Tab>('output')
  const [height,      setHeight]      = useState(220)
  const [isMaximized, setIsMaximized] = useState(false)
  const [termMounted, setTermMounted] = useState(false)
  const outputBottomRef = useRef<HTMLDivElement>(null)
  const dragStart       = useRef<{ y: number; h: number } | null>(null)

  // Auto-scroll output as new lines arrive
  useEffect(() => {
    outputBottomRef.current?.scrollIntoView({ behavior: 'smooth' })
  }, [output.content, gpuOutput])

  // Auto-switch to output tab whenever a run completes or errors
  useEffect(() => {
    if (output.mode === 'run' || output.mode === 'error' || output.mode === 'compare')
      setActiveTab('output')
  }, [output.mode])

  const handleTerminalTab = () => {
    setActiveTab('terminal')
    setTermMounted(true) // mount once, keep alive so the pty session persists
  }

  // ── Drag-to-resize on the top border ──────────────────────────────────────
  const onDragStart = (e: React.MouseEvent) => {
    e.preventDefault()
    dragStart.current = { y: e.clientY, h: height }

    const onMove = (ev: MouseEvent) => {
      if (!dragStart.current) return
      const delta = dragStart.current.y - ev.clientY
      setHeight(Math.max(100, Math.min(600, dragStart.current.h + delta)))
    }
    const onUp = () => {
      dragStart.current = null
      document.removeEventListener('mousemove', onMove)
      document.removeEventListener('mouseup',   onUp)
    }
    document.addEventListener('mousemove', onMove)
    document.addEventListener('mouseup',   onUp)
  }

  // ── Output renderer (same logic as old IDEPage.renderOutput) ──────────────
  const renderOutput = () => {
    if (loading && output.mode === 'idle') return (
      <div className="flex items-center justify-center h-full gap-2">
        <Loader2 size={16} className="animate-spin" style={{ color: '#00d4ff' }} />
        <span className="text-xs" style={{ color: '#8b949e' }}>Running…</span>
      </div>
    )

    if (output.mode === 'idle') return (
      <div className="flex items-center justify-center h-full">
        <p className="text-xs" style={{ color: '#8b949e' }}>Run your code to see output here.</p>
      </div>
    )

    if (output.mode === 'error') return (
      <div className="p-3">
        {output.ms != null && <div className="text-xs mb-1" style={{ color: '#8b949e' }}>{output.ms} ms</div>}
        <pre className="text-xs font-mono whitespace-pre-wrap" style={{ color: '#f85149' }}>{output.content}</pre>
      </div>
    )

    if (output.mode === 'run') return (
      <div className="p-3">
        {output.ms != null && <div className="text-xs mb-1" style={{ color: '#8b949e' }}>{output.ms} ms</div>}
        <pre className="text-xs font-mono whitespace-pre-wrap" style={{ color: '#c9d1d9' }}>
          {output.content || (loading ? '' : '(no output)')}
        </pre>
        {loading && (
          <div className="flex items-center gap-1.5 mt-2">
            <Loader2 size={11} className="animate-spin" style={{ color: '#8b949e' }} />
            <span className="text-xs" style={{ color: '#8b949e' }}>Running…</span>
          </div>
        )}
      </div>
    )

    if (output.mode === 'compare' && output.compareData) {
      const d = output.compareData
      return (
        <div className="p-3 space-y-3" style={{ fontSize: 12 }}>
          <div className="font-semibold" style={{ color: '#00d4ff' }}>Optimization Report</div>
          {d.speedup && (
            <div className="rounded p-2" style={{ background: '#0d1117', border: '1px solid #39ff8f' }}>
              <div className="mb-0.5" style={{ color: '#39ff8f', fontSize: 11 }}>Speedup</div>
              <div className="text-lg font-bold" style={{ color: '#c9d1d9' }}>{d.speedup}×</div>
            </div>
          )}
          <div className="grid grid-cols-2 gap-2">
            {(['normal', 'optimized'] as const).map(k => (
              <div key={k} className="rounded p-2" style={{ background: '#0d1117', border: '1px solid #30363d' }}>
                <div className="mb-1.5" style={{ color: k === 'normal' ? '#8b949e' : '#00d4ff', fontSize: 11 }}>
                  {k === 'normal' ? 'Normal' : 'Optimized'}
                </div>
                <div className="space-y-0.5" style={{ color: '#c9d1d9', fontSize: 11 }}>
                  <div>{toMs(d[k]?.us)}</div>
                  <div>{d[k]?.nodes ?? '—'} nodes</div>
                  {k === 'optimized' && d.optimized?.folds != null && <div>{d.optimized.folds} folds</div>}
                </div>
              </div>
            ))}
          </div>
          {d.nodesEliminated != null && (
            <div style={{ color: '#8b949e', fontSize: 11 }}>{d.nodesEliminated} AST nodes eliminated</div>
          )}
          {d.output && (
            <div>
              <div className="mb-1" style={{ color: '#8b949e', fontSize: 11 }}>Output</div>
              <pre className="rounded p-2 font-mono whitespace-pre-wrap"
                style={{ background: '#0d1117', color: '#c9d1d9', border: '1px solid #30363d', fontSize: 11 }}>
                {d.output}
              </pre>
            </div>
          )}
          {d.outputMismatch && (
            <div style={{ color: '#f85149', fontSize: 11 }}>Warning: output mismatch between runs</div>
          )}
        </div>
      )
    }
    return null
  }

  const panelHeight = isMaximized ? 420 : height

  // ── Tab button style helper ────────────────────────────────────────────────
  const tabStyle = (id: Tab): React.CSSProperties => ({
    display: 'flex', alignItems: 'center', gap: 6,
    padding: '4px 16px', fontSize: 12, cursor: 'pointer',
    background: 'none', border: 'none',
    borderTop: `2px solid ${activeTab === id ? '#1f6feb' : 'transparent'}`,
    color: activeTab === id ? '#c9d1d9' : '#8b949e',
  })

  return (
    <>
      {/* Drag handle — sits between editor and this panel */}
      <div
        onMouseDown={onDragStart}
        style={{
          height: 4, flexShrink: 0, cursor: 'row-resize',
          background: '#30363d', transition: 'background 0.15s',
        }}
        onMouseEnter={e => { (e.currentTarget as HTMLElement).style.background = '#1f6feb' }}
        onMouseLeave={e => { (e.currentTarget as HTMLElement).style.background = '#30363d' }}
      />

      <div className="flex flex-col shrink-0"
        style={{ height: panelHeight, background: '#0d1117', overflow: 'hidden' }}>

        {/* Tab bar */}
        <div className="flex items-center justify-between shrink-0"
          style={{ background: '#161b22', borderBottom: '1px solid #30363d' }}>

          <div className="flex">
            <button style={tabStyle('output')} onClick={() => setActiveTab('output')}>
              <FileText size={11} />
              Output
              {(output.mode === 'run' || output.mode === 'compare') && (
                <span style={{ width: 6, height: 6, borderRadius: '50%', background: '#39ff8f', display: 'inline-block' }} />
              )}
              {output.mode === 'error' && (
                <span style={{ width: 6, height: 6, borderRadius: '50%', background: '#f85149', display: 'inline-block' }} />
              )}
            </button>

            <button style={tabStyle('terminal')} onClick={handleTerminalTab}>
              <Terminal size={11} />
              Terminal
            </button>
          </div>

          <div className="flex items-center gap-1 pr-2">
            {loading && (
              <span className="text-xs" style={{ color: '#1f6feb', marginRight: 4 }}>Running…</span>
            )}
            <button
              onClick={() => setIsMaximized(v => !v)}
              style={{ background: 'none', border: 'none', cursor: 'pointer', color: '#8b949e', padding: 4 }}>
              {isMaximized ? <Minimize2 size={12} /> : <Maximize2 size={12} />}
            </button>
          </div>
        </div>

        {/* ── Output tab ── */}
        <div style={{
          display: activeTab === 'output' ? 'flex' : 'none',
          flexDirection: 'column', flex: 1, overflowY: 'auto',
        }}>
          {renderOutput()}

          {/* GPU status — appended below run output when a job is active */}
          {gpuStatus !== 'idle' && (
            <div className="shrink-0 mx-3 mb-3 rounded p-2"
              style={{ background: '#0d1117', border: '1px solid #30363d' }}>
              <div className="flex items-center gap-1.5 mb-2">
                <Zap size={11} style={{ color: '#a855f7' }} />
                <span className="text-xs font-semibold uppercase tracking-wider" style={{ color: '#a855f7' }}>GPU</span>
                {gpuScenario && (
                  <span className="ml-auto text-xs px-1.5 py-0.5 rounded"
                    style={{ background: '#2d1b4e', color: '#c084fc', fontSize: 10 }}>
                    {gpuScenario === 'cuda' ? 'CUDA'
                      : gpuScenario === 'hf'      ? 'HuggingFace'
                      : gpuScenario === 'sol'     ? 'SOL HPC'
                      : 'HF+SOL'}
                  </span>
                )}
              </div>

              {gpuStatus === 'submitting' && (
                <div className="flex items-center gap-2">
                  <Loader2 size={12} className="animate-spin" style={{ color: '#a855f7' }} />
                  <span className="text-xs" style={{ color: '#8b949e' }}>Submitting to SOL cluster…</span>
                </div>
              )}
              {gpuStatus === 'pending' && (
                <div className="flex items-center gap-2">
                  <Loader2 size={12} className="animate-spin" style={{ color: '#a855f7' }} />
                  <span className="text-xs" style={{ color: '#8b949e' }}>Queued — waiting for GPU node…</span>
                </div>
              )}
              {gpuStatus === 'running' && (
                <div>
                  <div className="flex items-center gap-2 mb-1">
                    <Loader2 size={12} className="animate-spin" style={{ color: '#a855f7' }} />
                    <span className="text-xs" style={{ color: '#a855f7' }}>
                      {gpuScenario === 'hf'     ? 'Calling HuggingFace GPU inference…'
                       : gpuScenario === 'sol'   ? 'Running on SOL HPC cluster…'
                       : gpuScenario === 'hf_sol'? 'Fine-tuning via SOL + HuggingFace…'
                       : 'Running on GPU…'}
                    </span>
                  </div>
                  {gpuBenefit && <p className="text-xs" style={{ color: '#8b949e' }}>{gpuBenefit}</p>}
                </div>
              )}
              {gpuStatus === 'completed' && (
                <div>
                  <div className="flex items-center gap-1.5 mb-1">
                    <Check size={11} style={{ color: '#39ff8f' }} />
                    <span className="text-xs font-medium" style={{ color: '#39ff8f' }}>Completed</span>
                  </div>
                  <pre className="text-xs font-mono whitespace-pre-wrap" style={{ color: '#c9d1d9' }}>
                    {gpuOutput || '(no output)'}
                  </pre>
                </div>
              )}
              {gpuStatus === 'failed' && (
                <div>
                  <div className="text-xs font-medium mb-1" style={{ color: '#f85149' }}>Job failed</div>
                  <pre className="text-xs font-mono whitespace-pre-wrap" style={{ color: '#f85149' }}>{gpuError}</pre>
                </div>
              )}
              {gpuStatus === 'unavailable' && (
                <div>
                  <div className="text-xs font-medium mb-1" style={{ color: '#e3b341' }}>GPU unavailable</div>
                  <p className="text-xs" style={{ color: '#8b949e' }}>SOL cluster unreachable. Check the SSH tunnel.</p>
                </div>
              )}
            </div>
          )}

          <div ref={outputBottomRef} />
        </div>

        {/* ── Terminal tab ── */}
        {/* Keep mounted in DOM once opened so the pty session isn't killed on tab switch */}
        <div style={{
          display: activeTab === 'terminal' ? 'flex' : 'none',
          flex: 1, flexDirection: 'column', overflow: 'hidden',
        }}>
          {termMounted && (
            <Suspense fallback={
              <div className="flex items-center justify-center h-full gap-2" style={{ color: '#8b949e' }}>
                <Loader2 size={14} className="animate-spin" /> Connecting…
              </div>
            }>
              <TerminalPanel
                onClose={() => setActiveTab('output')}
                onWsReady={onWsReady}
              />
            </Suspense>
          )}
        </div>

      </div>
    </>
  )
}
