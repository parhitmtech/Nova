import { AlertCircle, CheckCircle2, X } from 'lucide-react'

export interface Notice {
  text: string
  error?: boolean
}

interface Props {
  notice: Notice
  onDismiss: () => void
}

export default function Notification({ notice, onDismiss }: Props) {
  return (
    <div className={`notification ${notice.error ? 'error' : ''}`} role={notice.error ? 'alert' : 'status'}>
      {notice.error ? <AlertCircle size={18} /> : <CheckCircle2 size={18} />}
      <span>{notice.text}</span>
      <button className="icon-button" aria-label="Dismiss notification" onClick={onDismiss}>
        <X size={15} />
      </button>
    </div>
  )
}
