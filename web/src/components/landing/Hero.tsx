import { Link } from 'react-router-dom'
import { HeroVisual } from './HeroVisual'
import { REPO_URL } from '../../data/content'
import { useLang } from '../../hooks/useLang'
import { TRANSLATIONS } from '../../data/translations'

const BADGES = ['C++23', 'header-first', 'std-only core', 'MIT', 'Linux x86-64 · ARM64']

export function Hero() {
  const { lang } = useLang()
  const t = TRANSLATIONS[lang]

  return (
    <section className="hero">
      <div className="wrap hero-inner">
        <div className="hero-copy">
          <a className="hero-pill" href={`${REPO_URL}/tree/master/decisions`} target="_blank" rel="noreferrer noopener">
            <span className="pulse" />
            {t.hero.pill}
            <span className="pill-arrow">→</span>
          </a>

          <h1 className="hero-title">
            {t.hero.titleLead}
            <span className="grad-text">{t.hero.titleGrad}</span>
            {t.hero.titleTail}
          </h1>

          <p className="hero-lede">{t.hero.lede}</p>

          <div className="hero-cta">
            <Link className="btn btn-primary" to="/docs/How-To-Write-Your-First-Actor">
              {t.hero.ctaFirstActor}
              <span className="cta-arrow">→</span>
            </Link>
            <Link className="btn btn-ghost" to="/docs">
              {t.hero.ctaDocs}
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
