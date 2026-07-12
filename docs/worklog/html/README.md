# Worklog HTML

Hand-authored HTML renders of the worklogs in `../` (`*.md`). Open `index.html`.

Each page mirrors one worklog with diagrams/visuals drawn from its content
(flow diagrams, before/after bars, stat grids, callouts). **Each page is self-contained**
— CSS/JS/SVG all inlined, no external files — so any single `.html` renders on its own;
it also supports a light/dark theme + a per-page table of contents.
[`2026-07-11-esp32-mesh-swccmp-bulk-aes.html`](2026-07-11-esp32-mesh-swccmp-bulk-aes.html)
is the design reference — copy its `<style>` for new pages.
