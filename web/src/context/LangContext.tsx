import { createContext, useCallback, useEffect, useState, type ReactNode } from 'react'

export type Lang = 'en' | 'vi'

interface LangContextType {
  lang: Lang
  setLang: (lang: Lang) => void
  toggleLang: () => void
}

const KEY = 'quark-lang'

function initialLang(): Lang {
  if (typeof window === 'undefined') return 'en'
  const stored = window.localStorage.getItem(KEY)
  if (stored === 'en' || stored === 'vi') return stored
  const navLang = window.navigator.language.toLowerCase()
  return navLang.startsWith('vi') ? 'vi' : 'en'
}

export const LangContext = createContext<LangContextType | null>(null)

export function LangProvider({ children }: { children: ReactNode }) {
  const [lang, setLangState] = useState<Lang>(initialLang)

  useEffect(() => {
    document.documentElement.lang = lang
    window.localStorage.setItem(KEY, lang)
  }, [lang])

  const setLang = useCallback((nextLang: Lang) => {
    setLangState(nextLang)
  }, [])

  const toggleLang = useCallback(() => {
    setLangState((l) => (l === 'en' ? 'vi' : 'en'))
  }, [])

  return (
    <LangContext.Provider value={{ lang, setLang, toggleLang }}>
      {children}
    </LangContext.Provider>
  )
}
