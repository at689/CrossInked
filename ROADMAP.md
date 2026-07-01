# CrossInked Roadmap

Designed-but-not-yet-implemented features, with concrete implementation sketches.
Deferred from the first pass because they are either UI-heavy, touch reading-progress
state (needs on-device validation), or change library-wide behavior — none should ship
without the testing the implemented features got. File:line references are approximate
and against the fork at time of writing.

## 4. Jump-to-letter / typeahead in the browser  (effort: M–L, risk: Med)
**Problem:** no search/alpha-jump anywhere; reaching "Wodehouse" in a 95-author
folder means paging the whole list.
**Sketch:** both browser modes expose random access (in-RAM `files` vector;
indexed via `FileIndex::entryAt`/`findRowByName`). Add a long-press-Confirm A–Z/0–9
picker; on selection, binary-search (RAM) or scan (index) for the first entry whose
first sort-relevant char matches and set `selectorIndex`. Reuse the page-jump input
plumbing in `FileBrowserActivity.cpp` (~`:840-857`). Must not collide with the existing
long-press page jump.

## 5. Content-key cache fallback — survive manual reorg  (effort: L, risk: Med-High)
**Problem:** all per-book state (progress, cover cache, bookmarks, clippings, stats)
is keyed on `fnvHash64(full path)` (`Epub.cpp:~170`). Renaming/renumbering files
directly on the SD card orphans that state; the book reopens at page 1.
**Sketch:** persist a stable content key (`dc:identifier`, else hash of title+author)
in `book.bin`'s header (`BookMetadataCache`, bump `BOOK_CACHE_VERSION`). On a cache
*miss*, do a bounded scan of `/.crosspoint/epub_*/book.bin` headers for a matching
content key; on hit, migrate the cache dir (reuse `BookMoveUtils.cpp:~39-75`) plus
bookmark/clipping migration hooks. Scan must be miss-only and capped so first-open
doesn't become O(N). **Test via the simulator**: open a book, rename its path, reopen,
assert progress/cover adopted rather than rebuilt. This is the highest-value
*correctness* fix but touches the state the user most fears losing — do it after a
cache-migration simulator test exists.

## 6. Primary-author / role-filtered author string  (effort: M, risk: Low-Med)
**Problem:** every `dc:creator` is joined with `", "` (`ContentOpfParser.cpp:~329-333`),
so Gutenberg books listing translators/editors as creators show compound authors.
**Sketch:** capture `opf:role` on each `<dc:creator>` start tag; collect creators with
role `aut` separately; use the aut-only join when any exist, else fall back to the
current all-creators join (safe default). EPUB3 `refines`→role needs a deferred second
pass. Bump `BOOK_CACHE_VERSION`. **Note:** changes author display library-wide — add a
driveable unit test for the OPF parser before shipping.

## 7. Cover-cache prewarm ("Rebuild covers")  (effort: M, risk: Low)
**Problem:** covers decode lazily; first browse of each book pays a JPEG→dither cost.
**Sketch:** new Settings → System maintenance action modeled on `ClearCacheActivity`;
walk `/books` (bounded, yielding) calling the existing `Epub` cover-generation path for
each EPUB. Read-only w.r.t. book files; only writes caches. Needs progress UI + cancel.

## 8. Orphan-cache reaper / audit  (effort: M, risk: Med — depends on #5)
**Problem:** manual reorg orphans `epub_*` dirs and per-folder `.idx` files that are
never GC'd.
**Sketch:** with content keys from #5, walk `/.crosspoint/epub_*` and drop dirs whose
source path no longer resolves *and* whose content key matches no book; drop
`fileindex/*.idx` for vanished source dirs. Add a "Cache health" readout (dir count,
orphan count, size). Must never delete a dir mapping to a live book.

## Explicitly not planned
- **No folder-count limit to raise.** There is no 100-folder cap; the only inflection
  is `INDEX_THRESHOLD = 200` (`FileBrowserActivity.cpp`), and crossing it degrades
  gracefully to an on-SD index.
- **Keep "Move finished → Read folder" OFF** — it flattens a curated Genre/Author/Series
  tree into a flat `/Read/`.
