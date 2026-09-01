# Interface — research brief for Blocksmith v1.8.19 "The new interface"

Scope note: this is research only. **Nothing in the Blocksmith project tree was changed.**
`source/` was read, never written.

**The hard constraint governing this entire brief: nothing shipped is copied.** Blocksmith
ships no Minecraft assets, no Sony assets, no Nintendo assets, no ripped UI textures, no
copied icon art. Every asset the project ships is generated procedurally by Python scripts in
`tools/`. The Legacy Console Edition crafting menu and the PS3 XMB are referenced **only** as
interaction design — their navigation model, their layout logic, the reasons a controller-first
interface is shaped the way it is. Where this brief describes what either reference *looked*
like, it is describing a design pattern to reimplement from first principles, never artwork to
trace, screenshot, or extract. Nowhere below is a texture, icon, font, or color palette lifted
from either reference; Part D is written entirely in terms of what a procedural generator
should emit, not what to copy from a screenshot.

Provenance is marked throughout, matching the convention of `docs/research/sky-and-weather.md`:

- **[COMPUTED]** — arithmetic or a measurement I ran myself; scripts are throwaway, in the
  session scratchpad, prefixed `ui_`, not part of the repo.
- **[DERIVED]** — arithmetic on top of cited numbers, shown so it can be checked.
- **[INFERENCE]** — engineering judgement. Not sourced.
- **[NO SOURCE]** — looked and could not find one. Named explicitly so it is never implemented
  as if it were a fact.

---

# Part A — The references, analysed as interaction design

## A1. Legacy Console Edition: recipe-first, not ingredient-first

Java Edition's crafting table is **ingredient-first**: the player places items into a 3x3 (or
2x2 inventory) grid and the game pattern-matches the arrangement against a recipe table to
produce a result. The player has to know or discover the *shape*.

The Legacy Console Edition (4J Studios' Xbox 360 / PS3 / Wii U / Vita port, and the ancestor of
the "Legacy Minecraft crafting menu" the owner is referencing) inverted this. It is
**recipe-first**: *"it did not require the player to place items in the correct place in the
crafting interface, but instead displayed the ingredients required to craft the selected item"*
([Legacy Console Edition exclusive features](https://minecraft.wiki/w/Legacy_Console_Edition_exclusive_features)).
The player browses a list of *things that can be made*, not a grid of *cells to fill*. Selecting
an entry shows its ingredients and lets the player craft it — repeatedly, one press per unit —
as long as the materials are held, with no placement step at all.

**Why this suits a controller, in the reference's own words.** A Minecraft Forum thread on the
Xbox 360 crafting redesign gives the design reasoning directly: 4J *"eliminated the classic
grid from the Xbox version because it would be too hard to drag items around the screen to
build the item in a timely manner, and it would become tedious and boring"*
([Minecraft Forum — "legit crafting menu?"](https://www.minecraftforum.net/forums/minecraft-editions/minecraft-xbox-360-edition/mcx360-discussion/2013844-legit-crafting-menu)).
A mouse can drag an item from a hotbar cell to a grid cell in one continuous gesture; a d-pad or
stick cannot express "pick this up, move it, drop it" without a cursor and an extra button state
for "holding" — the exact problem a fixed-function embedded UI on a 3DS also has, since
Blocksmith's inventory has no mouse either. Removing the placement step removes the interaction
Java's grid needs a pointing device for.

**Layout, as documented and as played back by users who owned the games.** The same forum
thread describes a small **non-interactive 2x2 preview grid shown in a corner of the screen**,
which *"displays how the stuff is put together, but you aren't moving the pieces around on
it"* — crafting itself happens through the menu, and the grid is read-only decoration that
shows the shape for players who already know Java's version. A full crafting table swaps that
preview to a 3x3 equivalent for advanced recipes. Around it: a list of recipes grouped into tabs
— *Structures, Tools and Weapons, Food, Armor, Mechanisms, Transport, Decorations, Banners,
Fireworks, Dyes*, plus a separate Stonecutter screen — and the selected recipe's ingredient
list is shown alongside it ([Legacy Console Edition exclusive features](https://minecraft.wiki/w/Legacy_Console_Edition_exclusive_features)).
There is **no search**; a **"show craftable items only"** toggle is the sole filter, so with a
large catalog and no pointing device, browsing by category is the only navigation, and the
filter exists specifically to cut a category down when the player cannot type.

**Controller navigation, from a console strategy FAQ**: switching between recipe variants
within a tab is the **left stick or d-pad up/down**; switching **tabs** is **LB/RB** (the
shoulder buttons); crafting is a **face button**, pressed repeatedly to make multiples
([GameFAQs — Minecraft Xbox 360 Edition crafting interface](https://gamefaqs.gamespot.com/xbox360/632873-minecraft-xbox-360-edition/answers/332829-crafting-interface)).
That is a **two-axis input mapped to two different physical controls**: shoulder buttons move
across the *category* axis, stick/d-pad moves along the *item* axis within the current category.
That split — one physical control per axis, rather than one stick doing both — is the
transferable idea, because it removes the ambiguity of "which direction on one stick means
change category vs change item" that a single-axis d-pad menu would otherwise have to resolve
with a mode switch.

An options toggle, **"Classic Crafting"**, restores Java's 3x3 grid as an alternative for players
who prefer it ([Legacy Console Edition exclusive features](https://minecraft.wiki/w/Legacy_Console_Edition_exclusive_features)) —
evidence that 4J treated the two as genuinely different input models to choose between, not one
being a lesser version of the other.

## A2. PS3 XMB: the cross-shaped bar, as a navigation model

"A long bar with multiple options" describes the **XrossMediaBar (XMB)**, Sony's interface
across PSX, PSP, PS3, and much of its consumer electronics line, co-developed with Q-Games
([XrossMediaBar — Wikipedia](https://en.wikipedia.org/wiki/XrossMediaBar)).

**The two-axis model, exactly.** *"The interface features icons that are spread horizontally
across the screen ... These icons are used as categories to organize the options available to
the user. When an icon is selected on the horizontal bar, several more appear vertically, above
and below it"* ([XrossMediaBar — Wikipedia](https://en.wikipedia.org/wiki/XrossMediaBar)). The
"X" in XrossMediaBar names this directly — the crossing of one horizontal category row with one
vertical item column, forming a cross shape centered on the current selection
([PS3 manual — About the XMB menu](https://manuals.playstation.net/document/en/ps3/current/basicoperations/xmb.html)).
Categories run left–right (Users, Settings, Photo, Music, Video, Game, Network, Friends, etc. on
PS3); within the currently-selected category, items run up–down. Only one row and one column are
ever visible in full at once — everything else collapses out of view — which is the load-bearing
difference from a conventional 2D grid: **the player is never choosing from a whole grid, only
ever from a single row or a single column at a time.**

**D-pad mapping is one direction pair per axis:** *"A 4-way directional pad is used to choose
categories (using the left and right directions) as well as highlighting options or actions
within these categories (using the up and down directions). Two additional buttons are required
to select items which are highlighted, as well as to return to the previous level"* (Cross/Circle
on PlayStation) ([XrossMediaBar — Wikipedia](https://en.wikipedia.org/wiki/XrossMediaBar)). This
is the same left/right-for-category, up/down-for-item split the LCE crafting menu uses (A1), just
generalized to the whole system menu rather than one screen — strong independent convergence on
the same input mapping for the same problem shape.

**How focus is shown**, again in the reference's own words: *"unselected icons shrink slightly"*
when the selection moves ([XrossMediaBar — Wikipedia](https://en.wikipedia.org/wiki/XrossMediaBar)).
There is no cursor and no highlight rectangle — the selected item is the one at full scale,
everything else recedes. This is a legible focus indicator that needs **zero extra draw calls
and zero extra texture**: it is a transform on the item already being drawn, which is exactly
the kind of cost a fixed-function TexEnv pipeline with no spare stages rewards (Part B).

**What it does well, and why.** Design retrospectives converge on the same read: the XMB was
praised for being *"devoid of useless flashy crap, extremely scalable, discoverable, elegant and
intuitive,"* a *"simple and easy to navigate system that isn't cluttered with extraneous images,
buttons, arrows"*, with a *"sleek, minimalist aesthetic ... simple black background and white
text emphasizing clarity, focusing on images and icons rather than overwhelming users with text"*
([XrossMediaBar — Wikipedia](https://en.wikipedia.org/wiki/XrossMediaBar)). The interface won a
2006 Technology & Engineering Emmy for exactly this ([XrossMediaBar — Wikipedia](https://en.wikipedia.org/wiki/XrossMediaBar)).
The transferable lesson is not the icon art — it is that **a category axis you can always see the
current position on, an item axis that only exists once a category is chosen, and a focus
indicator that is a scale transform rather than a new draw** is a menu system that scales to a
large catalog without ever needing more than four directions and two buttons.

**What it does not do well, and why it matters here.** *"It's not suitable for mouse and touch
screen use because it's an interface based on operation with cross keys and OK/Cancel buttons"*
([XrossMediaBar — Wikipedia](https://en.wikipedia.org/wiki/XrossMediaBar), also stated on the
[PS3 developer wiki](https://www.psdevwiki.com/ps3/XMB)), and Sony itself replaced it on Vita
with a touch-first interface, LiveArea, once touch became the primary input
([XrossMediaBar — Wikipedia](https://en.wikipedia.org/wiki/XrossMediaBar)). The 3DS has **both**
a d-pad/buttons top screen and a touch bottom screen simultaneously (Part B) — the XMB pattern
is a strong fit for the top screen's button-only interaction and a poor fit if forced onto the
touch screen, which is a design fork addressed in Part D and flagged again in the closing report.

## A3. Where the two references pull in different directions

Both are controller-first category/item navigation systems, but they solve a different problem
and that difference is not fully reconcilable — see the closing report for why this is a
decision for the owner, not something resolved here:

- **LCE's tabs are a fixed, small, known-in-advance set** (about ten categories), chosen because
  the *content* — recipes — is naturally groupable and finite. **XMB's category bar is built to
  scale to an open-ended, growing set** of unrelated systems (photo, music, video, game, network)
  where the categories themselves are the top-level product structure, not a grouping of one
  content type.
- **LCE always shows a non-interactive preview** (the small crafting-grid graphic) alongside the
  list — a second, dependent panel. **XMB shows nothing but the cross itself** — no companion
  preview panel; the vertical column *is* the entire screen content once a category is chosen.
- **LCE's craft action is repeatable and immediate** (press to make one, hold/mash to make many).
  **XMB's select action normally navigates one level deeper** rather than performing the action
  in place.

A crafting/inventory screen inherits LCE's shape more directly (it needs a preview and an
immediate action); a top-level menu or settings shell inherits XMB's shape more directly (it
needs to scale across unrelated categories with no natural preview). Trying to force one screen
to be both — a long XMB-style bar that is also LCE's recipe-preview screen — is the fork Part D
flags explicitly rather than resolves.

---

# Part B — The hardware this must live inside

## B1. The two screens, as specified

| Screen | Native resolution | Notes |
|---|---|---|
| Top | **400×240** (or **800×240** in stereo 3D, 400×240 per eye) | 3.53"–4.88" autostereoscopic LCD |
| Bottom | **320×240** | 3.00"–4.18" resistive touchscreen |

Both figures are the ones the owner specified and match the hardware's documented panel sizes
([Nintendo 3DS hardware specs, aggregated](https://en.wikipedia.org/wiki/Nintendo_3DS)). One
implementation detail worth carrying into Part D: **both physical LCD panels are mounted rotated
90 degrees**, so on real hardware the framebuffers libctru hands back are stored **column-major**
— width and height are transposed in memory relative to how the screen is described logically —
which is a `gfx.h`/`gfxGetFramebuffer` detail, not a design one, but it is why screen-space UI
code on this platform is never quite "just" a 2D canvas the way a PC framebuffer is
([libctru gfx.h](https://github.com/devkitPro/libctru/blob/master/libctru/include/3ds/gfx.h)).

## B2. What the project currently does with each screen

Established by the codebase-exploration pass in Part C: **[see Part C for exact file:line
citations — summarized here for the hardware framing].** The short version, to be filled in
below: the top screen carries the 3D world and HUD; the bottom screen is already the owner's
stated home for the debug block list and biome readout, which makes the top/bottom split a real,
already-decided design axis for this brief rather than an open question — **whatever screen
layout Part D proposes must keep those two specific things on the bottom screen**, because that
placement was the owner's separate, explicit instruction, not a UI-brief judgement call.

## B3. No programmable fragment shader — the TexEnv budget

The PICA200 GPU has **programmable vertex shaders** (up to 3–4 vertex processors, one of which
can double as a geometry-shader-like "primitive engine") but **no programmable fragment/pixel
shader at all** — the fragment stage is entirely fixed-function
([3dbrew — Hardware](https://3dbrew.org/wiki/Hardware),
[3dbrew — Nintendo OpenGL](https://3dbrew.org/wiki/Nintendo_OpenGL),
[PICA200 — Wikipedia](https://en.wikipedia.org/wiki/PICA200)). Per-pixel effects (including
lighting) exist only through the TexEnv combiner chain and lookup tables, never through arbitrary
fragment math.

Concretely, citro3d exposes **6 TexEnv combiner stages** and **3 texture units**
([3dbrew — GPU/Internal Registers](https://www.3dbrew.org/wiki/GPU/Internal_Registers),
[GBATEK — 3DS GPU TexEnv registers](http://problemkaputt.de/gbatek-3ds-gpu-internal-registers-texturing-registers-environment.htm)),
matching what Blocksmith's own `sky-and-weather.md` brief already established for the terrain
renderer (D3 in that document): only the **first four** stages can write the TEV output buffer,
and a stage that writes the buffer is not readable until **two stages later**. Any UI rendering
sharing the GPU with the terrain renderer in a given frame is drawing from the **same** 6-stage,
3-unit budget — it is not a separate pipeline with its own allowance. This is the concrete reason
Part D's "focus by scale transform, not by a second draw" (A2) matters on this hardware
specifically: a stage or texture unit spent on a UI highlight is not free, and citro2d/UI drawing
composes with whatever the 3D scene already left on the TexEnv chain that frame.

---

# Part C — What the project's UI is today

*[Populated from a read-only codebase pass — see below for the sub-agent's findings, to be
merged in.]*

---

# Part D — Proposed design

*[Depends on Part C's inventory of reusable widgets — drafted after Part C.]*

---

# Open questions and gaps

*[To be completed alongside Part D.]*
