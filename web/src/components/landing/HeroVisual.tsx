/**
 * The hot path, drawn: three producers enqueue into one intrusive MPSC mailbox,
 * a single activation drains it in FIFO order onto one worker lane.
 * Pure SVG + CSS keyframes — no runtime animation loop.
 */
export function HeroVisual() {
  const producers = [0, 1, 2]
  const slots = [0, 1, 2, 3, 4, 5]

  return (
    <div className="hero-visual" aria-hidden="true">
      <svg viewBox="0 0 520 300" className="flow" role="presentation">
        <defs>
          <linearGradient id="lane" x1="0" x2="1">
            <stop offset="0%" stopColor="var(--a1)" stopOpacity="0" />
            <stop offset="50%" stopColor="var(--a2)" stopOpacity="0.55" />
            <stop offset="100%" stopColor="var(--a3)" stopOpacity="0" />
          </linearGradient>
          <linearGradient id="msg" x1="0" x2="1">
            <stop offset="0%" stopColor="var(--a1)" />
            <stop offset="100%" stopColor="var(--a2)" />
          </linearGradient>
          <filter id="soft" x="-60%" y="-60%" width="220%" height="220%">
            <feGaussianBlur stdDeviation="7" />
          </filter>
        </defs>

        {/* producer lanes feeding the single tail_ exchange */}
        {producers.map((p) => {
          const y = 62 + p * 66
          return (
            <g key={p} className="lane" style={{ '--i': p } as React.CSSProperties}>
              <path d={`M56 ${y} H150 Q186 ${y} 186 ${y + (128 - y) * 0.5} T196 150`} className="lane-path" />
              <rect x="18" y={y - 15} width="42" height="30" rx="9" className="node producer" />
              <text x="39" y={y + 4} className="node-label">
                P{p + 1}
              </text>
              <circle r="4.5" className="packet" fill="url(#msg)">
                <animateMotion
                  dur={`${2.4 + p * 0.35}s`}
                  repeatCount="indefinite"
                  path={`M56 ${y} H150 Q186 ${y} 186 ${y + (128 - y) * 0.5} T196 150`}
                />
              </circle>
            </g>
          )
        })}

        {/* the mailbox: intrusive queue nodes, the descriptor IS the node */}
        <g className="mailbox">
          <rect x="196" y="120" width="176" height="60" rx="14" className="mailbox-body" />
          <rect x="196" y="120" width="176" height="60" rx="14" className="mailbox-glow" filter="url(#soft)" />
          {slots.map((s) => (
            <rect
              key={s}
              x={206 + s * 27}
              y="136"
              width="19"
              height="28"
              rx="5"
              className="slot"
              style={{ '--i': s } as React.CSSProperties}
            />
          ))}
          <text x="284" y="200" className="cap">
            mailbox · MPSC · FIFO
          </text>
        </g>

        {/* drain edge into the single activation */}
        <path d="M372 150 H424" className="lane-path drain" />
        <circle r="4.5" className="packet drain-packet" fill="url(#msg)">
          <animateMotion dur="1.1s" repeatCount="indefinite" path="M372 150 H424" />
        </circle>

        <g className="worker">
          <rect x="424" y="122" width="72" height="56" rx="14" className="node activation" />
          <text x="460" y="146" className="node-label strong">
            actor
          </text>
          <text x="460" y="164" className="node-label dim">
            1 executor
          </text>
          <text x="460" y="200" className="cap">
            worker lane
          </text>
        </g>

        <line x1="18" y1="238" x2="502" y2="238" stroke="url(#lane)" strokeWidth="1" />
        <text x="18" y="262" className="cap left">
          0 heap allocations
        </text>
        <text x="260" y="262" className="cap">
          0 cross-core RMW on drain
        </text>
        <text x="502" y="262" className="cap right">
          descriptor ≤ 64 B
        </text>
      </svg>
    </div>
  )
}
