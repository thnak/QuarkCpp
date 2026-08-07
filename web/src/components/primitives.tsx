import type { ElementType, ReactNode, MouseEvent } from 'react'
import { useInView } from '../hooks/useInView'

interface RevealProps {
  children: ReactNode
  as?: ElementType
  delay?: number
  className?: string
  id?: string
}

/** Fades + lifts its children into place the first time they scroll into view. */
export function Reveal({ children, as: Tag = 'div', delay = 0, className = '', id }: RevealProps) {
  const [ref, inView] = useInView<HTMLDivElement>()
  return (
    <Tag
      ref={ref}
      id={id}
      className={`reveal ${inView ? 'in' : ''} ${className}`.trim()}
      style={{ '--delay': `${delay}ms` } as React.CSSProperties}
    >
      {children}
    </Tag>
  )
}

/** A card that tracks the cursor so its highlight follows the pointer. */
export function SpotlightCard({ children, className = '' }: { children: ReactNode; className?: string }) {
  const onMove = (e: MouseEvent<HTMLDivElement>): void => {
    const r = e.currentTarget.getBoundingClientRect()
    e.currentTarget.style.setProperty('--mx', `${e.clientX - r.left}px`)
    e.currentTarget.style.setProperty('--my', `${e.clientY - r.top}px`)
  }
  return (
    <div className={`card ${className}`.trim()} onMouseMove={onMove}>
      {children}
    </div>
  )
}

/** Section header: eyebrow + title + optional lede, revealed as a group. */
export function SectionHead({
  eyebrow,
  title,
  lede,
  center = false,
}: {
  eyebrow: string
  title: ReactNode
  lede?: ReactNode
  center?: boolean
}) {
  return (
    <Reveal className={`section-head ${center ? 'center' : ''}`.trim()}>
      <span className="eyebrow">{eyebrow}</span>
      <h2 className="section-title">{title}</h2>
      {lede ? <p className="section-lede">{lede}</p> : null}
    </Reveal>
  )
}
