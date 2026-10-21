import { BarChart2, ChevronRight, PanelLeft, Play, Save, Share2, Sparkles, Square } from 'lucide-react'

interface Props {
  documentName: string
  binary: boolean
  switching: boolean
  saving: boolean
  loading: boolean
  running: boolean
  filesOpen: boolean
  assistantOpen: boolean
  onToggleFiles: () => void
  onToggleAssistant: () => void
  onSave: () => void
  onCompare: () => void
  onShare: () => void
  onRun: () => void
  onStop: () => void
}

export default function WorkspaceToolbar(p: Props) {
  return (
    <div className="workspace-toolbar">
      <div className="toolbar-group">
        <button
          className={`icon-button ${p.filesOpen ? 'active' : ''}`}
          aria-label="Toggle file explorer"
          aria-expanded={p.filesOpen}
          onClick={p.onToggleFiles}
        >
          <PanelLeft size={18} />
        </button>
        <span className="breadcrumb">
          Workspace <ChevronRight size={13} />
          <strong>{p.documentName || 'Untitled draft'}</strong>
        </span>
      </div>
      <div className="toolbar-group">
        <button className="button" onClick={p.onSave} disabled={p.binary || p.switching || p.saving}>
          <Save size={15} />
          <span>Save</span>
        </button>
        <button
          className="button"
          aria-label="Compare optimization"
          onClick={p.onCompare}
          disabled={p.loading || p.binary || p.switching}
        >
          <BarChart2 size={15} />
          <span>Compare</span>
        </button>
        <button
          className="button"
          aria-label="Share source code"
          onClick={p.onShare}
          disabled={p.binary || p.switching}
        >
          <Share2 size={15} />
          <span>Share</span>
        </button>
        <button className="button primary" onClick={p.onRun} disabled={p.loading || p.binary || p.switching}>
          <Play size={15} />
          <span>Run</span>
          <kbd>⌘ ↵</kbd>
        </button>
        {p.running && (
          <button className="button danger" onClick={p.onStop}>
            <Square size={14} />
            Stop
          </button>
        )}
        <button
          className={`icon-button ${p.assistantOpen ? 'active' : ''}`}
          onClick={p.onToggleAssistant}
          aria-label="Toggle NovaBot assistant"
          aria-expanded={p.assistantOpen}
        >
          <Sparkles size={18} />
        </button>
      </div>
    </div>
  )
}
