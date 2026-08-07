import { CHOOSE_CAF, CHOOSE_QUARK, COMPARISON_ROWS, REPO_URL } from '../../data/content'
import { Reveal, SectionHead } from '../primitives'
import { Ticks } from '../Ticks'

export function Comparison() {
  return (
    <section className="section compare" id="compare">
      <div className="wrap">
        <SectionHead
          eyebrow="Honest comparison"
          title={
            <>
              Head to head with <span className="grad-text">CAF</span> — caveats included.
            </>
          }
          lede="The point of the comparison is to sanity-check Quark's hot-path claims against an established alternative, not to crown a universal winner. The two engines sit at different points on the latency-vs-generality spectrum."
        />

        <Reveal className="caveat" delay={60}>
          <span className="caveat-mark" aria-hidden="true">
            !
          </span>
          <div>
            <strong>This is not an apples-to-apples benchmark, and the writeup says so.</strong> CAF&apos;s{' '}
            <code>ask</code> runs through a general-purpose, fully dynamic mailbox; Quark&apos;s bypasses the mailbox
            via a dedicated single-shot reply slot because its actors are compile-time-typed. Quark&apos;s spawn
            benchmark never starts its scheduler, while CAF&apos;s is live from construction. And every number is a
            single unpinned Windows session on a shared dev laptop. Treat directional trends and cited mechanisms as
            the signal — not any one figure.
          </div>
        </Reveal>

        <Reveal className="compare-table" delay={100}>
          {COMPARISON_ROWS.map((r) => (
            <div key={r.dimension} className="compare-row">
              <span className="compare-dim">{r.dimension}</span>
              <span className={`compare-winner ${r.winner.toLowerCase()}`}>{r.winner}</span>
              <span className="compare-margin">{r.margin}</span>
            </div>
          ))}
        </Reveal>

        <div className="choose-grid">
          <Reveal className="choose choose-quark" delay={60}>
            <h3>
              Choose <span className="grad-text">Quark</span> if you need…
            </h3>
            <ul>
              {CHOOSE_QUARK.map((c) => (
                <li key={c}>
                  <Ticks>{c}</Ticks>
                </li>
              ))}
            </ul>
          </Reveal>
          <Reveal className="choose" delay={140}>
            <h3>Choose CAF if you need…</h3>
            <ul>
              {CHOOSE_CAF.map((c) => (
                <li key={c}>
                  <Ticks>{c}</Ticks>
                </li>
              ))}
            </ul>
          </Reveal>
        </div>

        <Reveal delay={160}>
          <a
            className="text-link"
            href={`${REPO_URL}/blob/master/bench/caf_comparison/README.md`}
            target="_blank"
            rel="noreferrer noopener"
          >
            Full methodology, every disclosed asymmetry with source-line citations →
          </a>
        </Reveal>
      </div>
    </section>
  )
}
