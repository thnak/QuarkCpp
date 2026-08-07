import { Link } from 'react-router-dom'
import { HeroVisual } from './HeroVisual'
import { REPO_URL } from '../../data/content'

const BADGES = ['C++23', 'header-first', 'std-only core', 'MIT', 'Linux x86-64 · ARM64']

export function Hero() {
  return (
    <section className="hero">
      <div className="wrap hero-inner">
        <div className="hero-copy">
          <a className="hero-pill" href={`${REPO_URL}/tree/master/decisions`} target="_blank" rel="noreferrer noopener">
            <span className="pulse" />
            11 rounds of red-teaming — the mailbox still wins
            <span className="pill-arrow">→</span>
          </a>

          <h1 className="hero-title">
            The actor engine that <span className="grad-text">proves</span> its hot path.
          </h1>

          <p className="hero-lede">
            Quark is a header-first <strong>C++23 actor engine</strong> — work-stealing scheduler, hybrid
            sync/async handlers, cluster distribution to 10³–10⁴ nodes, durable persistence and supervision.
            The runtime owns optimization; you express only intent.
          </p>

          <div className="hero-cta">
            <Link className="btn btn-primary" to="/docs/How-To-Write-Your-First-Actor">
              Write your first actor
              <span className="cta-arrow">→</span>
            </Link>
            <Link className="btn btn-ghost" to="/docs">
              Browse the wiki
            </Link>
          </div>

          <ul className="hero-badges">
            {BADGES.map((b) => (
              <li key={b} className="chip">
                {b}
              </li>
            ))}
          </ul>
        </div>

        <HeroVisual />
      </div>

      <div className="hero-fade" aria-hidden="true" />
    </section>
  )
}
