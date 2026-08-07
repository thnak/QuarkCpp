import { Link } from 'react-router-dom'
import { INVARIANT_GATES, PERF_ROWS } from '../../data/content'
import { Reveal, SectionHead } from '../primitives'
import { useInView } from '../../hooks/useInView'

function PerfRow({ row, delay }: { row: (typeof PERF_ROWS)[number]; delay: number }) {
  const [ref, inView] = useInView<HTMLDivElement>()

  return (
    <div ref={ref} className={`perf-row reveal ${inView ? 'in' : ''}`} style={{ '--delay': `${delay}ms` } as React.CSSProperties}>
      <div className="perf-feature">
        <span className="perf-spec">{row.spec}</span>
        <span className="perf-name">{row.feature}</span>
      </div>
      <div className="perf-metric">{row.metric}</div>
      <div className="perf-measured">{row.measured}</div>
      <div className="perf-bar-cell">
        <div className="perf-bar">
          <span
            className={`perf-fill ${row.verdict}`}
            style={{ width: inView ? `${Math.max(4, row.ratio * 100)}%` : '0%' }}
          />
        </div>
        <span className="perf-budget">{row.budget}</span>
      </div>
      <div className={`perf-verdict ${row.verdict}`}>{row.verdict === 'free' ? 'free' : 'goal'}</div>
    </div>
  )
}

export function Performance() {
  return (
    <section className="section" id="performance">
      <div className="wrap">
        <SectionHead
          eyebrow="Measured, not asserted"
          title={
            <>
              Every performance claim is a <span className="grad-text">verdict a benchmark prints</span>.
            </>
          }
          lede="The bench harness checks each figure against the 023 budget table on every push. Bars show measured headroom against budget — shorter is further inside the budget."
        />

        <Reveal className="perf-panel" delay={80}>
          <div className="perf-head">
            <span>Feature</span>
            <span>Metric</span>
            <span>Measured</span>
            <span>vs budget</span>
            <span />
          </div>
          {PERF_ROWS.map((row, i) => (
            <PerfRow key={`${row.feature}-${row.metric}`} row={row} delay={i * 45} />
          ))}
        </Reveal>

        <Reveal className="perf-foot" delay={140}>
          <p className="perf-note">
            Release + <code>-march=native</code>, single core pinned, on a virtualized Xeon Silver 4208 @ 2.1 GHz — a
            modest reference machine, so these are regression tripwires rather than a best-case stamp.
          </p>
          <div className="gate-list">
            <span className="gate-label">Machine-independent gates — pass/fail CTest, not benchmarks:</span>
            <ul>
              {INVARIANT_GATES.map((g) => (
                <li key={g} className="chip gate">
                  <CheckIcon />
                  {g}
                </li>
              ))}
            </ul>
          </div>
          <Link className="text-link" to="/docs/PERFORMANCE">
            Full performance report, with reproduce steps →
          </Link>
        </Reveal>
      </div>
    </section>
  )
}

function CheckIcon() {
  return (
    <svg viewBox="0 0 16 16" width="12" height="12" fill="none" stroke="currentColor" strokeWidth="2.2" strokeLinecap="round" strokeLinejoin="round">
      <path d="M3 8.5 6.2 12 13 4.6" />
    </svg>
  )
}
