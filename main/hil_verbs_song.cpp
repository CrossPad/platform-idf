/* CDC verbs: the sequencer, songs and the clock. Dispatch order is in hil_control.cpp. */
#include "include/hil_text.h"
#include "crosspad/instrument/InstrumentPlayState.hpp"
#include "SequenceStorePort.hpp"

#include "tinyusb.h"

#if __has_include(<crosspad-mixer/AudioMixerEngine.hpp>)
#include <crosspad-mixer/AudioMixerEngine.hpp>
#include <crosspad-mixer/MixerPresets.hpp>
#define HIL_HAS_MIXER 1
#endif
#include "crosspad/platform/PlatformServices.hpp"
#include "crosspad/song/ISongEngine.hpp"
#include "SongEnginePort.hpp"
#if __has_include("crosspad-dawcontrol/DawControlState.hpp")
#include "crosspad-dawcontrol/DawControlState.hpp"
#endif

#if CONFIG_CROSSPAD_SAMPLER_MARCEL
#include "ml_smpl_stream_ply.h"
#include "MarcelSampleNode.hpp"
#endif
#include "midi_clock.h"

#include "hil_verbs.h"

bool verb_clock_status(VerbLine &l)
{
    char *cmd = l.cmd;
    if (strncmp(cmd, "CLOCK_STATUS", 12) == 0) {
        char out[200];
        midi_clock_status(out, sizeof(out), strstr(cmd + 12, "RESET") != nullptr);
        cdc_reply(out);
        return true;
    }
    return false;
}

bool verb_click(VerbLine &l)
{
    char *cmd = l.cmd;
    /* CLICK <0|1> [bpm] [bars] -- the metronome, which is also the sequencer's
     * clock. Reports where it is, so a scenario can check the grid without
     * listening. */
    if (strncmp(cmd, "CLICK", 5) == 0) {
        int on = -1, bpm = 0, bars = 0;
        const int n = sscanf(cmd + 5, "%d %d %d", &on, &bpm, &bars);
        auto &m = getMetronome();
        if (n >= 2 && bpm > 0) m.setBpm((float)bpm);
        if (n >= 3 && bars > 0) m.setBarCount((uint8_t)bars);
        if (n >= 1) { if (on) m.start(); else m.stop(); }
        char reply[128];
        snprintf(reply, sizeof(reply),
                 "CLICK: running=%d bpm=%.1f beats=%u bars=%u bar=%u beat=%u pos=%lu/%lu\r\n",
                 m.running() ? 1 : 0, m.bpm(), (unsigned)m.beatsPerBar(),
                 (unsigned)m.barCount(), (unsigned)m.currentBar(), (unsigned)m.currentBeat(),
                 (unsigned long)m.positionSamples(), (unsigned long)m.loopSamples());
        cdc_reply(reply);
        return true;
    }
    return false;
}

bool verb_seq_dump(VerbLine &l)
{
    char *cmd = l.cmd;
    /* SEQ_DUMP -- every recorded event, so a scenario can check the grid a take
     * landed on without listening to it. len= is how long the pad was held, in
     * samples, 0 for a tap. Its own verb rather than a SEQ
     * subcommand: the reply is multi-line and a reader cannot tell the two
     * shapes apart from one prefix. */
    if (strncmp(cmd, "SEQ_DUMP", 8) == 0) {
        auto& sq = getSequencer();
        for (uint8_t s = 0; s < crosspad::PatternSequencer::kNumSequences; ++s) {
            for (uint16_t i = 0; i < sq.eventCount(s); ++i) {
                uint32_t pos = 0; uint8_t pad = 0, vel = 0;
                if (!sq.eventAt(s, i, pos, pad, vel)) break;
                char row[112];
                /* len: samples the note is held, 0 for a hit. */
                snprintf(row, sizeof(row), "SEQDUMP: seq=%u i=%u pos=%lu pad=%u vel=%u len=%lu\r\n",
                         (unsigned)s, (unsigned)i, (unsigned long)pos,
                         (unsigned)pad, (unsigned)vel, (unsigned long)sq.eventLength(s, i));
                cdc_reply(row);
            }
        }
        cdc_reply("SEQDUMP: end\r\n");
        return true;
    }
    return false;
}

bool verb_seq(VerbLine &l)
{
    char *cmd = l.cmd;
    /* SEQ [REC|OVERDUB|PLAY|STOP|CLEAR|DOUBLE|HALF|ERASE 0|1|COUNTIN 0|1|BARS n|BPM n|QUANT n] -- the loop recorder.
     * Bare SEQ reports, so a scenario can check what was captured without
     * listening to it. */
    if (strncmp(cmd, "SEQ", 3) == 0 && (cmd[3] == 0 || cmd[3] == ' ')) {
        auto& sq = getSequencer();
        const char* arg = cmd + 3;
        while (*arg == ' ') ++arg;
        int n = 0, n2 = 0;
        if      (strncmp(arg, "REC", 3) == 0)     sq.record();
        else if (strncmp(arg, "OVERDUB", 7) == 0) sq.overdub();
        else if (strncmp(arg, "PLAY", 4) == 0)    sq.playback();
        else if (strncmp(arg, "STOP", 4) == 0)    sq.stop();
        else if (strncmp(arg, "CLEAR", 5) == 0)   sq.reset();
        else if (sscanf(arg, "SCENE %d", &n) == 1 && n >= 0) sq.selectScene((uint8_t)n);
        else if (strncmp(arg, "SAVE", 4) == 0) crosspad::sequence_store_save_soon();
        else if (sscanf(arg, "BARS %d %d", &n, &n2) == 2 && n >= 0 && n2 > 0)
            sq.setSceneBars((uint8_t)n, (uint8_t)n2);
        else if (sscanf(arg, "BARS %d", &n) == 1 && n > 0)  sq.setBars((uint8_t)n);
        else if (sscanf(arg, "BPM %d", &n) == 1 && n > 0)   sq.setClickBpm((float)n);
        else if (strncmp(arg, "ADD", 3) == 0)     sq.addScene();
        else if (strncmp(arg, "DOUBLE", 6) == 0)  sq.doubleScene(sq.activeScene());
        else if (strncmp(arg, "HALF", 4) == 0)    sq.halveScene(sq.activeScene());
        else if (sscanf(arg, "ERASE %d", &n) == 1) sq.setErasing(n != 0);
        else if (sscanf(arg, "COUNTIN %d", &n) == 1) sq.setCountIn(n != 0);
        else if (sscanf(arg, "DEL %d", &n) == 1 && n >= 0) sq.removeScene((uint8_t)n);
        else if (strncmp(arg, "DEL", 3) == 0)     sq.removeScene(sq.activeScene());
        else if (sscanf(arg, "QUANT %d", &n) == 1)
            sq.setQuantization((crosspad::Quantization)n);
        else if (*arg) { cdc_reply("ERR SEQ REC|OVERDUB|PLAY|STOP|CLEAR|SAVE|SCENE n|ADD|DEL [n]|DOUBLE|HALF|ERASE 0|1|COUNTIN 0|1|BARS [s] n|BPM n|QUANT n\r\n"); return true; }

        /* Every scene the engine can hold, not four: a scenario that adds a
         * scene of its own lands past the player's, at any index up to 7. */
        char bars[48] = "", ev[64] = "";
        for (uint8_t s = 0; s < crosspad::PatternSequencer::kNumSequences; ++s) {
            const char* sep = s ? "/" : "";
            snprintf(bars + strlen(bars), sizeof(bars) - strlen(bars), "%s%u", sep, (unsigned)sq.sceneBars(s));
            snprintf(ev + strlen(ev), sizeof(ev) - strlen(ev), "%s%u", sep, (unsigned)sq.eventCount(s));
        }
        EXT_RAM_BSS_ATTR static char reply[320];   // static: the path makes it too big for the 5 kB main task
        snprintf(reply, sizeof(reply),
                 "SEQ: mode=%u state=%u scenes=%u scene=%u>%u bars=%s "
                 "ev=%s bpm=%.1f quant=%u pos=%.3f dirty=%d erase=%d countin=%d file=%s\r\n",
                 (unsigned)sq.mode(), (unsigned)sq.state(), (unsigned)sq.sceneCount(),
                 (unsigned)sq.activeScene(), (unsigned)sq.pendingScene(),
                 bars, ev,
                 sq.getClickBpm(), (unsigned)sq.quantization(),
                 (double)sq.progress(), sq.dirty() ? 1 : 0, sq.erasing() ? 1 : 0, sq.countIn() ? 1 : 0,
                 crosspad::sequence_store_current_file());
        cdc_reply(reply);
        return true;
    }
    return false;
}

bool verb_song(VerbLine &l)
{
    char *cmd = l.cmd;
    /* SONG [ADD s r|SET i s r|DEL i|CLEAR|PLAY [i]|LOOP 0|1] -- the song
     * arrangement. Bare SONG reports the blocks and where playback is. */
    if (strncmp(cmd, "SONG", 4) == 0 && (cmd[4] == 0 || cmd[4] == ' ')) {
        auto& sq = getSequencer();
        const char* arg = cmd + 4;
        while (*arg == ' ') ++arg;
        int a = 0, b = 0, c = 0;
        if      (sscanf(arg, "ADD %d %d", &a, &b) == 2) sq.songInsert(sq.songLength(), (uint8_t)a, (uint8_t)b);
        else if (sscanf(arg, "SET %d %d %d", &a, &b, &c) == 3) sq.songSet((uint8_t)a, (uint8_t)b, (uint8_t)c);
        else if (sscanf(arg, "DEL %d", &a) == 1) sq.songRemove((uint8_t)a);
        else if (strncmp(arg, "CLEAR", 5) == 0) { while (sq.songLength()) sq.songRemove(0); }
        else if (sscanf(arg, "LOOP %d", &a) == 1) sq.setSongLoop(a != 0);
        else if (strncmp(arg, "PLAY", 4) == 0) {
            if (sscanf(arg, "PLAY %d", &a) != 1) a = 0;
            sq.setSongMode(true);
            sq.songLocate((uint8_t)a);
            sq.playback();
        }
        else if (*arg) { cdc_reply("ERR SONG ADD s r|SET i s r|DEL i|CLEAR|PLAY [i]|LOOP 0|1\r\n"); return true; }

        char reply[240];
        constexpr int kTail = 3;
        int n = snprintf(reply, sizeof(reply), "SONG: mode=%d loop=%d len=%u pos=%u pass=%u scene=%u blocks=",
                         sq.songMode() ? 1 : 0, sq.songLoop() ? 1 : 0, (unsigned)sq.songLength(),
                         (unsigned)sq.songPosition(), (unsigned)sq.songPass(), (unsigned)sq.activeScene());
        for (uint8_t i = 0; i < sq.songLength(); ++i) {
            uint8_t sc = 0, r = 0;
            sq.songBlock(i, sc, r);
            n += snprintf(reply + n, sizeof(reply) - kTail - n, "%s%u:%u", i ? "," : "", (unsigned)sc, (unsigned)r);
            n = std::min(n, (int)sizeof(reply) - kTail);
        }
        snprintf(reply + n, sizeof(reply) - n, "\r\n");
        cdc_reply(reply);
        return true;
    }
    return false;
}

bool verb_asong_seq(VerbLine &l)
{
    char *cmd = l.cmd;
    /* ASONG [OPEN name|BOUNCE scene track|PLAY [bar]|STOP|CANCEL|LEN bars|SYNC 0|1|TEMPO bpm|RENAME name|
     *        REGION a b|MERGE mask from to|IMPORT path a b track bar lead|EXPORT|EXPORTWAV|REPORT|
     *        REC bar [beats]|DUB bar [beats]|PUNCH R|D|N|BOUNCESEQ track|VOL track pct|MUTE track 0|1|
 *        CUT track bar|DUPREGION|SEQTRACK [kit]|PITCHTRACK instrument|DELTRACK track|
     *        DUP track block|DELBLOCK track block|MOVE track block bar|PASTE a b at|TRACKKIT track kit] -- the
     * audio song: the open project, a bounce in progress, playback. Scene and
     * track are 0-based. Every verb only asks -- the card work runs on the
     * song engine's worker, never on this drain loop -- so ok=1 means the
     * request was taken; poll bare ASONG for open=, play= and bounce=.
     * EXPORT writes the song as an Ableton Live Set into its folder (edit=
     * says when it is done); REPORT puts the last export's lines, one
     * "REPORT:" row each, before the status. REC and DUB record the pads onto
     * the seq track from a bar (Arrange's REC/DUB, beats of count-in), PUNCH
     * puts a REC or DUB in, or the take out, while the song plays; ASONG_SEQ
     * says what landed. */
    /* ASONG_SEQ [track] -- a seq track's blocks (the first seq track's
     * without one), one "ASEQ:" row each (bar, bars, from, notes, own=1 for
     * notes of their own, 0 for the scene's), then the take running, the
     * block the seq view edits, the track's route and the armed track. */
    if (strncmp(cmd, "ASONG_SEQ", 9) == 0 && (cmd[9] == 0 || cmd[9] == ' ')) {
        auto* se = crosspad::getPlatformServices().songEngine;
        if (!se) { cdc_reply("ASEQ: no engine\r\n"); return true; }
        static char row[112];
        int track = -1, route = -1, armed = -1;
        int asked = -1;
        if (sscanf(cmd + 9, " %d", &asked) != 1) asked = -1;
        {
            std::lock_guard<std::recursive_mutex> l(se->songMutex());
            const crosspad::AudioSong& song = se->song();
            for (int t = 0; t < crosspad::AudioSong::kTracks && track < 0; ++t)
                if (song.seqRoute(t) >= 0 && (asked < 0 || t == asked)) track = t;
            route = song.seqRoute(track);
            armed = song.armedSeqTrack();
            if (track >= 0) {
                for (const auto& c : song.tracks[track].clips) {
                    snprintf(row, sizeof(row), "ASEQ: bar=%u bars=%u from=%u notes=%u own=%d\r\n",
                             (unsigned)c.bar, (unsigned)c.bars, (unsigned)c.from,
                             (unsigned)(c.notes ? c.notes->notes.size() : 0), c.notes ? 1 : 0);
                    cdc_reply(row);
                }
            }
        }
        snprintf(row, sizeof(row), "ASEQ: track=%d take=%d punchable=%d edit=%d route=%d armed=%d\r\n", track,
                 (int)se->seqTake(), se->seqPunchable() ? 1 : 0, (int)se->seqEditBar(), route, armed);
        cdc_reply(row);
        return true;
    }
    return false;
}

bool verb_asong_trk(VerbLine &l)
{
    char *cmd = l.cmd;
    /* ASONG_TRK -- every track in use, one "ATRK:" row each: its kind (a
     * audio, s seq), the route its notes take and the one they take now
     * (AudioSong::liveRoute(): -1 for a kit track the kits' engine does not
     * hold), its freeze (FreezeState: 0 none, 1 frozen, 2 stale, 3 freezing),
     * its blocks and its meter now (lvl=, the louder side, what the lane's
     * meter shows; a seq engine's peak is taken by the ask); then the armed
     * track and the live kit track (liveKit). */
    if (strcmp(cmd, "ASONG_TRK") == 0) {
        auto* se = crosspad::getPlatformServices().songEngine;
        if (!se) { cdc_reply("ATRK: no engine\r\n"); return true; }
        EXT_RAM_BSS_ATTR static char row[96];
        int armed = -1, live_kit = -1, used = 0;
        for (int t = 0; t < crosspad::AudioSong::kTracks; ++t) {
            bool inUse = false, seq = false;
            int route = -1, live = -1;
            unsigned blocks = 0;
            {
                std::lock_guard<std::recursive_mutex> l(se->songMutex());
                const crosspad::AudioSong& song = se->song();
                const crosspad::SongTrack& tr = song.tracks[t];
                inUse = tr.inUse;
                seq = tr.kind == crosspad::TrackKind::Seq;
                route = song.seqRoute(t);
                live = song.liveRoute(t);
                blocks = (unsigned)tr.clips.size();
                armed = song.armed;
                live_kit = song.liveKit;
            }
            if (!inUse) continue;
            ++used;
            float ll = 0.0f, lr = 0.0f;
            se->trackLevels(t, ll, lr);
            snprintf(row, sizeof(row), "ATRK: t=%d kind=%c route=%d live=%d fz=%d blocks=%u lvl=%.2f\r\n", t,
                     seq ? 's' : 'a', route, live, (int)se->freezeState((uint8_t)t), blocks, std::max(ll, lr));
            cdc_reply(row);
        }
        snprintf(row, sizeof(row), "ATRK: used=%d armed=%d live_kit=%d\r\n", used, armed, live_kit);
        cdc_reply(row);
        return true;
    }
    return false;
}

bool verb_asong_dbg(VerbLine &l)
{
    char *cmd = l.cmd;
    /* Why a playing song can be silent. ASONG already fills its 300-byte
     * budget on the 5 kB main task, so this is its own line. */
    if (strcmp(cmd, "ASONG_DBG") == 0) {
        auto* se = crosspad::getPlatformServices().songEngine;
        if (!se) { cdc_reply("ASONGDBG: no engine\r\n"); return true; }
        /* Static: the line outgrew what this task's stack should carry. */
        EXT_RAM_BSS_ATTR static char row[544];
        snprintf(row, sizeof(row), "ASONGDBG: %s\r\n", se->debugLine());
        cdc_reply(row);
        return true;
    }
    return false;
}

/* A track's fader in percent, or its M -- as Arrange's MIX sets them. */
static bool asong_track_mix(crosspad::ISongEngine* se, const char* arg, int a, int b)
{
    bool ok = true;
    ok = a >= 0 && a < crosspad::AudioSong::kTracks;
    if (ok) {
        {
            std::lock_guard<std::recursive_mutex> l(se->songMutex());
            auto& t = se->song().tracks[a];
            if (arg[0] == 'V') t.volume = (float)(b < 0 ? 0 : b) / 100.0f;
            else               t.muted = b != 0;
        }
        se->commit();
    }
    return ok;
}

/* A kit track on the lowest free track, armed, with a scene of
 * its own -- what Arrange's ADD TRACK > KITS does -- playing the
 * kit named (a kit.json as the kit list names it), or the song's.
 * It plays live once Arrange hands it the kits' engine. */
static bool asong_add_kit_track(crosspad::ISongEngine* se, const char* arg)
{
    bool ok = true;
    const char* kit = arg[8] == ' ' ? arg + 9 : "";
    auto* sq = crosspad::getPlatformServices().sequencer;
    const bool added = sq && sq->addScene();
    {
        std::lock_guard<std::recursive_mutex> l(se->songMutex());
        auto& song = se->song();
        const int scene = song.sceneForNewSeq(sq ? sq->getSceneCount() : 1, added);
        const int t = song.freeTrack();
        ok = t >= 0 && scene >= 0 && song.addSeqTrack(t, crosspad::SeqSource::Kit, (uint8_t)scene, kit);
        if (ok) song.armed = (int8_t)t;
    }
    if (ok) se->commit();
    return ok;
}

/* A Pitch track on the lowest free track, armed, with a scene of
 * its own, playing the instrument named (an instrument.json, or a
 * pitched kit.json, from the card's root) -- Arrange's KITS >
 * PITCH with the instrument already picked. Beside two kit tracks
 * since core 2.6 (route 2). */
static bool asong_add_pitch_track(crosspad::ISongEngine* se, const char* arg)
{
    bool ok = true;
    auto* sq = crosspad::getPlatformServices().sequencer;
    const bool added = sq && sq->addScene();
    {
        std::lock_guard<std::recursive_mutex> l(se->songMutex());
        auto& song = se->song();
        const int scene = song.sceneForNewSeq(sq ? sq->getSceneCount() : 1, added);
        const int t = song.freeTrack();
        ok = t >= 0 && scene >= 0 && song.addSeqTrack(t, crosspad::SeqSource::Pitch, (uint8_t)scene);
        if (ok) {
            song.armed = (int8_t)t;
            song.instrument = arg + 11;
        }
    }
    if (ok) {
        se->commit();
        /* What a pick in Pitch Play leaves behind: Arrange keeps this
         * instrument loaded for the track (its sound keeper). */
        crosspad::InstrumentPlayState& ps = crosspad::instrumentPlayState();
        ps.instrumentPath = arg + 11;
        crosspad::instrumentPlayStatePublished();
    }
    return ok;
}

/* DEL on a track's header, confirmed: its clips leave the song,
 * the tracks under it move up. */
static bool asong_del_track(crosspad::ISongEngine* se, int a)
{
    bool ok = true;
    {
        std::lock_guard<std::recursive_mutex> l(se->songMutex());
        auto& song = se->song();
        ok = a >= 0 && a < crosspad::AudioSong::kTracks && song.tracks[a].inUse;
        if (ok) song.removeTrack(a);
    }
    if (ok) se->commit();
    return ok;
}

/* A block's DUP (a copy straight after it), DEL, or MOVE to bar c3:
 * block b of track a, as the keys a block opens in Arrange. */
static bool asong_block_edit(crosspad::ISongEngine* se, const char* arg, int a, int b, int c3)
{
    bool ok = true;
    {
        std::lock_guard<std::recursive_mutex> l(se->songMutex());
        auto& song = se->song();
        ok = a >= 0 && a < crosspad::AudioSong::kTracks && b >= 0 &&
             b < (int)song.tracks[a].clips.size() && c3 >= 0;
        if (ok) {
            auto& cs = song.tracks[a].clips;
            if (arg[1] == 'U') {
                crosspad::SongClip copy = cs[(size_t)b];
                copy.bar = cs[(size_t)b].bar + cs[(size_t)b].bars;
                cs.push_back(copy);
            } else if (arg[0] == 'D') {
                cs.erase(cs.begin() + b);
            } else {
                cs[(size_t)b].bar = (uint32_t)c3;
            }
            song.sortTrack(a);
        }
    }
    if (ok) se->commit();
    return ok;
}

/* EDIT's copy of bars [a, b) and paste at bar c3: what was there
 * moves on, cut where the paste starts if it crossed it. */
static bool asong_paste(crosspad::ISongEngine* se, int a, int b, int c3)
{
    bool ok = true;
    {
        std::lock_guard<std::recursive_mutex> l(se->songMutex());
        auto& song = se->song();
        ok = a >= 0 && b > a && c3 >= 0;
        if (ok) {
            song.seqBlocksExplicit(se->playEndBars());
            const uint32_t spb = se->framesPerBar();
            const auto part = song.copyRange((uint32_t)a, (uint32_t)b, spb);
            song.insertBars((uint32_t)c3, (uint32_t)(b - a), spb);
            song.pasteRange(part, (uint32_t)c3);
        }
    }
    if (ok) se->commit();
    return ok;
}

/* EDIT's "Cut track T at bar B": the block of track a running
 * across bar b in two there (ok=0 when none does). */
static bool asong_cut(crosspad::ISongEngine* se, int a, int b)
{
    bool ok = true;
    int second = -1;
    if (a >= 0 && a < crosspad::AudioSong::kTracks && b >= 0) {
        std::lock_guard<std::recursive_mutex> l(se->songMutex());
        auto& song = se->song();
        song.seqBlocksExplicit(se->playEndBars());
        second = song.cutClip(a, (uint32_t)b, se->framesPerBar());
    }
    ok = second >= 0;
    if (ok) se->commit();
    return ok;
}

/* EDIT's "Duplicate the region x2": the region's bars again right
 * after it, every track, and the region twice as long. */
static bool asong_dup_region(crosspad::ISongEngine* se)
{
    bool ok = true;
    {
        std::lock_guard<std::recursive_mutex> l(se->songMutex());
        auto& song = se->song();
        ok = song.hasRegion();
        if (ok) {
            const uint32_t w = song.loopEnd - song.loopStart;
            song.seqBlocksExplicit(se->playEndBars());
            const uint32_t spb = se->framesPerBar();
            const auto part = song.copyRange(song.loopStart, song.loopEnd, spb);
            song.insertBars(song.loopEnd, w, spb);
            song.pasteRange(part, song.loopEnd);
            song.loopEnd += w;
        }
    }
    if (ok) se->commit();
    return ok;
}

/** The ASONG status line every action ends with. */
static void asong_reply_status(crosspad::ISongEngine* se, bool ok)
{
    const crosspad::BounceStatus bs = se->bounceStatus();
    std::lock_guard<std::recursive_mutex> songLock(se->songMutex());
    const crosspad::AudioSong& song = se->song();
    /* Static and integer-only: this runs on the 5 kB main task, where a
     * 300-byte frame plus printf's float path is enough to overflow it. */
    EXT_RAM_BSS_ATTR static char reply[448];   /* PSRAM: the bounce fields stand before file=/err= */
    constexpr int kTail = 3;   // "\r\n" and the terminator always fit
    const int posCenti = (int)(se->positionBars() * 100.0);
    /* Names last: a project may have spaces and an error always does, so
     * everything before them is key=value a host splits on spaces, and
     * "project=" / " err=" take the rest of the line. */
    int n = snprintf(reply, sizeof(reply) - kTail,
                     "ASONG: ok=%d open=%d edit=%d eprog=%u rgn=%u-%u bpm=%d len=%u sync=%d play=%d pos=%d.%02d under=%u "
                     "openfail=%u bounce=%d done=%u total=%u clips=",
                     ok ? 1 : 0, (int)se->openState(), (int)se->editState(), (unsigned)se->editProgress(),
                     (unsigned)song.loopStart, (unsigned)song.loopEnd, (int)lroundf(song.bpm),
                     (unsigned)song.playEnd(), song.syncSeq ? 1 : 0, se->playing() ? 1 : 0,
                     posCenti / 100, posCenti % 100, (unsigned)se->underruns(),
                     (unsigned)se->openFailures(), (int)bs.state,
                     (unsigned)bs.framesDone, (unsigned)bs.framesTotal);
    /* snprintf answers what it would have written: a long name or error
     * makes that more than the buffer, and every offset after it must be
     * clamped or the next write lands past the end. */
    n = std::min(n, (int)sizeof(reply) - kTail);
    for (int t = 0; t < std::max(song.usedTracks(), crosspad::AudioSong::kTracksV5); ++t) {
        n += snprintf(reply + n, sizeof(reply) - kTail - n, "%s%u", t ? "/" : "",
                      (unsigned)song.tracks[t].clips.size());
        n = std::min(n, (int)sizeof(reply) - kTail);
    }
    /* How the card kept up with the last bounce: what it could take is
     * 4096000 / chunk_avg_us kB/s; drain_kbps is the bus's own rate
     * while it keeps up. io_fb: pieces through stdio, since boot. */
    n += snprintf(reply + n, sizeof(reply) - kTail - n,
                  " chunks=%u chunk_avg_us=%u chunk_max_us=%u drain_kbps=%u io_fb=%u pieces=%u/%u",
                  (unsigned)bs.chunks, (unsigned)bs.chunkAvgUs, (unsigned)bs.chunkMaxUs,
                  (unsigned)bs.drainKBps, (unsigned)crosspad::song_engine_io_fallbacks(),
                  (unsigned)bs.piecesDone, (unsigned)bs.pieces);
    n = std::min(n, (int)sizeof(reply) - kTail);
    n += snprintf(reply + n, sizeof(reply) - kTail - n, " file=%s project=%s%s%s",
                  bs.file.empty() ? "-" : bs.file.c_str(), song.name.c_str(),
                  bs.error.empty() ? "" : " err=", bs.error.c_str());
    n = std::min(n, (int)sizeof(reply) - kTail);
    snprintf(reply + n, sizeof(reply) - n, "\r\n");
    cdc_reply(reply);
}

bool verb_asong(VerbLine &l)
{
    char *cmd = l.cmd;
    if (strncmp(cmd, "ASONG", 5) == 0 && (cmd[5] == 0 || cmd[5] == ' ')) {
        auto* se = crosspad::getPlatformServices().songEngine;
        if (!se) { cdc_reply("ERR ASONG no song engine\r\n"); return true; }
        const char* arg = cmd + 5;
        while (*arg == ' ') ++arg;
        int a = 0, b = 0, c3 = 0;
        bool ok = true;
        if      (strncmp(arg, "OPEN ", 5) == 0)             ok = se->requestOpen(arg + 5);
        else if (sscanf(arg, "BOUNCESEQ %d", &a) == 1)      ok = se->bounceSeqTrack((uint8_t)a);   // as Arrange's BOUNCE
        else if (sscanf(arg, "VOL %d %d", &a, &b) == 2 || sscanf(arg, "MUTE %d %d", &a, &b) == 2) {
            ok = asong_track_mix(se, arg, a, b);
        }
        else if (sscanf(arg, "FREEZE %d", &a) == 1) {
            /* Freeze track a now (Arrange does it when ARM moves on): renders
             * found on the card are taken, the rest made. */
            ok = a >= 0 && a < crosspad::AudioSong::kTracks && se->freezeTrack((uint8_t)a);
        }
        else if (sscanf(arg, "OFFLINE %d", &a) == 1) {
            /* A freeze off the clock (1, the default) or in real time (0). */
            crosspad::song_engine_set_offline(a != 0);
        }
        else if (sscanf(arg, "ARM %d", &a) == 1) {
            /* The seq track REC and DUB take onto -- Arrange's ARM. */
            {
                std::lock_guard<std::recursive_mutex> l(se->songMutex());
                se->song().armed = (int8_t)a;
            }
            se->commit();
        }
        else if (strncmp(arg, "SEQTRACK ", 9) == 0 || strcmp(arg, "SEQTRACK") == 0) {
            ok = asong_add_kit_track(se, arg);
        }
        else if (strncmp(arg, "PITCHTRACK ", 11) == 0) {
            ok = asong_add_pitch_track(se, arg);
        }
        else if (sscanf(arg, "DELTRACK %d", &a) == 1) {
            ok = asong_del_track(se, a);
        }
        else if (sscanf(arg, "DUP %d %d", &a, &b) == 2 || sscanf(arg, "DELBLOCK %d %d", &a, &b) == 2 ||
                 sscanf(arg, "MOVE %d %d %d", &a, &b, &c3) == 3) {
            ok = asong_block_edit(se, arg, a, b, c3);
        }
        else if (sscanf(arg, "PASTE %d %d %d", &a, &b, &c3) == 3) {
            ok = asong_paste(se, a, b, c3);
        }
        else if (int at = 0; sscanf(arg, "TRACKKIT %d %n", &a, &at) == 1 && at > 0 && arg[at]) {
            /* A kit track's kit picked again in Arrange's kit loader: the
             * song's too while it is the live one. */
            {
                std::lock_guard<std::recursive_mutex> l(se->songMutex());
                auto& song = se->song();
                ok = song.seqRoute(a) == 0;
                if (ok) song.setTrackKit(a, arg + at);
            }
            if (ok) se->commit();
        }
        else if (sscanf(arg, "BOUNCE %d %d", &a, &b) == 2)  ok = se->bounceScene((uint8_t)a, (uint8_t)b);
        else if (sscanf(arg, "PLAY %d", &a) == 1)           ok = se->play((uint32_t)a);
        else if (strncmp(arg, "PLAY", 4) == 0)              ok = se->play(0);
        else if (strncmp(arg, "STOP", 4) == 0)              se->stop();
        else if (strncmp(arg, "CANCEL", 6) == 0)            se->cancelBounce();
        else if (sscanf(arg, "REC %d %d", &a, &b) >= 1)     ok = se->recordSeq((uint32_t)a, crosspad::SyncTake::Record, (uint8_t)b);
        else if (sscanf(arg, "DUB %d %d", &a, &b) >= 1)     ok = se->recordSeq((uint32_t)a, crosspad::SyncTake::Overdub, (uint8_t)b);
        else if (strncmp(arg, "PUNCH ", 6) == 0)            ok = se->seqPunch(arg[6] == 'R' ? crosspad::SyncTake::Record
                                                                          : arg[6] == 'D' ? crosspad::SyncTake::Overdub
                                                                                          : crosspad::SyncTake::None);
        else if (sscanf(arg, "TEMPO %d", &a) == 1)         ok = se->requestTempo((float)a);
        else if (sscanf(arg, "MERGE %d %d %d", &a, &b, &c3) == 3) ok = se->requestMerge((uint32_t)a, (uint32_t)b, (uint32_t)c3);
        else if (strncmp(arg, "IMPORT ", 7) == 0) {
            EXT_RAM_BSS_ATTR static char src[128];
            unsigned ia = 0, ib = 0, it = 0, ibar = 0, ilead = 0;
            ok = sscanf(arg + 7, "%127s %u %u %u %u %u", src, &ia, &ib, &it, &ibar, &ilead) == 6 &&
                 se->requestImport(src, ia, ib, (uint8_t)it, ibar, ilead);
        }
        else if (sscanf(arg, "REGION %d %d", &a, &b) == 2) {
            {
                std::lock_guard<std::recursive_mutex> l(se->songMutex());
                se->song().loopStart = a > 0 ? (uint32_t)a : 0;
                se->song().loopEnd = b > 0 ? (uint32_t)b : 0;
            }
            se->commit();
        }
        else if (sscanf(arg, "CUT %d %d", &a, &b) == 2) {
            ok = asong_cut(se, a, b);
        }
        else if (strcmp(arg, "DUPREGION") == 0) {
            ok = asong_dup_region(se);
        }
        else if (strncmp(arg, "RENAME ", 7) == 0)           ok = se->requestRename(arg + 7);
        else if (strcmp(arg, "DELETE") == 0)                 ok = se->requestDelete();   // the open project, clips and all
        else if (strcmp(arg, "EXPORT") == 0)                 ok = se->requestExportLiveSet();
        else if (strcmp(arg, "EXPORTWAV") == 0)              ok = se->requestExportWav();
        else if (strcmp(arg, "REPORT") == 0) {
            EXT_RAM_BSS_ATTR static char row[200];
            for (const auto& ln : se->editReport()) {
                snprintf(row, sizeof(row), "REPORT: %s\r\n", ln.c_str());
                cdc_reply(row);
            }
        }
        else if (sscanf(arg, "SYNC %d", &a) == 1) {
            {
                std::lock_guard<std::recursive_mutex> l(se->songMutex());
                se->song().syncSeq = a != 0;
            }
            se->commit();
        }
        else if (sscanf(arg, "LEN %d", &a) == 1) {          // 0 = to the last block
            {
                std::lock_guard<std::recursive_mutex> l(se->songMutex());
                se->song().length = a > 0 ? (uint32_t)a : 0;
            }
            se->commit();
        }
        else if (*arg) { cdc_reply("ERR ASONG OPEN name|BOUNCE scene track|PLAY [bar]|STOP|CANCEL|LEN bars|SYNC 0|1|TEMPO bpm|RENAME name|REGION a b|MERGE mask from to|IMPORT path a b track bar lead|EXPORT|EXPORTWAV|REPORT|REC bar [beats]|DUB bar [beats]|PUNCH R|D|N|BOUNCESEQ track|VOL track pct|MUTE track 0|1|CUT track bar|DUPREGION|ARM track|SEQTRACK [kitjson]|DELTRACK t|DUP t b|DELBLOCK t b|MOVE t b bar|PASTE a b at|TRACKKIT t kitjson\r\n"); return true; }

        asong_reply_status(se, ok);
        return true;
    }
    return false;
}
