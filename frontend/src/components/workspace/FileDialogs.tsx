import { Loader2 } from 'lucide-react'
import Modal from '../Modal'
import type { Workspace } from '../../hooks/useWorkspace'

// "Create a new file" / "Save your draft" name prompt.
export function FileNameDialog({ workspace: w }: { workspace: Workspace }) {
  if (!w.nameDialog) return null
  const dialog = w.nameDialog
  return (
    <Modal
      title={dialog.kind === 'new' ? 'Create a new file' : 'Save your draft'}
      onClose={() => {
        if (!w.dialogBusy) w.closeNameDialog()
      }}
    >
      <form onSubmit={w.submitName}>
        <p className="modal-description">
          {dialog.after
            ? 'Save this draft before opening another file or leaving your workspace.'
            : 'Give your source a name in your workspace.'}
        </p>
        <label className="form-label" htmlFor="filename">
          Filename
        </label>
        <input
          className="form-input"
          id="filename"
          value={w.fileName}
          onChange={(e) => w.setFileName(e.target.value)}
          autoFocus
          required
          maxLength={128}
        />
        {w.dialogError && (
          <p className="form-error" role="alert">
            {w.dialogError}
          </p>
        )}
        <div className="modal-actions">
          <button type="button" className="button" disabled={w.dialogBusy} onClick={w.closeNameDialog}>
            Cancel
          </button>
          <button type="submit" className="button primary" disabled={w.dialogBusy}>
            {w.dialogBusy && <Loader2 size={14} className="animate-spin" />}
            {dialog.kind === 'new' ? 'Create file' : 'Save file'}
          </button>
        </div>
      </form>
    </Modal>
  )
}

// Delete confirmation for the file in `workspace.deleteTarget`.
export function DeleteFileDialog({ workspace: w }: { workspace: Workspace }) {
  if (!w.deleteTarget) return null
  return (
    <Modal
      title="Delete file?"
      onClose={() => {
        if (!w.dialogBusy) w.cancelDelete()
      }}
    >
      <p className="modal-description">
        Remove <strong>{w.deleteTarget}</strong> from your workspace? This cannot be undone.
        {w.deleteTarget === w.document.name && w.dirty ? ' This file has unsaved changes.' : ''}
      </p>
      <div className="modal-actions">
        <button className="button" disabled={w.dialogBusy} onClick={w.cancelDelete}>
          Keep file
        </button>
        <button className="button danger" disabled={w.dialogBusy} onClick={() => void w.remove()}>
          {w.dialogBusy ? 'Deleting…' : 'Delete file'}
        </button>
      </div>
    </Modal>
  )
}
