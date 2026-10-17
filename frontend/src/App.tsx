import { BrowserRouter, Routes, Route, Navigate } from 'react-router-dom'
import LoginPage from './pages/LoginPage'
import IDEPage from './pages/IDEPage'
import SharePage from './pages/SharePage'
import { useAuth } from './contexts/AuthContext'

function App() {
  const { token } = useAuth()

  // token presence is the single source of truth for auth state.
  // Logged-in users hitting /login are redirected to /ide and vice versa,
  // so the user is never stuck on the wrong page after login/logout.
  // /share/:id is public — no auth guard needed.
  return (
    <BrowserRouter>
      <Routes>
        <Route path="/login"     element={!token ? <LoginPage /> : <Navigate to="/ide" />} />
        <Route path="/ide"       element={token  ? <IDEPage />   : <Navigate to="/login" />} />
        <Route path="/share/:id" element={<SharePage />} />
        {/* Catch-all: send authenticated users to the IDE, guests to login */}
        <Route path="*"          element={<Navigate to={token ? '/ide' : '/login'} />} />
      </Routes>
    </BrowserRouter>
  )
}

export default App
