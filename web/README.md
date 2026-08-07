# web/ — the Quark marketing site + embedded wiki

A React 19 + TypeScript + Vite app. It builds into **`../docs/`**, which is what GitHub Pages
serves. Nothing here is part of the engine; the C++ build never sees this directory.

## Commands

```bash
cd web
npm install          # once
npm run dev          # local dev server (regenerates the doc corpus first)
npm run build        # tsc -b + vite build -> ../docs
npm run preview      # serve the built ../docs
npm run typecheck    # tsc -b only
npm run docs         # regenerate the doc corpus only
```

## How the wiki gets embedded

`scripts/build-wiki.ts` renders the repository's markdown into JSON that the app fetches at
runtime. It runs automatically before `dev` and `build`.

Sources, in precedence order:

1. **repo-root markdown** — `NNN-*.md` (the 28 RFC specs), `decisions/ADR-*.md`, and the
   reference docs (`ActorEngineSpecification`, `CONVENTIONS`, `VERIFICATION`, `PERFORMANCE`,
   `OpenQuestions`, `PersistenceAdapters`, `TAIL-CONTENTION`). These are authoritative and
   always current.
2. **`../wiki/*.md`** — only the wiki-owned guide pages that have no repo-root twin
   (`How-To-Write-Your-First-Actor`, `Samples`, `Benchmarks`, `Contributing`, `Project-Status`).

Root-first precedence is deliberate: `wiki/` also holds an older snapshot of the specs and the
first 19 ADRs, and rendering those would publish stale text. Anything sourced from `wiki/` is
still reproduced verbatim — see the staleness note below.

Output (git-ignored, regenerated on every build):

- `public/docs/index.json` — slug, title, category, summary and search text for every page;
  drives the sidebar, the browse view and the client-side search.
- `public/docs/<slug>.json` — the rendered HTML plus its heading outline, fetched lazily so the
  initial bundle stays small.

Markdown links are rewritten at build time: a link to any document in the corpus becomes an
in-site route (`#/docs/<slug>`), and everything else becomes a GitHub blob/tree URL. Readers
never leave the site to read a spec or an ADR.

### Adding a document

Drop the markdown in the repo (or `wiki/`) and rebuild — the corpus is discovered by glob. To
give a wiki-only page a curated title or category, add it to `WIKI_ONLY` in
`scripts/build-wiki.ts`.

## Routing

Hash routing (`#/`, `#/docs`, `#/docs/<slug>`), because GitHub Pages serves static files with no
rewrite rules and a deep link like `/docs/003-Memory` would 404. `vite.config.ts` sets
`base: './'` so the build works both at `https://thnak.github.io/QuarkCpp/` and from any local
static server.

## Publishing

The built output in `../docs/` is committed. Enable it once in the repository settings:

**Settings → Pages → Source: Deploy from a branch → Branch: `master`, folder: `/docs`.**

After changing anything under `web/`, run `npm run build` and commit the regenerated `../docs/`
along with your source change, or the published site will lag the source.

## Content accuracy

Marketing copy lives in `src/data/content.ts`. Every figure there is transcribed from the
repository's own records — `README.md`, `PERFORMANCE.md`, `VERIFICATION.md` and
`bench/caf_comparison/README.md` — including the CAF comparison's disclosed asymmetries. Keep it
that way: if a number changes in the repo, change it here in the same commit.

### Known staleness in `wiki/`

The five wiki-only pages predate the current repo state and still say **153 tests / 16 samples /
27 specs / 19 ADRs** where the repository is at **194 / 22 / 28 / 46**. They render verbatim, so
those figures appear in the site's guide pages and contradict the landing page. Fixing that means
editing `../wiki/*.md`; the site needs no change.
