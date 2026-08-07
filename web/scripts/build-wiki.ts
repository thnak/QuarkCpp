// Builds the embedded documentation corpus for the Quark site.
//
// Sources, in precedence order:
//   1. repo-root markdown  — the authoritative specs (001..028), ADRs (decisions/ADR-*)
//      and reference docs. Always current.
//   2. wiki/*.md           — the wiki-only guide pages that have no repo-root twin
//      (first-actor how-to, samples tour, contributor guide, ...).
//
// Emits:
//   public/docs/index.json     — slug/title/category/summary/search text for every page
//   public/docs/<slug>.json    — rendered HTML + heading outline, fetched lazily
//
// Run via `npm run prebuild` / `npm run predev`. Executed by Node's native
// TypeScript type-stripping, so keep the syntax erasable (no enums/namespaces).

import { readFile, writeFile, mkdir, readdir, rm } from 'node:fs/promises'
import { existsSync } from 'node:fs'
import path from 'node:path'
import { fileURLToPath } from 'node:url'
import { Marked } from 'marked'
import type { Tokens } from 'marked'
import hljs from 'highlight.js'

import type { DocCategory, DocHeading, DocIndexEntry, DocPage } from '../src/types.ts'

const HERE = path.dirname(fileURLToPath(import.meta.url))
const WEB = path.resolve(HERE, '..')
const REPO = path.resolve(WEB, '..')
const WIKI = path.join(REPO, 'wiki')
const OUT = path.join(WEB, 'public', 'docs')

const GH = 'https://github.com/thnak/QuarkCpp'
const GH_BLOB = `${GH}/blob/master/`
const GH_TREE = `${GH}/tree/master/`

// ---------------------------------------------------------------- source list

const CATEGORIES: Record<DocCategory, { label: string; blurb: string; order: number }> = {
  guide: { label: 'Guides', blurb: 'Start here — write an actor, tour the samples, read the status.', order: 0 },
  reference: { label: 'Reference', blurb: 'The architecture overview, conventions and the verification record.', order: 1 },
  spec: { label: 'Specifications', blurb: 'The 28 RFC documents. Authoritative design — when code and a spec disagree, the spec wins.', order: 2 },
  adr: { label: 'Decision records', blurb: 'Executed proofs. Real C++23 under GCC + Clang, ASan/UBSan/TSan, benchmarked.', order: 3 },
}

interface Source {
  slug: string
  file: string
  category: DocCategory
  title: string | null
  origin: string
}

/** Pages the wiki owns that have no authoritative repo-root twin. */
const WIKI_ONLY: Array<[string, DocCategory, string]> = [
  ['How-To-Write-Your-First-Actor.md', 'guide', 'Write your first actor'],
  ['Samples.md', 'guide', 'Samples'],
  ['Benchmarks.md', 'guide', 'Benchmarks'],
  ['Contributing.md', 'guide', 'Contributor guide'],
  ['Project-Status.md', 'guide', 'Project status'],
]

/** Reference docs that live at the repo root. */
const ROOT_REFERENCE: Array<[string, string]> = [
  ['ActorEngineSpecification.md', 'Architecture overview'],
  ['CONVENTIONS.md', 'Conventions'],
  ['VERIFICATION.md', 'Verification record'],
  ['PERFORMANCE.md', 'Performance report'],
  ['OpenQuestions.md', 'Open questions'],
  ['PersistenceAdapters.md', 'Persistence adapters'],
  ['TAIL-CONTENTION.md', 'Tail contention notes'],
]

async function collectSources(): Promise<Source[]> {
  const sources: Source[] = []
  const seen = new Set<string>()

  const push = (file: string, category: DocCategory, title: string | null, origin: string): void => {
    const slug = path.basename(file, '.md')
    if (seen.has(slug)) return
    seen.add(slug)
    sources.push({ slug, file, category, title, origin })
  }

  for (const [name, title] of ROOT_REFERENCE) {
    const file = path.join(REPO, name)
    if (existsSync(file)) push(file, 'reference', title, name)
  }

  for (const name of (await readdir(REPO)).sort()) {
    if (/^\d{3}-.+\.md$/.test(name)) push(path.join(REPO, name), 'spec', null, name)
  }

  const decisionsDir = path.join(REPO, 'decisions')
  if (existsSync(decisionsDir)) {
    for (const name of (await readdir(decisionsDir)).sort()) {
      if (/^ADR-.+\.md$/.test(name)) push(path.join(decisionsDir, name), 'adr', null, `decisions/${name}`)
    }
  }

  for (const [name, category, title] of WIKI_ONLY) {
    const file = path.join(WIKI, name)
    if (existsSync(file)) push(file, category, title, `wiki/${name}`)
  }

  return sources
}

// ------------------------------------------------------------------ rendering

const slugifyHeading = (text: string): string =>
  text
    .toLowerCase()
    .replace(/<[^>]+>/g, '')
    .replace(/[^\w\s-]/g, '')
    .trim()
    .replace(/\s+/g, '-') || 'section'

const escapeHtml = (s: string): string =>
  s.replace(/&/g, '&amp;').replace(/</g, '&lt;').replace(/>/g, '&gt;').replace(/"/g, '&quot;')

const NAMED_ENTITIES: Record<string, string> = {
  amp: '&',
  lt: '<',
  gt: '>',
  quot: '"',
  apos: "'",
  nbsp: ' ',
  hellip: '…',
  mdash: '—',
  ndash: '–',
}

/** Heading text and search text are plain strings — entities must come back out. */
const decodeEntities = (s: string): string =>
  s
    .replace(/&#x([0-9a-f]+);/gi, (_, hex: string) => String.fromCodePoint(parseInt(hex, 16)))
    .replace(/&#(\d+);/g, (_, dec: string) => String.fromCodePoint(Number(dec)))
    .replace(/&([a-z]+);/gi, (m, name: string) => NAMED_ENTITIES[name.toLowerCase()] ?? m)

const LANG_ALIASES: Record<string, string> = {
  cpp: 'cpp',
  'c++': 'cpp',
  cc: 'cpp',
  h: 'cpp',
  cmake: 'cmake',
  sh: 'bash',
  shell: 'bash',
  console: 'bash',
  text: '',
}

interface Resolved {
  href: string
  external: boolean
}

/** Resolves a markdown href to either an in-site docs route or a GitHub URL. */
function resolveHref(rawHref: string | null | undefined, slugs: Set<string>, originDir: string): Resolved {
  const href = (rawHref ?? '').trim()
  if (!href) return { href: '#', external: false }
  if (/^(https?:|mailto:|data:)/i.test(href)) return { href, external: true }
  if (href.startsWith('#')) return { href, external: false }

  const [pathPart = '', anchor] = href.split('#')
  const hash = anchor ? `#${anchor}` : ''
  const toRepo = (p: string): string => path.posix.normalize(path.posix.join(originDir, p)).replace(/^\.\//, '')

  // A markdown file anywhere in the repo -> the in-site page with that stem.
  if (/\.md$/i.test(pathPart)) {
    const stem = path.basename(pathPart, '.md')
    if (slugs.has(stem)) return { href: `#/docs/${stem}${hash}`, external: false }
    return { href: GH_BLOB + toRepo(pathPart) + hash, external: true }
  }

  // Wiki-style bare link: `[Samples](Samples)`.
  if (!pathPart.includes('/') && !path.extname(pathPart) && slugs.has(pathPart)) {
    return { href: `#/docs/${pathPart}${hash}`, external: false }
  }

  const isDir = pathPart.endsWith('/') || !path.extname(pathPart)
  return { href: (isDir ? GH_TREE : GH_BLOB) + toRepo(pathPart).replace(/\/$/, '') + hash, external: true }
}

function renderMarkdown(markdown: string, slugs: Set<string>, originDir: string): { html: string; headings: DocHeading[] } {
  const headings: DocHeading[] = []
  const marked = new Marked({ gfm: true, breaks: false })

  marked.use({
    renderer: {
      code({ text, lang }: Tokens.Code): string {
        const raw = (lang ?? '').split(/\s+/)[0]?.toLowerCase() ?? ''
        const key = raw in LANG_ALIASES ? LANG_ALIASES[raw]! : raw
        const body =
          key && hljs.getLanguage(key)
            ? hljs.highlight(text, { language: key, ignoreIllegals: true }).value
            : escapeHtml(text)
        const label = raw ? `<span class="code-lang">${escapeHtml(raw)}</span>` : ''
        return `<div class="code-block">${label}<pre><code class="hljs">${body}</code></pre></div>`
      },

      heading({ tokens, depth }: Tokens.Heading): string {
        const text = this.parser.parseInline(tokens)
        const plain = decodeEntities(text.replace(/<[^>]+>/g, ''))
        const base = slugifyHeading(plain)
        let id = base
        let n = 1
        while (headings.some((h) => h.id === id)) id = `${base}-${++n}`
        if (depth <= 3) headings.push({ id, text: plain, level: depth })
        return `<h${depth} id="${id}"><a class="anchor" href="#${id}" aria-hidden="true">#</a>${text}</h${depth}>\n`
      },

      link({ href, title, tokens }: Tokens.Link): string {
        const text = this.parser.parseInline(tokens)
        const r = resolveHref(href, slugs, originDir)
        const attrs = [
          `href="${escapeHtml(r.href)}"`,
          title ? `title="${escapeHtml(title)}"` : '',
          r.external ? 'target="_blank" rel="noreferrer noopener" class="ext"' : '',
        ]
          .filter(Boolean)
          .join(' ')
        return `<a ${attrs}>${text}</a>`
      },

      table({ header, rows }: Tokens.Table): string {
        const th = header.map((c) => `<th>${this.parser.parseInline(c.tokens)}</th>`).join('')
        const body = rows
          .map((row) => `<tr>${row.map((c) => `<td>${this.parser.parseInline(c.tokens)}</td>`).join('')}</tr>`)
          .join('')
        return `<div class="table-wrap"><table><thead><tr>${th}</tr></thead><tbody>${body}</tbody></table></div>`
      },
    },
  })

  return { html: marked.parse(markdown) as string, headings }
}

const stripTags = (html: string): string =>
  decodeEntities(
    html
      .replace(/<a class="anchor"[^>]*>[\s\S]*?<\/a>/g, '') // the "#" permalink glyphs
      .replace(/<[^>]+>/g, ' ')
  )
    .replace(/\s+/g, ' ')
    .trim()

/** The card summary should continue past the title, not repeat it. */
function summarize(text: string, title: string): string {
  const body = text.startsWith(title) ? text.slice(title.length) : text
  return body.replace(/^[\s—–-]+/, '').slice(0, 240).trim()
}

function deriveTitle(markdown: string, fallbackSlug: string): string {
  const m = markdown.match(/^#\s+(.+)$/m)
  return m?.[1] ? m[1].replace(/[*`]/g, '').trim() : fallbackSlug.replace(/-/g, ' ')
}

/** `003-Memory` -> `003`; `ADR-017-...` -> `ADR-017`. */
function deriveBadge(slug: string, category: DocCategory): string | null {
  if (category === 'spec') return slug.match(/^(\d{3})-/)?.[1] ?? null
  if (category === 'adr') return slug.match(/^(ADR-\d+)/)?.[1] ?? null
  return null
}

/** Short label for the sidebar, distinct from the long document title. */
function deriveNavLabel(slug: string, title: string, category: DocCategory): string {
  if (category === 'spec') return slug.replace(/^\d{3}-/, '').replace(/-/g, ' ')
  if (category === 'adr') {
    return slug
      .replace(/^ADR-\d+-/, '')
      .replace(/-/g, ' ')
      .replace(/\b\w/g, (c) => c.toUpperCase())
  }
  return title
}

// ----------------------------------------------------------------------- main

async function main(): Promise<void> {
  const sources = await collectSources()
  const slugs = new Set(sources.map((s) => s.slug))

  await rm(OUT, { recursive: true, force: true })
  await mkdir(OUT, { recursive: true })

  const index: DocIndexEntry[] = []

  for (const src of sources) {
    const markdown = await readFile(src.file, 'utf8')
    const origin = src.origin.split(path.sep).join('/')
    const originDir = path.posix.dirname(origin)
    const { html, headings } = renderMarkdown(markdown, slugs, originDir === '.' ? '' : originDir)

    // The display title may be a curated override; the summary has to strip whatever
    // heading the document itself opens with.
    const docTitle = deriveTitle(markdown, src.slug)
    const title = src.title ?? docTitle
    const text = stripTags(html)
    const badge = deriveBadge(src.slug, src.category)

    const page: DocPage = {
      slug: src.slug,
      title,
      category: src.category,
      badge,
      origin,
      githubUrl: GH_BLOB + origin,
      headings,
      html,
    }
    await writeFile(path.join(OUT, `${src.slug}.json`), JSON.stringify(page))

    index.push({
      slug: src.slug,
      title,
      navLabel: deriveNavLabel(src.slug, title, src.category),
      category: src.category,
      badge,
      origin,
      summary: summarize(text, docTitle),
      search: text.slice(0, 1400).toLowerCase(),
      readMinutes: Math.max(1, Math.round(text.split(' ').length / 220)),
    })
  }

  const counts = index.reduce<Record<string, number>>((acc, p) => {
    acc[p.category] = (acc[p.category] ?? 0) + 1
    return acc
  }, {})

  await writeFile(path.join(OUT, 'index.json'), JSON.stringify({ categories: CATEGORIES, counts, pages: index }))

  console.log(`[docs] ${index.length} pages -> public/docs/`)
  for (const [cat, n] of Object.entries(counts)) console.log(`       ${String(n).padStart(3)}  ${cat}`)
}

await main()
