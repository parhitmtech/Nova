import type { Dispatch, SetStateAction } from 'react'
import Modal from '../Modal'
import type { EditorSettings } from '../../hooks/useEditorSettings'

interface Props {
  settings: EditorSettings
  onChange: Dispatch<SetStateAction<EditorSettings>>
  onClose: () => void
}

export default function SettingsDialog({ settings, onChange, onClose }: Props) {
  return (
    <Modal title="Make yourself at home" onClose={onClose}>
      <p className="modal-description">Editor preferences are saved on this device.</p>
      <label className="setting-row" htmlFor="font-size">
        <span>
          Font size<small>Choose comfortable, readable code.</small>
        </span>
        <select
          id="font-size"
          value={settings.fontSize}
          onChange={(e) => onChange((s) => ({ ...s, fontSize: Number(e.target.value) }))}
        >
          {[12, 14, 16, 18, 20, 22].map((n) => (
            <option key={n} value={n}>
              {n}px
            </option>
          ))}
        </select>
      </label>
      <label className="setting-row">
        <span>
          Word wrap<small>Keep long lines inside the editor.</small>
        </span>
        <input
          type="checkbox"
          checked={settings.wrap}
          onChange={(e) => onChange((s) => ({ ...s, wrap: e.target.checked }))}
        />
      </label>
      <label className="setting-row">
        <span>
          Minimap<small>See an overview of your source.</small>
        </span>
        <input
          type="checkbox"
          checked={settings.minimap}
          onChange={(e) => onChange((s) => ({ ...s, minimap: e.target.checked }))}
        />
      </label>
      <div className="shortcut-list">
        <span>
          Save <kbd>⌘ / Ctrl S</kbd>
        </span>
        <span>
          Run <kbd>⌘ / Ctrl Enter</kbd>
        </span>
        <span>
          Commands <kbd>⌘ / Ctrl K</kbd>
        </span>
      </div>
    </Modal>
  )
}
