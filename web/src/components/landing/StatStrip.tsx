import { STATS } from '../../data/content'
import { useCountUp } from '../../hooks/useCountUp'
import { useInView } from '../../hooks/useInView'
import { useLang } from '../../hooks/useLang'
import { TRANSLATIONS } from '../../data/translations'

function Stat({ value, label, suffix, note, delay }: { value: number; label: string; suffix: string; note: string; delay: number }) {
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
  const { lang } = useLang()
  const t = TRANSLATIONS[lang]

  const stats = [
    { value: STATS[0]!.value, suffix: STATS[0]!.suffix, label: t.stats.testsLabel, note: t.stats.testsNote },
    { value: STATS[1]!.value, suffix: STATS[1]!.suffix, label: t.stats.docsLabel, note: t.stats.docsNote },
    { value: STATS[2]!.value, suffix: STATS[2]!.suffix, label: t.stats.p50Label, note: t.stats.p50Note },
    { value: STATS[3]!.value, suffix: STATS[3]!.suffix, label: t.stats.throughputLabel, note: t.stats.throughputNote },
  ]

  return (
    <section className="stat-strip">
      <div className="wrap stat-grid">
        {stats.map((s, i) => (
          <Stat key={s.label} {...s} delay={i * 90} />
        ))}
      </div>
    </section>
  )
}
