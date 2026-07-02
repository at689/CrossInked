# CrossInked Roadmap

Remaining designed-but-not-yet-implemented features, with concrete implementation
sketches. Features #1–#5, #7, #9 are implemented (see [CROSSINKED.md](CROSSINKED.md));
#8 was reframed by #5's approach. File:line references are approximate.

> The July 2026 audit fix wave landed first (see `docs/audit-2026-07.md`
> resolution table): #5's sidecar now carries a source-size field and there is a
> session-scoped orphan index in `Epub.cpp` — both directly reusable by #8.

## 6. Primary-author / role-filtered author string  (effort: M, risk: Low-Med)
**Problem:** every `dc:creator` is joined with `", "` (`ContentOpfParser.cpp:~329-333`),
so Gutenberg books listing translators/editors as creators show compound authors.
**Sketch:** capture `opf:role` on each `<dc:creator>` start tag; collect creators with
role `aut` separately; use the aut-only join when any exist, else fall back to the
current all-creators join (safe default). EPUB3 `refines`→role needs a deferred second
pass. Bump `BOOK_CACHE_VERSION`. **Note:** changes author display library-wide — add a
driveable unit test for the OPF parser before shipping (this is why it wasn't shipped
in the first pass).

## 8. Orphan-cache reaper / audit  (effort: M, risk: Med)
**Problem:** manual reorg can still orphan `epub_*` dirs (when the same book isn't
reopened to trigger #5's adopt) and per-folder `.idx` files that are never GC'd.
**Sketch:** #5 already writes a `content.key` sidecar (key + source path) in every
cache dir — reuse it. Add a Settings → System "Cache health" action that walks
`/.crosspoint/epub_*`, and drops dirs whose `content.key` source path no longer
resolves *and* whose key matches no book under `/books`; drop `fileindex/*.idx` for
vanished source dirs. Show a readout (dir count, orphan count, total size). Must never
delete a dir whose source still resolves. Model the UI on `ClearCacheActivity` /
`PrewarmCoversActivity`.

## Explicitly not planned
- **No folder-count limit to raise.** There is no 100-folder cap; the only inflection
  is `INDEX_THRESHOLD = 200` (`FileBrowserActivity.cpp`), and crossing it degrades
  gracefully to an on-SD index.
- **Keep "Move finished → Read folder" OFF** — it flattens a curated Genre/Author/Series
  tree into a flat `/Read/`.
