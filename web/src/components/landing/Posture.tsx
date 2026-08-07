import { POSTURE_ROWS } from '../../data/content'
import { Reveal, SectionHead } from '../primitives'

export function Posture() {
  return (
    <section className="section" id="posture">
      <div className="wrap">
        <SectionHead
          eyebrow="Dependency posture"
          title={
            <>
              A std-only core, and <span className="grad-text">seams</span> for everything else.
            </>
          }
          lede="Every subsystem that would otherwise pull a heavy dependency is a seam with a self-contained default. Heavier backends are optional adapters that are never linked into a minimal build."
        />

        <Reveal className="posture-table" delay={80}>
          <div className="posture-head">
            <span>Subsystem</span>
            <span>Std-only default</span>
            <span>Optional adapter</span>
          </div>
          {POSTURE_ROWS.map((r) => (
            <div key={r.subsystem} className="posture-row">
              <span className="posture-name">
                {r.subsystem}
                <em>{r.spec}</em>
              </span>
              <span className="posture-std">{r.std}</span>
              <span className="posture-adapter">{r.adapter}</span>
            </div>
          ))}
        </Reveal>
      </div>
    </section>
  )
}
