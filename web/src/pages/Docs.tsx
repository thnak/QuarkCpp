import { useEffect, useState } from 'react'
import { useParams } from 'react-router-dom'
import { useDocsIndex } from '../hooks/useDocs'
import { DocsSidebar } from '../components/docs/DocsSidebar'
import { DocsSearch } from '../components/docs/DocsSearch'
import { DocsArticle } from '../components/docs/DocsArticle'
import { DocsBrowse } from '../components/docs/DocsBrowse'
import { useLang } from '../hooks/useLang'
import { TRANSLATIONS } from '../data/translations'

export function Docs() {
  const { slug } = useParams<{ slug?: string }>()
  const { lang } = useLang()
  const { data: index, error, loading } = useDocsIndex(lang)
  const t = TRANSLATIONS[lang]
  const [navOpen, setNavOpen] = useState(false)

  useEffect(() => setNavOpen(false), [slug])

  if (loading) {
    return (
      <div className="docs-shell">
        <div className="docs-loading">
          {lang === 'vi' ? 'Đang tải hồ sơ thiết kế…' : 'Loading the design record…'}
        </div>
      </div>
    )
  }

  if (error || !index) {
    return (
      <div className="docs-shell">
        <div className="docs-loading error">
          {lang === 'vi'
            ? `Không thể tải mục mục tài liệu (${error ?? 'lỗi không xác định'}). Nếu bạn mở trực tiếp qua tệp tin, hãy phục vụ qua HTTP.`
            : `Could not load the documentation index (${error ?? 'unknown error'}). If you opened this page straight off the filesystem, serve the folder over HTTP instead — the docs are fetched as JSON.`}
        </div>
      </div>
    )
  }

  return (
    <div className="docs-shell">
      <div className="docs-bar">
        <button className="docs-nav-toggle" onClick={() => setNavOpen((v) => !v)} aria-expanded={navOpen}>
          <MenuIcon />
          {t.docsUI.documentsToggle}
        </button>
        <DocsSearch pages={index.pages} />
      </div>

      <div className="docs-layout">
        <DocsSidebar index={index} open={navOpen} onNavigate={() => setNavOpen(false)} />
        {navOpen ? <div className="docs-scrim" onClick={() => setNavOpen(false)} /> : null}
        {slug ? <DocsArticle slug={slug} siblings={index.pages} /> : <DocsBrowse index={index} />}
      </div>
    </div>
  )
}

function MenuIcon() {
  return (
    <svg viewBox="0 0 16 16" width="15" height="15" fill="none" stroke="currentColor" strokeWidth="1.9" strokeLinecap="round">
      <path d="M2.5 4h11M2.5 8h11M2.5 12h11" />
    </svg>
  )
}
