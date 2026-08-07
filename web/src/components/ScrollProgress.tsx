import { useEffect, useState } from 'react'

/** Thin gradient bar under the nav showing how far down the page you are. */
export function ScrollProgress() {
  const [pct, setPct] = useState(0)

  useEffect(() => {
    let raf = 0
    const update = (): void => {
      raf = 0
      const max = document.documentElement.scrollHeight - window.innerHeight
      setPct(max > 0 ? Math.min(1, window.scrollY / max) : 0)
    }
    const onScroll = (): void => {
      raf ||= requestAnimationFrame(update)
    }
    update()
    window.addEventListener('scroll', onScroll, { passive: true })
    window.addEventListener('resize', onScroll)
    return () => {
      window.removeEventListener('scroll', onScroll)
      window.removeEventListener('resize', onScroll)
      if (raf) cancelAnimationFrame(raf)
    }
  }, [])

  return <div className="scroll-progress" style={{ transform: `scaleX(${pct})` }} aria-hidden="true" />
}
