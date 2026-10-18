import { lazy, Suspense, useState, useEffect, useRef, useCallback } from 'react'
import { useNavigate } from 'react-router-dom'
import {
  Save,
  Play,
  BarChart2,
  LogOut,
  Plus,
  Trash2,
  FileCode,
  Loader2,
  Share2,
  Zap,
  Download,
  Search,
  PanelLeft,
  Sparkles,
  Settings2,
  Square,
  ChevronRight,
  Code2,
  Command as CommandIcon,
  X,
  CheckCircle2,
  AlertCircle,
} from 'lucide-react'
import { listFiles, saveFile, loadFile, deleteFile } from '../api/files'
import {
  runCodeStream,
  compareCode,
  submitGpuJob,
  getGpuStatus,
  getGpuResults,
  compileBytecode,
} from '../api/compiler'
import { shareCode } from '../api/share'
import { useAuth } from '../contexts/AuthContext'
import { registerNovaLanguage } from '../lib/novaLanguage'
import { EXAMPLES, isBinaryFile, validFileName, errorMessage, downloadBlob } from '../lib/workspace'
import BottomPanel, { type OutputState } from '../components/BottomPanel'
import NovaBot from '../components/NovaBot'
import Modal from '../components/Modal'
import CommandPalette, { type Command } from '../components/CommandPalette'
const MonacoEditor = lazy(() => import('../components/NovaEditor'))
interface FileEntry {
  fileName: string
  updatedAt: string
}
interface DocumentState {
  name: string
  code: string
  saved: string
  binary: boolean
}
type SaveStatus = 'saved' | 'unsaved' | 'saving' | 'failed'
type GpuStatus = 'idle' | 'submitting' | 'pending' | 'running' | 'completed' | 'failed' | 'unavailable'
const initialDocument = (): DocumentState => {
  const fork = sessionStorage.getItem('novacomp_fork')
  return { name: '', code: fork ?? EXAMPLES[0].code, saved: fork ? '' : EXAMPLES[0].code, binary: false }
}
const initialSettings = () => {
  try {
    const saved = JSON.parse(localStorage.getItem('nova_editor_settings') ?? '{}')
    return {
      fontSize: Math.min(22, Math.max(12, Number(saved.fontSize) || 14)),
      wrap: saved.wrap !== false,
      minimap: saved.minimap === true,
    }
  } catch {
    return { fontSize: 14, wrap: true, minimap: false }
  }
}
export default function IDEPage() {
  const { token, user, logout } = useAuth()
  const navigate = useNavigate()
  const [files, setFiles] = useState<FileEntry[]>([])
  const [document, setDocument] = useState<DocumentState>(initialDocument)
  const docRef = useRef(document)
  const [status, setStatus] = useState<SaveStatus>(document.code === document.saved ? 'saved' : 'unsaved')
  const forkOnEntry = useRef(sessionStorage.getItem('novacomp_fork') !== null)
  const [output, setOutput] = useState<OutputState>({ mode: 'idle', content: '' })
  const [loading, setLoading] = useState(false)
  const [running, setRunning] = useState(false)
  const [switching, setSwitching] = useState(false)
  const [fileError, setFileError] = useState('')
  const [query, setQuery] = useState('')
  const [filesOpen, setFilesOpen] = useState(() => window.innerWidth > 760)
  const [assistantOpen, setAssistantOpen] = useState(() => window.innerWidth > 1200)
  const [palette, setPalette] = useState(false)
  const [settingsOpen, setSettingsOpen] = useState(false)
  const [settings, setSettings] = useState(initialSettings)
  const [inputs, setInputs] = useState('')
  const [inputOpen, setInputOpen] = useState(false)
  const [notice, setNotice] = useState<{ text: string; error?: boolean } | null>(null)
  const [nameDialog, setNameDialog] = useState<{
    kind: 'save' | 'new'
    code?: string
    after?: () => void
  } | null>(null)
  const [fileName, setFileName] = useState('untitled.nova')
  const [dialogError, setDialogError] = useState('')
  const [dialogBusy, setDialogBusy] = useState(false)
  const [deleteTarget, setDeleteTarget] = useState('')
  const [gpuStatus, setGpuStatus] = useState<GpuStatus>('idle')
  const [gpuOutput, setGpuOutput] = useState('')
  const [gpuError, setGpuError] = useState('')
  const [gpuScenario, setGpuScenario] = useState('')
  const [gpuBenefit, setGpuBenefit] = useState('')
  const runAbort = useRef<AbortController | null>(null)
  const poll = useRef<ReturnType<typeof setInterval> | null>(null)
  const saveQueue = useRef<Promise<unknown>>(Promise.resolve())
  const termWs = useRef<WebSocket | null>(null)
  const restored = useRef(false)
  const revision = useRef(0)
  const transitionLock = useRef(false)
  const notify = useCallback((text: string, error = false) => setNotice({ text, error }), [])
  const updateDocument = useCallback((next: DocumentState) => {
    docRef.current = next
    setDocument(next)
    setStatus(next.code === next.saved ? 'saved' : 'unsaved')
  }, [])
  const fetchFiles = useCallback(async () => {
    if (!token) return []
    try {
      const res = await listFiles(token)
      const next: FileEntry[] = res.data.files ?? []
      setFiles(next)
      setFileError('')
      return next
    } catch (error) {
      setFileError(errorMessage(error, 'Could not load files'))
      return []
    }
  }, [token])
  const persist = useCallback(
    async (snapshot: DocumentState, name = snapshot.name) => {
      if (!token || snapshot.binary || !name) return false
      setStatus('saving')
      const task = saveQueue.current.catch(() => {}).then(() => saveFile(token, name, snapshot.code))
      saveQueue.current = task
      try {
        await task
        if (docRef.current.name === snapshot.name) {
          const latest = { ...docRef.current, name, saved: snapshot.code }
          updateDocument(latest)
        }
        setFiles((prev) =>
          prev.some((f) => f.fileName === name)
            ? prev
            : [...prev, { fileName: name, updatedAt: new Date().toISOString() }],
        )
        if (termWs.current?.readyState === WebSocket.OPEN)
          termWs.current.send(JSON.stringify({ type: 'write', fileName: name, content: snapshot.code }))
        return true
      } catch (error) {
        if (docRef.current.name === snapshot.name) setStatus('failed')
        notify(`Save failed: ${errorMessage(error, 'Please try again')}`, true)
        return false
      }
    },
    [token, updateDocument, notify],
  )
  useEffect(() => {
    const hasFork = forkOnEntry.current
    sessionStorage.removeItem('novacomp_fork')
    let active = true
    const startRevision = revision.current
    Promise.resolve()
      .then(fetchFiles)
      .then(async (entries) => {
        if (!active || restored.current) return
        restored.current = true
        const name = localStorage.getItem('novacomp_last_file')
        if (
          !hasFork &&
          name &&
          !isBinaryFile(name) &&
          entries.some((f) => f.fileName === name) &&
          revision.current === startRevision
        ) {
          try {
            const res = await loadFile(token!, name)
            if (active && revision.current === startRevision && typeof res.data.content === 'string')
              updateDocument({ name, code: res.data.content, saved: res.data.content, binary: false })
          } catch {
            if (active) notify('Your last file could not be restored. Select it in Files to retry.', true)
          }
        }
      })
    return () => {
      active = false
    }
  }, [fetchFiles, token, updateDocument, notify])
  useEffect(() => {
    if (document.name) localStorage.setItem('novacomp_last_file', document.name)
  }, [document.name])
  useEffect(() => {
    localStorage.setItem('nova_editor_settings', JSON.stringify(settings))
  }, [settings])
  useEffect(() => {
    if (
      !document.name ||
      document.binary ||
      document.code === document.saved ||
      switching ||
      deleteTarget ||
      status === 'failed' ||
      status === 'saving'
    )
      return
    const timer = setTimeout(() => {
      void persist(docRef.current)
    }, 1800)
    return () => clearTimeout(timer)
  }, [document, switching, persist, status, deleteTarget])
  useEffect(() => {
    const handle = (e: BeforeUnloadEvent) => {
      if (docRef.current.code !== docRef.current.saved && !docRef.current.binary) e.preventDefault()
    }
    window.addEventListener('beforeunload', handle)
    return () => {
      window.removeEventListener('beforeunload', handle)
      runAbort.current?.abort()
      if (poll.current) clearInterval(poll.current)
    }
  }, [])
  const openName = (kind: 'new' | 'save', code?: string, after?: () => void, suggested = 'untitled.nova') => {
    setFileName(suggested)
    setDialogError('')
    setNameDialog({ kind, code, after })
  }
  const save = async () => {
    if (document.binary) return
    revision.current++
    if (!docRef.current.name) {
      openName('save')
      return
    }
    if (await persist(docRef.current)) notify('File saved to your workspace')
  }
  const beforeSwitch = async (action: () => void | Promise<void>) => {
    if (transitionLock.current) return
    revision.current++
    const snapshot = docRef.current
    if (!snapshot.binary && snapshot.code !== snapshot.saved && !snapshot.name) {
      openName('save', undefined, () => {
        void beforeSwitch(action)
      })
      return
    }
    transitionLock.current = true
    setSwitching(true)
    try {
      await saveQueue.current.catch(() => {})
      if (!snapshot.binary && snapshot.code !== snapshot.saved && !(await persist(snapshot))) return
      await action()
    } finally {
      transitionLock.current = false
      setSwitching(false)
    }
  }
  const load = (name: string, force = false) => {
    if (!name || (name === docRef.current.name && !force)) return
    if (force && docRef.current.code !== docRef.current.saved) {
      notify('Save your local changes before reloading this file.', true)
      return
    }
    void beforeSwitch(async () => {
      if (isBinaryFile(name)) {
        updateDocument({ name, code: '', saved: '', binary: true })
        return
      }
      try {
        const res = await loadFile(token!, name)
        if (typeof res.data.content !== 'string') throw new Error('This file has no readable source content.')
        updateDocument({ name, code: res.data.content, saved: res.data.content, binary: false })
      } catch (error) {
        notify(`Could not open ${name}: ${errorMessage(error, 'Load failed')}`, true)
      }
    })
  }
  const newFile = (example?: (typeof EXAMPLES)[number]) =>
    void beforeSwitch(() => {
      openName('new', example?.code ?? '', undefined, example?.name)
    })
  const submitName = async (e: React.FormEvent) => {
    e.preventDefault()
    const name = fileName.trim()
    if (!validFileName(name) || isBinaryFile(name)) {
      setDialogError('Use a source filename with letters, numbers, dots, underscores or hyphens.')
      return
    }
    if (
      (files.some((f) => f.fileName === name) || name === docRef.current.name) &&
      (nameDialog?.kind === 'new' || name !== docRef.current.name)
    ) {
      setDialogError('A file with this name already exists. Choose another name.')
      return
    }
    setDialogBusy(true)
    const snapshot =
      nameDialog?.kind === 'new'
        ? { name, code: nameDialog.code ?? '', saved: '', binary: false }
        : docRef.current
    try {
      if (nameDialog?.kind === 'new') {
        await saveQueue.current.catch(() => {})
        await saveFile(token!, name, snapshot.code)
        updateDocument({ ...snapshot, saved: snapshot.code })
        await fetchFiles()
      } else if (!(await persist(snapshot, name))) {
        setDialogError('Save failed. Your draft is still available.')
        return
      }
      const after = nameDialog?.after
      setNameDialog(null)
      notify('File saved')
      after?.()
    } catch (error) {
      setDialogError(errorMessage(error, 'Could not create file'))
    } finally {
      setDialogBusy(false)
    }
  }
  const remove = async () => {
    setDialogBusy(true)
    try {
      await saveQueue.current.catch(() => {})
      await deleteFile(token!, deleteTarget)
      if (docRef.current.name === deleteTarget) {
        localStorage.removeItem('novacomp_last_file')
        updateDocument({ name: '', code: '', saved: '', binary: false })
      }
      setFiles((prev) => prev.filter((f) => f.fileName !== deleteTarget))
      setDeleteTarget('')
      notify('File deleted')
    } catch (error) {
      notify(errorMessage(error, 'Delete failed'), true)
    } finally {
      setDialogBusy(false)
    }
  }
  const run = async () => {
    if (loading || docRef.current.binary) return
    const controller = new AbortController()
    runAbort.current = controller
    setLoading(true)
    setRunning(true)
    setOutput({ mode: 'run', content: '' })
    let text = ''
    try {
      await runCodeStream(
        docRef.current.code,
        inputs,
        (type, chunk) => {
          if (controller.signal.aborted) return
          if (type === 'stdout' || type === 'stderr') {
            text += chunk
            setOutput((prev) => ({ ...prev, mode: prev.mode === 'error' ? 'error' : 'run', content: text }))
          } else if (type === 'error') {
            text += chunk
            setOutput({ mode: 'error', content: text })
          } else if (type === 'done') {
            setOutput((prev) => ({ ...prev, ms: Number(chunk) }))
            void fetchFiles()
          }
        },
        controller.signal,
        token ?? undefined,
      )
    } catch (error) {
      if (!controller.signal.aborted)
        setOutput({ mode: 'error', content: errorMessage(error, 'Execution failed') })
    } finally {
      if (runAbort.current === controller) {
        setLoading(false)
        setRunning(false)
        runAbort.current = null
      }
    }
  }
  const stop = () => {
    runAbort.current?.abort()
    notify('Execution stopped')
    setOutput((prev) => ({ ...prev, content: `${prev.content}\n[Execution stopped]` }))
  }
  const compare = async () => {
    if (loading || document.binary) return
    setLoading(true)
    setOutput({ mode: 'idle', content: '' })
    try {
      const res = await compareCode(document.code, inputs, token ?? undefined)
      setOutput(
        res.data.error
          ? { mode: 'error', content: res.data.error }
          : { mode: 'compare', content: '', compareData: res.data },
      )
    } catch (error) {
      setOutput({ mode: 'error', content: errorMessage(error, 'Comparison failed') })
    } finally {
      setLoading(false)
    }
  }
  const share = async () => {
    try {
      const res = await shareCode(document.code, token ?? undefined)
      const url = `${window.location.origin}/share/${res.data.id}`
      try {
        await navigator.clipboard.writeText(url)
        notify('Share link copied to clipboard')
      } catch {
        notify(`Your share link: ${url}`)
      }
    } catch (error) {
      notify(errorMessage(error, 'Could not share code'), true)
    }
  }
  const exportBytecode = async () => {
    try {
      const res = await compileBytecode(document.code, token ?? undefined)
      downloadBlob(new Blob([res.data]), `${document.name.replace(/\.[^.]+$/, '') || 'program'}.nbc`)
      notify('Bytecode downloaded')
    } catch (error) {
      notify(errorMessage(error, 'Bytecode export failed'), true)
    }
  }
  const runGpu = async () => {
    if (!token) return
    if (poll.current) clearInterval(poll.current)
    setGpuStatus('submitting')
    setGpuError('')
    setGpuOutput('')
    try {
      const res = await submitGpuJob(token, document.code)
      setGpuStatus(res.data.status ?? 'pending')
      setGpuScenario(res.data.scenario ?? '')
      setGpuBenefit(res.data.gpuBenefit ?? '')
      poll.current = setInterval(async () => {
        try {
          const response = await getGpuStatus(token, res.data.jobId)
          setGpuStatus(response.data.status)
          if (response.data.status === 'completed' || response.data.status === 'failed') {
            if (poll.current) clearInterval(poll.current)
            poll.current = null
            if (response.data.status === 'completed') {
              const result = await getGpuResults(token, res.data.jobId)
              setGpuOutput(result.data.output ?? '')
              void fetchFiles()
            } else setGpuError(response.data.error ?? 'GPU job failed')
          }
        } catch (error) {
          if (poll.current) clearInterval(poll.current)
          poll.current = null
          setGpuStatus('failed')
          setGpuError(errorMessage(error, 'Could not check job status'))
        }
      }, 5000)
    } catch (error) {
      setGpuStatus('failed')
      setGpuError(errorMessage(error, 'GPU submission failed'))
    }
  }
  const shortcut = useRef({ save, run })
  useEffect(() => {
    shortcut.current = { save, run }
  })
  useEffect(() => {
    const handle = (e: KeyboardEvent) => {
      if (!(e.ctrlKey || e.metaKey)) return
      if (e.key.toLowerCase() === 'k') {
        e.preventDefault()
        if (
          !window.document.querySelector('[role="dialog"]') ||
          window.document.querySelector('.command-modal')
        )
          setPalette((v) => !v)
      } else if (!window.document.querySelector('[role="dialog"]') && e.key.toLowerCase() === 's') {
        e.preventDefault()
        void shortcut.current.save()
      } else if (!window.document.querySelector('[role="dialog"]') && e.key === 'Enter') {
        e.preventDefault()
        void shortcut.current.run()
      }
    }
    window.addEventListener('keydown', handle, true)
    return () => window.removeEventListener('keydown', handle, true)
  }, [])
  const commands: Command[] = [
    {
      id: 'save',
      label: 'Save file',
      detail: '⌘ / Ctrl S',
      action: () => {
        void save()
      },
      disabled: document.binary,
    },
    {
      id: 'run',
      label: 'Run program',
      detail: '⌘ / Ctrl Enter',
      action: () => {
        void run()
      },
      disabled: loading || document.binary,
    },
    { id: 'new', label: 'Create a file', action: () => newFile() },
    {
      id: 'reload',
      label: 'Reload current file',
      detail: 'Fetch the latest saved source',
      action: () => load(document.name, true),
      disabled: !document.name || document.code !== document.saved,
    },
    {
      id: 'compare',
      label: 'Compare optimization',
      action: () => {
        void compare()
      },
      disabled: loading || document.binary,
    },
    {
      id: 'export',
      label: 'Download source code',
      action: () =>
        downloadBlob(new Blob([document.code], { type: 'text/plain' }), document.name || 'program.nova'),
      disabled: document.binary,
    },
    {
      id: 'bytecode',
      label: 'Export compiled bytecode (.nbc)',
      action: () => {
        void exportBytecode()
      },
      disabled: document.binary,
    },
    { id: 'settings', label: 'Editor settings', action: () => setSettingsOpen(true) },
    { id: 'files', label: 'Toggle file explorer', action: () => setFilesOpen((v) => !v) },
    { id: 'assistant', label: 'Toggle NovaBot', action: () => setAssistantOpen((v) => !v) },
    ...EXAMPLES.map((e) => ({
      id: `example-${e.name}`,
      label: `New example: ${e.title}`,
      detail: e.description,
      action: () => newFile(e),
    })),
    ...files.map((f) => ({
      id: `file-${f.fileName}`,
      label: `Open ${f.fileName}`,
      detail: 'Workspace file',
      action: () => load(f.fileName),
    })),
  ]
  const dirty = !document.binary && document.code !== document.saved
  return (
    <div className="ide-shell">
      <header className="workspace-header">
        <a className="brand" href="/" aria-label="NovaComp home">
          <span className="brand-symbol">
            <Code2 size={21} />
          </span>
          <span>
            Nova<span className="brand-light">Comp</span>
            <small>YOUR CODING WORKSPACE</small>
          </span>
        </a>
        <button
          className="command-trigger"
          aria-label="Open command palette"
          onClick={() => setPalette(true)}
        >
          <Search size={15} />
          <span>Search files & commands</span>
          <kbd>⌘ K</kbd>
        </button>
        <div className="header-actions">
          <button className="icon-button" onClick={() => setSettingsOpen(true)} aria-label="Editor settings">
            <Settings2 size={18} />
          </button>
          <span className="avatar" title={user?.email}>
            {user?.email?.slice(0, 1).toUpperCase() || 'N'}
          </span>
          <button
            className="icon-button"
            aria-label="Sign out"
            onClick={() =>
              void beforeSwitch(() => {
                logout()
                navigate('/login')
              })
            }
          >
            <LogOut size={18} />
          </button>
        </div>
      </header>
      <div className="workspace-toolbar">
        <div className="toolbar-group">
          <button
            className={`icon-button ${filesOpen ? 'active' : ''}`}
            aria-label="Toggle file explorer"
            aria-expanded={filesOpen}
            onClick={() => setFilesOpen((v) => !v)}
          >
            <PanelLeft size={18} />
          </button>
          <span className="breadcrumb">
            Workspace <ChevronRight size={13} />
            <strong>{document.name || 'Untitled draft'}</strong>
          </span>
        </div>
        <div className="toolbar-group">
          <button
            className="button"
            onClick={() => void save()}
            disabled={document.binary || switching || status === 'saving'}
          >
            <Save size={15} />
            <span>Save</span>
          </button>
          <button
            className="button"
            aria-label="Compare optimization"
            onClick={() => void compare()}
            disabled={loading || document.binary || switching}
          >
            <BarChart2 size={15} />
            <span>Compare</span>
          </button>
          <button
            className="button"
            aria-label="Share source code"
            onClick={() => void share()}
            disabled={document.binary || switching}
          >
            <Share2 size={15} />
            <span>Share</span>
          </button>
          <button
            className="button primary"
            onClick={() => void run()}
            disabled={loading || document.binary || switching}
          >
            <Play size={15} />
            <span>Run</span>
            <kbd>⌘ ↵</kbd>
          </button>
          {running && (
            <button className="button danger" onClick={stop}>
              <Square size={14} />
              Stop
            </button>
          )}
          <button
            className={`icon-button ${assistantOpen ? 'active' : ''}`}
            onClick={() => setAssistantOpen((v) => !v)}
            aria-label="Toggle NovaBot assistant"
            aria-expanded={assistantOpen}
          >
            <Sparkles size={18} />
          </button>
        </div>
      </div>
      <main className="workspace-body">
        {filesOpen && (
          <aside className="file-sidebar" aria-label="File explorer">
            <div className="section-heading">
              <span>EXPLORER</span>
              <button
                className="icon-button"
                onClick={() => newFile()}
                aria-label="Create new file"
                disabled={switching}
              >
                <Plus size={17} />
              </button>
            </div>
            <label className="file-search">
              <Search size={14} />
              <input
                value={query}
                onChange={(e) => setQuery(e.target.value)}
                placeholder="Find a file…"
                aria-label="Search files"
              />
            </label>
            <div className="workspace-label">
              <span className="status-dot" /> MY WORKSPACE <small>{files.length}</small>
            </div>
            <div className="file-list">
              {fileError && (
                <div className="sidebar-error">
                  <p>{fileError}</p>
                  <button className="button" onClick={() => void fetchFiles()}>
                    Retry
                  </button>
                </div>
              )}
              {files
                .filter((f) => f.fileName.toLowerCase().includes(query.toLowerCase()))
                .map((f) => (
                  <div
                    className={`file-row ${document.name === f.fileName ? 'selected' : ''}`}
                    key={f.fileName}
                  >
                    <button
                      className="file-open"
                      onClick={() => load(f.fileName)}
                      disabled={switching}
                      aria-current={document.name === f.fileName ? 'page' : undefined}
                    >
                      <FileCode size={16} />
                      <span>{f.fileName}</span>
                      {f.fileName === document.name && dirty && <span className="dirty-dot" />}
                    </button>
                    <button
                      className="file-delete icon-button"
                      aria-label={`Delete ${f.fileName}`}
                      onClick={() => {
                        setDeleteTarget(f.fileName)
                        setDialogError('')
                      }}
                      disabled={status === 'saving' || switching}
                    >
                      <Trash2 size={13} />
                    </button>
                  </div>
                ))}
              {!fileError &&
                !files.filter((f) => f.fileName.toLowerCase().includes(query.toLowerCase())).length && (
                  <p className="empty-state">
                    {query ? 'No files match your search.' : 'Save your first file to start your workspace.'}
                  </p>
                )}
            </div>
            <div className="starter-section">
              <div className="section-heading">QUICK START</div>
              {EXAMPLES.map((e) => (
                <button key={e.name} className="starter" onClick={() => newFile(e)} disabled={switching}>
                  <span className="starter-icon">
                    <Code2 size={16} />
                  </span>
                  <span>
                    <strong>{e.title}</strong>
                    <small>{e.description}</small>
                  </span>
                  <ChevronRight size={13} />
                </button>
              ))}
            </div>
            <div className="sidebar-footer">
              <span className="status-dot" /> Nova language{' '}
              <button
                className="icon-button"
                aria-label="Open command palette"
                onClick={() => setPalette(true)}
              >
                <CommandIcon size={14} />
              </button>
            </div>
          </aside>
        )}
        <section className="editor-workspace" aria-label="Code editor">
          <div className="editor-tabs">
            <div className="editor-tab">
              <FileCode size={15} />
              {document.name || 'Untitled.nova'}
              {dirty && <span className="dirty-dot" />}
            </div>
            <div className={`save-indicator ${status}`} role="status">
              {status === 'saving' ? (
                <Loader2 size={13} className="animate-spin" />
              ) : status === 'failed' ? (
                <AlertCircle size={13} />
              ) : status === 'saved' ? (
                <CheckCircle2 size={13} />
              ) : (
                <span className="dirty-dot" />
              )}
              {document.binary
                ? 'Binary artifact'
                : status === 'failed'
                  ? 'Save failed · retry Save'
                  : status === 'unsaved'
                    ? 'Unsaved changes'
                    : status === 'saving'
                      ? 'Saving…'
                      : !document.name
                        ? 'Starter example · Save to keep'
                        : 'All changes saved'}
            </div>
          </div>
          <div className="editor-surface">
            {document.binary ? (
              <div className="binary-state">
                <FileCode size={38} />
                <h2>Model artifact</h2>
                <p>{document.name} contains binary data and cannot be edited as source.</p>
                <button
                  className="button"
                  onClick={() => load(files.find((f) => !isBinaryFile(f.fileName))?.fileName ?? '')}
                  disabled={!files.some((f) => !isBinaryFile(f.fileName))}
                >
                  Open a source file
                </button>
              </div>
            ) : (
              <Suspense
                fallback={
                  <div className="editor-loading">
                    <Loader2 className="animate-spin" size={20} />
                    Preparing your editor…
                  </div>
                }
              >
                <MonacoEditor
                  height="100%"
                  language="nova"
                  theme="nova-dark"
                  value={document.code}
                  beforeMount={registerNovaLanguage}
                  onChange={(value) => {
                    if (switching || value === docRef.current.code) return
                    revision.current++
                    updateDocument({ ...docRef.current, code: value ?? '' })
                  }}
                  options={{
                    fontSize: settings.fontSize,
                    fontFamily: '"SFMono-Regular", Consolas, "Liberation Mono", monospace',
                    minimap: { enabled: settings.minimap },
                    scrollBeyondLastLine: false,
                    padding: { top: 22, bottom: 20 },
                    wordWrap: settings.wrap ? 'on' : 'off',
                    readOnly: switching,
                    automaticLayout: true,
                    smoothScrolling: true,
                    cursorSmoothCaretAnimation: 'on',
                    lineNumbersMinChars: 3,
                    bracketPairColorization: { enabled: true },
                  }}
                />
              </Suspense>
            )}
            {switching && (
              <div className="switching-overlay">
                <Loader2 className="animate-spin" size={18} />
                Opening file…
              </div>
            )}
          </div>
          <div className="execution-options">
            <button
              className={inputOpen ? 'text-button active' : 'text-button'}
              onClick={() => setInputOpen((v) => !v)}
              aria-expanded={inputOpen}
            >
              Standard input {inputs ? '•' : ''}
              <ChevronRight size={13} />
            </button>
            <div>
              <button
                className="text-button"
                onClick={() =>
                  downloadBlob(
                    new Blob([document.code], { type: 'text/plain' }),
                    document.name || 'program.nova',
                  )
                }
                disabled={document.binary}
              >
                <Download size={13} />
                Source
              </button>
              <button
                className="text-button"
                onClick={() => void exportBytecode()}
                disabled={document.binary}
              >
                .nbc
              </button>
              <button
                className="text-button gpu"
                onClick={() => void runGpu()}
                disabled={document.binary || ['submitting', 'pending', 'running'].includes(gpuStatus)}
              >
                <Zap size={13} />
                GPU
              </button>
            </div>
          </div>
          {inputOpen && (
            <div className="stdin-panel">
              <label htmlFor="stdin">Input values, one per line</label>
              <textarea
                id="stdin"
                value={inputs}
                onChange={(e) => setInputs(e.target.value)}
                placeholder={'3\n4'}
                rows={2}
              />
            </div>
          )}
          <BottomPanel
            output={output}
            loading={loading}
            gpuStatus={gpuStatus}
            gpuOutput={gpuOutput}
            gpuError={gpuError}
            gpuScenario={gpuScenario}
            gpuBenefit={gpuBenefit}
            onClear={() => setOutput({ mode: 'idle', content: '' })}
            onNotify={notify}
            onWsReady={(ws) => {
              termWs.current = ws
            }}
          />
        </section>
        {assistantOpen && (
          <aside className="assistant-sidebar" aria-label="NovaBot assistant">
            <div className="assistant-banner">
              <Sparkles size={16} />
              <span>Your coding companion</span>
              <button
                className="icon-button"
                onClick={() => setAssistantOpen(false)}
                aria-label="Close assistant"
              >
                <X size={15} />
              </button>
            </div>
            <NovaBot
              token={token!}
              currentCode={document.code}
              currentFile={document.name}
              recentOutput={output.content}
              onFileWrite={() => {
                void fetchFiles()
                notify(
                  'NovaBot updated a workspace file. Use Reload current file in Commands to review changes.',
                )
              }}
            />
          </aside>
        )}
      </main>
      <footer className="workspace-status">
        <span>
          <span className="status-dot" /> Nova workspace
        </span>
        <span>
          {document.code.split('\n').length} lines <span className="status-divider">|</span> UTF-8{' '}
          <span className="status-divider">|</span> Nova <span className="status-divider">|</span>
          <button onClick={() => setPalette(true)}>
            Keyboard shortcuts <kbd>⌘ K</kbd>
          </button>
        </span>
      </footer>
      {notice && (
        <div
          className={`notification ${notice.error ? 'error' : ''}`}
          role={notice.error ? 'alert' : 'status'}
        >
          {notice.error ? <AlertCircle size={18} /> : <CheckCircle2 size={18} />}
          <span>{notice.text}</span>
          <button className="icon-button" aria-label="Dismiss notification" onClick={() => setNotice(null)}>
            <X size={15} />
          </button>
        </div>
      )}
      {palette && <CommandPalette commands={commands} onClose={() => setPalette(false)} />}
      {settingsOpen && (
        <Modal title="Make yourself at home" onClose={() => setSettingsOpen(false)}>
          <p className="modal-description">Editor preferences are saved on this device.</p>
          <label className="setting-row" htmlFor="font-size">
            <span>
              Font size<small>Choose comfortable, readable code.</small>
            </span>
            <select
              id="font-size"
              value={settings.fontSize}
              onChange={(e) => setSettings((s) => ({ ...s, fontSize: Number(e.target.value) }))}
            >
              {[12, 14, 16, 18, 20, 22].map((n) => (
                <option key={n} value={n}>
                  {n}px
                </option>
              ))}
            </select>
          </label>
          <label className="setting-row">
            <span>
              Word wrap<small>Keep long lines inside the editor.</small>
            </span>
            <input
              type="checkbox"
              checked={settings.wrap}
              onChange={(e) => setSettings((s) => ({ ...s, wrap: e.target.checked }))}
            />
          </label>
          <label className="setting-row">
            <span>
              Minimap<small>See an overview of your source.</small>
            </span>
            <input
              type="checkbox"
              checked={settings.minimap}
              onChange={(e) => setSettings((s) => ({ ...s, minimap: e.target.checked }))}
            />
          </label>
          <div className="shortcut-list">
            <span>
              Save <kbd>⌘ / Ctrl S</kbd>
            </span>
            <span>
              Run <kbd>⌘ / Ctrl Enter</kbd>
            </span>
            <span>
              Commands <kbd>⌘ / Ctrl K</kbd>
            </span>
          </div>
        </Modal>
      )}
      {nameDialog && (
        <Modal
          title={nameDialog.kind === 'new' ? 'Create a new file' : 'Save your draft'}
          onClose={() => {
            if (!dialogBusy) setNameDialog(null)
          }}
        >
          <form onSubmit={submitName}>
            <p className="modal-description">
              {nameDialog.after
                ? 'Save this draft before opening another file or leaving your workspace.'
                : 'Give your source a name in your workspace.'}
            </p>
            <label className="form-label" htmlFor="filename">
              Filename
            </label>
            <input
              className="form-input"
              id="filename"
              value={fileName}
              onChange={(e) => setFileName(e.target.value)}
              autoFocus
              required
              maxLength={128}
            />
            {dialogError && (
              <p className="form-error" role="alert">
                {dialogError}
              </p>
            )}
            <div className="modal-actions">
              <button
                type="button"
                className="button"
                disabled={dialogBusy}
                onClick={() => setNameDialog(null)}
              >
                Cancel
              </button>
              <button type="submit" className="button primary" disabled={dialogBusy}>
                {dialogBusy && <Loader2 size={14} className="animate-spin" />}
                {nameDialog.kind === 'new' ? 'Create file' : 'Save file'}
              </button>
            </div>
          </form>
        </Modal>
      )}
      {deleteTarget && (
        <Modal
          title="Delete file?"
          onClose={() => {
            if (!dialogBusy) setDeleteTarget('')
          }}
        >
          <p className="modal-description">
            Remove <strong>{deleteTarget}</strong> from your workspace? This cannot be undone.
            {deleteTarget === document.name && dirty ? ' This file has unsaved changes.' : ''}
          </p>
          <div className="modal-actions">
            <button className="button" disabled={dialogBusy} onClick={() => setDeleteTarget('')}>
              Keep file
            </button>
            <button className="button danger" disabled={dialogBusy} onClick={() => void remove()}>
              {dialogBusy ? 'Deleting…' : 'Delete file'}
            </button>
          </div>
        </Modal>
      )}
    </div>
  )
}
