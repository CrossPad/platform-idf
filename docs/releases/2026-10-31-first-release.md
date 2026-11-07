# First release -- the firmware that went to users (31.10.2026)

On 31 October 2026 the first CrossPads (v2, PCB r20) to leave the team were
flashed with this firmware. It is the first firmware CrossPad ships to people
outside the team, and it is Steve Nash's work.

Git: branch `first-release` of platform-idf, tag `first-release-2026-10-31`.
Every submodule is pinned by a `first-release` branch of its own.

## The image

| What | Value |
|---|---|
| `APP_SHA` (sha256 of `CrossPad.elf`) | `da17a9799301d3e1e92064a230acd723d4eb017a28ef6f956a2be819dfbae8ef` |
| `CrossPad.bin` sha256 | `5991ef697d7be5869f04d7965a4f17b3e00359b1093dc16cc6fb28ae35dd32bb` |
| Version string on the board | `v1.1.0-rc6-209-gfb605a7e` |
| Assets id (`/assets/.assets_id`) | `b360b39790a7996fef0f32925b53442093666beae9b076a5996a661d9e29fd27` |
| `assets.bin` sha256 | `a78560368c70fa8fc78cb8e187073f2eee45beace8567f26f6461c087f91727d` |
| STM32 firmware | 1.14, protocol `0x0012` (bundled in assets) |
| Board revision | v2 (`sdkconfig.v2`, `build_v2`) |
| ESP-IDF | 5.5.4 (local build) |

The version string names `fb605a7e`, the commit the build was configured at.
The image was built from that commit plus two fixes made the same day --
crosspad-sampler 0.24.3 (kit browser encoder) and crosspad-sequencer 0.13.1
(top row under the transport bar) -- which the tagged commit records. The
code in the tag is the code on the boards; only the version string is a
commit behind. A build of the tag reconfigured from scratch says
`...-g<tag commit>` and has another `APP_SHA` for that reason alone.

The exact binaries (bootloader, partition table, `ota_data_initial`, app,
assets, ELF) and their sha256 are archived outside the repo. Flash offsets
(`flasher_args.json`): bootloader `0x0`, partition table `0x8000`, otadata
`0x10000`, app `0x20000`, assets `0x720000`.

## Versions

| Component | Version | Commit |
|---|---|---|
| crosspad-core | 2.44.0 | `8218896` |
| crosspad-gui | 2.11.3 | `f6a2292` |
| Mixer | 0.21.0 | `38d6796` |
| Sampler (Kit browser) | 0.24.3 | `7ee713d` |
| Sequencer | 0.13.1 | `01a1252` |
| Arrange | 0.42.1 | `c2ae5db` |
| Recorder | 0.9.0 | `2889d46` |
| Slicer | 0.5.0 | `cc6e3e9` |
| PitchPlay | 0.13.0 | `46dd7af` |
| Song | 0.3.1 | `71ba9fa` |
| Pad Mixer | 0.3.0 | `aff35dc` |
| DAW Control | 0.5.0 | `23700e9` |
| Instructions | 0.3.0 | `d4f620f` |

Built in, besides the apps: Settings, Firmware (slots A/B, the card's
firmware library). Doom, Pong and Piano Tiles are checked out but register no
app in this image; Fish Tank is an empty local stand-in (its repo is gone).

## What is in it

- USB records the Mixer's USB bus at every boot: ROUTE decides what a DAW
  gets. The outputs are named as the jacks: Out 1/2 = Phones (codec 0, with
  the speakers), Out 3/4 = Line out (codec 1, scale capped at the codec's 56).
- Line 3/4 correction without the 17 kHz howl; codec 0 noise after a restart
  cleared by a soft reset.
- Mixer 0.21: vertical strips, ROUTE as send sliders, the encoder on every
  element.
- Recorder: the input button names what the Mixer's pair puts on each
  converter (MIC, LINE 1/2, LINE 3/4).
- Slicer: FX (pitch with the length kept, low/high pass, NORMALIZE), SNAP
  to zero crossings, FREE with ATK, KIT as a User kit (PERF, a WAV per slice)
  or a Slice kit, the Sampler's PROCESSING box.
- Kit browser: a long press on the list goes to the lists on the left; a
  click there goes back to the list.
- Settings: the encoder keeps working after USB mode is changed.

## Flashed units

Every flashed unit keeps its previous firmware in slot A. A user's board has
the USB serial guard on: switching it to the CDC profile asks Allow on the
screen.
