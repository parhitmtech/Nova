import { useEffect, useState } from 'react'

export interface EditorSettings {
  fontSize: number
  wrap: boolean
  minimap: boolean
}

const STORAGE_KEY = 'nova_editor_settings'

const initialSettings = (): EditorSettings => {
  try {
    const saved = JSON.parse(localStorage.getItem(STORAGE_KEY) ?? '{}')
    return {
      fontSize: Math.min(22, Math.max(12, Number(saved.fontSize) || 14)),
      wrap: saved.wrap !== false,
      minimap: saved.minimap === true,
    }
  } catch {
    return { fontSize: 14, wrap: true, minimap: false }
  }
}

// Editor preferences, persisted on this device.
export function useEditorSettings() {
  const [settings, setSettings] = useState(initialSettings)
  useEffect(() => {
    localStorage.setItem(STORAGE_KEY, JSON.stringify(settings))
  }, [settings])
  return [settings, setSettings] as const
}
