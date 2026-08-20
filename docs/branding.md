# Branding & visual assets

Crypta's visual identity, where the master files live, and which paths the
README and the docs are wired to. Everything here is self-made, so it is
license-clean under the repo's AGPLv3 (see
[`docs/adr/0002-agplv3-licensing.md`](adr/0002-agplv3-licensing.md)).

## Plugin icon

The icon is the plugin's badge: it is the README header image, the manual's
header image, and the artwork used for the Basilica Audio suite listing.

| Property | Value |
|---|---|
| Motif | A gold serpent coiled around a bass clef |
| Treatment | Antique-gold bas-relief emblem on a flat near-black squircle (ICON-DIREKTIVE v3, "flat squircle" — no glass dish, no rim) |
| Committed at | [`docs/assets/icon.png`](assets/icon.png) (1024×1024, RGBA) and [`docs/assets/icon-256.png`](assets/icon-256.png) (256×256, RGBA) |
| Suite master | `brand/v3-flat/final/crypta.png` / `crypta_256.png` in the suite working tree |
| Landed in | #70 (`docs(branding): v3 flat squircle icon`), superseding the v2 "plastic" set |

The two committed files are byte-identical copies of the suite masters
(md5-verified) — the repo carries its own copies so a clone is
self-contained, while the suite `brand/` tree stays the single place where
the icon is regenerated.

The motif predates the rename from *Twist Your Guts* to *Crypta*. It was kept
deliberately: a serpent coiled around a bass clef reads as the low-end
foundation of the basilica either way.

### Palette

Sampled from the committed icon. The GUI work should treat these as the
anchor values rather than re-deriving them.

| Role | Value |
|---|---|
| Plate | `#000000` – `#202020` (flat near-black, ~70 % of the badge area) |
| Emblem, shadow side | `#302010` |
| Emblem, mid tone | `#504020` – `#605030` |
| Emblem, lit side | `#806040` – `#A08050` |

## GUI preview

The suite convention — established by Silentium, which shipped the first
custom editor — is a single rendered preview at **`docs/gui-preview.png`**.
Silentium references it from the CHANGELOG entry of the release that
introduced the GUI; Crypta's README additionally reserves the path, so the
image has one documented home rather than being linked ad hoc.

The preview is **generated, never mocked up**: Silentium's GUI test suite
takes an offscreen snapshot of the real editor, writes it to
`build/gui-preview.png`, asserts it is non-blank, and that file is what gets
committed as `docs/gui-preview.png`. Crypta follows the same route once the
M3 GUI editor exists (#45, #25, #26, #27, #28). The file is deliberately
absent until then rather than being filled with a stand-in image, because a
hand-made mockup committed under that name would be indistinguishable from a
real screenshot the moment anyone links to it.

Companion docs Silentium carries alongside the preview, worth mirroring when
Crypta's editor lands:

- `docs/gui-components.md` — component architecture of the editor
- `docs/gui-mapping.md` — which APVTS parameter each physical control drives

## Archived logo drafts

Two earlier standalone logo directions exist as history only, on the unmerged
`feat/branding` branch under `assets/logo/`:

- `logo-badge-v1-metalcore.svg` / `logo-lockup-v1-metalcore.svg` — v1
  "metalcore molten", rejected
- `logo-badge-v2-blackmetal.svg` / `logo-lockup-v2-blackmetal.svg` — v2
  black-metal thorned sigil, approved 2026-07-14

Both carry the pre-rename *Twist Your Guts* wordmark and predate the v3 icon
direction, so neither is wired into the README. They are kept on the branch
as design history; the v3 squircle icon is the current and only shipped brand
mark.
