import { Link } from 'react-router-dom'
import { QUICKSTART, REPO_URL } from '../../data/content'
import { Reveal, SectionHead } from '../primitives'
import { CodeBlock } from '../CodeBlock'

export function QuickStart() {
  return (
    <section className="section start" id="start">
      <div className="wrap start-grid">
        <div>
          <SectionHead
            eyebrow="Quick start"
            title={
              <>
                Clone, build, and watch <span className="grad-text">194 tests</span> go green.
              </>
            }
            lede="Requires CMake ≥ 3.24 and a C++23 compiler — verified on g++ 14.2 and clang 20.1."
          />

          <Reveal className="start-note" delay={100}>
            <strong>Machine-safety note.</strong> A build that saturates all cores can hang or power off a constrained
            dev box. Build with <code>-j4</code> (TSan with <code>-j1</code>) and run binaries under{' '}
            <code>taskset -c 0-3</code> — never <code>-j$(nproc)</code>.
          </Reveal>

          <Reveal className="start-links" delay={140}>
            <Link className="btn btn-primary" to="/docs/How-To-Write-Your-First-Actor">
              First-actor walkthrough <span className="cta-arrow">→</span>
            </Link>
            <a className="btn btn-ghost" href={REPO_URL} target="_blank" rel="noreferrer noopener">
              Clone the repository
            </a>
          </Reveal>
        </div>

        <Reveal delay={80}>
          <CodeBlock code={QUICKSTART} language="bash" filename="terminal" className="start-code" />
        </Reveal>
      </div>
    </section>
  )
}
