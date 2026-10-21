import { Code2, LogOut, Search, Settings2 } from 'lucide-react'

interface Props {
  email?: string
  onOpenPalette: () => void
  onOpenSettings: () => void
  onSignOut: () => void
}

export default function WorkspaceHeader({ email, onOpenPalette, onOpenSettings, onSignOut }: Props) {
  return (
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
      <button className="command-trigger" aria-label="Open command palette" onClick={onOpenPalette}>
        <Search size={15} />
        <span>Search files & commands</span>
        <kbd>⌘ K</kbd>
      </button>
      <div className="header-actions">
        <button className="icon-button" onClick={onOpenSettings} aria-label="Editor settings">
          <Settings2 size={18} />
        </button>
        <span className="avatar" title={email}>
          {email?.slice(0, 1).toUpperCase() || 'N'}
        </span>
        <button className="icon-button" aria-label="Sign out" onClick={onSignOut}>
          <LogOut size={18} />
        </button>
      </div>
    </header>
  )
}
