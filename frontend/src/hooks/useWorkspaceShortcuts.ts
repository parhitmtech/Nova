import { useEffect, useRef } from 'react'

// Global shortcuts: Ctrl/Cmd+K toggles the command palette (also while it is open),
// Ctrl/Cmd+S saves and Ctrl/Cmd+Enter runs (both suppressed while any dialog is open).
// Handlers are read through a ref so the listener is registered once.
export function useWorkspaceShortcuts(handlers: {
  save: () => unknown
  run: () => unknown
  togglePalette: () => void
}) {
  const current = useRef(handlers)
  useEffect(() => {
    current.current = handlers
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
          current.current.togglePalette()
      } else if (!window.document.querySelector('[role="dialog"]') && e.key.toLowerCase() === 's') {
        e.preventDefault()
        void current.current.save()
      } else if (!window.document.querySelector('[role="dialog"]') && e.key === 'Enter') {
        e.preventDefault()
        void current.current.run()
      }
    }
    window.addEventListener('keydown', handle, true)
    return () => window.removeEventListener('keydown', handle, true)
  }, [])
}
