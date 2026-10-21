import { useState } from 'react'
import { CODE_TABS, REPO_URL } from '../../data/content'
import { Reveal, SectionHead } from '../primitives'
import { CodeBlock } from '../CodeBlock'
import { useLang } from '../../hooks/useLang'
import { TRANSLATIONS } from '../../data/translations'

export function CodeShowcase() {
  const [active, setActive] = useState(CODE_TABS[0]!.id)
  const { lang } = useLang()
  const t = TRANSLATIONS[lang]

  const tabIndex = CODE_TABS.findIndex((t) => t.id === active)
  const tab = CODE_TABS[tabIndex >= 0 ? tabIndex : 0]!
  const tabTranslation = t.code.tabs[tabIndex >= 0 ? tabIndex : 0] ?? { label: tab.label, caption: tab.caption }

  return (
    <section className="section code-section" id="code">
      <div className="wrap">
        <SectionHead
          eyebrow={lang === 'vi' ? 'Giao diện phát triển' : 'The developer surface'}
          title={
            lang === 'vi' ? (
              <>
                Policy là các <span className="grad-text">tham số template</span>, không phải tệp cấu hình.
              </>
            ) : (
              <>
                Policies are <span className="grad-text">template parameters</span>, not config files.
              </>
            )
          }
          lede={
            lang === 'vi'
              ? 'Lớp cơ sở CRTP chính là metadata của actor — dải ưu tiên, ngân sách thu dọn, reentrancy, vị trí — được giải quyết khi khởi động với chi phí runtime bằng 0. Mỗi đoạn code dưới đây được trích trực tiếp từ sample thực tế.'
              : "An actor's CRTP base IS its metadata — band, drain budget, reentrancy, placement — resolved at startup with zero runtime cost. Every snippet below is lifted from a runnable sample."
          }
        />

        <Reveal className="code-shell" delay={80}>
          <div className="code-tabs" role="tablist">
            {CODE_TABS.map((tItem, idx) => {
              const label = t.code.tabs[idx]?.label ?? tItem.label
              return (
                <button
                  key={tItem.id}
                  role="tab"
                  aria-selected={tItem.id === active}
                  className={`code-tab ${tItem.id === active ? 'active' : ''}`.trim()}
                  onClick={() => setActive(tItem.id)}
                >
                  {label}
                </button>
              )
            })}
          </div>

          <div className="code-body">
            <CodeBlock code={tab.code} language="cpp" filename={`${tab.source}/main.cpp`} />
            <aside className="code-aside">
              <p className="code-caption">{tabTranslation.caption}</p>
              <a className="text-link" href={`${REPO_URL}/tree/master/${tab.source}`} target="_blank" rel="noreferrer noopener">
                {lang === 'vi' ? 'Mở toàn bộ mẫu mã nguồn →' : 'Open the full sample →'}
              </a>
            </aside>
          </div>
        </Reveal>
      </div>
    </section>
  )
}
