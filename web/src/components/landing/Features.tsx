import { FEATURES } from '../../data/content'
import { Reveal, SectionHead, SpotlightCard } from '../primitives'
import { Ticks } from '../Ticks'
import { FeatureIcon } from './FeatureIcon'
import { useLang } from '../../hooks/useLang'
import { TRANSLATIONS } from '../../data/translations'

export function Features() {
  const { lang } = useLang()
  const t = TRANSLATIONS[lang]

  return (
    <section className="section" id="features">
      <div className="wrap">
        <SectionHead
          eyebrow={lang === 'vi' ? 'Tổng quan' : 'What you get'}
          title={
            lang === 'vi' ? (
              <>
                Mọi phân hệ cần thiết để vận hành actor <span className="grad-text">trên thực tế</span>.
              </>
            ) : (
              <>
                Every subsystem you need to run actors <span className="grad-text">in production</span>.
              </>
            )
          }
          lede={
            lang === 'vi'
              ? 'Đã cài đặt hoàn chỉnh, bảo đảm bởi 194 bài kiểm thử tính đúng đắn, vượt qua ASan, UBSan và TSan trên từng push.'
              : 'Implemented, covered by the 194-test correctness gate, and verified clean under ASan, UBSan and TSan on every push.'
          }
        />

        <div className="feature-grid">
          {FEATURES.map((f, i) => {
            const item = t.features.items[i] ?? { title: f.title, body: f.body }
            return (
              <Reveal key={f.title} delay={(i % 3) * 90}>
                <SpotlightCard className="feature">
                  <div className="feature-top">
                    <span className="feature-badge">
                      <FeatureIcon name={f.icon} />
                    </span>
                    <span className="feature-tag">{f.tag}</span>
                  </div>
                  <h3 className="feature-title">{item.title}</h3>
                  <p className="feature-body">
                    <Ticks>{item.body}</Ticks>
                  </p>
                </SpotlightCard>
              </Reveal>
            )
          })}
        </div>
      </div>
    </section>
  )
}
