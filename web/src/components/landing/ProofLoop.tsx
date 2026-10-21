import { Link } from 'react-router-dom'
import { PROOF_STEPS, MAILBOX_ROUNDS } from '../../data/content'
import { Reveal, SectionHead } from '../primitives'
import { useLang } from '../../hooks/useLang'
import { TRANSLATIONS } from '../../data/translations'

export function ProofLoop() {
  const { lang } = useLang()
  const t = TRANSLATIONS[lang]

  return (
    <section className="section proof" id="proof">
      <div className="wrap">
        <SectionHead
          eyebrow={lang === 'vi' ? 'Phương pháp luận' : 'How decisions get made'}
          title={
            lang === 'vi' ? (
              <>
                Không tuyên bố suông. Chỉ đưa vào <span className="grad-text">bằng chứng thực tế</span>.
              </>
            ) : (
              <>
                Claims don&apos;t ship here. <span className="grad-text">Proofs</span> do.
              </>
            )
          }
          lede={
            lang === 'vi'
              ? 'Khi một lựa chọn hot-path hoặc an toàn cần nhiều hơn lời tranh luận, nó phải đi qua quy trình thiết kế → phản biện → thực chứng → đánh giá. Các thiết kế cạnh tranh được cài đặt bằng C++23 thực tế, biên dịch qua GCC + Clang, chạy sanitizer và benchmark trước khi chọn phương án tối ưu.'
              : 'Where a hot-path or safety-critical choice needs more than argument, it goes through a design → red-team → prove → judge loop. Competing designs are implemented in real C++23, compiled under GCC and Clang, run under sanitizers and benchmarked before a judge picks a winner.'
          }
        />

        <ol className="proof-steps">
          {PROOF_STEPS.map((s, i) => {
            const stepItem = t.proof.steps[i] ?? { step: s.step, title: s.step, body: s.body }
            return (
              <Reveal key={s.step} as="li" delay={i * 110} className="proof-step">
                <div className="proof-num">
                  <span>{String(i + 1).padStart(2, '0')}</span>
                </div>
                <h3 className="proof-title">{stepItem.step}</h3>
                <p className="proof-body">{stepItem.body}</p>
                {i < PROOF_STEPS.length - 1 ? <span className="proof-connector" aria-hidden="true" /> : null}
              </Reveal>
            )
          })}
        </ol>

        <Reveal className="proof-outcome" delay={120}>
          <div className="outcome-main">
            <div className="outcome-figure">
              <span className="outcome-num">{MAILBOX_ROUNDS}</span>
              <span className="outcome-unit">{lang === 'vi' ? 'vòng' : 'rounds'}</span>
            </div>
            <div>
              <h3 className="outcome-title">
                {lang === 'vi' ? 'Hàng chờ mailbox đã vượt qua mười một đợt thử thách.' : 'The mailbox has been challenged eleven times.'}
              </h3>
              <p className="outcome-body">
                {lang === 'vi'
                  ? 'Mỗi đợt phản biện đưa MPSC Vyukov intrusive hiện tại ra thi đấu với một đối thủ được thiết kế riêng. Mọi đối thủ đều bị loại vì vi phạm an toàn hoặc không đạt băng thông cam kết. Thiết kế hiện tại chiến thắng qua cả 11 vòng đấu.'
                  : 'Each round pits the incumbent intrusive Vyukov MPSC against a purpose-built challenger aimed at a specific residual — oldest-message-discovery cost, multi-producer scaling, hazard-pointer reclamation. Every challenger has been disqualified on safety or a falsified throughput claim. The incumbent has survived all eleven, with only host-noise-conditional misses, none safety-affecting.'}
              </p>
              <div className="outcome-links">
                <Link className="text-link" to="/docs/ADR-041-mailbox-mpsc-hot-path-r11-judgment">
                  {lang === 'vi' ? 'Đọc báo cáo vòng 11 →' : 'Read round 11 →'}
                </Link>
                <Link className="text-link" to="/docs?category=adr">
                  {lang === 'vi' ? 'Tất cả quyết định ADR →' : 'All decision records →'}
                </Link>
              </div>
            </div>
          </div>
        </Reveal>
      </div>
    </section>
  )
}
