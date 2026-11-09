# Chord mode for Tracker CE

An Omnichord-style chord player with an MPC "16 Levels"-style velocity row.
Status: builds for the original Tracker as `PolyendTrackerCHRD16.ptf` (version 2: voice tracking,
Release / Gain, track and step display). Version 1 was tried on hardware; version 2 is not yet tested.

## Using it

**Shift + I.Param** opens Chord mode from any screen, and **Shift + I.Param** again returns
to where you were. F8 (Close) also goes back. Any other mode key leaves as usual.

| Control | Action |
|---|---|
| Pad rows 1-3 | Chords. Columns = roots in Omnichord order: Db Ab Eb Bb F C G D A E B F#. Row 1 major, row 2 minor, row 3 seventh |
| Shift + pad, rows 1-3 | sus4 / m7 / maj7 |
| Pad row 4, Strum | 12-note strum plate over the selected chord (root upwards through the octaves). Drag a finger across it |
| Pad row 4, Levels | 12 velocity levels: a pad sets the level and plays the chord at it |
| Jog | Depends on the jog mode: **Strum** (up / down through the chord's notes at the current velocity), **Str+Vel** (strum, turning speed sets the velocity, smoothed), **Velocity** (sets the velocity 1-127, two per click, no notes), **FX** (moves the effect of the slot the jog drives: FX1 and FX2 each keep their own effect and value; what you hear glides smoothly to the jog's value, and while recording the value is written into that slot of the recording lane's current step; a slot holding a recorded arpeggio's Chord / Arp effect is left alone) |
| Shift + jog, Shift + Left / Right | Change the jog mode |
| Instr (hold) | The instrument popup, as in the other screens: pick with the pads or jog; Chord mode plays the chosen instrument |
| FX1 / FX2 (hold) | The effect popup: the effect picked (any in the list) goes into that slot; the jog drives that slot and switches to FX mode |
| F2 | Shows the jog mode; tap: next mode |
| F3 | FX mode: tap = drive FX1 or FX2 (each keeps its own effect), hold + jog = that slot's effect (Low-pass, High-pass, Band-pass, Volume, Pan, Reverb, Delay, Overdrive, Bit depth) |
| Up / Down | Level 1-12 (sets the velocity in twelve steps) |
| Left / Right | Octave 1-6 |
| F1 | Row 4: Strum / Levels |
| F4 | Latch: keep the chord selected after its pad is released (default on). With Latch off, releasing the chord pad also ends its strum notes |
| F5 | Chord pads play as: **Block** (the chord, while held), **Strum** (a quick strum through the chord and the root an octave up, using the strum voices), **Arp** (arpeggiated in time with the tempo, one voice; with Latch on it keeps going after release and the same pad again stops it), **Select** (only selects the chord) |
| Shift + F2 | Strum voices: 1, 2 or 3 strum notes ringing at once |
| Shift + F3 | Chord voices: 2, 3 or 4 notes in a block chord (the fifth is dropped first, then the top note) |
| Shift + F6 | Arp direction: Up, Down, Random - the sequencer's own Arp effect, over the chord's notes within one octave |
| Shift + F7 | Arp rate in pattern steps, as the Arp effect: 8, 6, 4, 3, 2, 1 steps per note, or 2, 3, 4, 6, 8 notes per step |
| Shift + F1 | Ring: how long each strum note (row 4 or jog) sounds before it is released: 0.3 s, 0.8 s (default), 2 s, or Hold (until a newer strum note pushes it out) |
| Shift + F4 | Release: Fade (default, short fade), Cut (stop at once), Instr (the instrument's own envelope) |
| Shift + F5 | Gain: 100 / 75 (default) / 50 % on every Chord mode note, for headroom with long samples |
| F6 | Shows the selected track. Hold F6 + jog: change track |
| F7 | Shows the playhead step / pattern length |
| F8 | Close |
| Shift + F8 | Strum wrap on / off: the jog strum carries on from the bottom note after the top one (and back the other way) instead of stopping (default on) |
| Play, Shift + Play | Play pattern / song, or stop |
| Rec + Play | Live recording (as in the pattern editor). Rec alone stops recording |

Chord mode notes always play and record at Chord mode's velocity (velocity x gain, then the instrument's
own velocity setting, as for live notes), whatever
**Config > Recording options** says. Changing the level while recording a running arpeggio writes a new
step with the new volume.

## How it works

Notes go through `Sequencer::handleNoteOn/Off` (channel `GRID_OUTSIDE_PATTERN`), the same path as
the normal note pads. So they play on the selected instrument (samples, MIDI, or synth on Plus/Mini),
spread across free tracks as voices, and live recording writes them to armed tracks like any
other pads - a chord lands on consecutive tracks.

Voice budget: the chord uses 3-4 voices and the strum keeps up to 3 notes ringing, leaving one
spare track. For sample instruments a new chord or strum note starts on a free track before the old one
is released, so ringing notes fade rather than being cut. While a pattern plays, tracks with open notes
are busy, so fewer voices are free.

Each sample note's track is remembered, and on release Chord mode ends that voice itself (Fade / Cut)
as well as sending the sequencer note-off, so notes cannot hang if the sequencer's own note-off misses
them (it only matches while it still marks the track as held). Caveat: if the pattern plays a step on that
track in the meantime, releasing the chord note also fades the pattern's note on that track.

Sequencer changes (only active during Chord mode's own note-ons): `Sequencer::liveVelocityAlways` keeps the
played velocity when recording velocity is off, and `Sequencer::liveRecSampleTracksOnly` stops live recording
of a sample instrument from spilling onto MIDI tracks.

Chord shapes come from the existing table in `core/source/mtChordDefs.h`
(entries 5, 4, 23 and 7, 20, 24).

## Recording

While recording, Chord mode records onto **armed tracks only**, starting from the **selected track** (F6)
(or the next armed track after it), staying on sample tracks for a sample instrument:
- **Arp**: one step on the selected track - the root note with the **Chord** and **Arp** effects - so the
  sequencer's own arpeggiator plays it back, at any rate, on one lane. Changing chord, direction (Shift + F6) or
  rate (Shift + F7) while it runs writes a new step there, and the live arpeggio restarts with it; stopping it writes a note-off. The live arpeggio copies the Arp effect exactly, and once the
  pattern comes back round to the recorded step Chord mode goes quiet and the sequencer takes over
  (no doubling). A press in the second half of a step starts on the next step.
- **Strum on press**: with an armed lane for every strum note, each note on its own lane (rings, polyphonic;
  turn on microtiming in Config > Recording options to keep the strum's timing). With fewer armed tracks
  (e.g. one), the strum goes onto one lane **one note per step**, each with Chord mode's velocity and a
  micromove putting it as close to where it was played as the step allows (micromove only delays a note
  within its step), then a note-off after the Ring time. Monophonic, and slower than the live strum.
- **Strum on row 4 / jog**: single notes, onto the armed lanes.
- The Roll effect repeats one note (stepping by semitones or at random), so it cannot play a chord strum.
- **Block chords**: one lane per chord note (Chord voices, Shift + F3).

The armed lanes need free steps where you record; with nothing armed, nothing is recorded. Sequencer hook:
`Sequencer::liveRecFirstTrack` / `liveRecTrackCount`, and `Sequencer::livePreviewOnly` (the live arpeggio
is heard but not recorded), all set only around Chord mode's own notes.

## Files

New: `core/interface/chordMode/` (`chordMode.h`, `fChordMode.cpp` logic, `gChordMode.cpp` screen).
Changed:
- `core/interface/core/interfaceDefs.h`: new event `eventActivateChordMode`.
- `core/interface/core/interface.cpp`: Shift + I.Param on `eventSwitchModule` opens Chord mode;
  handles `eventActivateChordMode`.
- `core/interface/instrumentEditor/fIinstrEdit.cpp`: the instrument editor handles I.Param itself
  (Params / Envelope), so Shift + I.Param there sends `eventActivateChordMode`.
- `core/source/mtSequencer.h` / `.cpp`: the two flags above.
- `core/include/Platform.h`: `Encoder::setReadInterval()` (default: ignored).
  `platforms/mk66/source/EncoderImpl.*`: implements it (the encoder is normally read every 100 ms).
  Chord mode reads it every 10 ms without acceleration while open, and restores 100 ms and the
  platform's acceleration (2 on the original Tracker, 3 on Plus / Mini) when it closes.
  On Plus / Mini only the acceleration changes.

`chord-mode.patch` holds all of it against unmodified tracker-ce (`patch -p1 < chord-mode.patch`
from the repository root).

## Building

The core makefile picks up new `.cpp` files under `core/interface/` by itself. With the GNU Arm
toolchain 10.3-2021.10 on the PATH or in `./agt/10.3/`: `make original` (or `plus`, `mini`).

## To check on hardware

- Version 2 fixes: no hanging chords with Latch off, fewer glitches on long samples, no recording onto MIDI tracks.
- Jog feel: the velocity curve is `mapClamped(rate, 3, 40, 30, 127)` clicks per second in `jogStrum`,
  smoothed 2:1 per click; a pause over 300 ms restarts at the level velocity.
- Pad light levels and flicker when chords change.
- Recording a chord while a pattern plays: which tracks it lands on, with and without
  a single armed track.
- Master opened from Sample Rec passes its source key instead of the pressed key, so
  Shift + I.Param from there opens the instrument editor, not Chord mode.
