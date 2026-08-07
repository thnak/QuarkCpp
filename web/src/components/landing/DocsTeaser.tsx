import { Link } from 'react-router-dom'
import { useDocsIndex } from '../../hooks/useDocs'
import { Reveal, SectionHead, SpotlightCard } from '../primitives'
import type { DocCategory } from '../../types'

const ORDER: DocCategory[] = ['guide', 'reference', 'spec', 'adr']

/** A few high-signal entry points, so the section is useful before you browse. */
const HIGHLIGHTS = [
  ['How-To-Write-Your-First-Actor', 'Write your first actor'],
  ['ActorEngineSpecification', 'Architecture overview'],
  ['003-Memory', '003 — Memory & the mailbox'],
  ['ADR-002-mailbox-mpsc-hot-path-r2', 'ADR-002 — the mailbox hot path'],
  ['VERIFICATION', 'Verification record'],
  ['OpenQuestions', 'Open questions'],
] as const

export function DocsTeaser() {
  const { data } = useDocsIndex()

  return (
    <section className="section docs-teaser" id="wiki">
      <div className="wrap">
        <SectionHead
          eyebrow="Wiki & docs"
          title={
            <>
              The whole design record, <span className="grad-text">readable right here</span>.
            </>
          }
          lede="Specs, decision records, guides and the verification history are rendered into this site — no jumping to a separate wiki. When code and a spec disagree, the spec wins."
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
                    <span className="docs-cat-go">Browse →</span>
                  </SpotlightCard>
                </Link>
              </Reveal>
            )
          })}
        </div>

        <Reveal className="docs-highlights" delay={120}>
          <span className="docs-highlights-label">Start with</span>
          <ul>
            {HIGHLIGHTS.map(([slug, label]) => (
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
