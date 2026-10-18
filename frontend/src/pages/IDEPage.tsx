import { useCallback, useRef, useState } from 'react'
import { useNavigate } from 'react-router-dom'
import { Sparkles, X } from 'lucide-react'
import { compileBytecode } from '../api/compiler'
import { shareCode } from '../api/share'
import { useAuth } from '../contexts/AuthContext'
import { EXAMPLES, errorMessage, downloadBlob } from '../lib/workspace'
import { useWorkspace } from '../hooks/useWorkspace'
import { useRunner } from '../hooks/useRunner'
import { useGpuJob } from '../hooks/useGpuJob'
import { useEditorSettings } from '../hooks/useEditorSettings'
import { useWorkspaceShortcuts } from '../hooks/useWorkspaceShortcuts'
import BottomPanel from '../components/BottomPanel'
import NovaBot from '../components/NovaBot'
import CommandPalette, { type Command } from '../components/CommandPalette'
import WorkspaceHeader from '../components/workspace/WorkspaceHeader'
import WorkspaceToolbar from '../components/workspace/WorkspaceToolbar'
import FileSidebar from '../components/workspace/FileSidebar'
import EditorPanel from '../components/workspace/EditorPanel'
import StatusBar from '../components/workspace/StatusBar'
import Notification, { type Notice } from '../components/workspace/Notification'
import SettingsDialog from '../components/workspace/SettingsDialog'
import { FileNameDialog, DeleteFileDialog } from '../components/workspace/FileDialogs'

// The IDE: wires the workspace (files + open document), runner, GPU jobs and settings to the layout.
export default function IDEPage() {
  const { token, user, logout } = useAuth()
  const navigate = useNavigate()
  const [notice, setNotice] = useState<Notice | null>(null)
  const notify = useCallback((text: string, error = false) => setNotice({ text, error }), [])
  const termWs = useRef<WebSocket | null>(null)
  const workspace = useWorkspace(token, notify, termWs)
  const { document, fetchFiles } = workspace
  const [inputs, setInputs] = useState('')
  const runner = useRunner({
    token,
    inputs,
    document,
    docRef: workspace.docRef,
    onFinished: () => void fetchFiles(),
    notify,
  })
  const gpu = useGpuJob(token, () => void fetchFiles())
  const [settings, setSettings] = useEditorSettings()
  const [filesOpen, setFilesOpen] = useState(() => window.innerWidth > 760)
  const [assistantOpen, setAssistantOpen] = useState(() => window.innerWidth > 1200)
  const [palette, setPalette] = useState(false)
  const [settingsOpen, setSettingsOpen] = useState(false)

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

  const signOut = () =>
    void workspace.beforeSwitch(() => {
      logout()
      navigate('/login')
    })

  useWorkspaceShortcuts({
    save: workspace.save,
    run: runner.run,
    togglePalette: () => setPalette((v) => !v),
  })

  const commands: Command[] = [
    {
      id: 'save',
      label: 'Save file',
      detail: '⌘ / Ctrl S',
      action: () => {
        void workspace.save()
      },
      disabled: document.binary,
    },
    {
      id: 'run',
      label: 'Run program',
      detail: '⌘ / Ctrl Enter',
      action: () => {
        void runner.run()
      },
      disabled: runner.loading || document.binary,
    },
    { id: 'new', label: 'Create a file', action: () => workspace.newFile() },
    {
      id: 'reload',
      label: 'Reload current file',
      detail: 'Fetch the latest saved source',
      action: () => workspace.load(document.name, true),
      disabled: !document.name || document.code !== document.saved,
    },
    {
      id: 'compare',
      label: 'Compare optimization',
      action: () => {
        void runner.compare()
      },
      disabled: runner.loading || document.binary,
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
      action: () => workspace.newFile(e),
    })),
    ...workspace.files.map((f) => ({
      id: `file-${f.fileName}`,
      label: `Open ${f.fileName}`,
      detail: 'Workspace file',
      action: () => workspace.load(f.fileName),
    })),
  ]

  return (
    <div className="ide-shell">
      <WorkspaceHeader
        email={user?.email}
        onOpenPalette={() => setPalette(true)}
        onOpenSettings={() => setSettingsOpen(true)}
        onSignOut={signOut}
      />
      <WorkspaceToolbar
        documentName={document.name}
        binary={document.binary}
        switching={workspace.switching}
        saving={workspace.status === 'saving'}
        loading={runner.loading}
        running={runner.running}
        filesOpen={filesOpen}
        assistantOpen={assistantOpen}
        onToggleFiles={() => setFilesOpen((v) => !v)}
        onToggleAssistant={() => setAssistantOpen((v) => !v)}
        onSave={() => void workspace.save()}
        onCompare={() => void runner.compare()}
        onShare={() => void share()}
        onRun={() => void runner.run()}
        onStop={runner.stop}
      />
      <main className="workspace-body">
        {filesOpen && <FileSidebar workspace={workspace} onOpenPalette={() => setPalette(true)} />}
        <EditorPanel
          workspace={workspace}
          settings={settings}
          inputs={inputs}
          onInputsChange={setInputs}
          onExportBytecode={() => void exportBytecode()}
          onRunGpu={() => void gpu.submit(document.code)}
          gpuBusy={gpu.busy}
        >
          <BottomPanel
            output={runner.output}
            loading={runner.loading}
            gpuStatus={gpu.status}
            gpuOutput={gpu.output}
            gpuError={gpu.error}
            gpuScenario={gpu.scenario}
            gpuBenefit={gpu.benefit}
            onClear={runner.clearOutput}
            onNotify={notify}
            onWsReady={(ws) => {
              termWs.current = ws
            }}
          />
        </EditorPanel>
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
              recentOutput={runner.output.content}
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
      <StatusBar lineCount={document.code.split('\n').length} onOpenPalette={() => setPalette(true)} />
      {notice && <Notification notice={notice} onDismiss={() => setNotice(null)} />}
      {palette && <CommandPalette commands={commands} onClose={() => setPalette(false)} />}
      {settingsOpen && (
        <SettingsDialog settings={settings} onChange={setSettings} onClose={() => setSettingsOpen(false)} />
      )}
      <FileNameDialog workspace={workspace} />
      <DeleteFileDialog workspace={workspace} />
    </div>
  )
}
