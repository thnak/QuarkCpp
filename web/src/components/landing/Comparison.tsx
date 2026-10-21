import { CHOOSE_CAF, CHOOSE_QUARK, COMPARISON_ROWS, REPO_URL } from '../../data/content'
import { Reveal, SectionHead } from '../primitives'
import { Ticks } from '../Ticks'
import { useLang } from '../../hooks/useLang'
import { TRANSLATIONS } from '../../data/translations'

export function Comparison() {
  const { lang } = useLang()
  const t = TRANSLATIONS[lang]

  const chooseQuark = lang === 'vi' ? [
    'Tải trọng chính là `ask` / `tell` đơn-actor có thể dự đoán được, zero-allocation và latency cực thấp',
    'Actor định kiểu compile-time — một giao thức cố định cho mỗi actor được trình biên dịch kiểm tra, không RTTI',
    'Mọi khẳng định an toàn và hot-path đều được chứng minh bằng mã thực thi chứ không phải assertion',
    'Kiểm thử mô phỏng định tính cho bơm lỗi không cần thời gian thực hay thread thực',
  ] : CHOOSE_QUARK

  const chooseCAF = lang === 'vi' ? [
    'Băng thông tổng hợp cao khi có nhiều producer đồng thời và mailbox bị tranh chấp',
    'Truyền tin linh hoạt runtime — một actor xử lý nhiều dạng bản tin khác nhau, `become` / `unbecome` hệ sinh thái',
    'Hệ sinh thái lớn có sẵn, lịch sử sử dụng sản xuất thực tế và độ chín qua thử thách',
    'Bộ công cụ reactive-streaming trưởng thành và hỗ trợ nền tảng rộng rãi',
  ] : CHOOSE_CAF

  return (
    <section className="section compare" id="compare">
      <div className="wrap">
        <SectionHead
          eyebrow={lang === 'vi' ? 'So sánh khách quan' : 'Honest comparison'}
          title={
            lang === 'vi' ? (
              <>
                Đối đầu trực tiếp với <span className="grad-text">CAF</span> — minh bạch mọi lưu ý.
              </>
            ) : (
              <>
                Head to head with <span className="grad-text">CAF</span> — caveats included.
              </>
            )
          }
          lede={
            lang === 'vi'
              ? 'Mục đích so sánh là kiểm tra các tuyên bố hot-path của Quark đối chiếu với giải pháp phổ biến, không nhằm tuyên bố chiến thắng tuyệt đối. Hai engine đứng ở các điểm khác nhau trên phổ latency-vs-tính tổng quát.'
              : "The point of the comparison is to sanity-check Quark's hot-path claims against an established alternative, not to crown a universal winner. The two engines sit at different points on the latency-vs-generality spectrum."
          }
        />

        <Reveal className="caveat" delay={60}>
          <span className="caveat-mark" aria-hidden="true">
            !
          </span>
          <div>
            <strong>{lang === 'vi' ? 'Đây không phải bài benchmark khập khiễng, và báo cáo nêu rõ điều đó.' : 'This is not an apples-to-apples benchmark, and the writeup says so.'}</strong>{' '}
            {lang === 'vi'
              ? 'Bản ask của CAF chạy qua mailbox đa năng; ask của Quark tối ưu slot phản hồi single-shot riêng biệt nhờ actor định kiểu tại thời điểm biên dịch. Hãy nhìn vào xu hướng và cơ chế cốt lõi.'
              : 'CAF’s ask runs through a general-purpose, fully dynamic mailbox; Quark’s bypasses the mailbox via a dedicated single-shot reply slot because its actors are compile-time-typed.'}
          </div>
        </Reveal>

        <Reveal className="compare-table" delay={100}>
          {COMPARISON_ROWS.map((r) => (
            <div key={r.dimension} className="compare-row">
              <span className="compare-dim">{r.dimension}</span>
              <span className={`compare-winner ${r.winner.toLowerCase()}`}>{r.winner}</span>
              <span className="compare-margin">{r.margin}</span>
            </div>
          ))}
        </Reveal>

        <div className="choose-grid">
          <Reveal className="choose choose-quark" delay={60}>
            <h3>{t.compare.chooseQuarkTitle}</h3>
            <ul>
              {chooseQuark.map((c) => (
                <li key={c}>
                  <Ticks>{c}</Ticks>
                </li>
              ))}
            </ul>
          </Reveal>
          <Reveal className="choose" delay={140}>
            <h3>{t.compare.chooseCAFTitle}</h3>
            <ul>
              {chooseCAF.map((c) => (
                <li key={c}>
                  <Ticks>{c}</Ticks>
                </li>
              ))}
            </ul>
          </Reveal>
        </div>

        <Reveal delay={160}>
          <a
            className="text-link"
            href={`${REPO_URL}/blob/master/bench/caf_comparison/README.md`}
            target="_blank"
            rel="noreferrer noopener"
          >
            {lang === 'vi' ? 'Phương pháp luận đầy đủ và dẫn chứng từng dòng mã nguồn →' : 'Full methodology, every disclosed asymmetry with source-line citations →'}
          </a>
        </Reveal>
      </div>
    </section>
  )
}
