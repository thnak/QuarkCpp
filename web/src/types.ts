export type DocCategory = 'guide' | 'reference' | 'spec' | 'adr'

export interface DocHeading {
  id: string
  text: string
  level: number
}

/** One fully rendered page — fetched lazily from `public/docs/<slug>.json`. */
export interface DocPage {
  slug: string
  title: string
  category: DocCategory
  badge: string | null
  origin: string
  githubUrl: string
  headings: DocHeading[]
  html: string
}

/** One entry in the always-loaded corpus index that drives nav + search. */
export interface DocIndexEntry {
  slug: string
  title: string
  navLabel: string
  category: DocCategory
  badge: string | null
  origin: string
  summary: string
  search: string
  readMinutes: number
}

export interface DocCategoryMeta {
  label: string
  blurb: string
  order: number
}

export interface DocIndex {
  categories: Record<DocCategory, DocCategoryMeta>
  counts: Partial<Record<DocCategory, number>>
  pages: DocIndexEntry[]
}
