// Chord mode: an Omnichord-style chord player with an MPC "16 Levels"-style
// velocity row, for the Polyend Tracker pad grid.
//
//   Pad rows 1-3 : chord buttons. 12 roots in Omnichord order (Db Ab Eb Bb F C G D A E B F#),
//                  row 1 major, row 2 minor, row 3 seventh; Shift + pad: sus4 / m7 / maj7.
//   Pad row 4    : Strum (a 12-note strum plate over the held chord) or
//                  Levels (12 velocity levels; a pad sets the level and plays the chord).
//   Chord pads   : play as Block, Strum, Arp (the sequencer's own Arp effect) or Select.
//   Jog          : Strum, Strum + velocity, Velocity, or FX (an effect swept live and recorded
//                  into FX1 / FX2); Shift + jog or Shift + Left / Right picks the mode.
//                  Hold F6 + jog: change the track.
//
// Entered and left with Shift + I.Param. Notes go through Sequencer::handleNoteOn/Off,
// so they play on the selected instrument and record like notes from the normal pads.
// Chord mode remembers which track (voice) each sample note landed on and releases that
// voice itself, so a note cannot hang when the sequencer's own bookkeeping misses it.

#ifndef INTERFACE_CHORDMODE_H_
#define INTERFACE_CHORDMODE_H_

#include "core/modulesBase.h"
#include "mtSequencer.h"

#include <stdint.h>

class cChordMode : public ModuleBase
{
  public:
    virtual void update();
    virtual void start(uint32_t options);
    virtual void stop();

    virtual void initDisplayControls();
    virtual void destroyDisplayControls() {}

    cChordMode() {}
    ~cChordMode() {}

    //---------------------------------- constants
    static constexpr uint8_t kColumns = 12;
    static constexpr uint8_t kChordRows = 3;
    static constexpr uint8_t kStrumRow = 3;
    static constexpr uint8_t kLevels = 12;
    static constexpr uint8_t kMaxChordNotes = 8;
    static constexpr uint8_t kStrumVoices = 3; // chord (up to 4) + strum (3) leaves one spare track
    static constexpr int8_t kMinOctave = 1;
    static constexpr int8_t kMaxOctave = 6;

    // While Chord mode is open the encoder is read every 10 ms without acceleration, so each
    // click arrives on its own and is timed accurately; the defaults come back on exit.
    static constexpr uint32_t kJogReadIntervalMs = 10;
    static constexpr uint32_t kDefaultReadIntervalMs = 100;
#ifdef PLATFORM_IMX
    static constexpr int kDefaultAcceleration = 3;
#else
    static constexpr int kDefaultAcceleration = 2;
#endif

    // note "sources" passed to the sequencer; pads use 0..47 and MIDI uses note + 100.
    // The chord block alternates between two banks and strum notes rotate through 20 ids,
    // so a new note never shares a source with a note that is still being released.
    static constexpr int16_t kSrcChordBankA = 400;
    static constexpr int16_t kSrcChordBankB = 410;
    static constexpr int16_t kSrcStrumBase = 420;
    static constexpr uint8_t kSrcStrumCount = 20;
    static constexpr int16_t kSrcArp = 440;

    enum RowMode : uint8_t
    {
        kRowStrum = 0,
        kRowLevels = 1,
    };

    // what a chord pad does when pressed
    enum ChordPlayMode : uint8_t
    {
        kPlayBlock = 0,  // sound the chord, held while the pad is held
        kPlayStrum = 1,  // a quick strum through the chord (uses the strum voices)
        kPlayArp = 2,    // arpeggiate the chord in time with the tempo (one voice)
        kPlaySelect = 3, // only select the chord, for strumming with row 4 or the jog
        kPlayModes
    };

    // The arpeggio is the sequencer's own Arp effect (so a recording plays back exactly as heard):
    // directions as Sequencer::arpOption_*, rates as Sequencer::arpPeriod_* (in pattern steps).
    enum ArpPattern : uint8_t
    {
        kArpUp = 0,
        kArpDown,
        kArpRandom,
        kArpPatterns
    };
    static constexpr uint8_t kArpRates = 11;     // 8 6 4 3 2 1 steps, then 1/2 1/3 1/4 1/6 1/8 step
    static constexpr uint8_t kArpRateDefault = 5; // one note per step

    static constexpr uint32_t kRollSpacingMs = 30; // gap between notes of a strum on press

    // what the jog does (Shift + jog or Shift + Left / Right to change)
    enum JogMode : uint8_t
    {
        kJogStrum = 0,    // strum the chord at the current velocity
        kJogStrumVel = 1, // strum, turning speed sets the velocity
        kJogVelocity = 2, // set the velocity (1..127), no notes
        kJogFx = 3,       // move an effect value: heard live, recorded into an FX slot
        kJogModes
    };
    static constexpr uint8_t kJogFxTypes = 9; // effects F3 + jog steps through (any via FX1 / FX2)

    enum RingMode : uint8_t
    {
        kRingShort = 0,  // 0.3 s
        kRingMedium = 1, // 0.8 s (default)
        kRingLong = 2,   // 2 s
        kRingHold = 3,   // until pushed out by a newer strum note
        kRingModes
    };

    enum ReleaseMode : uint8_t
    {
        kReleaseFade = 0,  // short fade on release (default)
        kReleaseCut = 1,   // stop at once
        kReleaseInstr = 2, // the instrument's own envelope / play mode
        kReleaseModes
    };

    struct strVoice
    {
        uint8_t note = 0;
        int16_t source = -1;
        int8_t track = -1; // sample track the note landed on, -1 = unknown (MIDI) or none
        bool on = false;
        uint32_t offAt = 0; // strum notes: release time in ms (0 = ring until replaced)
        int8_t recFirst = -1; // tracks this note records onto (see Sequencer::liveRecFirstTrack)
        uint8_t recCount = 0;
        bool previewOnly = false; // played live, never recorded
    };

    //---------------------------------- input handling
    void chordPadPress(uint8_t pad, bool alt);
    void chordPadRelease(uint8_t pad);
    void strumPadPress(uint8_t column);
    void levelPadPress(uint8_t pad);
    void levelPadRelease(uint8_t pad);
    void jogStrum(int16_t value);
    void jog(int16_t value);
    void cycleJogMode(int8_t delta);
    void jogVelocity(int16_t value);
    void jogFx(int16_t value);
    void changeFxType(int8_t delta);
    void toggleFxSlot();
    uint8_t fxType() const { return fxTypeId[fxSlot]; }
    uint8_t fxType(uint8_t slot) const { return fxTypeId[slot]; }
    void applyFxLive(uint8_t slot, int8_t track);
    void applyFxToVoices(uint8_t slot);
    void clearFxLive(uint8_t slot);
    void clearFxLive();
    void glideFx();
    void recordFxValue();
    void setLevel(uint8_t newLevel);
    void setVelocity(uint8_t newVelocity);
    void dropChord();
    void refreshScreen();
    bool lateInStep() const;
    Sequencer::strPattern::strTrack::strStep* stepAt(int8_t track, int16_t stepIndex) const;
    void scheduleArpSubNotes(uint32_t now);
    void toggleStrumWrap();
    void fxPicked(uint8_t slot, uint8_t type);
    const char* fxShortName() const { return fxShortName(fxSlot); }
    const char* fxShortName(uint8_t slot) const;

    void toggleRowMode();
    void changeOctave(int8_t delta);
    void changeLevel(int8_t delta);
    void toggleLatch();
    void cycleChordPlay();
    void cycleStrumVoices();
    void cycleChordVoices();
    void cycleArpPattern();
    void cycleArpRate();
    void cycleReleaseMode();
    void cycleGain();
    void cycleRing();
    uint32_t ringMs() const;
    uint32_t nowMs() const;
    void changeTrack(int8_t delta);
    void setShiftLabels(bool shift);

    //---------------------------------- notes
    bool sampleInstrument() const;
    strVoice startVoice(uint8_t note,
                        uint8_t velocity,
                        int16_t source,
                        int8_t recFirst,
                        uint8_t recCount,
                        bool previewOnly = false);
    void recLanes(uint8_t wanted, int8_t& first, uint8_t& count) const;
    uint8_t armedLanes() const;
    void writeOffStep(int8_t track, int16_t stepIndex);
    bool writeNoteStep(int8_t track, int16_t stepIndex, uint8_t note, int8_t volume, int8_t micromove);
    int8_t recordVolume() const;
    int16_t recordPosition() const;
    void endVoice(strVoice& voice);
    void handover(strVoice* old,
                  uint8_t oldCount,
                  const uint8_t* notes,
                  const int16_t* sources,
                  uint8_t count,
                  uint8_t velocity,
                  strVoice* out,
                  uint8_t lanes,
                  bool previewOnly = false);
    void playChordBlock(uint8_t velocity);
    void releaseChordBlock();
    void playStrumNote(uint8_t note, uint8_t velocity, bool previewOnly = false);
    void releaseStrumNotes();
    void releaseAll();
    void startArp(uint8_t pad);
    void stopArp();
    void arpStep();
    bool arpActive() const { return arpRunning || arpHandedOver; }
    uint32_t freeStepMs() const;
    void recordArpStep(int16_t step);
    void recordArpOff();
    void startRoll();
    void stopRoll();
    uint8_t voicedChordNotes(uint8_t* out) const;
    void clearChord();

    uint8_t buildChordNotes(uint8_t* out, uint8_t maxCount) const;
    int16_t strumNote(uint8_t index) const;
    uint8_t scaled(uint8_t velocity) const;
    bool hasChord() const { return currentRootPc >= 0; }
    const char* rootName() const;
    static const char* columnName(uint8_t column);
    void showRootRow();

    //---------------------------------- screen and pads
    void setDefaultScreenFunct();
    void showDefaultScreen();
    void showInfo();
    void showFunctionLabels();
    void showTrackLabel();
    void showStepLabel();
    void lightUpPadBoard();

    //---------------------------------- state
    uint8_t rowMode = kRowStrum;
    int8_t octave = 3;      // chord roots sit in C(octave) .. B(octave)
    uint8_t level = 9;          // 1..12 (Up / Down, the Levels row): sets noteVelocity in twelve steps
    uint8_t noteVelocity = 106; // 1..127: what Chord mode plays at (before Gain)
    uint8_t jogMode = kJogStrumVel;
    bool strumWrap = true; // jog strum: past the top note it carries on from the bottom (and back)
    // jog FX: each slot (FX1, FX2) has its own effect and value; the jog drives fxSlot
    uint8_t fxSlot = 0; // 0 = FX1, 1 = FX2
    uint8_t fxTypeId[2] = { Sequencer::strFxConsts::FX_TYPE_FILTER_LOWPASS,
                            Sequencer::strFxConsts::FX_TYPE_REVERB_SEND };
    int16_t fxValue[2] = { -1, -1 };        // the jog's value (target), -1 = not set yet
    int16_t fxLive[2] = { -1, -1 };         // the value heard now, gliding towards fxValue
    uint16_t fxLiveTracks[2] = { 0, 0 };    // sample tracks each slot's value is applied to
    uint32_t fxGlideMs = 0;
    bool fxUndoStored = false;
    bool f3Held = false;
    bool f3Turned = false;
    bool latch = true;      // keep the chord selected after its pad is released
    uint8_t chordPlay = kPlayBlock;
    uint8_t strumVoiceLimit = kStrumVoices; // 1..3 strum notes ringing at once
    uint8_t chordVoiceLimit = 4;            // 2..4 notes in a block chord (the fifth goes first)
    uint8_t arpPattern = kArpUp;
    uint8_t arpRate = kArpRateDefault;
    uint8_t releaseMode = kReleaseFade;
    uint8_t ringMode = kRingMedium;
    uint8_t gain = 75; // % applied to every Chord mode note: 100 / 75 / 50

    int8_t currentRootPc = -1;   // pitch class of the selected root, -1 = none
    uint8_t currentChordIdx = 0; // index into chordDef[]
    uint8_t currentChordPad = 255;
    bool currentChordPadHeld = false;
    const char* currentChordName = "";

    strVoice chordVoices[kMaxChordNotes];
    uint8_t chordVoiceCount = 0;
    bool chordBankB = false;
    uint8_t chordBlockOwnerPad = 255; // pad whose release stops the sounding chord block

    strVoice strumVoices[kStrumVoices];
    uint8_t strumNext = 0;
    uint8_t strumSourceNext = 0;

    // arpeggiator
    bool arpRunning = false;
    uint8_t arpPad = 255;
    int16_t arpIndex = -1;
    uint32_t arpNextMs = 0;
    int16_t arpLastPos = -1;   // sequencer step the arpeggio last followed
    int16_t arpStepCount = 0;  // pattern steps since the arpeggio started
    uint8_t arpSubLeft = 0;    // rates faster than a step: notes still to come in this step
    uint32_t arpSubMs = 0;     // when the next of those is due
    // recording: the arpeggio is written as one step (root + Chord + Arp effects)
    bool arpRecording = false;
    bool arpHandedOver = false; // the sequencer is playing the recorded step; Chord mode is quiet
    bool arpLeftRecStep = false;
    int8_t arpRecTrack = -1;
    int16_t arpRecStep = -1;
    int16_t seenPos = -1;      // sequencer step last seen by update()
    uint32_t seenPosMs = 0;    // when that step began
    uint32_t stepMs = 125;     // measured length of a sequencer step
    strVoice arpVoice;
    uint32_t arpRandom = 12345;
    bool arpSourceB = false; // alternate sources so the overlapping old/new notes never share one

    // strum on press
    uint8_t rollNotes[kMaxChordNotes + 1];
    uint8_t rollCount = 0;
    uint8_t rollPos = 0;
    uint32_t rollNextMs = 0;
    bool rollPreviewOnly = false; // the roll was written to one lane already, so its live notes are not recorded

    int8_t jogPos = -1;
    bool jogStarted = false;
    uint8_t lastJogVelocity = 0;
    int16_t jogSmoothVelocity = 0; // velocity smoothed over successive jog steps

    bool f6Held = false;
    bool shiftLabels = false;

    uint8_t shownSeqState = 255;
    int16_t shownStep = -1;
    int16_t shownLength = -1;
    int16_t shownTrack = -1;

    //---------------------------------- display
    hControl label[8];
    hControl bgLabel;
    hControl chordLabel = -1;
    hControl detailLabel = -1;
    hControl notesLabel = -1;
    hControl hintLabel = -1;
    hControl jogLabel = -1;
    hControl rootLabel[kColumns];

    char chordText[48];
    char detailText[128];
    char notesText[96];
    char hintText[96];
    char jogText[80];
    char rootText[kColumns][8];
    char fnText[8][16];
};

extern cChordMode chordMode;

#endif /* INTERFACE_CHORDMODE_H_ */
