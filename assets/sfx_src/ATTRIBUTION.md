# Sound effect attribution

Every sound shipped in `romfs/sfx/` is listed here with where it came from, under what
licence, and what crediting it requires. The `.ogg` files in this directory are the
untouched originals; `tools/make_sounds.py` is what turns them into the `.bsnd` files the
game actually loads.

**The rule this project works to:** CC0 or CC-BY only. Never non-commercial-only, never
share-alike, and nothing whatsoever taken from Minecraft or Nintendo — not one file, ever.
If a sound's licence cannot be established from the source, it does not go in.

## Shipped sounds

All three come from the same pack.

| Game sound | Source file | Pack |
|---|---|---|
| `romfs/sfx/block_break.bsnd` | `impactMining_000.ogg` | Kenney — Impact Sounds |
| `romfs/sfx/block_place.bsnd` | `impactPlank_medium_000.ogg` | Kenney — Impact Sounds |
| `romfs/sfx/footstep.bsnd` | `footstep_wood_000.ogg` | Kenney — Impact Sounds |

### Kenney — Impact Sounds (1.0)

- **Pack page:** https://kenney.nl/assets/impact-sounds
- **Downloaded from:** https://kenney.nl/media/pages/assets/impact-sounds/87b4ddecda-1677589768/kenney_impact-sounds.zip
- **Author:** Kenney (Kenney Vleugels), www.kenney.nl
- **Licence:** Creative Commons Zero (CC0 1.0 Universal) —
  http://creativecommons.org/publicdomain/zero/1.0/
- **Pack creation date:** 19-12-2019 (from the pack's own `License.txt`)

The pack's `License.txt`, verbatim on the two lines that matter:

> This content is free to use in personal, educational and commercial projects.
> Support us by crediting Kenney or www.kenney.nl (this is not mandatory)

**Required attribution: none.** CC0 waives it, and the pack's own licence file says the
credit is not mandatory. It is given here anyway, and the line below is the one to use if a
credits screen is ever added:

> Sound effects by Kenney (www.kenney.nl) — CC0

### Provenance, verified rather than remembered

The three `.ogg` files in this directory are **byte-identical** to the copies inside the
pack zip named above. That was checked rather than assumed, and the check mattered: the
first guess at which Kenney pack the footstep came from was *RPG Audio*, which turned out
to contain `footstep00.ogg`–`footstep09.ogg` and no `footstep_wood_000.ogg` at all. Writing
that guess down would have put a wrong source URL in a licence file, which is the one kind
of file where a plausible-looking wrong answer does real harm.

SHA-256 of the originals, all three confirmed identical to `Audio/<name>` in the pack zip:

```
5f1c252942a220d658121bd9417448cd19649ded9fff2ff15f6e50899373ce8e  footstep_wood_000.ogg
36b4ea107222d073c67ca64dde26944975609202d5090b2f2021213c1a7e35cd  impactMining_000.ogg
8403cd7a043f0391574c88c55dd205743725b5363d2407f6de145f201690b68f  impactPlank_medium_000.ogg
```

Each file also carries `ARTIST=KenneyG` in its own Vorbis comment header, which identifies
the author without reference to where the download came from.

## Planned for v1.8.17 — licence-verified, not yet downloaded

The sounds below have had their licence traced to the original upload page this session
(not an aggregator) and are cleared to source under this project's CC0/CC-BY rule. **None
of these files are in this directory yet** — no download has happened, per the file
being a plan, not an install, until steve authorises pulling them in. Full sourcing
reasoning, byte budget, and the ranked list these belong to are in
`docs/research/audio-sourcing.md`.

| Game sound | Original page | Uploader | Licence | Attribution required |
|---|---|---|---|---|
| Footstep — dirt/grass | https://opengameart.org/content/fantozzis-footsteps-grasssand-stone | Fantozzi (recorded); submitted by qubodup | CC0 | No |
| Footstep — stone | https://opengameart.org/content/fantozzis-footsteps-grasssand-stone | Fantozzi (recorded); submitted by qubodup | CC0 | No |
| Footstep — sand | https://opengameart.org/content/fantozzis-footsteps-grasssand-stone | Fantozzi (recorded); submitted by qubodup | CC0 | No |
| Block break — dirt/grass | https://kenney.nl/assets/impact-sounds | Kenney | CC0 | No |
| Block break — wood | https://kenney.nl/assets/impact-sounds | Kenney | CC0 | No |
| Block break — sand | https://kenney.nl/assets/impact-sounds | Kenney | CC0 | No |
| Block break — glass | https://freesound.org/people/Ruben_Uitenweerde/sounds/486166/ | Ruben_Uitenweerde | **CC BY 3.0** | **Yes** — "Glass breaking" by Ruben_Uitenweerde (Freesound.org, CC BY 3.0) |
| Block place — stone | https://kenney.nl/assets/impact-sounds | Kenney | CC0 | No |
| Block place — dirt/grass | https://kenney.nl/assets/impact-sounds | Kenney | CC0 | No |
| Block place — sand | https://kenney.nl/assets/impact-sounds | Kenney | CC0 | No |
| Player — hurt | https://freesound.org/people/MAJ061785/sounds/85553/ | MAJ061785 | **CC BY 3.0** | **Yes** — "male pain grunt" by MAJ061785 (Freesound.org, CC BY 3.0) |
| Player — eat | https://opengameart.org/content/apple-bite | AntumDeluge | CC0 | No |
| Door/chest — open | https://freesound.org/people/spookymodem/sounds/202092/ | spookymodem | CC0 | No |
| Water — enter/splash | https://freesound.org/people/qubodup/sounds/210428/ | qubodup | CC0 | No |
| Water — exit | https://freesound.org/people/speedygonzo/sounds/235725/ | speedygonzo | CC0 | No |

**Not sourced — recommended for synthesis instead** (see `audio-sourcing.md` §4 for why):
UI click/move, UI confirm/select, block place — glass.

**Known gap, still open:** the Kenney Impact Sounds rows above and the Fantozzi footsteps
rows are verified CC0 at the pack/author level; which individual file inside each archive
maps to which game sound is not chosen yet — that requires opening the archive, which is
the download step this pass does not take.

## Adding a sound

1. Establish the licence **first**, from the source itself — the pack's own `License.txt`
   or the asset page. "It looked free" is not a licence. If it is unclear, do not use it.
2. Drop the original in this directory unmodified, and add a row above with the pack page
   URL, the direct download URL, the author, the licence, and the required credit text.
3. Add it to `MANIFEST` in `tools/make_sounds.py` and re-run that script. It prints the
   exact byte cost of the whole set; that number is the linear-memory budget in
   `source/audio/audio.h` being spent, and linear memory is what render distance comes out
   of on this console.
