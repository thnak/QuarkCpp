import { useMemo, useState } from 'react'
import hljs from 'highlight.js/lib/core'
import cpp from 'highlight.js/lib/languages/cpp'
import bash from 'highlight.js/lib/languages/bash'

hljs.registerLanguage('cpp', cpp)
hljs.registerLanguage('bash', bash)

interface Props {
  code: string
  language: 'cpp' | 'bash'
  filename?: string
  className?: string
}

export function CodeBlock({ code, language, filename, className = '' }: Props) {
  const html = useMemo(() => hljs.highlight(code, { language, ignoreIllegals: true }).value, [code, language])
  const [copied, setCopied] = useState(false)

  const copy = async (): Promise<void> => {
    try {
      await navigator.clipboard.writeText(code)
      setCopied(true)
      window.setTimeout(() => setCopied(false), 1600)
    } catch {
      /* clipboard blocked — the code is still selectable */
    }
  }

  return (
    <figure className={`codeblock ${className}`.trim()}>
      <div className="codeblock-bar">
        <span className="dots" aria-hidden="true">
          <i />
          <i />
          <i />
        </span>
        {filename ? <span className="codeblock-name">{filename}</span> : null}
        <button className="copy-btn" onClick={copy} aria-label="Copy code">
          {copied ? 'copied' : 'copy'}
        </button>
      </div>
      <pre>
        <code className="hljs" dangerouslySetInnerHTML={{ __html: html }} />
      </pre>
    </figure>
  )
}
