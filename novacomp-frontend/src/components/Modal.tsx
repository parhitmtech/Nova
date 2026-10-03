import { useEffect, useRef, type ReactNode } from 'react'
import { X } from 'lucide-react'

export default function Modal({ title, onClose, children, className = '' }: { title: string; onClose: () => void; children: ReactNode; className?: string }) {
  const ref = useRef<HTMLDivElement>(null)
  const closeRef = useRef(onClose)
  useEffect(() => { closeRef.current = onClose }, [onClose])
  useEffect(() => {
    const previous = document.activeElement as HTMLElement | null
    const dialog = ref.current!
    const focusable = () => Array.from(dialog.querySelectorAll<HTMLElement>('button:not(:disabled), input, textarea, select, [tabindex="0"]'))
    const initialFocus = dialog.querySelector<HTMLElement>('input, textarea, select') ?? focusable()[0]
    initialFocus?.focus()
    const handle = (e: KeyboardEvent) => {
      if (e.key === 'Escape') { e.preventDefault(); closeRef.current() }
      if (e.key === 'Tab') {
        const items = focusable(); const first = items[0]; const last = items[items.length - 1]
        if (e.shiftKey && document.activeElement === first) { e.preventDefault(); last?.focus() }
        else if (!e.shiftKey && document.activeElement === last) { e.preventDefault(); first?.focus() }
      }
    }
    dialog.addEventListener('keydown', handle)
    return () => { dialog.removeEventListener('keydown', handle); previous?.focus() }
  }, [])
  return <div className="modal-backdrop" onMouseDown={e => { if (e.target === e.currentTarget) onClose() }}><div ref={ref} role="dialog" aria-modal="true" aria-label={title} className={`modal ${className}`}><div className="modal-heading"><h2>{title}</h2><button className="icon-button" aria-label="Close dialog" onClick={onClose}><X size={18} /></button></div>{children}</div></div>
}
