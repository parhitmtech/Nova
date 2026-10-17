import Editor, { loader } from '@monaco-editor/react'
import * as monaco from 'monaco-editor/esm/vs/editor/editor.api.js'
import EditorWorker from 'monaco-editor/esm/vs/editor/editor.worker?worker'

// Keep Monaco inside the lazy editor chunk and load its worker from this build.
// The workspace remains usable without requesting editor code from a CDN.
self.MonacoEnvironment = { getWorker: () => new EditorWorker() }
loader.config({ monaco })

export default Editor
