# Fonts

Subsets packed into the flash `fonts` partition by `pio run -t uploadfonts`.
The text fonts each cover GB2312 (all 6,763 hanzi), ASCII/Latin-1, CJK and full-width
punctuation, arrows and common symbols. Name records are stripped, so these are
unnamed Modified Versions of the originals.

| File | Source | License |
| --- | --- | --- |
| `sans.otf`, `sans-bold.otf` | [Noto Sans SC](https://github.com/notofonts/noto-cjk) Regular / Bold (SubsetOTF) | SIL OFL 1.1 — `OFL-NotoSansSC.txt` |
| `kai.ttf` | [LXGW WenKai](https://github.com/lxgw/LxgwWenKai) Regular v1.520 | SIL OFL 1.1 — `OFL-LXGWWenKai.txt` |
| `display.otf` | [Smiley Sans](https://github.com/atelier-anchor/smiley-sans) Oblique v2.0.1 | SIL OFL 1.1 — `OFL-SmileySans.txt` |
| `icons.otf` | [Material Icons Round](https://github.com/google/material-design-icons), names in `icons.txt` | Apache 2.0 — `LICENSE-MaterialIcons.txt` |
| `emoji.ttf` | [Twemoji Mozilla](https://github.com/mozilla/twemoji-colr) v0.7.0 (COLR/CPAL color font), single-codepoint emoji only | Graphics © Twitter, Inc. and other contributors, [CC-BY 4.0](https://creativecommons.org/licenses/by/4.0/); font build Apache 2.0 — `LICENSE-Twemoji.md` |

To regenerate (e.g. after editing `icons.txt`), download the files above into a
folder together with `MaterialIconsRound-Regular.codepoints` and run (append
output names, e.g. `emoji.ttf`, to regenerate only those):

```bash
pip install fonttools
python3 tools/subset_fonts.py <download-dir>
```
