import { useCallback, useEffect, useRef, useState, type FormEvent } from 'react'
import { listFiles, saveFile, loadFile, deleteFile } from '../api/files'
import { EXAMPLES, isBinaryFile, validFileName, errorMessage } from '../lib/workspace'

export interface FileEntry {
  fileName: string
  updatedAt: string
}

export interface DocumentState {
  name: string
  code: string
  saved: string
  binary: boolean
}

export type SaveStatus = 'saved' | 'unsaved' | 'saving' | 'failed'

export interface NameDialogState {
  kind: 'save' | 'new'
  code?: string
  after?: () => void
}

const LAST_FILE_KEY = 'novacomp_last_file'
const FORK_KEY = 'novacomp_fork'

const initialDocument = (): DocumentState => {
  const fork = sessionStorage.getItem(FORK_KEY)
  const starter = EXAMPLES[0]?.code ?? ''
  return { name: '', code: fork ?? starter, saved: fork ? '' : starter, binary: false }
}

// The open document and the user's files: loading, saving (serialized through one queue),
// autosave, restoring the last file, switching files without losing drafts, and the
// name/delete dialogs. `termWs` is the terminal socket, told about saved files.
export function useWorkspace(
  token: string | null,
  notify: (text: string, error?: boolean) => void,
  termWs: { current: WebSocket | null },
) {
  const [files, setFiles] = useState<FileEntry[]>([])
  const [document, setDocument] = useState<DocumentState>(initialDocument)
  const docRef = useRef(document)
  const [status, setStatus] = useState<SaveStatus>(document.code === document.saved ? 'saved' : 'unsaved')
  const forkOnEntry = useRef(sessionStorage.getItem(FORK_KEY) !== null)
  const [switching, setSwitching] = useState(false)
  const [fileError, setFileError] = useState('')
  const [nameDialog, setNameDialog] = useState<NameDialogState | null>(null)
  const [fileName, setFileName] = useState('untitled.nova')
  const [dialogError, setDialogError] = useState('')
  const [dialogBusy, setDialogBusy] = useState(false)
  const [deleteTarget, setDeleteTarget] = useState('')
  const saveQueue = useRef<Promise<unknown>>(Promise.resolve())
  const restored = useRef(false)
  const revision = useRef(0)
  const transitionLock = useRef(false)

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
    [token, updateDocument, notify, termWs],
  )

  // Load the file list, then reopen the last file (unless the page was opened from a share link).
  useEffect(() => {
    const hasFork = forkOnEntry.current
    sessionStorage.removeItem(FORK_KEY)
    let active = true
    const startRevision = revision.current
    Promise.resolve()
      .then(fetchFiles)
      .then(async (entries) => {
        if (!active || restored.current) return
        restored.current = true
        const name = localStorage.getItem(LAST_FILE_KEY)
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
    if (document.name) localStorage.setItem(LAST_FILE_KEY, document.name)
  }, [document.name])

  // Autosave named files shortly after the last edit.
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

  // Warn before leaving the page with unsaved changes.
  useEffect(() => {
    const handle = (e: BeforeUnloadEvent) => {
      if (docRef.current.code !== docRef.current.saved && !docRef.current.binary) e.preventDefault()
    }
    window.addEventListener('beforeunload', handle)
    return () => window.removeEventListener('beforeunload', handle)
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

  // Run `action` after making sure the current draft is saved (asking for a name if it has none).
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

  const submitName = async (e: FormEvent) => {
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
        localStorage.removeItem(LAST_FILE_KEY)
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

  // An edit from the editor (ignored while a file switch is in progress).
  const edit = (value: string | undefined) => {
    if (switching || value === docRef.current.code) return
    revision.current++
    updateDocument({ ...docRef.current, code: value ?? '' })
  }

  const askDelete = (name: string) => {
    setDeleteTarget(name)
    setDialogError('')
  }

  return {
    files,
    document,
    docRef,
    status,
    switching,
    fileError,
    dirty: !document.binary && document.code !== document.saved,
    fetchFiles,
    save,
    beforeSwitch,
    load,
    newFile,
    edit,
    nameDialog,
    closeNameDialog: () => setNameDialog(null),
    fileName,
    setFileName,
    dialogError,
    dialogBusy,
    submitName,
    deleteTarget,
    askDelete,
    cancelDelete: () => setDeleteTarget(''),
    remove,
  }
}

export type Workspace = ReturnType<typeof useWorkspace>
