/**
 * Fixed, non-interactive page backdrop: three drifting aurora blobs over a
 * perspective grid, with a fine noise layer to kill gradient banding.
 */
export function Backdrop() {
  return (
    <div className="backdrop" aria-hidden="true">
      <div className="backdrop-grid" />
      <span className="blob blob-1" />
      <span className="blob blob-2" />
      <span className="blob blob-3" />
      <div className="backdrop-noise" />
    </div>
  )
}
