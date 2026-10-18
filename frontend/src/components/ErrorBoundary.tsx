import { Component, type ErrorInfo, type ReactNode } from 'react'

interface Props {
  children: ReactNode
}

interface State {
  error: Error | null
}

// Catches render errors (and failed lazy chunk loads) anywhere below it, so one broken component
// shows a recovery screen instead of blanking the whole app.
export default class ErrorBoundary extends Component<Props, State> {
  state: State = { error: null }

  static getDerivedStateFromError(error: Error): State {
    return { error }
  }

  componentDidCatch(error: Error, info: ErrorInfo) {
    console.error('Unhandled UI error', error, info.componentStack)
  }

  render() {
    if (!this.state.error) return this.props.children
    return (
      <main className="app-error" role="alert">
        <h1>Something went wrong</h1>
        <p>The workspace hit an unexpected error. Reloading usually fixes it.</p>
        <pre>{this.state.error.message}</pre>
        <button type="button" className="button primary" onClick={() => window.location.reload()}>
          Reload
        </button>
      </main>
    )
  }
}
