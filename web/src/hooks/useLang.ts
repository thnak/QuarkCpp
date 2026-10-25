import { useContext } from 'react'
import { LangContext, type Lang } from '../context/LangContext'

export type { Lang }

export function useLang() {
  const context = useContext(LangContext)
  if (!context) {
    throw new Error('useLang must be used within a LangProvider')
  }
  return context
}
