import { lazy, Suspense, useState, type ReactNode } from 'react'
import { AlertCircle, CheckCircle2, ChevronRight, Download, FileCode, Loader2, Zap } from 'lucide-react'
import { registerNovaLanguage } from '../../lib/novaLanguage'
import { isBinaryFile, downloadBlob } from '../../lib/workspace'
import type { Workspace } from '../../hooks/useWorkspace'
import type { EditorSettings } from '../../hooks/useEditorSettings'

const MonacoEditor = lazy(() => import('../NovaEditor'))

interface Props {
  workspace: Workspace
  settings: EditorSettings
  inputs: string
  onInputsChange: (value: string) => void
  onExportBytecode: () => void
  onRunGpu: () => void
  gpuBusy: boolean
  children: ReactNode // the output panel below the editor
}

const statusLabel = (w: Workspace) =>
  w.document.binary
    ? 'Binary artifact'
    : w.status === 'failed'
      ? 'Save failed · retry Save'
      : w.status === 'unsaved'
        ? 'Unsaved changes'
        : w.status === 'saving'
          ? 'Saving…'
          : !w.document.name
            ? 'Starter example · Save to keep'
            : 'All changes saved'

export default function EditorPanel({
  workspace: w,
  settings,
  inputs,
  onInputsChange,
  onExportBytecode,
  onRunGpu,
  gpuBusy,
  children,
}: Props) {
  const [inputOpen, setInputOpen] = useState(false)
  const { document } = w
  return (
    <section className="editor-workspace" aria-label="Code editor">
      <div className="editor-tabs">
        <div className="editor-tab">
          <FileCode size={15} />
          {document.name || 'Untitled.nova'}
          {w.dirty && <span className="dirty-dot" />}
        </div>
        <div className={`save-indicator ${w.status}`} role="status">
          {w.status === 'saving' ? (
            <Loader2 size={13} className="animate-spin" />
          ) : w.status === 'failed' ? (
            <AlertCircle size={13} />
          ) : w.status === 'saved' ? (
            <CheckCircle2 size={13} />
          ) : (
            <span className="dirty-dot" />
          )}
          {statusLabel(w)}
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
              onClick={() => w.load(w.files.find((f) => !isBinaryFile(f.fileName))?.fileName ?? '')}
              disabled={!w.files.some((f) => !isBinaryFile(f.fileName))}
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
              onChange={w.edit}
              options={{
                fontSize: settings.fontSize,
                fontFamily: '"SFMono-Regular", Consolas, "Liberation Mono", monospace',
                minimap: { enabled: settings.minimap },
                scrollBeyondLastLine: false,
                padding: { top: 22, bottom: 20 },
                wordWrap: settings.wrap ? 'on' : 'off',
                readOnly: w.switching,
                automaticLayout: true,
                smoothScrolling: true,
                cursorSmoothCaretAnimation: 'on',
                lineNumbersMinChars: 3,
                bracketPairColorization: { enabled: true },
              }}
            />
          </Suspense>
        )}
        {w.switching && (
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
              downloadBlob(new Blob([document.code], { type: 'text/plain' }), document.name || 'program.nova')
            }
            disabled={document.binary}
          >
            <Download size={13} />
            Source
          </button>
          <button className="text-button" onClick={onExportBytecode} disabled={document.binary}>
            .nbc
          </button>
          <button className="text-button gpu" onClick={onRunGpu} disabled={document.binary || gpuBusy}>
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
            onChange={(e) => onInputsChange(e.target.value)}
            placeholder={'3\n4'}
            rows={2}
          />
        </div>
      )}
      {children}
    </section>
  )
}
