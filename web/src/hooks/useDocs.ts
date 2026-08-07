import { useEffect, useState } from 'react'
import type { DocIndex, DocPage } from '../types'

const asset = (file: string): string => `${import.meta.env.BASE_URL}docs/${file}`

let indexPromise: Promise<DocIndex> | null = null
const pageCache = new Map<string, Promise<DocPage>>()

export function loadIndex(): Promise<DocIndex> {
  indexPromise ??= fetch(asset('index.json')).then((r) => {
    if (!r.ok) throw new Error(`docs index: HTTP ${r.status}`)
    return r.json() as Promise<DocIndex>
  })
  return indexPromise
}

export function loadPage(slug: string): Promise<DocPage> {
  let p = pageCache.get(slug)
  if (!p) {
    p = fetch(asset(`${encodeURIComponent(slug)}.json`)).then((r) => {
      if (!r.ok) throw new Error(`Not found: ${slug}`)
      return r.json() as Promise<DocPage>
    })
    pageCache.set(slug, p)
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

export const useDocsIndex = (): Async<DocIndex> => useAsync(loadIndex, [])

export const useDocPage = (slug: string | undefined): Async<DocPage> =>
  useAsync(() => (slug ? loadPage(slug) : Promise.reject(new Error('no slug'))), [slug])
