import { lazy, Suspense, useState, useEffect, useRef, useCallback } from 'react'
import { useNavigate } from 'react-router-dom'
import {
  Save, Play, BarChart2, LogOut, Plus, Trash2, FileCode,
  Loader2, Share2, TerminalSquare, Check, Zap,
} from 'lucide-react'
import { listFiles, saveFile, loadFile, deleteFile } from '../api/files'
import { runCode, compareCode, submitGpuJob, getGpuStatus, getGpuResults } from '../api/compiler'
import { shareCode } from '../api/share'
import { useAuth } from '../contexts/AuthContext'
import { registerNovaLanguage } from '../lib/novaLanguage'

const MonacoEditor   = lazy(() => import('@monaco-editor/react'))
const TerminalPanel  = lazy(() => import('../components/TerminalPanel'))

interface FileEntry  { fileName: string; updatedAt: string }

interface CompareResult {
  output: string
  outputMismatch: boolean
  normal:    { us: number; nodes: number; iters: number }
  optimized: { us: number; nodes: number; iters: number; folds?: number; removed?: number }
  speedup:   string | null
  nodesEliminated: number
  error?: string
}

type OutputMode = 'idle' | 'run' | 'compare' | 'error'

interface OutputState {
  mode: OutputMode
  content: string
  ms?: number
  compareData?: CompareResult
}

export default function IDEPage() {
  const { token, user, logout } = useAuth()
  const navigate = useNavigate()
  const [files,       setFiles]       = useState<FileEntry[]>([])
  const [currentFile, setCurrentFile] = useState('')
  const [code,        setCode]        = useState('// Start writing Nova code here\n')
  const [output,      setOutput]      = useState<OutputState>({ mode: 'idle', content: '' })
  const [loading,     setLoading]     = useState(false)
  const [saving,      setSaving]      = useState(false)
  const [termOpen,    setTermOpen]    = useState(false)
  const [shareMsg,    setShareMsg]    = useState('')
  const [gpuStatus,  setGpuStatus]  = useState<'idle' | 'submitting' | 'pending' | 'running' | 'completed' | 'failed' | 'unavailable'>('idle')
  const [gpuOutput,  setGpuOutput]  = useState('')
  const [gpuError,   setGpuError]   = useState('')
  const outputRef     = useRef<HTMLDivElement>(null)
  const handleSaveRef = useRef<() => Promise<void>>(async () => {})
  const termWsRef     = useRef<WebSocket | null>(null)
  const pollRef       = useRef<ReturnType<typeof setInterval> | null>(null)

  // Pre-load forked code from SharePage
  useEffect(() => {
    const fork = sessionStorage.getItem('novacomp_fork')
    if (fork) { setCode(fork); sessionStorage.removeItem('novacomp_fork') }
  }, [])

  const fetchFiles = useCallback(async () => {
    if (!token) return
    try { const res = await listFiles(token); setFiles(res.data.files ?? []) }
    catch { /* ignore */ }
  }, [token])

  useEffect(() => { fetchFiles() }, [fetchFiles])

  useEffect(() => {
    if (outputRef.current)
      outputRef.current.scrollTop = outputRef.current.scrollHeight
  }, [output])

  const handleSave = useCallback(async () => {
    if (!token) return
    let name = currentFile
    if (!name) {
      name = prompt('Enter file name:', 'untitled.nova') ?? ''
      if (!name) return
    }
    setSaving(true)
    try {
      await saveFile(token, name, code)
      setCurrentFile(name)
      await fetchFiles()
      if (termWsRef.current?.readyState === WebSocket.OPEN)
        termWsRef.current.send(JSON.stringify({ type: 'write', fileName: name, content: code }))
    } catch (err: unknown) {
      const msg = (err as { response?: { data?: { error?: string } } })?.response?.data?.error
        ?? (err instanceof Error ? err.message : 'Save failed')
      setOutput({ mode: 'error', content: `Save failed: ${msg}` })
    } finally { setSaving(false) }
  }, [token, currentFile, code, fetchFiles])

  useEffect(() => { handleSaveRef.current = handleSave }, [handleSave])

  const handleLoadFile = async (fileName: string) => {
    if (!token) return
    try {
      const res = await loadFile(token, fileName)
      setCode(res.data.content ?? '')
      setCurrentFile(fileName)
      setOutput({ mode: 'idle', content: '' })
      setGpuStatus('idle'); setGpuOutput(''); setGpuError('')
      if (pollRef.current) { clearInterval(pollRef.current); pollRef.current = null }
    } catch (err: unknown) {
      const msg = (err as { response?: { data?: { error?: string } } })?.response?.data?.error
        ?? (err instanceof Error ? err.message : 'Load failed')
      setOutput({ mode: 'error', content: `Failed to load file: ${msg}` })
    }
  }

  const handleNewFile = async () => {
    const name = prompt('Enter file name:', 'untitled.nova')
    if (!name) return
    setCurrentFile(name); setCode(''); setOutput({ mode: 'idle', content: '' })
    try {
      await saveFile(token!, name, '')
      await fetchFiles()
      if (termWsRef.current?.readyState === WebSocket.OPEN)
        termWsRef.current.send(JSON.stringify({ type: 'write', fileName: name, content: '' }))
    } catch { /* user can still edit and save manually */ }
  }

  const handleDeleteFile = async (fileName: string) => {
    if (!token || !confirm(`Delete ${fileName}?`)) return
    try {
      await deleteFile(token, fileName)
      if (currentFile === fileName) { setCurrentFile(''); setCode('') }
      await fetchFiles()
    } catch (err: unknown) {
      const msg = (err as { response?: { data?: { error?: string } } })?.response?.data?.error
        ?? (err instanceof Error ? err.message : 'Delete failed')
      setOutput({ mode: 'error', content: `Failed to delete: ${msg}` })
    }
  }

  const handleRun = async () => {
    setLoading(true); setOutput({ mode: 'idle', content: '' })
    try {
      const res = await runCode(code)
      setOutput(res.data.error
        ? { mode: 'error', content: res.data.error, ms: res.data.ms }
        : { mode: 'run',   content: res.data.output ?? '', ms: res.data.ms })
    } catch (err: unknown) {
      const msg = (err as { response?: { data?: { error?: string } } })?.response?.data?.error
        ?? (err instanceof Error ? err.message : 'Run failed')
      setOutput({ mode: 'error', content: msg })
    } finally { setLoading(false) }
  }

  const handleCompare = async () => {
    setLoading(true); setOutput({ mode: 'idle', content: '' })
    try {
      const res = await compareCode(code)
      setOutput(res.data.error
        ? { mode: 'error',   content: res.data.error }
        : { mode: 'compare', content: '', compareData: res.data })
    } catch (err: unknown) {
      const msg = (err as { response?: { data?: { error?: string } } })?.response?.data?.error
        ?? (err instanceof Error ? err.message : 'Compare failed')
      setOutput({ mode: 'error', content: msg })
    } finally { setLoading(false) }
  }

  const handleShare = async () => {
    try {
      const res = await shareCode(code)
      const url = `${window.location.origin}/share/${res.data.id}`
      await navigator.clipboard.writeText(url)
      setShareMsg('copied!')
    } catch {
      setShareMsg('failed')
    } finally {
      setTimeout(() => setShareMsg(''), 2500)
    }
  }

  const handleRunOnGpu = async () => {
    if (!token) return
    setGpuStatus('submitting')
    setGpuOutput('')
    setGpuError('')
    if (pollRef.current) { clearInterval(pollRef.current); pollRef.current = null }
    try {
      const res = await submitGpuJob(token, code)
      const jobId: string = res.data.jobId
      setGpuStatus('pending')
      pollRef.current = setInterval(async () => {
        try {
          const statusRes = await getGpuStatus(token, jobId)
          const st: string = statusRes.data.status
          setGpuStatus(st as typeof gpuStatus)
          if (st === 'completed' || st === 'failed') {
            clearInterval(pollRef.current!); pollRef.current = null
            if (st === 'completed') {
              const resultRes = await getGpuResults(token, jobId)
              setGpuOutput(resultRes.data.output ?? '')
            } else {
              setGpuError(statusRes.data.error ?? 'Job failed on cluster')
            }
          }
        } catch {
          clearInterval(pollRef.current!); pollRef.current = null
          setGpuStatus('failed')
          setGpuError('Failed to poll job status')
        }
      }, 5000)
    } catch (err: unknown) {
      const msg = (err as { response?: { data?: { error?: string } } })?.response?.data?.error
        ?? (err instanceof Error ? err.message : 'GPU submission failed')
      setGpuStatus(msg.toLowerCase().includes('unavailable') || msg.toLowerCase().includes('ssh') ? 'unavailable' : 'failed')
      setGpuError(msg)
    }
  }

  useEffect(() => () => { if (pollRef.current) clearInterval(pollRef.current) }, [])

  const handleLogout = () => { logout(); navigate('/login') }

  const handleEditorMount = useCallback((editor: unknown, monaco: unknown) => {
    const m = monaco as { KeyMod: { CtrlCmd: number }; KeyCode: { KeyS: number } }
    const e = editor as { addCommand: (b: number, h: () => void) => void }
    e.addCommand(m.KeyMod.CtrlCmd | m.KeyCode.KeyS, () => handleSaveRef.current())
  }, [])

  const toMs = (us: number | undefined) =>
    us != null ? `${(us / 1000).toFixed(2)} ms` : '—'

  const renderOutput = () => {
    if (loading) return (
      <div className="flex items-center justify-center h-full gap-2">
        <Loader2 size={18} className="animate-spin" style={{ color: '#00d4ff' }} />
        <span className="text-sm" style={{ color: '#8b949e' }}>Running…</span>
      </div>
    )
    if (output.mode === 'idle') return (
      <div className="flex items-center justify-center h-full">
        <p className="text-sm" style={{ color: '#8b949e' }}>Press Run to execute your code</p>
      </div>
    )
    if (output.mode === 'error') return (
      <div className="p-4">
        {output.ms != null && <div className="text-xs mb-2" style={{ color: '#8b949e' }}>{output.ms} ms</div>}
        <pre className="text-sm font-mono whitespace-pre-wrap" style={{ color: '#f85149' }}>{output.content}</pre>
      </div>
    )
    if (output.mode === 'run') return (
      <div className="p-4">
        {output.ms != null && <div className="text-xs mb-2" style={{ color: '#8b949e' }}>{output.ms} ms</div>}
        <pre className="text-sm font-mono whitespace-pre-wrap" style={{ color: '#c9d1d9' }}>{output.content || '(no output)'}</pre>
      </div>
    )
    if (output.mode === 'compare' && output.compareData) {
      const d = output.compareData
      return (
        <div className="p-4 space-y-3 text-sm">
          <div className="font-semibold" style={{ color: '#00d4ff' }}>Optimization Report</div>
          {d.speedup && (
            <div className="rounded-lg p-3" style={{ background: '#0d1117', border: '1px solid #39ff8f' }}>
              <div className="text-xs font-medium mb-1" style={{ color: '#39ff8f' }}>Speedup</div>
              <div className="text-xl font-bold" style={{ color: '#c9d1d9' }}>{d.speedup}×</div>
            </div>
          )}
          <div className="grid grid-cols-2 gap-2">
            {(['normal', 'optimized'] as const).map(k => (
              <div key={k} className="rounded-lg p-3" style={{ background: '#0d1117', border: '1px solid #30363d' }}>
                <div className="text-xs font-medium mb-2" style={{ color: k === 'normal' ? '#8b949e' : '#00d4ff' }}>
                  {k === 'normal' ? 'Normal' : 'Optimized'}
                </div>
                <div className="text-xs space-y-1" style={{ color: '#c9d1d9' }}>
                  <div>{toMs(d[k]?.us)}</div>
                  <div>{d[k]?.nodes ?? '—'} nodes</div>
                  {k === 'optimized' && d.optimized?.folds != null && <div>{d.optimized.folds} folds</div>}
                </div>
              </div>
            ))}
          </div>
          {d.nodesEliminated != null && (
            <div className="text-xs" style={{ color: '#8b949e' }}>{d.nodesEliminated} AST nodes eliminated</div>
          )}
          {d.output && (
            <div>
              <div className="text-xs font-medium mb-1" style={{ color: '#8b949e' }}>Output</div>
              <pre className="text-xs font-mono whitespace-pre-wrap p-2 rounded"
                style={{ background: '#0d1117', color: '#c9d1d9', border: '1px solid #30363d' }}>{d.output}</pre>
            </div>
          )}
          {d.outputMismatch && <div className="text-xs" style={{ color: '#f85149' }}>Warning: output mismatch between runs</div>}
        </div>
      )
    }
    return null
  }

  return (
    <div className="flex flex-col" style={{ height: '100vh', background: '#0d1117', color: '#c9d1d9' }}>
      {/* ── Toolbar ── */}
      <div className="flex items-center justify-between px-4 py-2 shrink-0"
        style={{ background: '#161b22', borderBottom: '1px solid #30363d' }}>
        <div className="flex items-center gap-2">
          <svg width="18" height="18" viewBox="0 0 24 24" fill="none" stroke="#00d4ff" strokeWidth="2" strokeLinecap="round" strokeLinejoin="round">
            <polyline points="16 18 22 12 16 6" /><polyline points="8 6 2 12 8 18" />
          </svg>
          <span className="font-semibold text-sm" style={{ color: '#c9d1d9' }}>NovaComp</span>
          {currentFile && (
            <>
              <span style={{ color: '#30363d' }}>/</span>
              <span className="text-sm" style={{ color: '#8b949e' }}>{currentFile}</span>
              {saving && <Loader2 size={11} className="animate-spin" style={{ color: '#8b949e' }} />}
            </>
          )}
        </div>

        <div className="flex items-center gap-1.5">
          {/* Primary actions */}
          <button onClick={handleSave} disabled={saving} title="Save (Ctrl+S)"
            className="flex items-center gap-1.5 px-3 py-1.5 rounded-md text-xs font-medium disabled:opacity-50"
            style={{ background: '#21262d', color: '#c9d1d9', border: '1px solid #30363d' }}>
            <Save size={12} /> Save
          </button>
          <button onClick={handleRun} disabled={loading}
            className="flex items-center gap-1.5 px-3 py-1.5 rounded-md text-xs font-medium disabled:opacity-50"
            style={{ background: '#1f6feb', color: '#fff', border: '1px solid #1f6feb' }}>
            <Play size={12} /> Run
          </button>
          <button onClick={handleCompare} disabled={loading}
            className="flex items-center gap-1.5 px-3 py-1.5 rounded-md text-xs font-medium disabled:opacity-50"
            style={{ background: '#21262d', color: '#c9d1d9', border: '1px solid #30363d' }}>
            <BarChart2 size={12} /> Compare
          </button>
          <button onClick={handleRunOnGpu}
            disabled={gpuStatus === 'submitting' || gpuStatus === 'pending' || gpuStatus === 'running'}
            className="flex items-center gap-1.5 px-3 py-1.5 rounded-md text-xs font-medium disabled:opacity-50"
            style={{ background: '#1a0a2e', color: '#a855f7', border: '1px solid #7c3aed' }}>
            {gpuStatus === 'submitting' || gpuStatus === 'pending' || gpuStatus === 'running'
              ? <Loader2 size={12} className="animate-spin" />
              : <Zap size={12} />}
            Run on GPU
          </button>

          {/* Share */}
          <button onClick={handleShare}
            className="flex items-center gap-1.5 px-3 py-1.5 rounded-md text-xs font-medium"
            style={{ background: '#21262d', color: '#c9d1d9', border: '1px solid #30363d' }}>
            {shareMsg === 'copied!' ? <Check size={12} style={{ color: '#39ff8f' }} /> : <Share2 size={12} />}
            {shareMsg ? shareMsg : 'Share'}
          </button>

          <div className="w-px h-4 mx-0.5" style={{ background: '#30363d' }} />

          {/* Terminal toggle */}
          <button onClick={() => setTermOpen(v => !v)}
            className="flex items-center gap-1.5 px-3 py-1.5 rounded-md text-xs font-medium"
            style={termOpen
              ? { background: '#00d4ff20', color: '#00d4ff', border: '1px solid #00d4ff40' }
              : { background: '#21262d',   color: '#8b949e', border: '1px solid #30363d' }}>
            <TerminalSquare size={12} /> Terminal
          </button>

          <div className="w-px h-4 mx-0.5" style={{ background: '#30363d' }} />

          <span className="text-xs" style={{ color: '#8b949e' }}>{user?.email}</span>
          <button onClick={handleLogout}
            className="flex items-center gap-1.5 px-3 py-1.5 rounded-md text-xs font-medium"
            style={{ background: '#21262d', color: '#8b949e', border: '1px solid #30363d' }}>
            <LogOut size={12} /> Logout
          </button>
        </div>
      </div>

      {/* ── Main area (editor + panels) ── */}
      <div className="flex flex-1 min-h-0">
        {/* Sidebar */}
        <div className="flex flex-col shrink-0" style={{ width: '220px', background: '#161b22', borderRight: '1px solid #30363d' }}>
          <div className="flex items-center justify-between px-3 py-2 shrink-0" style={{ borderBottom: '1px solid #30363d' }}>
            <span className="text-xs font-semibold uppercase tracking-wider" style={{ color: '#8b949e' }}>Files</span>
            <button onClick={handleNewFile} title="New file" className="p-0.5 rounded" style={{ color: '#8b949e' }}>
              <Plus size={14} />
            </button>
          </div>
          <div className="flex-1 overflow-y-auto py-1">
            {files.length === 0
              ? <p className="px-3 py-2 text-xs" style={{ color: '#8b949e' }}>No files yet</p>
              : files.map(f => (
                <div key={f.fileName}
                  className="group flex items-center justify-between px-3 py-1.5 cursor-pointer"
                  style={currentFile === f.fileName ? { background: '#21262d' } : undefined}
                  onClick={() => handleLoadFile(f.fileName)}>
                  <div className="flex items-center gap-1.5 min-w-0">
                    <FileCode size={12} style={{ flexShrink: 0, color: currentFile === f.fileName ? '#00d4ff' : '#8b949e' }} />
                    <span className="text-xs truncate" style={{ color: currentFile === f.fileName ? '#00d4ff' : '#c9d1d9' }}>
                      {f.fileName}
                    </span>
                  </div>
                  <button onClick={e => { e.stopPropagation(); handleDeleteFile(f.fileName) }}
                    title="Delete" className="opacity-0 group-hover:opacity-100 p-0.5 rounded"
                    style={{ color: '#f85149' }}>
                    <Trash2 size={11} />
                  </button>
                </div>
              ))
            }
          </div>
        </div>

        {/* Editor column — split vertically for terminal */}
        <div className="flex flex-col flex-1 min-w-0">
          {/* Monaco Editor */}
          <div className="flex-1 min-h-0">
            <Suspense fallback={
              <div className="flex items-center justify-center h-full gap-2" style={{ color: '#8b949e' }}>
                <Loader2 size={18} className="animate-spin" /> Loading editor…
              </div>
            }>
              <MonacoEditor
                height="100%"
                language="nova"
                theme="nova-dark"
                value={code}
                onChange={v => setCode(v ?? '')}
                onMount={handleEditorMount}
                beforeMount={registerNovaLanguage}
                options={{
                  fontSize: 14,
                  fontFamily: 'ui-monospace, Consolas, monospace',
                  minimap: { enabled: false },
                  scrollBeyondLastLine: false,
                  padding: { top: 16 },
                  wordWrap: 'on',
                }}
              />
            </Suspense>
          </div>

          {/* Terminal panel */}
          {termOpen && (
            <div className="shrink-0" style={{ height: '240px', borderTop: '1px solid #30363d' }}>
              <Suspense fallback={
                <div className="flex items-center justify-center h-full gap-2" style={{ color: '#8b949e', background: '#0d1117' }}>
                  <Loader2 size={16} className="animate-spin" /> Connecting…
                </div>
              }>
                <TerminalPanel
                  key={termOpen ? 'open' : 'closed'}
                  onClose={() => setTermOpen(false)}
                  onWsReady={ws => { termWsRef.current = ws }}
                />
              </Suspense>
            </div>
          )}
        </div>

        {/* Output panel */}
        <div className="flex flex-col shrink-0" style={{ width: '300px', background: '#161b22', borderLeft: '1px solid #30363d' }}>
          <div className="flex items-center px-3 py-2 shrink-0" style={{ borderBottom: '1px solid #30363d' }}>
            <span className="text-xs font-semibold uppercase tracking-wider" style={{ color: '#8b949e' }}>Output</span>
          </div>
          <div ref={outputRef} className="flex-1 overflow-y-auto">{renderOutput()}</div>

          {/* GPU status section */}
          {gpuStatus !== 'idle' && (
            <div className="shrink-0" style={{ borderTop: '1px solid #30363d' }}>
              <div className="flex items-center gap-1.5 px-3 py-2" style={{ borderBottom: '1px solid #30363d' }}>
                <Zap size={11} style={{ color: '#a855f7' }} />
                <span className="text-xs font-semibold uppercase tracking-wider" style={{ color: '#a855f7' }}>GPU</span>
              </div>
              <div className="p-3">
                {gpuStatus === 'submitting' && (
                  <div className="flex items-center gap-2">
                    <Loader2 size={14} className="animate-spin" style={{ color: '#a855f7' }} />
                    <span className="text-xs" style={{ color: '#8b949e' }}>Submitting job…</span>
                  </div>
                )}
                {gpuStatus === 'pending' && (
                  <div className="flex items-center gap-2">
                    <Loader2 size={14} className="animate-spin" style={{ color: '#a855f7' }} />
                    <span className="text-xs" style={{ color: '#8b949e' }}>Queued on SOL cluster…</span>
                  </div>
                )}
                {gpuStatus === 'running' && (
                  <div className="flex items-center gap-2">
                    <Loader2 size={14} className="animate-spin" style={{ color: '#a855f7' }} />
                    <span className="text-xs" style={{ color: '#a855f7' }}>Running on GPU…</span>
                  </div>
                )}
                {gpuStatus === 'completed' && (
                  <div>
                    <div className="flex items-center gap-1.5 mb-2">
                      <Check size={12} style={{ color: '#39ff8f' }} />
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
                    <p className="text-xs" style={{ color: '#8b949e' }}>SOL cluster is unreachable. Check the SSH tunnel.</p>
                  </div>
                )}
              </div>
            </div>
          )}
        </div>
      </div>
    </div>
  )
}
