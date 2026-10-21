import { Link, useSearchParams } from 'react-router-dom'
import type { DocCategory, DocIndex } from '../../types'
import { SpotlightCard } from '../primitives'
import { useLang } from '../../hooks/useLang'
import { TRANSLATIONS } from '../../data/translations'

const ORDER: DocCategory[] = ['guide', 'reference', 'spec', 'adr']
const isCategory = (v: string | null): v is DocCategory => ORDER.includes(v as DocCategory)

export function DocsBrowse({ index }: { index: DocIndex }) {
  const [params, setParams] = useSearchParams()
  const { lang } = useLang()
  const t = TRANSLATIONS[lang]
  const raw = params.get('category')
  const active = isCategory(raw) ? raw : null
  const shown = active ? ORDER.filter((c) => c === active) : ORDER

  return (
    <div className="docs-main">
      <div className="docs-browse">
        <header className="browse-head">
          <h1>
            {t.docsUI.browseTitleLead}
            <span className="grad-text">{t.docsUI.browseTitleGrad}</span>
          </h1>
          <p>
            {index.pages.length} {t.docsUI.browseDesc}
          </p>

          <div className="browse-filters">
            <button className={`filter ${!active ? 'active' : ''}`.trim()} onClick={() => setParams({})}>
              {t.docsUI.browseFilterAll}
              <em>{index.pages.length}</em>
            </button>
            {ORDER.map((cat) => (
              <button
                key={cat}
                className={`filter ${active === cat ? 'active' : ''}`.trim()}
                onClick={() => setParams({ category: cat })}
              >
                {index.categories[cat]?.label ?? cat}
                <em>{index.counts[cat] ?? 0}</em>
              </button>
            ))}
          </div>
        </header>

        {shown.map((cat) => {
          const pages = index.pages.filter((p) => p.category === cat)
          if (!pages.length) return null
          return (
            <section key={cat} className="browse-group">
              <div className="browse-group-head">
                <h2>{index.categories[cat]?.label ?? cat}</h2>
                <p>{index.categories[cat]?.blurb ?? ''}</p>
              </div>
              <div className="browse-grid">
                {pages.map((p) => (
                  <Link key={p.slug} to={`/docs/${p.slug}`} className="browse-card-link">
                    <SpotlightCard className="browse-card">
                      <div className="browse-card-top">
                        {p.badge ? <span className="browse-badge">{p.badge}</span> : <span className="browse-badge muted">doc</span>}
                        <span className="browse-read">{p.readMinutes} {t.docsUI.readTime}</span>
                      </div>
                      <h3>{p.title}</h3>
                      <p>{p.summary}…</p>
                      <span className="browse-origin">{p.origin}</span>
                    </SpotlightCard>
                  </Link>
                ))}
              </div>
            </section>
          )
        })}
      </div>
    </div>
  )
}
