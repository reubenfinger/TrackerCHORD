#include "chordMode.h"

#include <stdio.h>
#include <string.h>

#include "core/interface.h"
#include "core/interfaceMenuPopup.h"
#include "core/interfacePopups.h"
#include "elapsedMillis.h"
#include "fileManager.h"
#include "mtAudioEngine.h"
#include "mtChordDefs.h"
#include "mtConfig.h"
#include "mtPadBoard.h"
#include "mtPadsBacklight.h"
#include "mtSequencer.h"
#include "Platform.h"
#include "mtStructs.h"
#include "patternEditor/patternEditor.h"
#include "performanceMode/performanceMode.h"
#include "util_math.h"

cChordMode chordMode;
static cChordMode* CM = &chordMode;
static elapsedMillis jogTimer;
static elapsedMillis clockMs; // running ms clock for strum ring times

static constexpr uint8_t kNoVoice = 255;

//==============================================================================================
// Chord tables
//==============================================================================================

// Omnichord column order of roots: Db Ab Eb Bb F C G D A E B F# (pitch class = 1 + 7 * column)
static inline uint8_t columnToPitchClass(uint8_t column)
{
    return static_cast<uint8_t>((1 + 7 * column) % 12);
}

struct strChordType
{
    uint8_t chordDefIdx; // index into chordDef[] (mtChordDefs.h)
    const char* name;
};

// [set][row]: set 0 = plain pads, set 1 = Shift + pad
static const strChordType chordTypes[2][cChordMode::kChordRows] = {
    { { 5, "maj" }, { 4, "min" }, { 23, "7" } },     // 0 4 7 / 0 3 7 / 0 4 7 10
    { { 7, "sus4" }, { 20, "m7" }, { 24, "maj7" } }, // 0 5 7 / 0 3 7 10 / 0 4 7 11
};

static const char* const pitchClassNames[12] = { "C",  "C#", "D",  "Eb", "E",  "F",
                                                  "F#", "G",  "Ab", "A",  "Bb", "B" };

//==============================================================================================
// Input callbacks
//==============================================================================================

static void functPads(uint8_t pad, uint8_t state, int16_t velo);
static void functEncoder(int16_t value);
static void functActionButton(uint8_t button, uint8_t state);
static void functShift(uint8_t state);
static void functPlayAction();
static void functRecAction();
static void functLeft();
static void functRight();
static void functUp();
static void functDown();
static void functParams();
static void functInstrKey(uint8_t state);
static void functFx1Key(uint8_t state);
static void functFx2Key(uint8_t state);
static void functSwitchModule(uint8_t button);

//==============================================================================================
// Module
//==============================================================================================

uint32_t cChordMode::nowMs() const
{
    return static_cast<uint32_t>(clockMs);
}

uint32_t cChordMode::ringMs() const
{
    static const uint32_t ringTimes[kRingModes] = { 300, 800, 2000, 0 };
    return ringTimes[ringMode];
}

void cChordMode::update()
{
    uint32_t now = nowMs();

    if (jogMode == kJogFx)
    {
        glideFx();
    }

    // measure the sequencer's step length while it plays
    int16_t posNow = sequencer.getActualPos();
    if (posNow != seenPos)
    {
        uint32_t length = now - seenPosMs;
        if (sequencer.isPlay() && seenPos >= 0 && length > 10 && length < 2000)
        {
            stepMs = length;
        }
        seenPos = posNow;
        seenPosMs = now;
    }

    // strum on press: the next note of the roll
    if (rollPos < rollCount && static_cast<int32_t>(now - rollNextMs) >= 0)
    {
        playStrumNote(rollNotes[rollPos++], levelVelocity(), rollPreviewOnly);
        rollNextMs = now + kRollSpacingMs;
    }

    // arpeggiator, timed like the sequencer's Arp effect: rates of a step or longer play on
    // every Nth step from the step it started on; faster rates play N notes spread over each
    // step. While the pattern plays it follows the sequencer's steps; when stopped, the tempo.
    if (arpRunning)
    {
        static const uint8_t periodSteps[kArpRates] = { 8, 6, 4, 3, 2, 1, 0, 0, 0, 0, 0 };
        static const uint8_t notesPerStep[kArpRates] = { 0, 0, 0, 0, 0, 0, 2, 3, 4, 6, 8 };

        bool boundary = false;
        uint32_t stepLength = sequencer.isPlay() ? stepMs : freeStepMs();

        if (sequencer.isPlay())
        {
            int16_t pos = sequencer.getActualPos();
            if (pos != arpLastPos)
            {
                arpLastPos = pos;
                boundary = true;
            }
        }
        else if (static_cast<int32_t>(now - arpNextMs) >= 0)
        {
            arpNextMs += stepLength;
            if (static_cast<int32_t>(now - arpNextMs) >= 0)
            {
                arpNextMs = now + stepLength; // fell behind: carry on from now
            }
            boundary = true;
        }

        if (boundary)
        {
            arpStepCount++;

            // recording: once the pattern comes back round to the recorded step, the sequencer
            // plays the arpeggio itself and Chord mode goes quiet
            if (arpRecording && sequencer.isPlay())
            {
                if (arpLastPos != arpRecStep)
                {
                    arpLeftRecStep = true;
                }
                else if (arpLeftRecStep)
                {
                    arpRunning = false;
                    arpHandedOver = true;
                    endVoice(arpVoice);
                }
            }

            if (arpRunning && arpStepCount >= 0)
            {
                uint8_t n = periodSteps[arpRate];
                if (n > 0)
                {
                    if (arpStepCount % n == 0)
                    {
                        arpStep();
                    }
                }
                else
                {
                    arpStep();
                    arpSubLeft = static_cast<uint8_t>(notesPerStep[arpRate] - 1);
                    arpSubMs = now + stepLength / notesPerStep[arpRate];
                }
            }
        }
        else if (arpRunning && arpSubLeft > 0 && static_cast<int32_t>(now - arpSubMs) >= 0)
        {
            arpSubLeft--;
            arpSubMs += stepLength / notesPerStep[arpRate];
            arpStep();
        }
    }

    // strum notes end after their ring time
    for (uint8_t i = 0; i < kStrumVoices; i++)
    {
        strVoice& voice = strumVoices[i];
        if (voice.on && voice.offAt != 0 && static_cast<int32_t>(now - voice.offAt) >= 0)
        {
            endVoice(voice);
        }
    }

    uint8_t state = sequencer.getSeqState() + (sequencer.isRec() ? 10 : 0);
    if (!sequencer.isRec())
    {
        fxUndoStored = false; // the next recording stores a fresh undo point
    }
    if (state != shownSeqState)
    {
        shownSeqState = state;
        showInfo();
    }

    int16_t step = sequencer.getActualPos();
    int16_t length = sequencer.getPatternLength();
    if (step != shownStep || length != shownLength)
    {
        showStepLabel();
    }

    if (getActualTrack() != shownTrack)
    {
        showTrackLabel();
    }
}

void cChordMode::start([[maybe_unused]] uint32_t options)
{
    moduleRefresh = 1;

    jogPos = -1;
    jogStarted = false;
    f6Held = false;
    shiftLabels = false;
    shownSeqState = 255;
    shownStep = -1;
    shownLength = -1;
    shownTrack = -1;

    ip->setButtonObj(interfaceButtonPerformance, buttonPress, functSwitchModule);
    ip->setButtonObj(interfaceButtonFile, buttonPress, functSwitchModule);
    ip->setButtonObj(interfaceButtonConfig, buttonPress, functSwitchModule);
    ip->setButtonObj(interfaceButtonMaster, buttonPress, functSwitchModule);
    ip->setButtonObj(interfaceButtonSamplePlay, buttonPress, functSwitchModule);
    ip->setButtonObj(interfaceButtonSampleEdit, buttonPress, functSwitchModule);
    ip->setButtonObj(interfaceButtonSampleRec, buttonPress, functSwitchModule);
    ip->setButtonObj(interfaceButtonSampleLoad, buttonPress, functSwitchModule);
    ip->setButtonObj(interfaceButtonSong, buttonPress, functSwitchModule);
    ip->setButtonObj(interfaceButtonPattern, buttonPress, functSwitchModule);
    ip->setButtonObj(interfaceButtonGame, buttonPress, functSwitchModule);

    // I.Param: Shift + I.Param leaves chord mode, I.Param alone opens the instrument editor
    ip->setButtonObj(interfaceButtonParams, buttonPress, functParams);

    if (Encoder* encoder = platform->getEncoder())
    {
        encoder->setAcceleration(0);
        encoder->setReadInterval(kJogReadIntervalMs);
    }

    showDefaultScreen();
    setDefaultScreenFunct();
    lightUpPadBoard();
}

void cChordMode::stop()
{
    moduleRefresh = 0;
    clearFxLive();
    stopArp();
    stopRoll();
    releaseAll();

    if (Encoder* encoder = platform->getEncoder())
    {
        encoder->setAcceleration(kDefaultAcceleration);
        encoder->setReadInterval(kDefaultReadIntervalMs);
    }
    padsBacklight.clearGlobalPads(1, 1, 1);
}

void cChordMode::setDefaultScreenFunct()
{
    ip->clearButtonsRange(interfaceButton0, interfaceButton7);
    ip->clearAllPots();

    ip->setButtonObj(interfaceButtonPlay, buttonPress, functPlayAction);
    ip->setButtonObj(interfaceButtonRec, buttonPress, functRecAction);
    ip->setButtonObj(interfaceButtonShift, functShift);

    ip->setButtonObj(interfaceButtonLeft, buttonPress, functLeft);
    ip->setButtonObj(interfaceButtonRight, buttonPress, functRight);
    ip->setButtonObj(interfaceButtonUp, buttonPress, functUp);
    ip->setButtonObj(interfaceButtonDown, buttonPress, functDown);

    for (uint8_t i = interfaceButton0; i <= interfaceButton7; i++)
    {
        ip->setButtonObj(i, functActionButton);
    }

    // Instr: the instrument popup, as in the other screens (hold, pick with pads or jog)
    // FX1 / FX2: the effect popup; the effect picked becomes the jog's effect in that slot
    ip->setButtonObj(interfaceButtonInstr, functInstrKey);
    ip->setButtonObj(interfaceButtonFx1, functFx1Key);
    ip->setButtonObj(interfaceButtonFx2, functFx2Key);

    // no Shift acceleration: every encoder step is one strum note
    ip->setPotObj(interfacePot0, functEncoder, false);

    ip->setPadsGlobal(functPads);
}

//==============================================================================================
// Chord building
//==============================================================================================

uint8_t cChordMode::buildChordNotes(uint8_t* out, uint8_t maxCount) const
{
    if (!hasChord())
    {
        return 0;
    }

    uint16_t bitmask = chordDef[currentChordIdx];
    int root = 12 * octave + currentRootPc;
    uint8_t count = 0;

    for (uint8_t interval = 0; interval < 16 && count < maxCount; interval++)
    {
        if (bitmask & (1u << interval))
        {
            int note = root + interval;
            if (note >= 0 && note <= 127)
            {
                out[count++] = static_cast<uint8_t>(note);
            }
        }
    }
    return count;
}

// Note for strum position 0..11: the chord's notes going up through the octaves from the root.
int16_t cChordMode::strumNote(uint8_t index) const
{
    uint8_t tones[kMaxChordNotes];
    uint8_t count = buildChordNotes(tones, kMaxChordNotes);
    if (count == 0)
    {
        return -1;
    }

    int note = tones[index % count] + 12 * (index / count);
    return note <= 127 ? note : -1;
}

const char* cChordMode::columnName(uint8_t column)
{
    return pitchClassNames[columnToPitchClass(column)];
}

const char* cChordMode::rootName() const
{
    return hasChord() ? pitchClassNames[static_cast<uint8_t>(currentRootPc)] : "";
}

uint8_t cChordMode::levelVelocity() const
{
    return velocity;
}

// A level 1..12 (Up / Down, the Levels row) sets the velocity to that step of 127.
void cChordMode::setLevel(uint8_t newLevel)
{
    level = newLevel;
    velocity = static_cast<uint8_t>((127 * level + kLevels - 1) / kLevels);
}

uint8_t cChordMode::scaled(uint8_t velocity) const
{
    int v = (velocity * gain + 50) / 100;
    return static_cast<uint8_t>(v < 1 ? 1 : (v > 127 ? 127 : v));
}

//==============================================================================================
// Voices
//
// Every note goes through the sequencer (so it plays the selected instrument and records),
// and for sample instruments Chord mode notes which track the note landed on, so it can end
// that voice itself (fade / cut) even if the sequencer's own note-off no longer matches it.
//==============================================================================================

bool cChordMode::sampleInstrument() const
{
    return isSampleInstrument(mtProject.values.lastUsedInstrument);
}

// While recording, Chord mode records onto armed tracks only, starting from the selected track
// (F6) and staying on sample tracks for a sample instrument. One lane: the first armed track from
// the selected one on. More: that track and any armed tracks after it.
void cChordMode::recLanes(uint8_t wanted, int8_t& first, uint8_t& count) const
{
    int limit = sampleInstrument() ? kSampleTracks : kAllTracks;
    int selected = getActualTrack();
    if (selected >= limit)
    {
        selected = limit - 1;
    }
    if (selected < 0)
    {
        selected = 0;
    }

    first = -1;
    for (int i = 0; i < limit && first < 0; i++)
    {
        int track = (selected + i) % limit; // from the selected track, wrapping round
        if (sequencer.isRecArmedOnTrack(static_cast<uint8_t>(track)))
        {
            first = static_cast<int8_t>(track);
        }
    }
    if (first < 0)
    {
        count = 0; // nothing armed: leave it to the sequencer (it records nothing)
        return;
    }
    count = static_cast<uint8_t>(wanted <= 1 ? 1 : limit - first);
}

// Armed tracks available for separate lanes, from the first armed one on.
uint8_t cChordMode::armedLanes() const
{
    int8_t first;
    uint8_t count;
    recLanes(kMaxChordNotes + 1, first, count);
    uint8_t lanes = 0;
    for (int i = 0; first >= 0 && i < count; i++)
    {
        if (sequencer.isRecArmedOnTrack(static_cast<uint8_t>(first + i)))
        {
            lanes++;
        }
    }
    return lanes;
}

// The step a recording made now lands on: this one, or the next if we are past its middle.
int16_t cChordMode::recordPosition() const
{
    int16_t pos = sequencer.getActualPos();
    if (sequencer.isPlay() && (nowMs() - seenPosMs) * 2 > stepMs)
    {
        pos = static_cast<int16_t>(pos + 1);
    }
    int16_t length = sequencer.getPatternLength();
    return static_cast<int16_t>(length > 0 ? pos % length : 0);
}

cChordMode::strVoice cChordMode::startVoice(uint8_t note,
                                            uint8_t velocity,
                                            int16_t source,
                                            int8_t recFirst,
                                            uint8_t recCount,
                                            bool previewOnly)
{
    strVoice voice;
    voice.note = note;
    voice.source = source;
    voice.on = true;
    voice.recFirst = recFirst;
    voice.recCount = recCount;
    voice.previewOnly = previewOnly;

    uint8_t previousVoice = engine.getLastUsedVoice();
    engine.setLastUsedVoice(kNoVoice);

    sequencer.liveVelocityAlways = true;
    sequencer.liveRecSampleTracksOnly = true;
    sequencer.liveRecFirstTrack = recFirst;
    sequencer.liveRecTrackCount = recCount;
    sequencer.livePreviewOnly = previewOnly;
    sequencer.handleNoteOn(Sequencer::GRID_OUTSIDE_PATTERN, note, static_cast<int8_t>(scaled(velocity)), source);
    sequencer.liveVelocityAlways = false;
    sequencer.liveRecSampleTracksOnly = false;
    sequencer.liveRecFirstTrack = -1;
    sequencer.liveRecTrackCount = 0;
    sequencer.livePreviewOnly = false;

    uint8_t usedVoice = engine.getLastUsedVoice();
    if (usedVoice < kSampleTracks)
    {
        voice.track = static_cast<int8_t>(usedVoice);
        if (jogMode == kJogFx)
        {
            applyFxLive(0, voice.track);
            applyFxLive(1, voice.track);
        }
    }
    else
    {
        engine.setLastUsedVoice(previousVoice);
        // a sample note with no voice found no free track (unless recording, where the
        // sequencer may have recorded it without a preview)
        if (sampleInstrument() && (!sequencer.isRec() || previewOnly))
        {
            voice.on = false;
        }
    }
    return voice;
}

void cChordMode::endVoice(strVoice& voice)
{
    if (!voice.on)
    {
        return;
    }

    sequencer.liveRecFirstTrack = voice.recFirst;
    sequencer.liveRecTrackCount = voice.recCount;
    sequencer.livePreviewOnly = voice.previewOnly;
    sequencer.handleNoteOff(Sequencer::GRID_OUTSIDE_PATTERN, voice.note, 0, voice.source);
    sequencer.liveRecFirstTrack = -1;
    sequencer.liveRecTrackCount = 0;
    sequencer.livePreviewOnly = false;

    if (voice.track >= 0 && voice.track < kSampleTracks)
    {
        if (releaseMode == kReleaseFade)
        {
            instrumentPlayer[voice.track].noteOff(Sequencer::STEP_NOTE_OFF_FADE);
        }
        else if (releaseMode == kReleaseCut)
        {
            instrumentPlayer[voice.track].noteOff(Sequencer::STEP_NOTE_OFF_CUT);
        }
    }

    voice.on = false;
    voice.track = -1;
}

// Hand over from old voices to new notes without gaps or lost recordings:
//  1. old voices at a pitch the new notes also use end first (while recording and for MIDI
//     the sequencer turns notes off by pitch, which would otherwise end the new note too);
//  2. the new notes start, on free tracks where there are any, so ringing notes are not cut;
//  3. the remaining old voices end (recording on one armed track only writes its note-off
//     once no note is held, so it no longer lands in the step the new note needs);
//  4. a sample note that found no free track tries again now that old tracks are free.
void cChordMode::handover(strVoice* old,
                          uint8_t oldCount,
                          const uint8_t* notes,
                          const int16_t* sources,
                          uint8_t count,
                          uint8_t velocity,
                          strVoice* out,
                          uint8_t lanes,
                          bool previewOnly)
{
    int8_t recFirst;
    uint8_t recCount;
    recLanes(lanes, recFirst, recCount);

    for (uint8_t i = 0; i < oldCount; i++)
    {
        for (uint8_t j = 0; j < count; j++)
        {
            if (old[i].on && old[i].note == notes[j])
            {
                endVoice(old[i]);
                break;
            }
        }
    }

    for (uint8_t j = 0; j < count; j++)
    {
        out[j] = startVoice(notes[j], velocity, sources[j], recFirst, recCount, previewOnly);
    }

    for (uint8_t i = 0; i < oldCount; i++)
    {
        endVoice(old[i]);
    }

    for (uint8_t j = 0; j < count; j++)
    {
        if (!out[j].on)
        {
            out[j] = startVoice(notes[j], velocity, sources[j], recFirst, recCount, previewOnly);
        }
    }
}

void cChordMode::playChordBlock(uint8_t velocity)
{
    uint8_t notes[kMaxChordNotes];
    uint8_t count = voicedChordNotes(notes);

    strVoice old[kMaxChordNotes];
    uint8_t oldCount = chordVoiceCount;
    for (uint8_t i = 0; i < oldCount; i++)
    {
        old[i] = chordVoices[i];
    }
    chordVoiceCount = 0;
    chordBlockOwnerPad = 255;

    chordBankB = !chordBankB;
    int16_t base = chordBankB ? kSrcChordBankB : kSrcChordBankA;
    int16_t sources[kMaxChordNotes];
    for (uint8_t i = 0; i < count; i++)
    {
        sources[i] = static_cast<int16_t>(base + i);
    }

    handover(old, oldCount, notes, sources, count, velocity, chordVoices, chordVoiceLimit);
    chordVoiceCount = count;
}

void cChordMode::releaseChordBlock()
{
    for (uint8_t i = 0; i < chordVoiceCount; i++)
    {
        endVoice(chordVoices[i]);
    }
    chordVoiceCount = 0;
    chordBlockOwnerPad = 255;
}

// Strum notes ring on in a small pool; when the pool is full the oldest note is released.
// For samples the new note starts on a free track before the oldest is released.
void cChordMode::playStrumNote(uint8_t note, uint8_t velocity, bool previewOnly)
{
    int16_t source = kSrcStrumBase + strumSourceNext;
    strumSourceNext = static_cast<uint8_t>((strumSourceNext + 1) % kSrcStrumCount);

    strVoice& slot = strumVoices[strumNext];
    strumNext = static_cast<uint8_t>((strumNext + 1) % strumVoiceLimit);

    strVoice oldest = slot;
    strVoice fresh;
    handover(&oldest, 1, &note, &source, 1, velocity, &fresh, kMaxChordNotes + 1, previewOnly);
    slot = fresh;
    slot.offAt = ringMs() ? (nowMs() + ringMs()) | 1u : 0;
}

void cChordMode::releaseStrumNotes()
{
    for (uint8_t i = 0; i < kStrumVoices; i++)
    {
        endVoice(strumVoices[i]);
    }
    strumNext = 0;
}

void cChordMode::releaseAll()
{
    releaseChordBlock();
    releaseStrumNotes();
    endVoice(arpVoice);
}

//==============================================================================================
// Voicing, strum on press, arpeggiator
//==============================================================================================

// The block chord, cut down to chordVoiceLimit notes: the perfect fifth goes first (it adds
// least to the harmony), then the highest notes.
uint8_t cChordMode::voicedChordNotes(uint8_t* out) const
{
    uint8_t tones[kMaxChordNotes];
    uint8_t count = buildChordNotes(tones, kMaxChordNotes);
    if (count == 0)
    {
        return 0;
    }

    bool dropFifth = count > chordVoiceLimit;
    uint8_t kept = 0;
    for (uint8_t i = 0; i < count; i++)
    {
        if (dropFifth && tones[i] - tones[0] == 7)
        {
            dropFifth = false;
            continue;
        }
        out[kept++] = tones[i];
    }
    return kept > chordVoiceLimit ? chordVoiceLimit : kept;
}

// Strum on press: the chord's notes and the root an octave up, kRollSpacingMs apart,
// played into the strum voices (so the Ring and Release settings apply).
void cChordMode::startRoll()
{
    uint8_t tones[kMaxChordNotes];
    uint8_t count = buildChordNotes(tones, kMaxChordNotes);
    rollCount = 0;
    for (uint8_t i = 0; i < count; i++)
    {
        rollNotes[rollCount++] = tones[i];
    }
    if (count > 0 && tones[0] + 12 <= 127)
    {
        rollNotes[rollCount++] = static_cast<uint8_t>(tones[0] + 12);
    }
    rollPos = 0;
    rollNextMs = nowMs();
    rollPreviewOnly = false;

    // Recording with too few armed tracks for a lane per note: a track holds one note per step,
    // so the strum goes onto one lane, one note per step, each with Chord mode's velocity and a
    // micromove keeping it as close to where it was played as the step allows (micromove can only
    // delay a note within its step). The live notes are then not recorded again.
    if (sequencer.isRec() && rollCount > 0 && armedLanes() < rollCount)
    {
        int8_t track;
        uint8_t count;
        recLanes(1, track, count);
        if (track >= 0 && count > 0)
        {
            uint32_t length = stepMs > 0 ? stepMs : 1;
            uint32_t intoStep = sequencer.isPlay() ? nowMs() - seenPosMs : 0;
            int16_t start = sequencer.getActualPos();
            int32_t startOffset = static_cast<int32_t>(intoStep); // ms after the start of `start`
            int8_t volume = recordVolume();

            newFileManager.storePatternUndoRevision();
            for (uint8_t i = 0; i < rollCount; i++)
            {
                int32_t played = startOffset + static_cast<int32_t>(i * kRollSpacingMs);
                int32_t offset = played - static_cast<int32_t>(i * length); // into step start + i
                if (offset < 0)
                {
                    offset = 0;
                }
                int8_t micromove = static_cast<int8_t>(offset >= static_cast<int32_t>(length)
                                                         ? 100
                                                         : (offset * 100) / static_cast<int32_t>(length));
                writeNoteStep(track, static_cast<int16_t>(start + i), rollNotes[i], volume, micromove);
            }
            // the last note rings for the Ring time, then a note-off (Hold: until the next note)
            if (ringMs() > 0)
            {
                uint32_t ringSteps = (ringMs() + length - 1) / length;
                writeOffStep(track, static_cast<int16_t>(start + rollCount - 1 + (ringSteps < 1 ? 1 : ringSteps)));
            }
            newFileManager.setActualPatternStructChanged();
            rollPreviewOnly = true;
        }
    }
}

void cChordMode::stopRoll()
{
    rollCount = 0;
    rollPos = 0;
}

// One pattern step at the project tempo (four steps to the beat), for when the pattern is stopped.
uint32_t cChordMode::freeStepMs() const
{
    float tempo = mtProject.values.globalTempo;
    if (tempo < 20.0f)
    {
        tempo = 20.0f;
    }
    return static_cast<uint32_t>(60000.0f / tempo / 4.0f);
}

// Recording: write the whole arpeggio as one step on the selected track - root note, the Chord
// effect (the chord) and the Arp effect (direction and rate) - so the sequencer plays it back
// itself on one lane, with no extra tracks or voices.
// Write root + Chord + Arp effects as one step on the first armed lane.
bool cChordMode::writeChordStep(int16_t stepIndex, uint8_t arpValue, int8_t& trackOut)
{
    int8_t track;
    uint8_t count;
    recLanes(1, track, count);
    if (count == 0 || track < 0 || !hasChord())
    {
        return false;
    }

    Sequencer::strPattern* pattern = sequencer.getActualPattern();
    int16_t length = sequencer.getPatternLength();
    if (pattern == nullptr || length <= 0)
    {
        return false;
    }
    stepIndex = static_cast<int16_t>(((stepIndex % length) + length) % length);

    newFileManager.storePatternUndoRevision();

    Sequencer::strPattern::strTrack::strStep& step = pattern->track[track].step[stepIndex];
    step.note = static_cast<int8_t>(12 * octave + currentRootPc);
    step.instrument = mtProject.values.lastUsedInstrument;

    step.volume = recordVolume();

    step.fx[0].type = sequencer.fx.FX_TYPE_MIDI_CHORD;
    step.fx[0].value = currentChordIdx;
    step.fx[1].type = sequencer.fx.FX_TYPE_ARP;
    step.fx[1].value = arpValue;

    newFileManager.setActualPatternStructChanged();
    trackOut = track;
    return true;
}

// Chord mode velocity (level x gain) as a step volume (0..100). Always recorded: what Chord mode
// plays always uses it, whatever Config > Recording options says.
int8_t cChordMode::recordVolume() const
{
    // as the sequencer does for live notes: the velocity is scaled by the instrument's own
    // velocity setting before it becomes a volume, so a recorded step sounds as loud as live
    int velocity = scaled(levelVelocity());
    int instrumentVelo = sequencer.getInstrumentVelo(mtProject.values.lastUsedInstrument);
    velocity = (velocity * instrumentVelo + 63) / 127;
    return static_cast<int8_t>((velocity * 100 + 63) / 127);
}

// One note on an empty step (an occupied step is left alone, as live recording does).
bool cChordMode::writeNoteStep(int8_t track, int16_t stepIndex, uint8_t note, int8_t volume, int8_t micromove)
{
    Sequencer::strPattern* pattern = sequencer.getActualPattern();
    int16_t length = sequencer.getPatternLength();
    if (pattern == nullptr || length <= 0 || track < 0)
    {
        return false;
    }
    stepIndex = static_cast<int16_t>(((stepIndex % length) + length) % length);
    Sequencer::strPattern::strTrack::strStep& step = pattern->track[track].step[stepIndex];
    if (!step.isNoteEmpty() || step.hasAnyFx())
    {
        return false;
    }
    step.note = static_cast<int8_t>(note);
    step.instrument = mtProject.values.lastUsedInstrument;
    step.volume = volume;
    step.micromove = micromove;
    return true;
}

void cChordMode::writeOffStep(int8_t track, int16_t stepIndex)
{
    Sequencer::strPattern* pattern = sequencer.getActualPattern();
    int16_t length = sequencer.getPatternLength();
    if (pattern == nullptr || length <= 0 || track < 0)
    {
        return;
    }
    stepIndex = static_cast<int16_t>(((stepIndex % length) + length) % length);
    Sequencer::strPattern::strTrack::strStep& step = pattern->track[track].step[stepIndex];
    if (step.isNoteEmpty())
    {
        step.note = Sequencer::STEP_NOTE_OFF_DEFAULT;
        newFileManager.setActualPatternStructChanged();
    }
}

// Recording: the whole arpeggio as one step, so the sequencer's own arpeggiator plays it back.
void cChordMode::recordArpStep(int16_t stepIndex)
{
    int8_t track;
    uint8_t value = static_cast<uint8_t>(arpPattern * sequencer.fx.arpPeriodsCount + arpRate + 1);
    if (!writeChordStep(stepIndex, value, track))
    {
        return;
    }
    stepIndex = static_cast<int16_t>(stepIndex % sequencer.getPatternLength());

    arpRecording = true;
    arpRecTrack = track;
    arpRecStep = stepIndex;
    arpLeftRecStep = false;
}

// Recording: the arpeggio stopped - end the recorded step with a note-off on the step it stopped on.
void cChordMode::recordArpOff()
{
    if (arpRecording && arpRecTrack >= 0 && sequencer.isRec())
    {
        int16_t pos = recordPosition();
        if (pos != arpRecStep)
        {
            writeOffStep(arpRecTrack, pos);
        }
    }
    arpRecording = false;
}

void cChordMode::startArp(uint8_t pad)
{
    arpPad = pad;

    // where the arpeggio starts: a press in the first half of a step plays at once, a later
    // press waits for the next step (that is also where a recording puts it)
    bool late = sequencer.isPlay() && (nowMs() - seenPosMs) * 2 > stepMs;

    if (sequencer.isRec())
    {
        int16_t pos = sequencer.getActualPos();
        recordArpStep(static_cast<int16_t>(late ? pos + 1 : pos));
    }

    if (arpRunning || arpHandedOver)
    {
        // a new chord while running: the next note uses it; a recording gets a new step
        arpRunning = true;
        arpHandedOver = false;
        arpIndex = -1;
        arpSubLeft = 0;
        if (!late)
        {
            arpStepCount = 0;
            arpStep();
        }
        else
        {
            arpStepCount = -1;
        }
        return;
    }

    arpRunning = true;
    arpHandedOver = false;
    arpIndex = -1;
    arpSubLeft = 0;
    arpLastPos = sequencer.getActualPos();
    arpNextMs = nowMs() + freeStepMs();

    if (late)
    {
        arpStepCount = -1; // the next step boundary counts as step 0 and plays the first note
        return;
    }

    arpStepCount = 0;
    arpStep();
    static const uint8_t notesPerStep[kArpRates] = { 0, 0, 0, 0, 0, 0, 2, 3, 4, 6, 8 };
    if (notesPerStep[arpRate] > 0)
    {
        uint32_t stepLength = sequencer.isPlay() ? stepMs : freeStepMs();
        arpSubLeft = static_cast<uint8_t>(notesPerStep[arpRate] - 1);
        arpSubMs = nowMs() + stepLength / notesPerStep[arpRate];
    }
}

void cChordMode::stopArp()
{
    if (arpActive())
    {
        recordArpOff();
    }
    arpRunning = false;
    arpHandedOver = false;
    arpPad = 255;
    arpSubLeft = 0;
    endVoice(arpVoice);
}

// Next note, chosen as the sequencer's Arp effect does: the chord's notes within one octave
// above the root, up / down in turn or at random. The first note up is the root, down the top note.
void cChordMode::arpStep()
{
    uint8_t tones[kMaxChordNotes];
    uint8_t count = buildChordNotes(tones, kMaxChordNotes);
    uint8_t inOctave = 0;
    for (uint8_t i = 0; i < count; i++)
    {
        if (tones[i] - tones[0] < 12)
        {
            tones[inOctave++] = tones[i];
        }
    }
    if (inOctave == 0)
    {
        stopArp();
        return;
    }

    switch (arpPattern)
    {
        case kArpUp: arpIndex = static_cast<int16_t>((arpIndex + 1) % inOctave); break;
        case kArpDown: arpIndex = static_cast<int16_t>(arpIndex <= 0 ? inOctave - 1 : arpIndex - 1); break;
        default:
            arpRandom = arpRandom * 1103515245u + 12345u;
            arpIndex = static_cast<int16_t>((arpRandom >> 16) % inOctave);
            break;
    }
    if (arpIndex < 0 || arpIndex >= inOctave)
    {
        arpIndex = 0;
    }

    // legato: the next note starts before the previous one is released. While recording, the
    // live notes are not recorded (the arpeggio is already written as one step).
    strVoice previous = arpVoice;
    arpSourceB = !arpSourceB;
    int16_t source = static_cast<int16_t>(kSrcArp + (arpSourceB ? 1 : 0));
    uint8_t arpNote = tones[arpIndex];
    bool previewOnly = sequencer.isRec();

    if (previous.on && previous.note == arpNote)
    {
        endVoice(previous);
    }
    arpVoice = startVoice(arpNote, levelVelocity(), source, -1, 0, previewOnly);
    endVoice(previous);
    if (!arpVoice.on)
    {
        arpVoice = startVoice(arpNote, levelVelocity(), source, -1, 0, previewOnly);
    }
}

//==============================================================================================
// Pad, jog and key actions
//==============================================================================================

void cChordMode::clearChord()
{
    currentRootPc = -1;
    currentChordPad = 255;
    currentChordPadHeld = false;
    currentChordName = "";
    jogPos = -1;
}

void cChordMode::chordPadPress(uint8_t pad, bool alt)
{
    uint8_t row = pad / kColumns;
    uint8_t column = pad % kColumns;
    const strChordType& type = chordTypes[alt ? 1 : 0][row];

    currentRootPc = static_cast<int8_t>(columnToPitchClass(column));
    currentChordIdx = type.chordDefIdx;
    currentChordName = type.name;
    currentChordPad = pad;
    currentChordPadHeld = true;
    jogPos = -1;

    switch (chordPlay)
    {
        case kPlayBlock:
            stopRoll();
            playChordBlock(levelVelocity());
            chordBlockOwnerPad = pad;
            break;

        case kPlayStrum:
            releaseChordBlock();
            startRoll();
            break;

        case kPlayArp:
            releaseChordBlock();
            if (latch && arpActive() && arpPad == pad)
            {
                stopArp(); // latched: the same pad again stops it
            }
            else
            {
                startArp(pad);
            }
            break;

        default: // select only
            releaseChordBlock();
            break;
    }

    showInfo();
    lightUpPadBoard();
}

void cChordMode::chordPadRelease(uint8_t pad)
{
    if (pad == chordBlockOwnerPad)
    {
        releaseChordBlock();
    }

    if (!latch && arpActive() && pad == arpPad)
    {
        stopArp();
    }

    if (pad == currentChordPad)
    {
        currentChordPadHeld = false;
        if (!latch)
        {
            stopRoll();
            releaseStrumNotes();
            clearChord();
            showInfo();
        }
    }
    lightUpPadBoard();
}

void cChordMode::strumPadPress(uint8_t column)
{
    int16_t note = strumNote(column);
    if (note < 0)
    {
        return;
    }
    playStrumNote(static_cast<uint8_t>(note), levelVelocity());
    jogPos = static_cast<int8_t>(column);
}

void cChordMode::levelPadPress(uint8_t pad)
{
    setLevel(static_cast<uint8_t>(pad % kColumns + 1));

    if (hasChord())
    {
        if (chordPlay == kPlayStrum)
        {
            startRoll();
        }
        else if (chordPlay != kPlayArp)
        {
            playChordBlock(levelVelocity());
            chordBlockOwnerPad = pad;
        }
        else if (arpActive() && sequencer.isRec())
        {
            startArp(arpPad); // the arpeggio takes the new level; recording writes it as a new step
        }
    }

    showInfo();
    lightUpPadBoard();
}

void cChordMode::levelPadRelease(uint8_t pad)
{
    if (pad == chordBlockOwnerPad)
    {
        releaseChordBlock();
    }
    lightUpPadBoard();
}

void cChordMode::jog(int16_t value)
{
    if (value == 0)
    {
        return;
    }
    int8_t dir = value > 0 ? 1 : -1;

    if (buttons->isButtonPressed(interfaceButtonShift))
    {
        cycleJogMode(dir);
    }
    else if (f6Held)
    {
        changeTrack(dir);
    }
    else if (f3Held)
    {
        f3Turned = true;
        changeFxType(dir);
    }
    else
    {
        switch (jogMode)
        {
            case kJogVelocity: jogVelocity(value); break;
            case kJogFx: jogFx(value); break;
            default: jogStrum(value); break;
        }
    }
}

//==============================================================================================
// Jog modes
//==============================================================================================

// Effects the jog can move in FX mode: the sequencer's step effects, applied live to Chord mode's
// voices and recorded into the chosen FX slot.
static const uint8_t jogFxTypes[cChordMode::kJogFxTypes] = {
    Sequencer::strFxConsts::FX_TYPE_FILTER_LOWPASS, Sequencer::strFxConsts::FX_TYPE_FILTER_HIGHPASS,
    Sequencer::strFxConsts::FX_TYPE_FILTER_BANDPASS, Sequencer::strFxConsts::FX_TYPE_VOLUME,
    Sequencer::strFxConsts::FX_TYPE_PANNING,        Sequencer::strFxConsts::FX_TYPE_REVERB_SEND,
    Sequencer::strFxConsts::FX_TYPE_DELAY_SEND,     Sequencer::strFxConsts::FX_TYPE_OVERDRIVE,
    Sequencer::strFxConsts::FX_TYPE_BIT_DEPTH,
};

uint8_t cChordMode::fxType(uint8_t slot) const
{
    return fxCustomType[slot] >= 0 ? static_cast<uint8_t>(fxCustomType[slot])
                                   : jogFxTypes[fxTypeIdx[slot] % kJogFxTypes];
}

const char* cChordMode::fxShortName(uint8_t slot) const
{
    return interfaceGlobals.fxShortNames[interfaceGlobals.fxIdToName(fxType(slot))];
}

// FX1 / FX2 key: the effect chosen in the popup goes into that slot, the jog drives that slot and
// switches to FX mode. The other slot keeps its own effect.
void cChordMode::fxPicked(uint8_t slot, uint8_t type)
{
    clearFxLive(slot);
    fxSlot = slot;
    fxCustomType[slot] = -1;
    for (uint8_t i = 0; i < kJogFxTypes; i++)
    {
        if (jogFxTypes[i] == type)
        {
            fxTypeIdx[slot] = i;
        }
    }
    if (jogFxTypes[fxTypeIdx[slot]] != type)
    {
        fxCustomType[slot] = type;
    }
    fxValue[slot] = -1;
    jogMode = kJogFx;
}

void cChordMode::cycleJogMode(int8_t delta)
{
    if (jogMode == kJogFx)
    {
        clearFxLive();
    }
    jogMode = static_cast<uint8_t>((jogMode + kJogModes + delta) % kJogModes);
    jogStarted = false;
    showFunctionLabels();
    showInfo();
}

// Velocity mode: two per click, no notes. The level (Up / Down, Levels row) follows it.
void cChordMode::jogVelocity(int16_t value)
{
    int next = velocity + 2 * value;
    velocity = static_cast<uint8_t>(next < 1 ? 1 : (next > 127 ? 127 : next));
    level = static_cast<uint8_t>((velocity * kLevels + 63) / 127);
    if (level < 1)
    {
        level = 1;
    }
    showInfo();
    lightUpPadBoard();
}

void cChordMode::changeFxType(int8_t delta)
{
    clearFxLive(fxSlot);
    fxCustomType[fxSlot] = -1;
    fxTypeIdx[fxSlot] = static_cast<uint8_t>((fxTypeIdx[fxSlot] + kJogFxTypes + delta) % kJogFxTypes);
    fxValue[fxSlot] = -1;
    showFunctionLabels();
    showInfo();
}

// F3 tap: the jog drives the other slot (both keep their effect, value and what is heard)
void cChordMode::toggleFxSlot()
{
    fxSlot = fxSlot ? 0 : 1;
    showFunctionLabels();
    showInfo();
}

// FX mode: about two turns of the jog cover the effect's whole range. The jog sets the target;
// what is heard glides to it (glideFx) so a sweep is smooth, and a recording takes the target.
void cChordMode::jogFx(int16_t value)
{
    uint8_t type = fxType();
    int16_t lo = sequencer.getFxMin(type);
    int16_t hi = sequencer.getFxMax(type);
    if (hi <= lo)
    {
        return;
    }
    int16_t& target = fxValue[fxSlot];
    if (target < 0)
    {
        target = static_cast<int16_t>((lo + hi) / 2);
    }
    int stepSize = (hi - lo) / 48;
    if (stepSize < 1)
    {
        stepSize = 1;
    }
    int next = target + value * stepSize;
    target = static_cast<int16_t>(next < lo ? lo : (next > hi ? hi : next));
    if (fxLive[fxSlot] < 0)
    {
        fxLive[fxSlot] = target; // first touch: start there
        applyFxToVoices(fxSlot);
    }

    if (sequencer.isRec())
    {
        recordFxValue();
    }
    showInfo();
}

// Every few ms, move what is heard a part of the way to the jog's value, on every track Chord mode
// is playing on and the recording lane, for a smooth sweep like Performance mode.
void cChordMode::glideFx()
{
    uint32_t now = nowMs();
    if (static_cast<int32_t>(now - fxGlideMs) < 4)
    {
        return;
    }
    fxGlideMs = now;

    for (uint8_t slot = 0; slot < 2; slot++)
    {
        if (fxValue[slot] < 0 || fxLive[slot] < 0 || fxLive[slot] == fxValue[slot])
        {
            continue;
        }
        int diff = fxValue[slot] - fxLive[slot];
        int move = diff / 4;
        if (move == 0)
        {
            move = diff > 0 ? 1 : -1;
        }
        fxLive[slot] = static_cast<int16_t>(fxLive[slot] + move);
        applyFxToVoices(slot);
    }
}

void cChordMode::applyFxToVoices(uint8_t slot)
{
    for (uint8_t i = 0; i < chordVoiceCount; i++)
    {
        if (chordVoices[i].on)
        {
            applyFxLive(slot, chordVoices[i].track);
        }
    }
    for (uint8_t i = 0; i < kStrumVoices; i++)
    {
        if (strumVoices[i].on)
        {
            applyFxLive(slot, strumVoices[i].track);
        }
    }
    if (arpVoice.on)
    {
        applyFxLive(slot, arpVoice.track);
    }

    int8_t lane;
    uint8_t count;
    recLanes(1, lane, count);
    applyFxLive(slot, lane);
}

void cChordMode::applyFxLive(uint8_t slot, int8_t track)
{
    if (track < 0 || track >= kSampleTracks || fxLive[slot] < 0)
    {
        return;
    }
    instrumentPlayer[track].seqFx(fxType(slot), static_cast<uint8_t>(fxLive[slot]), slot);
    fxLiveTracks[slot] = static_cast<uint16_t>(fxLiveTracks[slot] | (1u << track));
}

// Take a slot's live effect off again (another effect chosen, FX mode left, module closed).
void cChordMode::clearFxLive(uint8_t slot)
{
    for (uint8_t track = 0; track < kSampleTracks; track++)
    {
        if (fxLiveTracks[slot] & (1u << track))
        {
            instrumentPlayer[track].endFx(fxType(slot), slot);
        }
    }
    fxLiveTracks[slot] = 0;
    fxLive[slot] = -1;
}

void cChordMode::clearFxLive()
{
    clearFxLive(0);
    clearFxLive(1);
}

// Recording: the jog's value goes into the driven slot of the recording lane's current step (an
// empty step gets the effect alone, which the sequencer applies to the note that is playing). A
// slot holding a recorded arpeggio's Chord or Arp effect is left alone.
void cChordMode::recordFxValue()
{
    int8_t lane;
    uint8_t count;
    recLanes(1, lane, count);
    Sequencer::strPattern* pattern = sequencer.getActualPattern();
    if (lane < 0 || count == 0 || pattern == nullptr || fxValue[fxSlot] < 0)
    {
        return;
    }

    int16_t pos = recordPosition();
    Sequencer::strPattern::strTrack::strStep& step = pattern->track[lane].step[pos];
    uint8_t held = step.fx[fxSlot].type;
    if (held == sequencer.fx.FX_TYPE_MIDI_CHORD || held == sequencer.fx.FX_TYPE_ARP)
    {
        return;
    }

    if (!fxUndoStored)
    {
        newFileManager.storePatternUndoRevision();
        fxUndoStored = true;
    }
    step.fx[fxSlot].type = fxType();
    step.fx[fxSlot].value = static_cast<uint8_t>(fxValue[fxSlot]);
    newFileManager.setActualPatternStructChanged();
}

// Each encoder step plays the next chord note up or down; how fast the steps come sets the velocity.
void cChordMode::jogStrum(int16_t value)
{
    if (!hasChord() || value == 0)
    {
        return;
    }

    uint32_t dt = jogStarted ? static_cast<uint32_t>(jogTimer) : 10000;
    jogTimer = 0;
    jogStarted = true;
    if (dt < 1)
    {
        dt = 1;
    }

    int steps = value > 0 ? value : -value;
    int dir = value > 0 ? 1 : -1;

    // Clicks per second -> velocity: about 3/s or slower is soft, about 40/s (a fast spin)
    // is full. A turn that starts after a pause begins at the level velocity, then follows
    // the turning speed, smoothed so one uneven click does not jump the loudness.
    int rate = static_cast<int>((steps * 1000u) / dt);
    int target = mapClamped<int>(rate, 3, 40, 30, 127);

    if (dt > 300 || jogMode == kJogStrum)
    {
        jogSmoothVelocity = levelVelocity(); // Strum mode: always the current velocity
    }
    else
    {
        jogSmoothVelocity = static_cast<int16_t>((jogSmoothVelocity * 2 + target + 1) / 3);
    }
    uint8_t velocity = static_cast<uint8_t>(clamp<int>(jogSmoothVelocity, 1, 127));
    lastJogVelocity = velocity;

    if (steps > 3)
    {
        steps = 3; // a fast flick plays a short run, not a flood of notes
    }

    for (int i = 0; i < steps; i++)
    {
        int next = jogPos + dir;
        if (jogPos < 0)
        {
            next = dir > 0 ? 0 : kColumns - 1;
        }
        if (next < 0 || next >= kColumns)
        {
            if (!strumWrap)
            {
                break; // the end of the strum plate
            }
            next = (next + kColumns) % kColumns; // wrap: past the top note back to the bottom, and back
        }

        jogPos = static_cast<int8_t>(next);
        int16_t note = strumNote(static_cast<uint8_t>(jogPos));
        if (note >= 0)
        {
            playStrumNote(static_cast<uint8_t>(note), velocity);
        }
    }

    showInfo();
}

void cChordMode::toggleStrumWrap()
{
    strumWrap = !strumWrap;
    showFunctionLabels();
}

void cChordMode::toggleRowMode()
{
    rowMode = (rowMode == kRowStrum) ? kRowLevels : kRowStrum;
    releaseChordBlock();
    showFunctionLabels();
    showInfo();
    lightUpPadBoard();
}

void cChordMode::changeOctave(int8_t delta)
{
    int next = octave + delta;
    if (next < kMinOctave || next > kMaxOctave)
    {
        return;
    }
    releaseAll();
    octave = static_cast<int8_t>(next);
    jogPos = -1;
    showInfo();
}

void cChordMode::changeLevel(int8_t delta)
{
    int next = level + delta;
    if (next < 1 || next > kLevels)
    {
        return;
    }
    setLevel(static_cast<uint8_t>(next));
    if (chordPlay == kPlayArp && arpActive() && sequencer.isRec() && hasChord())
    {
        startArp(arpPad); // record the new volume as a new arp step
    }
    showInfo();
    lightUpPadBoard();
}

void cChordMode::toggleLatch()
{
    latch = !latch;
    if (!latch && hasChord() && !currentChordPadHeld)
    {
        stopArp();
        stopRoll();
        releaseStrumNotes();
        clearChord(); // its pad is no longer held
    }
    showFunctionLabels();
    showInfo();
    lightUpPadBoard();
}

void cChordMode::cycleChordPlay()
{
    chordPlay = static_cast<uint8_t>((chordPlay + 1) % kPlayModes);
    stopArp();
    stopRoll();
    releaseChordBlock();
    showFunctionLabels();
    showInfo();
}

void cChordMode::cycleStrumVoices()
{
    strumVoiceLimit = static_cast<uint8_t>(strumVoiceLimit % kStrumVoices + 1); // 1 2 3 1 ...
    for (uint8_t i = strumVoiceLimit; i < kStrumVoices; i++)
    {
        endVoice(strumVoices[i]);
    }
    if (strumNext >= strumVoiceLimit)
    {
        strumNext = 0;
    }
    showFunctionLabels();
    showInfo();
}

void cChordMode::cycleChordVoices()
{
    chordVoiceLimit = static_cast<uint8_t>(chordVoiceLimit >= 4 ? 2 : chordVoiceLimit + 1); // 2 3 4 2 ...
    showFunctionLabels();
    showInfo();
}

// While recording a running arpeggio, a new direction or rate is written as a new step at that
// point (root + Chord + the new Arp setting), and the live arpeggio restarts with it there, as
// the sequencer will when it plays the new step.
void cChordMode::cycleArpPattern()
{
    arpPattern = static_cast<uint8_t>((arpPattern + 1) % kArpPatterns);
    arpDir = 1;
    if (arpActive() && sequencer.isRec() && hasChord())
    {
        startArp(arpPad);
    }
    showFunctionLabels();
    showInfo();
}

void cChordMode::cycleArpRate()
{
    arpRate = static_cast<uint8_t>((arpRate + 1) % kArpRates);
    if (arpActive() && sequencer.isRec() && hasChord())
    {
        startArp(arpPad);
    }
    showFunctionLabels();
    showInfo();
}

void cChordMode::cycleReleaseMode()
{
    releaseMode = static_cast<uint8_t>((releaseMode + 1) % kReleaseModes);
    showFunctionLabels();
    showInfo();
}

void cChordMode::cycleRing()
{
    ringMode = static_cast<uint8_t>((ringMode + 1) % kRingModes);
    showFunctionLabels();
    showInfo();
}

void cChordMode::cycleGain()
{
    gain = (gain == 100) ? 75 : (gain == 75) ? 50 : 100;
    showFunctionLabels();
    showInfo();
}

// The selected track: where live notes start looking for a free track, and where recording starts.
void cChordMode::changeTrack(int8_t delta)
{
    int track = getActualTrack() + delta;
    if (track < 0 || track >= kAllTracks)
    {
        return;
    }

    patternEditor.trackerPattern.actualTrack = static_cast<int16_t>(track);
    if (!isMultiSelection())
    {
        sendSelection();
    }
    showTrackLabel();
}

void cChordMode::setShiftLabels(bool shift)
{
    if (shift == shiftLabels)
    {
        return;
    }
    shiftLabels = shift;
    showFunctionLabels();
}

//==============================================================================================
// Pads
//==============================================================================================

static void functPads(uint8_t pad, uint8_t state, [[maybe_unused]] int16_t velo)
{
    if (pad >= cChordMode::kColumns * 4 || (state != padPress && state != padRelease))
    {
        return; // only press and release (padChange is pressure/velocity updates)
    }

    uint8_t row = pad / cChordMode::kColumns;
    bool pressed = (state == padPress);

    if (row < cChordMode::kChordRows)
    {
        if (pressed)
        {
            padsBacklight.setFrontLayer(1, mtConfig.values.padsLightFront, pad);
            CM->chordPadPress(pad, CM->buttons->isButtonPressed(interfaceButtonShift));
        }
        else
        {
            CM->chordPadRelease(pad);
        }
        return;
    }

    if (CM->rowMode == cChordMode::kRowStrum)
    {
        if (pressed)
        {
            padsBacklight.setFrontLayer(1, mtConfig.values.padsLightFront, pad);
            CM->strumPadPress(pad % cChordMode::kColumns);
        }
        else
        {
            padsBacklight.setFrontLayer(0, 0, pad);
        }
    }
    else
    {
        if (pressed)
        {
            CM->levelPadPress(pad);
        }
        else
        {
            CM->levelPadRelease(pad);
        }
    }
}

void cChordMode::lightUpPadBoard()
{
    padsBacklight.clearGlobalPads(1, 1, 1);

    for (uint8_t pad = 0; pad < kColumns * kChordRows; pad++)
    {
        padsBacklight.setBackLayer(1, mtConfig.values.padsLightBackDim, pad);
    }
    if (currentChordPad < kColumns * kChordRows)
    {
        padsBacklight.setFrontLayer(1, mtConfig.values.padsLightFront, currentChordPad);
    }

    for (uint8_t column = 0; column < kColumns; column++)
    {
        uint8_t pad = kStrumRow * kColumns + column;
        if (rowMode == kRowLevels && column + 1 == level)
        {
            padsBacklight.setFrontLayer(1, mtConfig.values.padsLightFront, pad);
        }
        else
        {
            padsBacklight.setBackLayer(1, mtConfig.values.padsLightBack, pad);
        }
    }
}

//==============================================================================================
// Keys
//==============================================================================================

static void functEncoder(int16_t value)
{
    CM->jog(value);
}

static void functActionButton(uint8_t button, uint8_t state)
{
    if (button == 5) // F6: hold + jog changes the track; Shift + F6: arpeggio pattern
    {
        if (state == buttonPress && CM->buttons->isButtonPressed(interfaceButtonShift))
        {
            CM->cycleArpPattern();
        }
        else if (state == buttonPress)
        {
            CM->f6Held = true;
        }
        else if (state == buttonRelease)
        {
            CM->f6Held = false;
        }
        return;
    }

    bool shift = CM->buttons->isButtonPressed(interfaceButtonShift);

    if (button == 2 && !shift) // F3: tap = FX1 / FX2, hold + jog = which effect
    {
        if (state == buttonPress)
        {
            CM->f3Held = true;
            CM->f3Turned = false;
        }
        else if (state == buttonRelease)
        {
            if (CM->f3Held && !CM->f3Turned)
            {
                CM->toggleFxSlot();
            }
            CM->f3Held = false;
        }
        return;
    }

    if (state != buttonPress)
    {
        return;
    }

    switch (button)
    {
        case 0: shift ? CM->cycleRing() : CM->toggleRowMode(); break;
        case 1: shift ? CM->cycleStrumVoices() : CM->cycleJogMode(1); break;
        case 2: CM->cycleChordVoices(); break; // Shift + F3 (F3 alone is handled above)
        case 3: shift ? CM->cycleReleaseMode() : CM->toggleLatch(); break;
        case 4: shift ? CM->cycleGain() : CM->cycleChordPlay(); break;
        case 6:
            if (shift)
            {
                CM->cycleArpRate();
            }
            break;
        case 7:
            if (shift)
            {
                CM->toggleStrumWrap();
            }
            else
            {
                CM->eventFunct(eventSwitchToPreviousModule, CM, 0, 0);
            }
            break;
        default: break;
    }
}

static void functShift(uint8_t state)
{
    if (state == buttonPress)
    {
        CM->setShiftLabels(true);
    }
    else if (state == buttonRelease)
    {
        CM->setShiftLabels(false);
    }
}

static void functLeft()
{
    if (CM->buttons->isButtonPressed(interfaceButtonShift))
    {
        CM->cycleJogMode(-1);
    }
    else
    {
        CM->changeOctave(-1);
    }
}

static void functRight()
{
    if (CM->buttons->isButtonPressed(interfaceButtonShift))
    {
        CM->cycleJogMode(1);
    }
    else
    {
        CM->changeOctave(1);
    }
}

static void functUp()
{
    CM->changeLevel(1);
}

static void functDown()
{
    CM->changeLevel(-1);
}

// Same transport as the pattern editor: Rec + Play starts live recording,
// Rec + Play while playing punches recording in, Play alone starts / stops.
static void functPlayAction()
{
    bool recHeld = CM->buttons->isButtonPressed(interfaceButtonRec);

    if (sequencer.getSeqState() == Sequencer::SEQ_STATE_STOP)
    {
        CM->releaseAll();

        if (recHeld)
        {
            sequencer.recStart();
        }
        else if (CM->buttons->isButtonPressed(interfaceButtonShift))
        {
            sequencer.playSong();
        }
        else
        {
            sequencer.playPattern();
        }
    }
    else if (recHeld)
    {
        sequencer.recEnable(true);
    }
    else
    {
        sequencer.stop();
    }
    CM->showInfo();
}

static void functRecAction()
{
    if (sequencer.isRec())
    {
        sequencer.recEnable(false);
        CM->showInfo();
    }
}

static void functParams()
{
    if (CM->buttons->isButtonPressed(interfaceButtonShift))
    {
        CM->eventFunct(eventSwitchToPreviousModule, CM, 0, 0);
    }
    else
    {
        functSwitchModule(interfaceButtonParams);
    }
}

static void functSwitchModule(uint8_t button)
{
    if (!performanceMode.canPerformanceModeContinueAfterSwitchingTo(button))
    {
        performanceMode.turnOffAtEndOfPattern();
    }

    CM->eventFunct(eventSwitchModule, CM, &button, 0);
}

//==============================================================================================
// Instr / FX1 / FX2: the standard popups, held while choosing
//==============================================================================================

// A popup takes the pads, so their releases never reach Chord mode: let go of everything now,
// as releasing the pads would (with Latch off the chord and its arpeggio end too).
static void beforePopup()
{
    CM->releaseAll();
    if (!CM->latch)
    {
        CM->stopArp();
        CM->stopRoll();
        CM->clearChord();
    }
}

// Close the popup and give Chord mode its keys, pads and screen back.
static void closePopup()
{
    if (mtPopups.getStepPopupState() != stepPopupNone)
    {
        mtPopups.hideStepPopups();
    }
    CM->start(0);
}

static void functInstrKey(uint8_t state)
{
    if (state == buttonPress)
    {
        beforePopup();
        mtPopups.showStepPopup(stepPopupInstr, mtProject.values.lastUsedInstrument);
    }
    else if (state == buttonRelease)
    {
        closePopup(); // the popup has set the instrument Chord mode plays
    }
}

static void fxKey(uint8_t slot, uint8_t state)
{
    if (state == buttonPress)
    {
        beforePopup();
        mtPopups.showStepPopup(stepPopupFx, static_cast<int8_t>(interfaceGlobals.fxIdToName(CM->fxType(slot))));
    }
    else if (state == buttonRelease)
    {
        uint8_t type = static_cast<uint8_t>(interfaceGlobals.fxNameToId(mtProject.values.lastUsedFx));
        if (type != Sequencer::strFxConsts::FX_TYPE_NONE)
        {
            CM->fxPicked(slot, type);
        }
        closePopup();
    }
}

static void functFx1Key(uint8_t state)
{
    fxKey(0, state);
}

static void functFx2Key(uint8_t state)
{
    fxKey(1, state);
}
