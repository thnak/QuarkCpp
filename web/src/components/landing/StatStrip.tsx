import { STATS } from '../../data/content'
import { useCountUp } from '../../hooks/useCountUp'
import { useInView } from '../../hooks/useInView'

function Stat({ value, label, suffix, note, delay }: (typeof STATS)[number] & { delay: number }) {
  const [ref, inView] = useInView<HTMLDivElement>()
  const n = useCountUp(value, inView)
  const decimals = Number.isInteger(value) ? 0 : 1

  return (
    <div ref={ref} className={`stat reveal ${inView ? 'in' : ''}`} style={{ '--delay': `${delay}ms` } as React.CSSProperties}>
      <div className="stat-value">
        {n.toFixed(decimals)}
        <span className="stat-suffix">{suffix}</span>
      </div>
      <div className="stat-label">{label}</div>
      <div className="stat-note">{note}</div>
    </div>
  )
}

export function StatStrip() {
  return (
    <section className="stat-strip">
      <div className="wrap stat-grid">
        {STATS.map((s, i) => (
          <Stat key={s.label} {...s} delay={i * 90} />
        ))}
      </div>
    </section>
  )
}
