import { Link } from 'react-router-dom'
import { PROOF_STEPS, MAILBOX_ROUNDS } from '../../data/content'
import { Reveal, SectionHead } from '../primitives'

export function ProofLoop() {
  return (
    <section className="section proof" id="proof">
      <div className="wrap">
        <SectionHead
          eyebrow="How decisions get made"
          title={
            <>
              Claims don&apos;t ship here. <span className="grad-text">Proofs</span> do.
            </>
          }
          lede="Where a hot-path or safety-critical choice needs more than argument, it goes through a design → red-team → prove → judge loop. Competing designs are implemented in real C++23, compiled under GCC and Clang, run under sanitizers and benchmarked before a judge picks a winner."
        />

        <ol className="proof-steps">
          {PROOF_STEPS.map((s, i) => (
            <Reveal key={s.step} as="li" delay={i * 110} className="proof-step">
              <div className="proof-num">
                <span>{String(i + 1).padStart(2, '0')}</span>
              </div>
              <h3 className="proof-title">{s.step}</h3>
              <p className="proof-body">{s.body}</p>
              {i < PROOF_STEPS.length - 1 ? <span className="proof-connector" aria-hidden="true" /> : null}
            </Reveal>
          ))}
        </ol>

        <Reveal className="proof-outcome" delay={120}>
          <div className="outcome-main">
            <div className="outcome-figure">
              <span className="outcome-num">{MAILBOX_ROUNDS}</span>
              <span className="outcome-unit">rounds</span>
            </div>
            <div>
              <h3 className="outcome-title">The mailbox has been challenged eleven times.</h3>
              <p className="outcome-body">
                Each round pits the incumbent intrusive Vyukov MPSC against a purpose-built challenger aimed at a
                specific residual — oldest-message-discovery cost, multi-producer scaling, hazard-pointer
                reclamation. Every challenger has been disqualified on safety or a falsified throughput claim.
                The incumbent has survived all eleven, with only host-noise-conditional misses, none safety-affecting.
              </p>
              <div className="outcome-links">
                <Link className="text-link" to="/docs/ADR-041-mailbox-mpsc-hot-path-r11-judgment">
                  Read round 11 →
                </Link>
                <Link className="text-link" to="/docs?category=adr">
                  All decision records →
                </Link>
              </div>
            </div>
          </div>
        </Reveal>
      </div>
    </section>
  )
}
