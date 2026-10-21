import { Link } from 'react-router-dom'
import { QUICKSTART, REPO_URL } from '../../data/content'
import { Reveal, SectionHead } from '../primitives'
import { CodeBlock } from '../CodeBlock'
import { useLang } from '../../hooks/useLang'
import { TRANSLATIONS } from '../../data/translations'

const QUICKSTART_VI = `# Biên dịch & chạy toàn bộ cổng kiểm thử tính đúng đắn (Release)
cmake -S . -B build
cmake --build build
ctest --test-dir build --output-on-failure   # 194 / 194

# Sanitizers (cùng bộ suite, trừ các trường hợp ngoại lệ theo thiết kế)
cmake -S . -B build-asan -DQUARK_SANITIZE="address;undefined"
cmake -S . -B build-tsan -DQUARK_SANITIZE="thread"

# Benchmarks (mặc định BẬT) và các sample có thể chạy (mặc định TẮT)
cmake -S . -B build -DQUARK_BUILD_SAMPLES=ON
build/samples/01_hello_counter    # in OK / exit 0`

export function QuickStart() {
  const { lang } = useLang()
  const t = TRANSLATIONS[lang]

  return (
    <section className="section start" id="start">
      <div className="wrap start-grid">
        <div>
          <SectionHead
            eyebrow={t.quickstart.sectionTitle}
            title={
              lang === 'vi' ? (
                <>
                  Clone, biên dịch và theo dõi <span className="grad-text">194 kiểm thử</span> vượt qua thành công.
                </>
              ) : (
                <>
                  Clone, build, and watch <span className="grad-text">194 tests</span> go green.
                </>
              )
            }
            lede={
              lang === 'vi'
                ? 'Yêu cầu CMake ≥ 3.24 và trình biên dịch C++23 — đã xác minh trên g++ 14.2 và clang 20.1.'
                : 'Requires CMake ≥ 3.24 and a C++23 compiler — verified on g++ 14.2 and clang 20.1.'
            }
          />

          <Reveal className="start-links" delay={140}>
            <Link className="btn btn-primary" to="/docs/How-To-Write-Your-First-Actor">
              {lang === 'vi' ? 'Hướng dẫn actor đầu tiên' : 'First-actor walkthrough'} <span className="cta-arrow">→</span>
            </Link>
            <a className="btn btn-ghost" href={REPO_URL} target="_blank" rel="noreferrer noopener">
              {lang === 'vi' ? 'Clone kho mã nguồn' : 'Clone the repository'}
            </a>
          </Reveal>
        </div>

        <Reveal delay={80}>
          <CodeBlock code={lang === 'vi' ? QUICKSTART_VI : QUICKSTART} language="bash" filename="terminal" className="start-code" />
        </Reveal>
      </div>
    </section>
  )
}
