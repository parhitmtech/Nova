import { BrowserRouter, Routes, Route, Navigate } from 'react-router-dom'
import LoginPage from './pages/LoginPage'
import IDEPage from './pages/IDEPage'
import SharePage from './pages/SharePage'
import { useAuth } from './contexts/AuthContext'

function App() {
  const { token } = useAuth()

  return (
    <BrowserRouter>
      <Routes>
        <Route path="/login"     element={!token ? <LoginPage /> : <Navigate to="/ide" />} />
        <Route path="/ide"       element={token  ? <IDEPage />   : <Navigate to="/login" />} />
        <Route path="/share/:id" element={<SharePage />} />
        <Route path="*"          element={<Navigate to={token ? '/ide' : '/login'} />} />
      </Routes>
    </BrowserRouter>
  )
}

export default App
