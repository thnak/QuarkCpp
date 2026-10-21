import { Link } from 'react-router-dom'
import { REPO_URL } from '../data/content'
import { useLang } from '../hooks/useLang'
import { TRANSLATIONS } from '../data/translations'

export function Footer() {
  const { lang } = useLang()
  const t = TRANSLATIONS[lang]

  return (
    <footer className="footer">
      <div className="wrap footer-inner">
        <div className="footer-brand">
          <span className="brand-mark" aria-hidden="true">
            <span className="brand-core" />
            <span className="brand-orbit" />
          </span>
          <div>
            <strong>Quark Engine</strong>
            <p>{t.footer.brandDesc}</p>
          </div>
        </div>

        <nav className="footer-cols">
          <div>
            <h4>{t.footer.docsCol}</h4>
            <Link to="/docs/How-To-Write-Your-First-Actor">{t.footer.firstActor}</Link>
            <Link to="/docs/Samples">{t.footer.samples}</Link>
            <Link to="/docs?category=spec">{t.footer.specs}</Link>
            <Link to="/docs?category=adr">{t.footer.adrs}</Link>
          </div>
          <div>
            <h4>{t.footer.recordsCol}</h4>
            <Link to="/docs/VERIFICATION">{t.footer.verification}</Link>
            <Link to="/docs/PERFORMANCE">{t.footer.performance}</Link>
            <Link to="/docs/OpenQuestions">{t.footer.openQuestions}</Link>
            <Link to="/docs/CONVENTIONS">{t.footer.conventions}</Link>
          </div>
          <div>
            <h4>{t.footer.projectCol}</h4>
            <a href={REPO_URL} target="_blank" rel="noreferrer noopener">
              {t.footer.repository}
            </a>
            <a href={`${REPO_URL}/actions/workflows/ci.yml`} target="_blank" rel="noreferrer noopener">
              {t.footer.ciStatus}
            </a>
            <a href={`${REPO_URL}/blob/master/LICENSE`} target="_blank" rel="noreferrer noopener">
              {t.footer.license}
            </a>
            <Link to="/docs/Contributing">{t.footer.contributing}</Link>
          </div>
        </nav>
      </div>

      <div className="wrap footer-bottom">
        <span>{t.footer.bottomNote}</span>
        <span className="footer-mono">quark · C++23 · header-first</span>
      </div>
    </footer>
  )
}
