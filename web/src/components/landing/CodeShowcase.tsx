import { useState } from 'react'
import { CODE_TABS, REPO_URL } from '../../data/content'
import { Reveal, SectionHead } from '../primitives'
import { CodeBlock } from '../CodeBlock'

export function CodeShowcase() {
  const [active, setActive] = useState(CODE_TABS[0]!.id)
  const tab = CODE_TABS.find((t) => t.id === active) ?? CODE_TABS[0]!

  return (
    <section className="section code-section" id="code">
      <div className="wrap">
        <SectionHead
          eyebrow="The developer surface"
          title={
            <>
              Policies are <span className="grad-text">template parameters</span>, not config files.
            </>
          }
          lede="An actor's CRTP base IS its metadata — band, drain budget, reentrancy, placement — resolved at startup with zero runtime cost. Every snippet below is lifted from a runnable sample."
        />

        <Reveal className="code-shell" delay={80}>
          <div className="code-tabs" role="tablist">
            {CODE_TABS.map((t) => (
              <button
                key={t.id}
                role="tab"
                aria-selected={t.id === active}
                className={`code-tab ${t.id === active ? 'active' : ''}`.trim()}
                onClick={() => setActive(t.id)}
              >
                {t.label}
              </button>
            ))}
          </div>

          <div className="code-body">
            <CodeBlock code={tab.code} language="cpp" filename={`${tab.source}/main.cpp`} />
            <aside className="code-aside">
              <p className="code-caption">{tab.caption}</p>
              <a className="text-link" href={`${REPO_URL}/tree/master/${tab.source}`} target="_blank" rel="noreferrer noopener">
                Open the full sample →
              </a>
            </aside>
          </div>
        </Reveal>
      </div>
    </section>
  )
}
