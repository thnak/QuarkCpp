import { useEffect, useRef, useState } from 'react'

/**
 * Fires once when the element scrolls into view. Used for reveal animations and
 * for kicking off count-ups only when the number is actually on screen.
 */
export function useInView<T extends HTMLElement>(rootMargin = '0px 0px -12% 0px'): [React.RefObject<T | null>, boolean] {
  const ref = useRef<T>(null)
  const [inView, setInView] = useState(false)

  useEffect(() => {
    const el = ref.current
    if (!el) return
    if (typeof IntersectionObserver === 'undefined') {
      setInView(true)
      return
    }

    const io = new IntersectionObserver(
      (entries) => {
        for (const entry of entries) {
          if (entry.isIntersecting) {
            setInView(true)
            io.disconnect()
          }
        }
      },
      { rootMargin, threshold: 0.05 }
    )
    io.observe(el)
    return () => io.disconnect()
  }, [rootMargin])

  return [ref, inView]
}
