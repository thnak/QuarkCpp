import { Hero } from '../components/landing/Hero'
import { StatStrip } from '../components/landing/StatStrip'
import { Features } from '../components/landing/Features'
import { ProofLoop } from '../components/landing/ProofLoop'
import { Performance } from '../components/landing/Performance'
import { CodeShowcase } from '../components/landing/CodeShowcase'
import { Comparison } from '../components/landing/Comparison'
import { Posture } from '../components/landing/Posture'
import { DocsTeaser } from '../components/landing/DocsTeaser'
import { QuickStart } from '../components/landing/QuickStart'

export function Landing() {
  return (
    <>
      <Hero />
      <StatStrip />
      <Features />
      <CodeShowcase />
      <ProofLoop />
      <Performance />
      <Comparison />
      <Posture />
      <DocsTeaser />
      <QuickStart />
    </>
  )
}
