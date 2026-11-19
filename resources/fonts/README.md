# Aurora Glass design fonts

Bundled UI/mono typefaces for the Aurora Glass design system, loaded at
runtime by `KhensuTheme::uiFont()`/`monoFont()` (desktop-app only; falls back
to the platform system font if a file here is missing or fails to load).

## Licensing

Both families are licensed under the **SIL Open Font License, Version 1.1**
(full text in each family's `OFL.txt`), which explicitly permits bundling,
embedding, and redistributing the fonts as part of software, including
commercially, without royalty. No attribution notice is required in the
running application, but the license files are kept alongside the fonts per
the OFL's own redistribution terms.

## Sources

- **Space Grotesk** — Regular/Medium/Bold static instances from
  [floriankarsten/space-grotesk](https://github.com/floriankarsten/space-grotesk)
  (`fonts/ttf/static/`), the original type designer's repository. Google Fonts'
  own `google/fonts` repo only ships the variable-font instance for this
  family (no static weights), so the designer's repo was used instead.
- **JetBrains Mono** — Regular/Medium/Bold static instances from
  [JetBrains/JetBrainsMono](https://github.com/JetBrains/JetBrainsMono)
  (`fonts/ttf/`).

## Wiring status

`KhensuTheme::uiFont()` and `monoFont()` currently route **Regular** (plain)
and **Bold** through `FontOptions` as real hinted faces (not synthetic bold).
Italic stays synthetic via `Font::italicised()`, matching how the pre-bundling
system-font path already synthesized italic.

**Medium is downloaded but not yet used** - `uiFont()`'s API only
distinguishes plain/bold/italic via `juce::Font::FontStyleFlags` today, so
there's no style slot for a fourth weight without a small type-scale design
pass (a distinct enum/parameter beyond bold/plain, threaded through every
call site that currently passes a style flag). Kept on disk for whoever picks
that up rather than re-downloading it later.
