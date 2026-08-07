import { useCallback, useEffect, useState } from 'react'
import { Link, useLocation, useNavigate } from 'react-router-dom'
import { ScrollProgress } from './ScrollProgress'
import { REPO_URL } from '../data/content'
import type { Theme } from '../hooks/useTheme'

const SECTIONS = [
  ['features', 'Features'],
  ['proof', 'Proof'],
  ['performance', 'Performance'],
  ['compare', 'Compare'],
  ['start', 'Get started'],
] as const

export function NavBar({ theme, onToggleTheme }: { theme: Theme; onToggleTheme: () => void }) {
  const [scrolled, setScrolled] = useState(false)
  const [menuOpen, setMenuOpen] = useState(false)
  const { pathname } = useLocation()
  const navigate = useNavigate()
  const onDocs = pathname.startsWith('/docs')

  useEffect(() => {
    const onScroll = (): void => setScrolled(window.scrollY > 12)
    onScroll()
    window.addEventListener('scroll', onScroll, { passive: true })
    return () => window.removeEventListener('scroll', onScroll)
  }, [])

  useEffect(() => setMenuOpen(false), [pathname])

  /** Anchor links must survive being clicked from the docs route. */
  const goToSection = useCallback(
    (id: string): void => {
      const scroll = (): void => document.getElementById(id)?.scrollIntoView({ behavior: 'smooth', block: 'start' })
      if (onDocs) {
        navigate('/')
        requestAnimationFrame(() => requestAnimationFrame(scroll))
      } else {
        scroll()
      }
      setMenuOpen(false)
    },
    [onDocs, navigate]
  )

  return (
    <header className={`nav ${scrolled ? 'nav-scrolled' : ''}`.trim()}>
      <div className="nav-inner wrap">
        <Link to="/" className="brand" aria-label="Quark home">
          <span className="brand-mark" aria-hidden="true">
            <span className="brand-core" />
            <span className="brand-orbit" />
          </span>
          <span className="brand-text">
            Quark<span className="brand-suffix">C++</span>
          </span>
        </Link>

        <nav className={`nav-links ${menuOpen ? 'open' : ''}`.trim()}>
          {SECTIONS.map(([id, label]) => (
            <button key={id} className="nav-link" onClick={() => goToSection(id)}>
              {label}
            </button>
          ))}
          <Link to="/docs" className={`nav-link ${onDocs ? 'active' : ''}`.trim()}>
            Wiki &amp; docs
          </Link>
        </nav>

        <div className="nav-actions">
          <button className="icon-btn" onClick={onToggleTheme} aria-label={`Switch to ${theme === 'dark' ? 'light' : 'dark'} theme`}>
            {theme === 'dark' ? <SunIcon /> : <MoonIcon />}
          </button>
          <a className="btn btn-ghost nav-gh" href={REPO_URL} target="_blank" rel="noreferrer noopener">
            <GitHubIcon />
            <span>GitHub</span>
          </a>
          <button
            className={`burger ${menuOpen ? 'open' : ''}`.trim()}
            onClick={() => setMenuOpen((v) => !v)}
            aria-label="Toggle navigation"
            aria-expanded={menuOpen}
          >
            <span />
            <span />
          </button>
        </div>
      </div>
      <ScrollProgress />
    </header>
  )
}

function SunIcon() {
  return (
    <svg viewBox="0 0 24 24" width="17" height="17" fill="none" stroke="currentColor" strokeWidth="1.8" strokeLinecap="round">
      <circle cx="12" cy="12" r="4.2" />
      <path d="M12 2.5v2M12 19.5v2M2.5 12h2M19.5 12h2M5.2 5.2l1.4 1.4M17.4 17.4l1.4 1.4M18.8 5.2l-1.4 1.4M6.6 17.4l-1.4 1.4" />
    </svg>
  )
}

function MoonIcon() {
  return (
    <svg viewBox="0 0 24 24" width="17" height="17" fill="none" stroke="currentColor" strokeWidth="1.8" strokeLinejoin="round">
      <path d="M20 14.2A8.2 8.2 0 0 1 9.8 4a8.4 8.4 0 1 0 10.2 10.2Z" />
    </svg>
  )
}

function GitHubIcon() {
  return (
    <svg viewBox="0 0 16 16" width="16" height="16" fill="currentColor" aria-hidden="true">
      <path d="M8 0C3.58 0 0 3.58 0 8c0 3.54 2.29 6.53 5.47 7.59.4.07.55-.17.55-.38 0-.19-.01-.82-.01-1.49-2.01.37-2.53-.49-2.69-.94-.09-.23-.48-.94-.82-1.13-.28-.15-.68-.52-.01-.53.63-.01 1.08.58 1.23.82.72 1.21 1.87.87 2.33.66.07-.52.28-.87.51-1.07-1.78-.2-3.64-.89-3.64-3.95 0-.87.31-1.59.82-2.15-.08-.2-.36-1.02.08-2.12 0 0 .67-.21 2.2.82a7.4 7.4 0 0 1 2-.27c.68 0 1.36.09 2 .27 1.53-1.04 2.2-.82 2.2-.82.44 1.1.16 1.92.08 2.12.51.56.82 1.27.82 2.15 0 3.07-1.87 3.75-3.65 3.95.29.25.54.73.54 1.48 0 1.07-.01 1.93-.01 2.2 0 .21.15.46.55.38A8.01 8.01 0 0 0 16 8c0-4.42-3.58-8-8-8Z" />
    </svg>
  )
}
