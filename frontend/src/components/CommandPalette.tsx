import { useState } from 'react'
import { Search, ArrowUp, ArrowDown, CornerDownLeft } from 'lucide-react'
import Modal from './Modal'
export interface Command {
  id: string
  label: string
  detail?: string
  action: () => void
  disabled?: boolean
}
export default function CommandPalette({ commands, onClose }: { commands: Command[]; onClose: () => void }) {
  const [query, setQuery] = useState('')
  const [selected, setSelected] = useState(0)
  const filtered = commands.filter(
    (c) => !c.disabled && `${c.label} ${c.detail ?? ''}`.toLowerCase().includes(query.toLowerCase()),
  )
  const execute = (command?: Command) => {
    if (command) {
      onClose()
      command.action()
    }
  }
  return (
    <Modal title="Command center" onClose={onClose} className="command-modal">
      <div className="command-search">
        <Search size={18} />
        <input
          autoFocus
          aria-label="Search commands and files"
          role="combobox"
          aria-expanded="true"
          aria-controls="command-list"
          aria-activedescendant={filtered[selected] ? `command-${filtered[selected].id}` : undefined}
          value={query}
          onChange={(e) => {
            setQuery(e.target.value)
            setSelected(0)
          }}
          placeholder="Search commands or open a file…"
          onKeyDown={(e) => {
            if (e.key === 'ArrowDown') {
              e.preventDefault()
              setSelected((i) => Math.min(i + 1, filtered.length - 1))
            }
            if (e.key === 'ArrowUp') {
              e.preventDefault()
              setSelected((i) => Math.max(i - 1, 0))
            }
            if (e.key === 'Enter') {
              e.preventDefault()
              execute(filtered[selected])
            }
          }}
        />
      </div>
      <div className="command-list" role="listbox" id="command-list">
        {filtered.map((c, i) => (
          <button
            role="option"
            aria-selected={i === selected}
            id={`command-${c.id}`}
            key={c.id}
            className={i === selected ? 'command selected' : 'command'}
            onMouseEnter={() => setSelected(i)}
            onClick={() => execute(c)}
          >
            <span>{c.label}</span>
            <small>{c.detail}</small>
          </button>
        ))}
        {!filtered.length && <p className="empty-state">No matching commands.</p>}
      </div>
      <div className="command-footer">
        <ArrowUp size={12} />
        <ArrowDown size={12} /> Navigate <CornerDownLeft size={12} /> Select <kbd>esc</kbd> Close
      </div>
    </Modal>
  )
}
