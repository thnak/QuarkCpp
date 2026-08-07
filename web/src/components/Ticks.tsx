import { Fragment } from 'react'

/**
 * Renders `back-ticked` spans in plain copy as inline code, so the marketing
 * strings can stay readable markdown-ish text in `data/content.ts`.
 */
export function Ticks({ children }: { children: string }) {
  const parts = children.split(/`([^`]+)`/g)
  return (
    <>
      {parts.map((part, i) =>
        i % 2 === 1 ? (
          <code key={i}>{part}</code>
        ) : (
          <Fragment key={i}>{part}</Fragment>
        )
      )}
    </>
  )
}
