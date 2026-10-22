# DEV NOTE (HARD): GUI output verification — every headless dump/readout must match what the REAL GUI displays

Status: incident-driven (2026-10-19, the CVBS R/B texture swap: three full
headless verification rounds green while the GUI displayed mirrored hues).
Read this before verifying ANY panel/decoder output change, and before
touching ANY render-path pixel conversion (RGB24/RGBA/Color packing, texture
upload paths, display crops).

## The incident (why this note exists)

2026-10-19, the CVBS colour decode: the RGB24 -> texture conversion packed
raw uint32s (`0xFF000000 | r<<16 | g<<8 | b`). Little-endian memory for that
value is the byte sequence **b,g,r,a** — but the image buffer is raylib
`Color` memory (r,g,b,a bytes; `PIXELFORMAT_UNCOMPRESSED_R8G8B8A8`). The GUI
displayed the decoded BLUE as red and RED as blue: yellow<->cyan, red<->blue
(white/green/magenta/black unchanged — the mirror only shows on
red/blue-asymmetric hues, which is what made it look like a "phase" bug).

THREE consecutive headless verification rounds were green (per-bar U/V
angles vs the EBU vectors, PPM/PNG dumps, the synthetic suite, the full
meson suite) because EVERY verification path read the decoder's RGB24
buffers directly and BYPASSED the conversion. The user visually confirmed
the PNG correct while the GUI was wrong. The bug was only found by the
user's GUI bar-order report (White/Cyan/Yellow/... instead of
White/Yellow/Cyan/...) plus comparing the packing against the RF preview's
byte-wise rgba convention (`rf_render_rows`, colour user-verified).

## HARD RULES

1. A headless dump/readout only verifies the GUI if it reads what the GUI
   reads. Verifying an intermediate decoder buffer (pre-conversion) proves
   NOTHING about the display. Verification dumps must come from the final
   GUI-consumed buffer (post-conversion: the frame image / texture data),
   or the conversion itself must carry its own test (rule 2).
2. Every pixel-format/packing conversion between decoder buffers and the
   GUI texture is CODE UNDER TEST, not glue: keep it a pure function and
   give it a byte-layout contract (known RGB input -> asserted output
   bytes; known pitch/stride; an incident-discriminator pixel). The test
   and any packing edit land in the SAME change — a packing change without
   its test edit is the exact mistake this note forbids.
3. Write conversions through TYPE-SAFE struct fields (raylib `Color`
   `.r/.g/.b/.a`) or byte-wise arrays — never hand-packed uint32 literals.
   A packed uint32 in a little-endian build silently reverses the byte
   order the GPU format expects; the arithmetic LOOKS right in review and
   is wrong in memory.
4. A numeric proxy (angles/levels/counts) is not a GUI verdict: for any
   "colours/levels correct" claim, the render-path conversion must be
   separately accounted for in the same verification round (rule 1 or 2) —
   green numerics + an untested render path = this incident.
5. When adding a new panel/decoder output or display mode: list in the
   change's prompt log every buffer between the decode and the screen,
   which one the verification reads, and what covers the remaining
   conversions. If any link is uncovered, say so — do not report the
   output as verified.

## Contracts in force

- `misrc_tools/misrc_gui/signal/gui_cvbs.c`: `cvbs_blit_rgb24_to_color`
  (the pure RGB24 -> Color blit the render path calls) +
  `misrc_tools/test/gui_cvbs_test.c`'s render-path conversion contract
  (distinct-channel pixels, the incident's yellow-vs-cyan discriminator,
  byte assertions on `.r/.g/.b/.a`, pitch/stride checks). Moving or editing
  the blit REQUIRES editing the contract with it.
- `misrc_tools/misrc_gui/signal/gui_rf_preview.c` `rf_render_rows`: the
  reference byte-wise [r,g,b,a] rgba convention (colour user-verified on
  VHS FM) — any new packing must match it or cite a better-tested one.

## Applies to future work

- Any new decoder output format (RGB565, floating point, planar YUV, ...).
- Any new display path (new panels, offscreen render targets, exports,
  screenshots) — the same rules: the export/screenshot buffer is only
  "verified" if the conversion into it is tested or the dump is taken
  downstream of it.
- Any change that adds a numeric readout used as a proxy for a visual
  property (colour angles, level percentages, geometry offsets): state in
  the log which render-path conversion stands between the number and the
  screen, and what covers it.
