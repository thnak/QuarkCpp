import { Link } from 'react-router-dom'
import { useDocsIndex } from '../../hooks/useDocs'
import { Reveal, SectionHead, SpotlightCard } from '../primitives'
import type { DocCategory } from '../../types'
import { useLang } from '../../hooks/useLang'
import { TRANSLATIONS } from '../../data/translations'

const ORDER: DocCategory[] = ['guide', 'reference', 'spec', 'adr']

export function DocsTeaser() {
  const { lang } = useLang()
  const { data } = useDocsIndex(lang)
  const t = TRANSLATIONS[lang]

  const highlights = lang === 'vi' ? [
    ['How-To-Write-Your-First-Actor', 'Viết actor đầu tiên của bạn'],
    ['ActorEngineSpecification', 'Tổng quan kiến trúc'],
    ['003-Memory', '003 — Bộ nhớ & Mailbox'],
    ['ADR-002-mailbox-mpsc-hot-path-r2', 'ADR-002 — Hot path của mailbox'],
    ['VERIFICATION', 'Hồ sơ kiểm thử'],
    ['OpenQuestions', 'Câu hỏi mở'],
  ] : [
    ['How-To-Write-Your-First-Actor', 'Write your first actor'],
    ['ActorEngineSpecification', 'Architecture overview'],
    ['003-Memory', '003 — Memory & the mailbox'],
    ['ADR-002-mailbox-mpsc-hot-path-r2', 'ADR-002 — the mailbox hot path'],
    ['VERIFICATION', 'Verification record'],
    ['OpenQuestions', 'Open questions'],
  ]

  return (
    <section className="section docs-teaser" id="wiki">
      <div className="wrap">
        <SectionHead
          eyebrow={t.nav.docs}
          title={
            lang === 'vi' ? (
              <>
                Toàn bộ hồ sơ thiết kế, <span className="grad-text">đọc trực tiếp tại đây</span>.
              </>
            ) : (
              <>
                The whole design record, <span className="grad-text">readable right here</span>.
              </>
            )
          }
          lede={
            lang === 'vi'
              ? 'Tất cả các spec, bản ghi quyết định ADR, hướng dẫn và lịch sử kiểm thử được render trực tiếp — không cần chuyển hướng đến wiki riêng. Khi mã nguồn và đặc tả mâu thuẫn, đặc tả thắng.'
              : 'Specs, decision records, guides and the verification history are rendered into this site — no jumping to a separate wiki. When code and a spec disagree, the spec wins.'
          }
        />

        <div className="docs-cat-grid">
          {ORDER.map((cat, i) => {
            const meta = data?.categories[cat]
            const count = data?.counts[cat]
            return (
              <Reveal key={cat} delay={i * 90}>
                <Link to={`/docs?category=${cat}`} className="docs-cat-link">
                  <SpotlightCard className="docs-cat">
                    <span className="docs-cat-count">{count ?? '—'}</span>
                    <h3>{meta?.label ?? cat}</h3>
                    <p>{meta?.blurb ?? ''}</p>
                    <span className="docs-cat-go">{lang === 'vi' ? 'Xem chi tiết →' : 'Browse →'}</span>
                  </SpotlightCard>
                </Link>
              </Reveal>
            )
          })}
        </div>

        <Reveal className="docs-highlights" delay={120}>
          <span className="docs-highlights-label">{lang === 'vi' ? 'Bắt đầu với' : 'Start with'}</span>
          <ul>
            {highlights.map(([slug, label]) => (
              <li key={slug}>
                <Link to={`/docs/${slug}`} className="chip chip-link">
                  {label}
                </Link>
              </li>
            ))}
          </ul>
        </Reveal>
      </div>
    </section>
  )
}
