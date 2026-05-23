import { lazy, Suspense, useState, useEffect, useRef, useCallback } from 'react'
import { useNavigate } from 'react-router-dom'
import {
  Save, Play, BarChart2, LogOut, Plus, Trash2, FileCode,
  Loader2, Share2, Check, Zap, Download,
} from 'lucide-react'
import { listFiles, saveFile, loadFile, deleteFile } from '../api/files'
import { runCodeStream, compareCode, submitGpuJob, getGpuStatus, getGpuResults, compileBytecode } from '../api/compiler'
import { shareCode } from '../api/share'
import { useAuth } from '../contexts/AuthContext'
import { registerNovaLanguage } from '../lib/novaLanguage'
import BottomPanel from '../components/BottomPanel'
import NovaBot from '../components/NovaBot'

// Monaco is code-split to keep the initial bundle small.
const MonacoEditor = lazy(() => import('@monaco-editor/react'))

// Metadata returned by GET /files — content is fetched separately via loadFile.
interface FileEntry { fileName: string; updatedAt: string }

// Shape of the data returned by POST /compare.
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
  ms?: number           // elapsed milliseconds, set when the "done" SSE event arrives
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
  const [shareMsg,    setShareMsg]    = useState('')

  // GPU job state machine: idle → submitting → pending → running → completed/failed
  const [gpuStatus,   setGpuStatus]   = useState<'idle' | 'submitting' | 'pending' | 'running' | 'completed' | 'failed' | 'unavailable'>('idle')
  const [gpuOutput,   setGpuOutput]   = useState('')
  const [gpuError,    setGpuError]    = useState('')
  const [gpuScenario, setGpuScenario] = useState('')
  const [gpuBenefit,  setGpuBenefit]  = useState('')

  // Stored in a ref so Monaco's onMount closure always calls the latest version
  // without needing to be re-registered on every render.
  const handleSaveRef   = useRef<() => Promise<void>>(async () => {})
  const termWsRef       = useRef<WebSocket | null>(null)
  const pollRef         = useRef<ReturnType<typeof setInterval> | null>(null)
  const restoredFileRef = useRef(false)
  const runAbortRef     = useRef<AbortController | null>(null)
  const autoSaveTimer   = useRef<ReturnType<typeof setTimeout> | null>(null)
  const [autoSaveStatus, setAutoSaveStatus] = useState<'idle' | 'saving' | 'saved'>('idle')

  // If the user arrived by clicking "Fork in IDE" on a share page, pre-populate
  // the editor with that code and clear the sessionStorage entry immediately.
  useEffect(() => {
    const fork = sessionStorage.getItem('novacomp_fork')
    if (fork) { setCode(fork); sessionStorage.removeItem('novacomp_fork') }
  }, [])

  // Fetch the user's file list from DynamoDB. On the very first call, also
  // auto-restore the last-open file so the editor isn't blank after a refresh.
  const fetchFiles = useCallback(async () => {
    if (!token) return
    try {
      const res = await listFiles(token)
      const fileList: FileEntry[] = res.data.files ?? []
      setFiles(fileList)

      if (!restoredFileRef.current && fileList.length > 0) {
        restoredFileRef.current = true
        const lastFile = localStorage.getItem('novacomp_last_file')
        if (lastFile && fileList.some(f => f.fileName === lastFile)) {
          try {
            const r = await loadFile(token, lastFile)
            // Only update editor if S3 returned actual string content —
            // a null response must not wipe the editor placeholder.
            if (typeof r.data.content === 'string') setCode(r.data.content)
            setCurrentFile(lastFile)
          } catch { /* ignore — user can pick manually */ }
        }
      }
    } catch { /* ignore network errors on background refresh */ }
  }, [token])

  useEffect(() => { fetchFiles() }, [fetchFiles])

  // Remember the open file name across page refreshes so auto-restore works.
  useEffect(() => {
    if (currentFile) localStorage.setItem('novacomp_last_file', currentFile)
  }, [currentFile])

  // Auto-save: 2 seconds after the user stops typing, silently persist to S3.
  useEffect(() => {
    if (!currentFile || !token) return
    if (autoSaveTimer.current) clearTimeout(autoSaveTimer.current)
    setAutoSaveStatus('idle')
    autoSaveTimer.current = setTimeout(async () => {
      setAutoSaveStatus('saving')
      try {
        await saveFile(token, currentFile, code)
        setAutoSaveStatus('saved')
        setTimeout(() => setAutoSaveStatus('idle'), 2000)
      } catch {
        setAutoSaveStatus('idle')
      }
    }, 2000)
    return () => { if (autoSaveTimer.current) clearTimeout(autoSaveTimer.current) }
  }, [code, currentFile, token])

  const handleSave = useCallback(async () => {
    if (!token) return
    let name = currentFile
    // Prompt for a file name only when there is no current file open.
    if (!name) {
      name = prompt('Enter file name:', 'untitled.nova') ?? ''
      if (!name) return
    }
    setSaving(true)
    try {
      await saveFile(token, name, code)
      setCurrentFile(name)
      await fetchFiles()
      // Sync the file to the server-side workspace so the terminal sees it too.
      if (termWsRef.current?.readyState === WebSocket.OPEN)
        termWsRef.current.send(JSON.stringify({ type: 'write', fileName: name, content: code }))
    } catch (err: unknown) {
      const msg = (err as { response?: { data?: { error?: string } } })?.response?.data?.error
        ?? (err instanceof Error ? err.message : 'Save failed')
      setOutput({ mode: 'error', content: `Save failed: ${msg}` })
    } finally { setSaving(false) }
  }, [token, currentFile, code, fetchFiles])

  // Keep the ref in sync so Monaco's Ctrl+S binding always calls the latest save.
  useEffect(() => { handleSaveRef.current = handleSave }, [handleSave])

  const handleLoadFile = async (fileName: string) => {
    if (!token) return
    // Clicking the already-open file is a no-op — avoids unnecessary S3 fetch
    // and prevents wiping unsaved changes if the file content comes back empty.
    if (fileName === currentFile) return

    // Binary model archives (.tar.gz) can't be displayed as text.
    // Show an informational message instead of loading garbage bytes into Monaco.
    if (/\.(tar\.gz|gz|bin|pt|pth)$/.test(fileName)) {
      setCurrentFile(fileName)
      setOutput({ mode: 'run', content: `Binary file — ${fileName}\nThis is a trained model archive. Download it to use locally with HuggingFace.` })
      return
    }

    try {
      const res = await loadFile(token, fileName)
      const content = res.data.content
      // Guard: only update editor if S3 returned a real string.
      // A null/undefined response must never wipe the current editor content.
      if (typeof content === 'string') setCode(content)
      setCurrentFile(fileName)
      // Intentionally NOT clearing output — the user may want to keep the
      // results from a previous run visible while browsing other files.
    } catch (err: unknown) {
      const msg = (err as { response?: { data?: { error?: string } } })?.response?.data?.error
        ?? (err instanceof Error ? err.message : 'Load failed')
      setOutput({ mode: 'error', content: `Failed to load file: ${msg}` })
    }
  }

  const handleNewFile = async () => {
    const name = prompt('Enter file name:', 'untitled.nova')
    if (!name) return
    setCurrentFile(name)
    setCode('')
    setOutput({ mode: 'idle', content: '' })
    try {
      // Create an empty placeholder in S3 so the file appears in the sidebar immediately.
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
      // If the deleted file was open, reset to a blank editor.
      if (currentFile === fileName) { setCurrentFile(''); setCode('') }
      await fetchFiles()
    } catch (err: unknown) {
      const msg = (err as { response?: { data?: { error?: string } } })?.response?.data?.error
        ?? (err instanceof Error ? err.message : 'Delete failed')
      setOutput({ mode: 'error', content: `Failed to delete: ${msg}` })
    }
  }

  const handleRun = async () => {
    // Abort any in-flight stream before starting a new one (e.g. user clicks Run again).
    if (runAbortRef.current) runAbortRef.current.abort()
    runAbortRef.current = new AbortController()
    setLoading(true)
    setOutput({ mode: 'run', content: '' })

    // Accumulate stdout in a local variable so the closure always appends to
    // the full text rather than the stale state snapshot from the previous render.
    const acc = { text: '' }

    try {
      await runCodeStream(code, '', (type, text) => {
        if (type === 'stdout') {
          acc.text += text
          setOutput({ mode: 'run', content: acc.text })
        } else if (type === 'stderr') {
          if (text.trim()) {
            acc.text += (acc.text ? '\n' : '') + text
            setOutput({ mode: 'run', content: acc.text })
          }
        } else if (type === 'error') {
          setOutput({ mode: 'error', content: text })
        } else if (type === 'done') {
          // "done" text is elapsed milliseconds as a string.
          const ms = parseInt(text, 10)
          setOutput(prev => ({ ...prev, ms }))
          // Refresh the file list in case sol.save() uploaded new model files.
          fetchFiles()
        }
      }, runAbortRef.current.signal, token ?? undefined)
    } catch (err: unknown) {
      if ((err as { name?: string }).name === 'AbortError') return
      const msg = (err as { message?: string }).message ?? 'Run failed'
      setOutput({ mode: 'error', content: msg })
    } finally {
      setLoading(false)
      runAbortRef.current = null
    }
  }

  const handleCompare = async () => {
    setLoading(true)
    setOutput({ mode: 'idle', content: '' })
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
      // Copy the shareable URL directly to the clipboard for convenience.
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
    setGpuScenario('')
    setGpuBenefit('')
    if (pollRef.current) { clearInterval(pollRef.current); pollRef.current = null }

    try {
      const res = await submitGpuJob(token, code)
      const jobId: string = res.data.jobId
      setGpuScenario(res.data.scenario || '')
      setGpuBenefit(res.data.gpuBenefit || '')
      // Local HF/SOL jobs start as 'running'; pure CUDA jobs enter the queue as 'pending'.
      setGpuStatus((res.data.status as typeof gpuStatus) ?? 'pending')

      // Poll for job completion every 5 seconds.
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
      // Distinguish "SOL tunnel is down" from generic job failure.
      setGpuStatus(msg.toLowerCase().includes('unavailable') || msg.toLowerCase().includes('ssh') ? 'unavailable' : 'failed')
      setGpuError(msg)
    }
  }

  // Clear the GPU polling interval when the component unmounts (e.g. user logs out).
  useEffect(() => () => { if (pollRef.current) clearInterval(pollRef.current) }, [])

  const handleExportBytecode = async () => {
    try {
      const res = await compileBytecode(code)
      // Create a temporary object URL and click it to trigger a browser download.
      const url = URL.createObjectURL(new Blob([res.data], { type: 'application/octet-stream' }))
      const a = document.createElement('a')
      a.href = url
      a.download = (currentFile ? currentFile.replace(/\.[^.]+$/, '') : 'program') + '.nbc'
      a.click()
      URL.revokeObjectURL(url)
    } catch (err: unknown) {
      const msg = (err as { response?: { data?: { error?: string } } })?.response?.data?.error
        ?? (err instanceof Error ? err.message : 'Bytecode export failed')
      setOutput({ mode: 'error', content: `Export failed: ${msg}` })
    }
  }

  const handleLogout = () => { logout(); navigate('/login') }

  // Register the Ctrl+S / Cmd+S shortcut inside Monaco.
  // We pass a stable ref instead of handleSave directly because this callback
  // is only registered once on editor mount — the ref always points to the latest save.
  const handleEditorMount = useCallback((editor: unknown, monaco: unknown) => {
    const m = monaco as { KeyMod: { CtrlCmd: number }; KeyCode: { KeyS: number } }
    const e = editor as { addCommand: (b: number, h: () => void) => void }
    e.addCommand(m.KeyMod.CtrlCmd | m.KeyCode.KeyS, () => handleSaveRef.current())
  }, [])


  // Plain text passed to NovaBot so it can reference the last run output
  const recentOutput = output.mode === 'run' || output.mode === 'error' ? output.content : ''

  return (
    <div className="flex flex-col" style={{ height: '100vh', background: '#0d1117', color: '#c9d1d9' }}>

      {/* ── Toolbar ── */}
      <div className="flex items-center justify-between px-4 py-2 shrink-0"
        style={{ background: '#161b22', borderBottom: '1px solid #30363d' }}>

        {/* Left: logo + open file */}
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
              {!saving && autoSaveStatus === 'saving' && <Loader2 size={11} className="animate-spin" style={{ color: '#8b949e' }} />}
              {!saving && autoSaveStatus === 'saved' && <Check size={11} style={{ color: '#39ff8f' }} />}
            </>
          )}
        </div>

        {/* Right: action buttons */}
        <div className="flex items-center gap-1.5">
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

          <button onClick={handleExportBytecode}
            className="flex items-center gap-1.5 px-3 py-1.5 rounded-md text-xs font-medium"
            style={{ background: '#0d2a1a', color: '#3fb950', border: '1px solid #238636' }}>
            <Download size={12} /> Export .nbc
          </button>

          <button onClick={handleShare}
            className="flex items-center gap-1.5 px-3 py-1.5 rounded-md text-xs font-medium"
            style={{ background: '#21262d', color: '#c9d1d9', border: '1px solid #30363d' }}>
            {shareMsg === 'copied!' ? <Check size={12} style={{ color: '#39ff8f' }} /> : <Share2 size={12} />}
            {shareMsg || 'Share'}
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

      {/* ── Main area: three columns ── */}
      <div className="flex flex-1 min-h-0">

        {/* Left: file browser */}
        <div className="flex flex-col shrink-0"
          style={{ width: 220, background: '#161b22', borderRight: '1px solid #30363d' }}>
          <div className="flex items-center justify-between px-3 py-2 shrink-0"
            style={{ borderBottom: '1px solid #30363d' }}>
            <span className="text-xs font-semibold uppercase tracking-wider" style={{ color: '#8b949e' }}>Files</span>
            <button onClick={handleNewFile} title="New file" className="p-0.5 rounded"
              style={{ color: '#8b949e', background: 'none', border: 'none', cursor: 'pointer' }}>
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
                    <span className="text-xs truncate"
                      style={{ color: currentFile === f.fileName ? '#00d4ff' : '#c9d1d9' }}>
                      {f.fileName}
                    </span>
                  </div>
                  <button onClick={e => { e.stopPropagation(); handleDeleteFile(f.fileName) }}
                    title="Delete" className="opacity-0 group-hover:opacity-100 p-0.5 rounded"
                    style={{ color: '#f85149', background: 'none', border: 'none', cursor: 'pointer' }}>
                    <Trash2 size={11} />
                  </button>
                </div>
              ))
            }
          </div>
        </div>

        {/* Center: editor + bottom panel (vertical flex) */}
        <div className="flex flex-col flex-1 min-w-0">
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

          <BottomPanel
            output={output}
            loading={loading}
            gpuStatus={gpuStatus}
            gpuOutput={gpuOutput}
            gpuError={gpuError}
            gpuScenario={gpuScenario}
            gpuBenefit={gpuBenefit}
            onWsReady={(ws: WebSocket) => { termWsRef.current = ws }}
          />
        </div>

        {/* Right: NovaBot */}
        <div className="shrink-0 flex flex-col"
          style={{ width: 320, borderLeft: '1px solid #30363d' }}>
          <NovaBot
            token={token!}
            currentCode={code}
            currentFile={currentFile}
            recentOutput={recentOutput}
            onFileWrite={() => { fetchFiles() }}
          />
        </div>

      </div>
    </div>
  )
}
