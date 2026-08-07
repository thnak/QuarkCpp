import { FEATURES } from '../../data/content'
import { Reveal, SectionHead, SpotlightCard } from '../primitives'
import { Ticks } from '../Ticks'
import { FeatureIcon } from './FeatureIcon'

export function Features() {
  return (
    <section className="section" id="features">
      <div className="wrap">
        <SectionHead
          eyebrow="What you get"
          title={
            <>
              Every subsystem you need to run actors <span className="grad-text">in production</span>.
            </>
          }
          lede="Implemented, covered by the 194-test correctness gate, and verified clean under ASan, UBSan and TSan on every push."
        />

        <div className="feature-grid">
          {FEATURES.map((f, i) => (
            <Reveal key={f.title} delay={(i % 3) * 90}>
              <SpotlightCard className="feature">
                <div className="feature-top">
                  <span className="feature-badge">
                    <FeatureIcon name={f.icon} />
                  </span>
                  <span className="feature-tag">{f.tag}</span>
                </div>
                <h3 className="feature-title">{f.title}</h3>
                <p className="feature-body">
                  <Ticks>{f.body}</Ticks>
                </p>
              </SpotlightCard>
            </Reveal>
          ))}
        </div>
      </div>
    </section>
  )
}
