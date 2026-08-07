import { Link } from 'react-router-dom'
import { REPO_URL } from '../data/content'

export function Footer() {
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
            <p>A high-performance C++23 actor engine. The runtime owns optimization; developers express only intent.</p>
          </div>
        </div>

        <nav className="footer-cols">
          <div>
            <h4>Docs</h4>
            <Link to="/docs/How-To-Write-Your-First-Actor">First actor</Link>
            <Link to="/docs/Samples">Samples</Link>
            <Link to="/docs?category=spec">Specifications</Link>
            <Link to="/docs?category=adr">Decision records</Link>
          </div>
          <div>
            <h4>Records</h4>
            <Link to="/docs/VERIFICATION">Verification</Link>
            <Link to="/docs/PERFORMANCE">Performance</Link>
            <Link to="/docs/OpenQuestions">Open questions</Link>
            <Link to="/docs/CONVENTIONS">Conventions</Link>
          </div>
          <div>
            <h4>Project</h4>
            <a href={REPO_URL} target="_blank" rel="noreferrer noopener">
              Repository
            </a>
            <a href={`${REPO_URL}/actions/workflows/ci.yml`} target="_blank" rel="noreferrer noopener">
              CI status
            </a>
            <a href={`${REPO_URL}/blob/master/LICENSE`} target="_blank" rel="noreferrer noopener">
              MIT license
            </a>
            <Link to="/docs/Contributing">Contributing</Link>
          </div>
        </nav>
      </div>

      <div className="wrap footer-bottom">
        <span>MIT licensed. Built from the repository&apos;s own specs, ADRs and measured results.</span>
        <span className="footer-mono">quark · C++23 · header-first</span>
      </div>
    </footer>
  )
}
