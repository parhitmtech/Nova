import { useState } from 'react'
import { ChevronRight, Code2, Command as CommandIcon, FileCode, Plus, Search, Trash2 } from 'lucide-react'
import { EXAMPLES } from '../../lib/workspace'
import type { Workspace } from '../../hooks/useWorkspace'

interface Props {
  workspace: Workspace
  onOpenPalette: () => void
}

// File explorer: search, open, delete, and starter examples.
export default function FileSidebar({ workspace: w, onOpenPalette }: Props) {
  const [query, setQuery] = useState('')
  const matches = w.files.filter((f) => f.fileName.toLowerCase().includes(query.toLowerCase()))
  return (
    <aside className="file-sidebar" aria-label="File explorer">
      <div className="section-heading">
        <span>EXPLORER</span>
        <button
          className="icon-button"
          onClick={() => w.newFile()}
          aria-label="Create new file"
          disabled={w.switching}
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
        <span className="status-dot" /> MY WORKSPACE <small>{w.files.length}</small>
      </div>
      <div className="file-list">
        {w.fileError && (
          <div className="sidebar-error">
            <p>{w.fileError}</p>
            <button className="button" onClick={() => void w.fetchFiles()}>
              Retry
            </button>
          </div>
        )}
        {matches.map((f) => (
          <div className={`file-row ${w.document.name === f.fileName ? 'selected' : ''}`} key={f.fileName}>
            <button
              className="file-open"
              onClick={() => w.load(f.fileName)}
              disabled={w.switching}
              aria-current={w.document.name === f.fileName ? 'page' : undefined}
            >
              <FileCode size={16} />
              <span>{f.fileName}</span>
              {f.fileName === w.document.name && w.dirty && <span className="dirty-dot" />}
            </button>
            <button
              className="file-delete icon-button"
              aria-label={`Delete ${f.fileName}`}
              onClick={() => w.askDelete(f.fileName)}
              disabled={w.status === 'saving' || w.switching}
            >
              <Trash2 size={13} />
            </button>
          </div>
        ))}
        {!w.fileError && !matches.length && (
          <p className="empty-state">
            {query ? 'No files match your search.' : 'Save your first file to start your workspace.'}
          </p>
        )}
      </div>
      <div className="starter-section">
        <div className="section-heading">QUICK START</div>
        {EXAMPLES.map((e) => (
          <button key={e.name} className="starter" onClick={() => w.newFile(e)} disabled={w.switching}>
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
        <button className="icon-button" aria-label="Open command palette" onClick={onOpenPalette}>
          <CommandIcon size={14} />
        </button>
      </div>
    </aside>
  )
}
