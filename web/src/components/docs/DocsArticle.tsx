import { useCallback, useEffect, useRef, useState } from 'react'
import { Link, useLocation } from 'react-router-dom'
import { useDocPage } from '../../hooks/useDocs'
import type { DocIndexEntry } from '../../types'

const CATEGORY_LABEL: Record<string, string> = {
  guide: 'Guide',
  reference: 'Reference',
  spec: 'Specification',
  adr: 'Decision record',
}

export function DocsArticle({ slug, siblings }: { slug: string; siblings: DocIndexEntry[] }) {
  const { data, error, loading } = useDocPage(slug)
  const { hash } = useLocation()
  const bodyRef = useRef<HTMLDivElement>(null)
  const [activeHeading, setActiveHeading] = useState<string>('')

  // Jump to a #heading carried on the route, once the HTML is in the DOM.
  useEffect(() => {
    if (!data) return
    if (!hash) {
      window.scrollTo({ top: 0 })
      return
    }
    const id = decodeURIComponent(hash.slice(1))
    requestAnimationFrame(() => document.getElementById(id)?.scrollIntoView({ block: 'start' }))
  }, [data, hash])

  // Highlight the section currently under the nav.
  useEffect(() => {
    const root = bodyRef.current
    if (!root || !data) return
    const targets = Array.from(root.querySelectorAll<HTMLElement>('h2[id], h3[id]'))
    if (!targets.length) return

    const io = new IntersectionObserver(
      (entries) => {
        for (const e of entries) if (e.isIntersecting) setActiveHeading(e.target.id)
      },
      { rootMargin: '-80px 0px -70% 0px', threshold: 0 }
    )
    targets.forEach((t) => io.observe(t))
    return () => io.disconnect()
  }, [data])

  /** In-page `#id` links must not be mistaken for hash routes. */
  const onClick = useCallback((e: React.MouseEvent<HTMLDivElement>): void => {
    const link = (e.target as HTMLElement).closest('a')
    if (!link) return
    const href = link.getAttribute('href') ?? ''
    if (href.startsWith('#') && !href.startsWith('#/')) {
      e.preventDefault()
      const el = document.getElementById(decodeURIComponent(href.slice(1)))
      el?.scrollIntoView({ behavior: 'smooth', block: 'start' })
    }
  }, [])

  if (loading) return <DocsSkeleton />

  if (error || !data) {
    return (
      <article className="docs-article">
        <h1>Document not found</h1>
        <p className="docs-missing">
          No document is published at <code>{slug}</code>.
        </p>
        <Link className="btn btn-ghost" to="/docs">
          Back to all documents
        </Link>
      </article>
    )
  }

  const i = siblings.findIndex((p) => p.slug === slug)
  const prev = i > 0 ? siblings[i - 1] : undefined
  const next = i >= 0 && i < siblings.length - 1 ? siblings[i + 1] : undefined
  const entry = i >= 0 ? siblings[i] : undefined

  return (
    <div className="docs-main">
      <article className="docs-article">
        <header className="docs-article-head">
          <div className="docs-crumbs">
            <Link to="/docs">Docs</Link>
            <span>/</span>
            <Link to={`/docs?category=${data.category}`}>{CATEGORY_LABEL[data.category] ?? data.category}</Link>
          </div>
          <div className="docs-meta">
            {data.badge ? <span className="chip">{data.badge}</span> : null}
            {entry ? <span className="chip">{entry.readMinutes} min read</span> : null}
            <a className="chip chip-link" href={data.githubUrl} target="_blank" rel="noreferrer noopener">
              {data.origin}
            </a>
          </div>
        </header>

        <div ref={bodyRef} className="markdown" onClick={onClick} dangerouslySetInnerHTML={{ __html: data.html }} />

        <nav className="docs-pager">
          {prev ? (
            <Link to={`/docs/${prev.slug}`} className="pager-link prev">
              <span>← Previous</span>
              <strong>{prev.title}</strong>
            </Link>
          ) : (
            <span />
          )}
          {next ? (
            <Link to={`/docs/${next.slug}`} className="pager-link next">
              <span>Next →</span>
              <strong>{next.title}</strong>
            </Link>
          ) : (
            <span />
          )}
        </nav>
      </article>

      {data.headings.length > 2 ? (
        <aside className="docs-toc">
          <span className="toc-label">On this page</span>
          <ul>
            {data.headings
              .filter((h) => h.level <= 3)
              .map((h) => (
                <li key={h.id} className={`toc-l${h.level} ${activeHeading === h.id ? 'active' : ''}`.trim()}>
                  <a
                    href={`#${h.id}`}
                    onClick={(e) => {
                      e.preventDefault()
                      document.getElementById(h.id)?.scrollIntoView({ behavior: 'smooth', block: 'start' })
                    }}
                  >
                    {h.text}
                  </a>
                </li>
              ))}
          </ul>
        </aside>
      ) : null}
    </div>
  )
}

function DocsSkeleton() {
  return (
    <div className="docs-main">
      <article className="docs-article">
        <div className="skeleton sk-badge" />
        <div className="skeleton sk-title" />
        <div className="skeleton sk-line" />
        <div className="skeleton sk-line short" />
        <div className="skeleton sk-block" />
        <div className="skeleton sk-line" />
        <div className="skeleton sk-line short" />
      </article>
    </div>
  )
}
