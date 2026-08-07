import { useEffect, useState } from 'react'

const prefersReducedMotion = (): boolean =>
  typeof window !== 'undefined' && window.matchMedia('(prefers-reduced-motion: reduce)').matches

/** Eases a number from 0 to `target` once `active` flips true. */
export function useCountUp(target: number, active: boolean, durationMs = 1400): number {
  const [value, setValue] = useState(0)

  useEffect(() => {
    if (!active) return
    if (prefersReducedMotion()) {
      setValue(target)
      return
    }

    let raf = 0
    const start = performance.now()

    const tick = (now: number): void => {
      const t = Math.min(1, (now - start) / durationMs)
      // easeOutExpo — fast start, long settle.
      const eased = t === 1 ? 1 : 1 - Math.pow(2, -10 * t)
      setValue(target * eased)
      if (t < 1) raf = requestAnimationFrame(tick)
    }

    raf = requestAnimationFrame(tick)
    return () => cancelAnimationFrame(raf)
  }, [target, active, durationMs])

  return value
}
