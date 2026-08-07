import { useEffect } from 'react'
import { Route, Routes, useLocation } from 'react-router-dom'
import { NavBar } from './components/NavBar'
import { Footer } from './components/Footer'
import { Backdrop } from './components/Backdrop'
import { Landing } from './pages/Landing'
import { Docs } from './pages/Docs'
import { useTheme } from './hooks/useTheme'

/** Route changes start at the top — except in-page anchor jumps, owned by the article. */
function ScrollReset() {
  const { pathname, hash } = useLocation()
  useEffect(() => {
    if (!hash) window.scrollTo({ top: 0, behavior: 'instant' as ScrollBehavior })
  }, [pathname, hash])
  return null
}

export default function App() {
  const [theme, toggleTheme] = useTheme()

  return (
    <>
      <Backdrop />
      <ScrollReset />
      <NavBar theme={theme} onToggleTheme={toggleTheme} />
      <main id="top">
        <Routes>
          <Route path="/" element={<Landing />} />
          <Route path="/docs" element={<Docs />} />
          <Route path="/docs/:slug" element={<Docs />} />
          <Route path="*" element={<Landing />} />
        </Routes>
      </main>
      <Footer />
    </>
  )
}
