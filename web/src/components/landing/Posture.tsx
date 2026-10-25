import { POSTURE_ROWS } from '../../data/content'
import { Reveal, SectionHead } from '../primitives'
import { useLang } from '../../hooks/useLang'
import { TRANSLATIONS } from '../../data/translations'

export function Posture() {
  const { lang } = useLang()
  const t = TRANSLATIONS[lang]

  return (
    <section className="section" id="posture">
      <div className="wrap">
        <SectionHead
          eyebrow={lang === 'vi' ? 'Kiến trúc phụ thuộc' : 'Dependency posture'}
          title={
            lang === 'vi' ? (
              <>
                Lõi std thuần túy, và <span className="grad-text">điểm nối</span> cho tất cả thành phần mở rộng.
              </>
            ) : (
              <>
                A std-only core, and <span className="grad-text">seams</span> for everything else.
              </>
            )
          }
          lede={
            lang === 'vi'
              ? 'Mọi phân hệ phụ thuộc nặng đều được thiết kế dạng điểm nối (seam) với mặc định tự đóng gói. Các backend mở rộng là adapter tùy chọn và không bao giờ bị link cưỡng bức vào bản build tối thiểu.'
              : 'Every subsystem that would otherwise pull a heavy dependency is a seam with a self-contained default. Heavier backends are optional adapters that are never linked into a minimal build.'
          }
        />

        <Reveal className="posture-table" delay={80}>
          <div className="posture-head">
            <span>{t.posture.subsystemCol}</span>
            <span>{t.posture.stdCol}</span>
            <span>{t.posture.adapterCol}</span>
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
