import { useState } from 'react'
import { NavLink } from 'react-router-dom'
import type { DocCategory, DocIndex } from '../../types'

const ORDER: DocCategory[] = ['guide', 'reference', 'spec', 'adr']

export function DocsSidebar({ index, open, onNavigate }: { index: DocIndex; open: boolean; onNavigate: () => void }) {
  const [collapsed, setCollapsed] = useState<Partial<Record<DocCategory, boolean>>>({ adr: false })

  return (
    <aside className={`docs-sidebar ${open ? 'open' : ''}`.trim()}>
      <nav>
        <NavLink to="/docs" end className={({ isActive }) => `side-home ${isActive ? 'active' : ''}`} onClick={onNavigate}>
          All documents
          <span className="side-count">{index.pages.length}</span>
        </NavLink>

        {ORDER.map((cat) => {
          const pages = index.pages.filter((p) => p.category === cat)
          if (!pages.length) return null
          const isCollapsed = collapsed[cat] ?? false

          return (
            <section key={cat} className="side-group">
              <button
                className={`side-group-head ${isCollapsed ? 'collapsed' : ''}`.trim()}
                onClick={() => setCollapsed((c) => ({ ...c, [cat]: !isCollapsed }))}
                aria-expanded={!isCollapsed}
              >
                <ChevronIcon />
                {index.categories[cat].label}
                <span className="side-count">{pages.length}</span>
              </button>

              {!isCollapsed ? (
                <ul className="side-list">
                  {pages.map((p) => (
                    <li key={p.slug}>
                      <NavLink
                        to={`/docs/${p.slug}`}
                        className={({ isActive }) => `side-item ${isActive ? 'active' : ''}`}
                        onClick={onNavigate}
                        title={p.title}
                      >
                        {p.badge ? <em className="side-badge">{p.badge}</em> : null}
                        <span>{p.navLabel}</span>
                      </NavLink>
                    </li>
                  ))}
                </ul>
              ) : null}
            </section>
          )
        })}
      </nav>
    </aside>
  )
}

function ChevronIcon() {
  return (
    <svg className="chev" viewBox="0 0 16 16" width="12" height="12" fill="none" stroke="currentColor" strokeWidth="2" strokeLinecap="round" strokeLinejoin="round">
      <path d="m4 6 4 4 4-4" />
    </svg>
  )
}
