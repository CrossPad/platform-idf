# Nested song elements: arrangements inside arrangements, baked in the background

Status: design, agreed with the user 2026-10-24 (brainstorm; GUI variant B chosen
from mock-ups over a real Arrange screenshot). Builds on Steve's freeze
(core 2.9–2.13) and Arrange as of core 2.19.0 / crosspad-arrange 0.21.1.

Calls marked **(builder's call, made here)** were not discussed with the user;
they are defaults a builder may revisit without asking. Everything else is what
the user asked for.

## 1. Intent

A song on the CrossPad is built from pieces that are themselves arrangements: a
drum loop made of a kit track and a clap recording, a chorus made of that loop,
a bass line and a vocal, the song made of the chorus four times and a verse.
The user wants to:

- **reuse** a piece across songs (a good loop ends up in many projects, made
  long ago, maybe on older firmware);
- **open any piece and edit everything** that could be edited when it was made
  (notes, kit, recordings, later effects), years later, from any song using it;
- **never wait**: what is not being edited costs one file stream instead of
  engines, and that baking happens in the background, so moving between levels
  and pressing PLAY sounds at once. A progress bar is acceptable when the user
  outruns the cache; regular background baking makes that rare, including when
  old projects are opened.

The hardware is why baking matters: one song ring of 64 kB for all tracks,
three engine routes at most (kit, second kit, Pitch), a 1451 µs audio block
of which a four-source song already uses ~760–850 µs, an SD card that reads
three tracks slower than real time on a 1-bit bus and stalls ~0.5 s at times,
and FAT `max_files` 20. An audio stream costs ~8 µs a block; an engine
150–400 µs.

## 2. Model

### 2.1 Element

An **element** is an arrangement with the same shape as a song today: up to
four tracks (`AudioSong::kTracks`), clips on them, a length, its scenes
(`sequences.json`), its kit / instrument references, its clips folder. A song
is simply the element at the top. Elements nest to any depth (the user expects
"a dozen levels" to be possible).

A clip on a track is one of three things:

1. a recording (an audio file) — today's audio block;
2. a block of notes through a kit or instrument — today's seq block;
3. **an element** — new: a reference to another element by id.

An element may be anything from one recording to a full arrangement; the
same rules apply to all of them.

**Element clips live on audio tracks** (builder's call, made here): baked, an
element plays as audio, so it sits beside recordings and needs no engine
route. Like a seq block, an element clip longer than its element goes round
it (a one-bar loop under a 16-bar chorus); like an audio block it has an
offset into the element (in bars) and a length.

Cycles are refused at insertion: an element can never contain itself,
directly or through others.

### 2.2 Library and identity

Elements live in a **library on the card**, outside any single song, so songs
can share them. Each element has a stable id (assigned at creation) and a
display name.

**Every element is a project folder of today's layout** (builder's call, made
here): `song.json`, `sequences.json`, `clips/`, `pitch/`, … — so the existing
loader, saver and engine open an element exactly as they open a song. Songs
stay in `/sdcard/SONGS/<project>/`; elements that are not songs live beside
them in a library folder (e.g. `/sdcard/SONGS/.elements/<id>/`); any song can
also be placed into another song as an element. The songs list shows songs;
the "element from the library" picker shows both.

An element carries copies of the recordings it uses (as a project does today
with `clips/`), so it stays editable when the original file is gone. Kits and
instruments are referenced by path, as today, plus an identity hash (§3.1).

The library keeps a **reverse index** — which songs and elements use each
element — so the copy/replace question (§5) can say "used in 3 other songs".
The index is a cache: the `song.json` files are the truth and the index is
rebuilt by a scan when missing or inconsistent. Counts cover this card only.

### 2.3 Content hash

Each element has a **content hash computed like a Merkle tree**:
its own `song.json` content (tracks, clips, levels, mute/solo, input, stretch),
the notes its seq blocks play, the identity of every recording it uses, the
identity of its kit and instrument, and the hashes of its child elements. A
change anywhere below changes the hash of every ancestor and of nothing else.

A **bake key** adds what the render depends on outside the element: the tempo
and sample rate it is played at, and a render version (`kRenderVersion`,
bumped when engine or mixdown output changes on purpose).

Today's freeze keys (`seqKey`, `pieceKey`) name the kit by its path only and
leave the kit's content out; a kit edited under the same name keeps an old
freeze. The kit/instrument identity hash closes that gap (kit.json bytes plus
each sample file's size and modification time, builder's call, made here).

### 2.4 Tempo

One tempo, the song's (the user's decisions):

- an element plays at the tempo of the song it is in; a 90 BPM loop in a
  120 BPM song plays at 120, and **its recordings are fitted to the tempo**
  (stretched, as `StretchMode` does today, so they are baked);
- inside an element the top bar shows and changes the song's tempo; an
  element's own tempo is used only when it is opened as a song;
- after a tempo change every bake is stale: **old bakes play varispeed**
  (resampled to the new length, so a little higher or lower in pitch) until
  the new ones exist, and the focus plays live.

## 3. Live and baked

### 3.1 The rule

- **Live** means played from its sources: engines for seq blocks, streams for
  recordings, (later) effects in real time.
- **Baked** means one WAV, rendered from the live form, found in the cache by
  its bake key.
- **Anything that does not play 1:1 from a single file is baked**: seq blocks,
  element clips, and recordings with stretch (already rendered today,
  `StretchMode`) or, later, effects. A recording with neither plays from its
  file and has nothing to bake.

### 3.2 The open path

The user is always "in" one element: the **focus**. The **open path** is the
chain from the song down to the focus. Only the focus plays live. Everything
else plays baked.

With PLAY the user hears the song around the focus by default (§6.3 SOLO
for the focus alone). To keep that to a constant number of streams at any
depth, the engine plays two things:

1. **the focus, live**: its own tracks, at every place the focus occurs in the
   song (the occurrences are known from the open path);
2. **one context stem**: the whole song with every occurrence of the focus left
   silent, baked into one WAV. Its key is the song tree with the focus's
   occurrences replaced by silence, so **it does not change while the focus is
   edited**: it is baked once on entering a level and stays valid for the
   whole edit.

**Time inside a focus** (the user's decisions): bars count from the focus's
own start (the position, LOOP's region, the playhead are in its bars). PLAY
with the context goes round **the occurrence nearest the playhead**, with its
context, over and over. **REC and DUB record into the element itself**, so the
take is heard at every occurrence of it.

Until the context stem is baked, the engine falls back to expanding each
ancestor level with its other clips baked (more streams; if the card cannot
keep up, the context is left out and the focus plays alone with a "context
baking" progress shown). Entering a level whose context stem is cached is
instant.

This is what "baking in both directions" means in this design: going down
bakes the context around the new focus; going up bakes the edited element
and, from it, its ancestors.

### 3.3 Staleness never means silence

When an element changes, every bake that depends on it is stale. Until the new
bake exists, the old bake keeps playing where nothing changed, and the changed
part plays live where the resources allow (it usually is the focus anyway).
Silence where music should play is acceptable only when the user outruns the
cache and there is neither a valid bake nor an older one, and it comes with a
progress bar on the clip.

## 4. Baking

### 4.1 Two kinds of bake

- **Engine bakes** render seq blocks through an engine. This is today's
  freeze: offline (~5.5x real time) through the one sequencer and clock, only
  while the song is stopped, cancelled by PLAY/REC/DUB/open/edit, renders
  reused from the card after a cancel. Unchanged, except that its results go
  into the shared cache (§4.3) and its key gains the kit identity.
- **Mixdown bakes** build an element's WAV from WAVs that already exist
  (recordings, engine bakes, baked child elements) by summing them with the
  element's levels and mute/solo, on the `song_io` worker. No engine, no
  sequencer, no audio clock: **they can run while the song plays**, limited
  by card bandwidth (§4.2). When effects arrive, a mixdown also runs the
  element's effect chains (on the worker, not in the RT task).

An element's bake is engine bakes for its seq blocks, then one mixdown;
ancestors are mixdowns only. After an edit in a deep element, everything above
it re-bakes as mixdowns while the user keeps playing.

### 4.2 Scheduling

One background baker on the `song_io` worker, lowest priority:

- playback's ring fill always comes first; the baker reads and writes only in
  the time the ring is full, in 4 kB pieces as bounces do now, and backs off
  on any sign of the ring running low;
- order: the context stem for the current focus first, then the path upward
  from an edited element, then everything else in the open song, then
  (idle, Arrange stopped and no pad played for a while) other songs that use
  changed elements;
- engine bakes wait for stop, as today; auto-freeze keeps its trigger (ARM
  leaving a track) and gains "leaving a level";
- opening a song bakes whatever it misses in the background; the song plays
  from older bakes or live meanwhile.
- **the baker never takes the user's kit** (builder's call, made here): an
  engine bake of an element whose kit or instrument is not the one loaded runs
  only on an engine route nobody uses at that moment (the second kit engine,
  the Pitch instrument built beside the kits), while stopped, and gives the
  route back before the user needs it; otherwise it waits, and that element
  plays from an older bake or live when it is the focus. The pads never play
  a kit the user did not pick.

### 4.3 Cache

All bakes go into **one content-addressed cache shared by every song**
(e.g. `/sdcard/SONGS/.bake/<bakekey>.wav`, written as `.part` and renamed, as
freeze renders are today). A loop used in five songs is baked once.

The cache is **bounded** (builder's call, made here: a size cap and a share of
the card's free space, the smaller wins). Eviction is least recently used, and
it never removes a bake that the open song's current tree uses. An evicted bake
is just baked again when next needed.

**The newest bake of every element is also kept with the element** (in its
own folder, builder's call, made here) and is never evicted: an element
whose sources are gone (kit deleted, sample renamed) can always still play.
The shared cache holds every other version and key (other tempos, the
context stems, older renders).

Today's per-song `clips/.freeze/` keeps working for songs that have not been
moved to the shared cache.

## 5. Editing a shared element: copy or replace

Edits to an element used elsewhere go to a **draft** of it, not to the element
itself (copy-on-write): this is what makes "copy" possible after the fact, and
it keeps other songs playing the unchanged element meanwhile.

When the user leaves an element they changed (goes up a level, closes the
song) and the reverse index says it is used in other songs or elements:

> "REFREN is used in 3 other songs." **Make a copy** · **Replace everywhere**

- **Make a copy**: the draft becomes a new element ("LOOP DR 2") and only
  the current song gets it; every other user keeps the original. **The copy
  goes up the open path**: every ancestor on it that is shared too is copied
  as well, or the other songs using it would get the change. One question
  covers it, with a summary ("LOOP DR 2 and REFREN 2 will be made for this
  song"). Every occurrence of the edited element in the current song gets
  the copy (also one under VERSE, outside the open path), as it is heard live
  at all of them while edited (§3.2).
- **Replace everywhere**: the draft becomes the element; every user now refers
  to the new version, their bakes are stale and re-bake in the background (the
  current song at once, the others when opened or when idle), playing their
  old bakes until then.

The question comes on leaving, not on entering, and only when something
changed; an element used nowhere else is edited in place with no question
(builder's calls, made here). A draft survives a power cut; the next time the
element is opened the user is offered to continue it.

## 6. GUI (variant B)

Mock-ups painted over a real Arrange screenshot from the board, next to this
file: `2026-10-24-nested-song-elements/today.png` (Arrange now),
`variant-b.png` (inside SONG › REFREN › LOOP DR), `solo_off.png`,
`solo_on.png`.

### 6.1 Where you are

- **Breadcrumbs** replace the project row inside an element: `SONG › REFREN
  › LOOP DR`, the focus in lime (the colour of a lit key today). A tap on a
  crumb jumps to that level; the power button's click goes up one level (in the
  existing back chain, before leaving Arrange). A deep path collapses to
  `… › REFREN › LOOP DR`; a tap on `…` shows the whole path.
- At the top level the row is as today (project, EDIT, SYNC, LOOP, ZOOM).
  Inside an element, EDIT and SYNC move under a `⋯` key at the row's end
  (builder's call, made here).
- **Mini-map of the parent** in place of the bar ruler: the parent level in
  miniature, its clips in their baked/live colours, every occurrence of the
  focus in orange. A tap on one puts the playhead there (builder's call, made
  here).

### 6.2 What is what

- orange block: plays live;
- ice block: baked (today's freeze colour, `kIce`);
- a stacked-layers icon on a block: an element, with levels inside;
- a blue bar along a block's bottom edge: baking in the background, with
  progress.

### 6.3 SOLO of the focus

A **SOLO** key in the empty cell above the track headers, left of the
mini-map, in the column of the tracks' S keys and lit in the same yellow.
On, only the focus is heard and the mini-map goes dim (the parent is not
heard); off, PLAY plays the song around the focus. Not shown at the top level.
It turns off on leaving the level (builder's call, made here).

### 6.4 Entering, creating, inserting

- **Enter**: select an element block, CLIP EDIT (the key that opens a seq
  block today).
- **Create** (builder's calls, made here): *element from region* — the loop
  region across the tracks becomes a new element and is replaced by one
  element clip (the main way a chorus is made from what is already there);
  *new empty element* on a track; both under EDIT.
- **Insert**: EDIT → an element from the library (beside today's "clip from
  the library", which lists only the project's `clips/`).

## 7. Compatibility

- **Old songs open unchanged.** A song with no element clips loads, plays,
  freezes and saves exactly as today and keeps its `song.json` version.
- **A song or element that uses element clips** is written with the next
  `song.json` version (5). Firmware before it reports such a song unreadable,
  as today for any newer version. Forward compatibility is not a goal.
- **Old elements stay editable**: an element made on older firmware loads by
  the same versioned rules as any song; nothing in an element is rewritten
  until it is edited.
- **A changed renderer** (`kRenderVersion`) makes bakes stale, not invalid:
  the old bake plays until the new one exists.
- **Missing sources** (a kit deleted, a sample renamed) never break playback:
  the bake plays. Editing such an element says what is missing.
- **Deleting or renaming an element that something uses** is refused with
  "used in N songs", and offers instead to keep it as a library element
  hidden from the songs list (builder's call, made here). References are by
  id, so a rename of the display name breaks nothing.
- Unknown keys are dropped on save today; anything new in `song.json` must
  be added so that the version tells an old reader to refuse rather than
  silently lose it.

## 8. Effects (not built yet)

Effects are not part of this work, but the design keeps their place:

- an effect chain belongs to a track, a clip or an element, is stored in that
  element's `song.json` and is part of its content hash;
- a recording with effects is baked (§3.1); a mixdown runs the chains (§4.1);
- editing an effect is an edit like any other: the element's hash changes and
  its ancestors re-bake.

## 9. Goals, non-goals, anti-goals

**Goals**

- Elements nest to any depth, are reused across songs, stay fully editable.
- Moving between levels and pressing PLAY sound at once when the cache is
  warm, with a constant number of streams at any depth (focus + context).
- Background baking, including during playback for everything that is a
  mixdown.
- Copy or replace when editing a shared element.
- Variant B GUI with SOLO.

**Non-goals**

- Implementing effects.
- Sharing elements between cards or through crosspad.app.
- More than four tracks per level.
- Editing two elements at once; undo history.

**Anti-goals** (failure even if it technically works)

- A modal "please wait" on an action that could have played an older bake.
- Silence or a gap where the user expects to hear the song, beyond the
  accepted progress bar when the cache is cold.
- An edit lost, or a shared element changed in a song the user did not choose
  to change.
- Any existing song that plays, opens or saves differently than before.
- The user losing track of where they are (the reason for crumbs + mini-map).

## 10. Constraints the build must respect

- No card I/O on the LVGL thread or the CDC loop: every card operation is a
  `song_io` request (CLAUDE.md, Audio Song).
- One song ring (64 kB), one `readers_` fd per track, FAT `max_files` 20:
  the open path must not open more files than today's four tracks plus the
  context stem and a baker's two.
- Engine routes: at most three seq engines; only the focus uses them.
- Offline rendering is refused within 2 s of a pad press or while a USB host
  streams; mixdowns must not depend on it.
- PSRAM: rings never take the last 128 kB; a baker's buffers come from the
  same rules.
- Core owns the model and engine (portable, Catch2-tested); platforms only
  implement `ISongEnginePlatform`.

## 11. Stages

Each stage ships on its own and leaves every existing song working.

1. **Element clips and the library**: element id, library folder, element
   clips on audio tracks, cycle check, content hash, mixdown bake on the
   worker (while stopped first), shared cache; Arrange enters an element with
   CLIP EDIT, leaves with power click; breadcrumbs. Focus live, all else baked
   (expanded per level — fine at shallow depth).
2. **Context stem, mini-map and SOLO**: constant streams at any depth.
3. **Copy or replace**: drafts, reverse index, the question on leaving,
   background re-bake of other users.
4. **Baking while playing**: mixdowns during playback under the bandwidth
   rule, idle baking of other songs, cache cap and eviction, kit identity in
   freeze keys.
5. **Effects** when they exist (§8).

## 12. Verification

- Core Catch2: hash propagation (a leaf edit changes exactly its ancestors),
  cycle refusal, context-stem key independent of the focus's content, flatten
  with nesting, copy/replace on the reverse index, version-5 round trip and
  version-4 songs untouched.
- HIL, driving the GUI as a person would (`UI_TREE`, `TOUCH_TAP`, the
  encoder, the pads; verbs only to read results): a three-level song (song →
  chorus → drum loop with a kit track), enter the loop, play the pads with its
  kit, change notes, go up twice, PLAY at once. Pass: sound within one ring
  prime of PLAY, no ring underrun, the edited loop heard live and then
  replaced by its bake without a gap, files open within budget.
- The same scenario after a cold cache (cache folder cleared) shows progress
  and recovers.

## 13. Left to the builder

- Exact hash composition and byte formats, the library and cache paths, the
  index format.
- Cache cap numbers and the eviction details.
- The baker's back-off thresholds.
- How the songs list and the library picker present songs vs elements.
- Mini-map rendering details and how a tap maps to the playhead.
