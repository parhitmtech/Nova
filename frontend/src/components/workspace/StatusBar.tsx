interface Props {
  lineCount: number
  onOpenPalette: () => void
}

export default function StatusBar({ lineCount, onOpenPalette }: Props) {
  return (
    <footer className="workspace-status">
      <span>
        <span className="status-dot" /> Nova workspace
      </span>
      <span>
        {lineCount} lines <span className="status-divider">|</span> UTF-8{' '}
        <span className="status-divider">|</span> Nova <span className="status-divider">|</span>
        <button onClick={onOpenPalette}>
          Keyboard shortcuts <kbd>⌘ K</kbd>
        </button>
      </span>
    </footer>
  )
}
