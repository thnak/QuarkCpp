import { useEffect, useState } from 'react'
import type { DocIndex, DocPage } from '../types'
import type { Lang } from '../context/LangContext'

const asset = (file: string): string => `${import.meta.env.BASE_URL}docs/${file}`

const indexCache = new Map<Lang, Promise<DocIndex>>()
const pageCache = new Map<string, Promise<DocPage>>()

export function loadIndex(lang: Lang = 'en'): Promise<DocIndex> {
  let p = indexCache.get(lang)
  if (!p) {
    const file = lang === 'en' ? 'index.json' : 'index_vi.json'
    p = fetch(asset(file)).then((r) => {
      if (!r.ok) throw new Error(`docs index: HTTP ${r.status}`)
      return r.json() as Promise<DocIndex>
    })
    indexCache.set(lang, p)
  }
  return p
}

export function loadPage(slug: string, lang: Lang = 'en'): Promise<DocPage> {
  const cacheKey = `${lang}:${slug}`
  let p = pageCache.get(cacheKey)
  if (!p) {
    const file = lang === 'en' ? `${encodeURIComponent(slug)}.json` : `${encodeURIComponent(slug)}_vi.json`
    p = fetch(asset(file))
      .then((r) => {
        if (!r.ok && lang !== 'en') {
          // Fallback to English page if Vietnamese version is missing
          return fetch(asset(`${encodeURIComponent(slug)}.json`))
        }
        return r
      })
      .then((r) => {
        if (!r.ok) throw new Error(`Not found: ${slug}`)
        return r.json() as Promise<DocPage>
      })
    pageCache.set(cacheKey, p)
  }
  return p
}

interface Async<T> {
  data: T | null
  error: string | null
  loading: boolean
}

function useAsync<T>(run: () => Promise<T>, deps: unknown[]): Async<T> {
  const [state, setState] = useState<Async<T>>({ data: null, error: null, loading: true })

  useEffect(() => {
    let live = true
    setState({ data: null, error: null, loading: true })
    run()
      .then((data) => live && setState({ data, error: null, loading: false }))
      .catch((e: unknown) =>
        live ? setState({ data: null, error: e instanceof Error ? e.message : String(e), loading: false }) : undefined
      )
    return () => {
      live = false
    }
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, deps)

  return state
}

export const useDocsIndex = (lang: Lang = 'en'): Async<DocIndex> => useAsync(() => loadIndex(lang), [lang])

export const useDocPage = (slug: string | undefined, lang: Lang = 'en'): Async<DocPage> =>
  useAsync(() => (slug ? loadPage(slug, lang) : Promise.reject(new Error('no slug'))), [slug, lang])
