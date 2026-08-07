const PATHS: Record<string, React.ReactNode> = {
  header: (
    <>
      <path d="M4 6h16M4 12h10M4 18h13" />
      <circle cx="18.5" cy="12" r="1.6" />
    </>
  ),
  split: (
    <>
      <path d="M3 12h5l3-6h10M8 12l3 6h10" />
      <circle cx="20" cy="6" r="1.6" />
      <circle cx="20" cy="18" r="1.6" />
    </>
  ),
  policy: (
    <>
      <rect x="4" y="4" width="7" height="7" rx="2" />
      <rect x="13" y="4" width="7" height="7" rx="2" />
      <rect x="8.5" y="14" width="7" height="7" rx="2" />
      <path d="M7.5 11v2h9v-2" />
    </>
  ),
  sched: (
    <>
      <rect x="3" y="5" width="18" height="5" rx="2" />
      <rect x="3" y="14" width="11" height="5" rx="2" />
      <path d="M17 16.5h4" />
    </>
  ),
  fanout: (
    <>
      <circle cx="5" cy="12" r="2.2" />
      <circle cx="19" cy="5.5" r="2.2" />
      <circle cx="19" cy="12" r="2.2" />
      <circle cx="19" cy="18.5" r="2.2" />
      <path d="M7.2 12h3.3M10.5 12c3 0 3-6.5 6.3-6.5M10.5 12h6.3M10.5 12c3 0 3 6.5 6.3 6.5" />
    </>
  ),
  stream: (
    <>
      <path d="M3 8h13a4 4 0 0 1 0 8H6" />
      <path d="M8.5 12.5 6 16l2.5 3.5" />
      <path d="M20 6.5h1" />
    </>
  ),
  cluster: (
    <>
      <circle cx="12" cy="12" r="3" />
      <circle cx="5" cy="6" r="2" />
      <circle cx="19" cy="6" r="2" />
      <circle cx="5" cy="18" r="2" />
      <circle cx="19" cy="18" r="2" />
      <path d="M6.6 7.4 10 10.2M17.4 7.4 14 10.2M6.6 16.6 10 13.8M17.4 16.6 14 13.8" />
    </>
  ),
  store: (
    <>
      <ellipse cx="12" cy="6" rx="7.5" ry="3" />
      <path d="M4.5 6v12c0 1.7 3.4 3 7.5 3s7.5-1.3 7.5-3V6" />
      <path d="M4.5 12c0 1.7 3.4 3 7.5 3s7.5-1.3 7.5-3" />
    </>
  ),
  shield: (
    <>
      <path d="M12 3 5 6v6c0 4.2 2.9 7.6 7 9 4.1-1.4 7-4.8 7-9V6l-7-3Z" />
      <path d="M9 12.2l2.2 2.2L15.5 10" />
    </>
  ),
  gauge: (
    <>
      <path d="M4 16a8 8 0 1 1 16 0" />
      <path d="M12 16l4.2-4.6" />
      <circle cx="12" cy="16" r="1.4" />
    </>
  ),
  lock: (
    <>
      <rect x="4.5" y="10" width="15" height="10" rx="3" />
      <path d="M8 10V7.5a4 4 0 0 1 8 0V10" />
      <path d="M12 14v2.5" />
    </>
  ),
  sim: (
    <>
      <rect x="3" y="4" width="18" height="14" rx="3" />
      <path d="M3 8.5h18M8 21h8" />
      <path d="M7.5 13.5l2-2.2 2 2.2 2-3.4 2 3.4" />
    </>
  ),
}

export function FeatureIcon({ name }: { name: string }) {
  return (
    <svg
      className="feature-icon"
      viewBox="0 0 24 24"
      width="22"
      height="22"
      fill="none"
      stroke="currentColor"
      strokeWidth="1.6"
      strokeLinecap="round"
      strokeLinejoin="round"
      aria-hidden="true"
    >
      {PATHS[name] ?? PATHS.header}
    </svg>
  )
}
