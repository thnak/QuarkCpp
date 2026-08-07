import { useEffect, useMemo, useRef, useState } from 'react'
import { useNavigate } from 'react-router-dom'
import type { DocIndexEntry } from '../../types'

interface Hit extends DocIndexEntry {
  score: number
}

function rank(pages: DocIndexEntry[], q: string): Hit[] {
  const needle = q.trim().toLowerCase()
  if (needle.length < 2) return []

  const hits: Hit[] = []
  for (const p of pages) {
    const title = p.title.toLowerCase()
    const slug = p.slug.toLowerCase()
    let score = 0
    if (title.startsWith(needle)) score += 120
    else if (title.includes(needle)) score += 70
    if (slug.includes(needle)) score += 50
    const bodyAt = p.search.indexOf(needle)
    if (bodyAt >= 0) score += Math.max(6, 30 - bodyAt / 60)
    if (score > 0) hits.push({ ...p, score })
  }
  return hits.sort((a, b) => b.score - a.score).slice(0, 12)
}

export function DocsSearch({ pages }: { pages: DocIndexEntry[] }) {
  const [q, setQ] = useState('')
  const [open, setOpen] = useState(false)
  const [cursor, setCursor] = useState(0)
  const inputRef = useRef<HTMLInputElement>(null)
  const navigate = useNavigate()

  const hits = useMemo(() => rank(pages, q), [pages, q])

  useEffect(() => setCursor(0), [q])

  useEffect(() => {
    const onKey = (e: KeyboardEvent): void => {
      const typing = document.activeElement instanceof HTMLInputElement
      if (e.key === '/' && !typing) {
        e.preventDefault()
        inputRef.current?.focus()
      }
      if (e.key === 'Escape') {
        setOpen(false)
        inputRef.current?.blur()
      }
    }
    window.addEventListener('keydown', onKey)
    return () => window.removeEventListener('keydown', onKey)
  }, [])

  const go = (slug: string): void => {
    navigate(`/docs/${slug}`)
    setQ('')
    setOpen(false)
    inputRef.current?.blur()
  }

  const onKeyDown = (e: React.KeyboardEvent<HTMLInputElement>): void => {
    if (!hits.length) return
    if (e.key === 'ArrowDown') {
      e.preventDefault()
      setCursor((c) => (c + 1) % hits.length)
    } else if (e.key === 'ArrowUp') {
      e.preventDefault()
      setCursor((c) => (c - 1 + hits.length) % hits.length)
    } else if (e.key === 'Enter') {
      const hit = hits[cursor]
      if (hit) go(hit.slug)
    }
  }

  return (
    <div className="docs-search">
      <SearchIcon />
      <input
        ref={inputRef}
        type="search"
        value={q}
        placeholder="Search 86 documents…"
        aria-label="Search documentation"
        onChange={(e) => {
          setQ(e.target.value)
          setOpen(true)
        }}
        onFocus={() => setOpen(true)}
        onBlur={() => window.setTimeout(() => setOpen(false), 140)}
        onKeyDown={onKeyDown}
      />
      {!q ? <kbd>/</kbd> : null}

      {open && hits.length > 0 ? (
        <ul className="search-results">
          {hits.map((h, i) => (
            <li key={h.slug}>
              <button className={i === cursor ? 'active' : ''} onMouseEnter={() => setCursor(i)} onClick={() => go(h.slug)}>
                <span className="search-title">
                  {h.badge ? <em>{h.badge}</em> : null}
                  {h.title}
                </span>
                <span className="search-summary">{h.summary}</span>
              </button>
            </li>
          ))}
        </ul>
      ) : null}

      {open && q.trim().length >= 2 && hits.length === 0 ? (
        <div className="search-results empty">No document matches “{q}”.</div>
      ) : null}
    </div>
  )
}

function SearchIcon() {
  return (
    <svg viewBox="0 0 24 24" width="16" height="16" fill="none" stroke="currentColor" strokeWidth="1.9" strokeLinecap="round">
      <circle cx="10.6" cy="10.6" r="6.4" />
      <path d="m15.4 15.4 4.1 4.1" />
    </svg>
  )
}
