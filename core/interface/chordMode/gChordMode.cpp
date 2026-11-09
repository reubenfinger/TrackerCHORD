#include "chordMode/chordMode.h"

#include <stdio.h>
#include <string.h>

#include "mtConfig.h"
#include "mtSequencer.h"
#include "mtStructs.h"
#include "patternEditor/patternEditor.h"

void cChordMode::initDisplayControls()
{
    strControlProperties prop;

    // screen-key labels
    for (uint8_t i = 0; i < 8; i++)
    {
        prop.colors = display.getTheme().activeLabelsColors;
        prop.text = (char*)"";
        prop.style = (controlStyleShow | controlStyleCenterX | controlStyleCenterY | controlStyleFont3);
        prop.x = (800 / 8) * i + (800 / 16);
        prop.y = 424;
        prop.w = 800 / 8 - 6;
        prop.h = 55;
        prop.value = 0;
        label[i] = display.createControl<cLabel>(&prop);
    }

    strControlProperties bg;
    bg.text = nullptr;
    bg.colors = nullptr;
    bg.value = 255;
    bg.style = controlStyleNoTransparency | controlStyleShow;
    bg.x = 0;
    bg.w = 800;
    bg.y = 425;
    bg.h = 54;
    bgLabel = display.createControl<cBgLabel>(&bg);

    // the pad columns' roots along the top, as a reminder
    for (uint8_t i = 0; i < kColumns; i++)
    {
        strControlProperties root;
        root.colors = display.getTheme().activeLabelsColors;
        root.text = (char*)"";
        root.value = 0;
        root.style = (controlStyleShow | controlStyleCenterX | controlStyleCenterY | controlStyleFont4);
        root.x = (800 / kColumns) * i + (800 / kColumns) / 2;
        root.y = 60;
        root.w = 800 / kColumns - 4;
        root.h = 30;
        rootLabel[i] = display.createControl<cLabel>(&root);
    }

    // information lines
    strControlProperties info;
    info.colors = display.getTheme().activeLabelsColors;
    info.text = (char*)"";
    info.value = 0;
    info.x = 400;
    info.w = 780;
    info.h = 40;

    info.style = (controlStyleShow | controlStyleCenterX | controlStyleCenterY | controlStyleFont4);
    info.y = 120;
    chordLabel = display.createControl<cLabel>(&info);

    info.style = (controlStyleShow | controlStyleCenterX | controlStyleCenterY | controlStyleFont2);
    info.y = 190;
    notesLabel = display.createControl<cLabel>(&info);

    info.y = 250;
    detailLabel = display.createControl<cLabel>(&info);

    info.y = 290;
    jogLabel = display.createControl<cLabel>(&info);

    info.style = (controlStyleShow | controlStyleCenterX | controlStyleCenterY | controlStyleFont5);
    info.y = 340;
    hintLabel = display.createControl<cLabel>(&info);
}

void cChordMode::showDefaultScreen()
{
    titleBar->show();
    titleBar->setTitle("Chord");

    showFunctionLabels();
    showInfo();

    display.refreshControl(bgLabel);
    display.synchronizeRefresh();
}

static const char* const releaseNames[cChordMode::kReleaseModes] = { "Fade", "Cut", "Instr" };
static const char* const ringNames[cChordMode::kRingModes] = { "0.3s", "0.8s", "2s", "Hold" };
static const char* const jogNames[cChordMode::kJogModes] = { "Strum", "Str+Vel", "Velocity", "FX" };
static const char* const playNames[cChordMode::kPlayModes] = { "Block", "Strum", "Arp", "Select" };
static const char* const arpPatternNames[cChordMode::kArpPatterns] = { "Up", "Down", "Random" };
// rates in pattern steps, as the Arp effect shows them (/ 1 = one note per step, /.2 = two per step)
static const char* const arpRateNames[cChordMode::kArpRates] = { "8 st", "6 st", "4 st", "3 st", "2 st", "1 st",
                                                                 "1/2st", "1/3st", "1/4st", "1/6st", "1/8st" };

void cChordMode::showFunctionLabels()
{
    if (shiftLabels)
    {
        snprintf(fnText[0], sizeof(fnText[0]), "Ring %s", ringNames[ringMode]);
    }
    else
    {
        snprintf(fnText[0], sizeof(fnText[0]), "%s", rowMode == kRowStrum ? "Strum" : "Levels");
    }
    if (shiftLabels)
    {
        snprintf(fnText[1], sizeof(fnText[1]), "Strum %dv", strumVoiceLimit);
        snprintf(fnText[2], sizeof(fnText[2]), "Chord %dv", chordVoiceLimit);
        snprintf(fnText[3], sizeof(fnText[3]), "Rel %s", releaseNames[releaseMode]);
        snprintf(fnText[4], sizeof(fnText[4]), "Gain %d%%", gain);
    }
    else
    {
        snprintf(fnText[1], sizeof(fnText[1]), "Jog %s", jogNames[jogMode]);
        snprintf(fnText[2], sizeof(fnText[2]), "FX%d %s", fxSlot + 1, fxShortName());
        snprintf(fnText[3], sizeof(fnText[3]), "%s", latch ? "Latch On" : "Latch Off");
        snprintf(fnText[4], sizeof(fnText[4]), "%s", playNames[chordPlay]);
    }
    if (shiftLabels)
    {
        snprintf(fnText[7], sizeof(fnText[7]), "%s", strumWrap ? "Wrap On" : "Wrap Off");
    }
    else
    {
        snprintf(fnText[7], sizeof(fnText[7]), "Close");
    }

    for (uint8_t i = 0; i < 8; i++)
    {
        if (i == 5 || i == 6)
        {
            continue; // track and step, drawn by their own functions
        }
        display.setControlText(label[i], fnText[i]);
        display.refreshControl(label[i]);
    }

    showTrackLabel();
    showStepLabel();
}

// F6: the selected track (hold F6 + jog to change it)
void cChordMode::showTrackLabel()
{
    int16_t track = getActualTrack();
    shownTrack = track;

    if (shiftLabels)
    {
        snprintf(fnText[5], sizeof(fnText[5]), "Arp %s", arpPatternNames[arpPattern]);
    }
    else if (track < kSampleTracks)
    {
        snprintf(fnText[5], sizeof(fnText[5]), "Track %d", track + 1);
    }
    else
    {
        snprintf(fnText[5], sizeof(fnText[5]), "MIDI %d", track - kSampleTracks + 1);
    }
    display.setControlText(label[5], fnText[5]);
    display.refreshControl(label[5]);
}

// F7: playhead step / pattern length
void cChordMode::showStepLabel()
{
    shownStep = sequencer.getActualPos();
    shownLength = sequencer.getPatternLength();

    if (shiftLabels)
    {
        snprintf(fnText[6], sizeof(fnText[6]), "Rate %s", arpRateNames[arpRate]);
    }
    else
    {
        snprintf(fnText[6], sizeof(fnText[6]), "%d / %d", shownStep + 1, shownLength);
    }
    display.setControlText(label[6], fnText[6]);
    display.refreshControl(label[6]);
}

// The root of each pad column, the selected one in brackets.
void cChordMode::showRootRow()
{
    for (uint8_t i = 0; i < kColumns; i++)
    {
        bool selected = hasChord() && currentChordPad != 255 && currentChordPad % kColumns == i;
        snprintf(rootText[i], sizeof(rootText[i]), selected ? "[%s]" : "%s", columnName(i));
        display.setControlText(rootLabel[i], rootText[i]);
        display.refreshControl(rootLabel[i]);
    }
}

void cChordMode::showInfo()
{
    showRootRow();

    if (hasChord())
    {
        snprintf(chordText, sizeof(chordText), "%s %s", rootName(), currentChordName);

        uint8_t tones[kMaxChordNotes];
        uint8_t count = buildChordNotes(tones, kMaxChordNotes);
        size_t pos = 0;
        notesText[0] = 0;
        for (uint8_t i = 0; i < count && pos < sizeof(notesText); i++)
        {
            int written = snprintf(&notesText[pos], sizeof(notesText) - pos, i ? "  %s" : "%s", mtNotes[tones[i]]);
            if (written < 0)
            {
                break;
            }
            pos += static_cast<size_t>(written);
        }
    }
    else
    {
        snprintf(chordText, sizeof(chordText), "No chord");
        snprintf(notesText, sizeof(notesText), "Pads rows 1-3: major / minor / 7th   (Shift: sus4 / m7 / maj7)");
    }

    // line 3: how the pads play, the velocity, octave and gain
    snprintf(detailText,
             sizeof(detailText),
             "Pads: %s    Row 4: %s    Velocity %d    Octave %d    Gain %d%%",
             playNames[chordPlay],
             rowMode == kRowStrum ? "Strum" : "Levels",
             levelVelocity(),
             octave,
             gain);

    // line 4: the jog
    if (jogMode == kJogFx)
    {
        // both slots, the one the jog drives marked with >
        char slotText[2][28];
        for (uint8_t slot = 0; slot < 2; slot++)
        {
            if (fxValue[slot] >= 0)
            {
                snprintf(slotText[slot], sizeof(slotText[slot]), "%sFX%d %s %d", slot == fxSlot ? ">" : " ",
                         slot + 1, fxShortName(slot), fxValue[slot]);
            }
            else
            {
                snprintf(slotText[slot], sizeof(slotText[slot]), "%sFX%d %s", slot == fxSlot ? ">" : " ",
                         slot + 1, fxShortName(slot));
            }
        }
        snprintf(jogText, sizeof(jogText), "Jog: FX    %s    %s", slotText[0], slotText[1]);
    }
    else if (jogMode == kJogStrumVel)
    {
        snprintf(jogText, sizeof(jogText), "Jog: Strum + velocity    last %d", lastJogVelocity);
    }
    else if (jogMode == kJogVelocity)
    {
        snprintf(jogText, sizeof(jogText), "Jog: Velocity");
    }
    else
    {
        snprintf(jogText, sizeof(jogText), "Jog: Strum");
    }

    const char* transport = sequencer.isRec() ? "REC" : (sequencer.isPlay() ? "PLAY" : "STOP");
    snprintf(hintText,
             sizeof(hintText),
             "%s   Voices chord %d strum %d   Ring %s  Rel %s   Arp %s %s",
             transport,
             chordVoiceLimit,
             strumVoiceLimit,
             ringNames[ringMode],
             releaseNames[releaseMode],
             arpPatternNames[arpPattern],
             arpRateNames[arpRate]);

    display.setControlText(chordLabel, chordText);
    display.setControlText(notesLabel, notesText);
    display.setControlText(detailLabel, detailText);
    display.setControlText(hintLabel, hintText);
    display.setControlText(jogLabel, jogText);

    display.refreshControl(chordLabel);
    display.refreshControl(notesLabel);
    display.refreshControl(detailLabel);
    display.refreshControl(hintLabel);
    display.refreshControl(jogLabel);
}
