import { StrictMode } from 'react'
import { createRoot } from 'react-dom/client'
import { HashRouter } from 'react-router-dom'
import App from './App'

import './styles/base.css'
import './styles/landing.css'
import './styles/docs.css'
import './styles/markdown.css'

const host = document.getElementById('root')
if (!host) throw new Error('#root missing')

createRoot(host).render(
  <StrictMode>
    {/* Hash routing: GitHub Pages serves static files with no rewrite rules. */}
    <HashRouter>
      <App />
    </HashRouter>
  </StrictMode>
)
