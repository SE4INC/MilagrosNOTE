#include "PluginEditor.h"
#include <algorithm>

namespace
{
    const juce::Colour bgColour    (0xff14161c);
    const juce::Colour panelColour (0xff1d2029);
    const juce::Colour accent      (0xffffb347);
    const juce::Colour cyan        (0xff7ad7f0);
    const juce::Colour chordColour (0xff3d4a6b);

    void setFloat (juce::AudioProcessorValueTreeState& s, const char* id, float v)
    {
        if (auto* p = dynamic_cast<juce::AudioParameterFloat*> (s.getParameter (id)))
        {
            p->beginChangeGesture();
            *p = v;
            p->endChangeGesture();
        }
    }

    void setChoice (juce::AudioProcessorValueTreeState& s, const char* id, int idx)
    {
        if (auto* p = dynamic_cast<juce::AudioParameterChoice*> (s.getParameter (id)))
        {
            p->beginChangeGesture();
            *p = idx;
            p->endChangeGesture();
        }
    }

    juce::String keyText (int root, bool minor)
    {
        return juce::String (afina::noteNames (root)) + (minor ? es (" menor") : es (" mayor"));
    }
}

//==============================================================================
// Pantalla de notas (tipo piano roll): bloques que se pueden seleccionar y mover
class MilagrosNoteEditor::PianoRoll : public juce::Component
{
public:
    explicit PianoRoll (MilagrosNoteProcessor& p) : proc (p) {}

    std::function<void()> onSelect;
    int getSelected() const { return selected; }

    void refresh()
    {
        if (proc.getNotesVersion() != lastVersion)
        {
            lastVersion = proc.getNotesVersion();
            selected = -1;
            if (onSelect)
                onSelect();
        }
        repaint();
    }

    void paint (juce::Graphics& g) override
    {
        const float W = (float) getWidth(), H = (float) getHeight();
        g.setColour (panelColour);
        g.fillRoundedRectangle (getLocalBounds().toFloat(), 8.0f);

        const auto& notes = proc.getNotes();
        const double sr = proc.getSampleRate();
        const double secPerFrame = (double) proc.getFrameSize() / sr;
        const double capturedSec = (double) proc.getCapturedFrames() * secPerFrame;
        if (autoFit)
        {
            viewStart = 0.0;
            viewLen = juce::jmax (8.0, capturedSec + 1.0);
        }

        if (! notes.empty())
        {
            float mn = 1000.0f, mx = -1000.0f;
            for (const auto& nt : notes)
            {
                mn = juce::jmin (mn, nt.medianMidi, nt.targetMidi);
                mx = juce::jmax (mx, nt.medianMidi, nt.targetMidi);
            }
            centerMidi += (0.5f * (mn + mx) - centerMidi) * 0.2f;
            spanMidi += (juce::jmax (16.0f, mx - mn + 8.0f) - spanMidi) * 0.2f;
        }

        const int root = (int) proc.apvts.getRawParameterValue ("key")->load();
        const int scale = (int) proc.apvts.getRawParameterValue ("scale")->load();

        // Rejilla de notas
        g.setFont (11.0f);
        const float lo = centerMidi - spanMidi * 0.5f;
        for (int n = (int) std::ceil (lo); n <= (int) std::floor (lo + spanMidi); ++n)
        {
            const float y = yOfMidi ((float) n);
            const bool allowed = afina::inScale (scale, root, ((n % 12) + 12) % 12);
            g.setColour (allowed ? juce::Colour (0x38ffffff) : juce::Colour (0x10ffffff));
            g.drawHorizontalLine ((int) y, 0.0f, W);
            if (allowed)
            {
                g.setColour (juce::Colours::lightgrey);
                g.drawText (afina::noteWithOctave (n), 4, (int) y - 13, 44, 13, juce::Justification::left);
            }
        }

        // Carril de acordes del beat
        {
            const double chopSec = (double) SharedChords::hopSamples / sr;
            const int c0 = juce::jmax (0, (int) std::floor (viewStart / chopSec));
            const int c1 = juce::jmin (SharedChords::maxFrames - 1, (int) std::ceil ((viewStart + viewLen) / chopSec));
            auto& sh = SharedChords::get();
            int runStart = c0, runId = -1;
            auto flush = [&] (int endC)
            {
                if (runId < 0)
                    return;
                const float x0 = xOfSec ((double) runStart * chopSec);
                const float x1 = xOfSec ((double) endC * chopSec);
                g.setColour (chordColour);
                g.fillRect (x0, 1.0f, juce::jmax (1.0f, x1 - x0 - 1.0f), laneH - 2.0f);
                if (x1 - x0 > 26.0f)
                {
                    g.setColour (juce::Colours::white);
                    g.drawText (notechords::chordName (runId), (int) x0 + 2, 1, (int) (x1 - x0), (int) laneH - 2,
                                juce::Justification::centredLeft);
                }
            };
            for (int c = c0; c <= c1; ++c)
            {
                int id = sh.chord[c].load();
                if (id < 0)
                    id = -1;
                if (id != runId)
                {
                    flush (c);
                    runStart = c;
                    runId = id;
                }
            }
            flush (c1 + 1);
        }

        // Contornos: tu voz (celeste) y la voz corregida (naranja)
        if (proc.getCapturedFrames() > 0)
        {
            const float refA = proc.getRefA();
            const int f0i = juce::jmax (0, (int) std::floor (viewStart / secPerFrame));
            const int f1i = juce::jmin (proc.getCapturedFrames() - 1, (int) std::ceil ((viewStart + viewLen) / secPerFrame));
            const int stride = juce::jmax (1, (f1i - f0i) / juce::jmax (1, getWidth()));
            juce::Path pIn, pOut;
            bool pen = false;
            for (int f = f0i; f <= f1i; f += stride)
            {
                const float hz = proc.getF0 (f);
                if (hz <= 0.0f)
                {
                    pen = false;
                    continue;
                }
                const float m = 69.0f + 12.0f * std::log2 (hz / refA);
                const float x = xOfSec ((double) f * secPerFrame);
                const float yIn = juce::jlimit (laneH, H, yOfMidi (m));
                const float yOut = juce::jlimit (laneH, H, yOfMidi (m + proc.getShift (f)));
                if (! pen)
                {
                    pIn.startNewSubPath (x, yIn);
                    pOut.startNewSubPath (x, yOut);
                    pen = true;
                }
                else
                {
                    pIn.lineTo (x, yIn);
                    pOut.lineTo (x, yOut);
                }
            }
            g.setColour (cyan.withAlpha (0.7f));
            g.strokePath (pIn, juce::PathStrokeType (1.2f));
            g.setColour (accent.withAlpha (0.9f));
            g.strokePath (pOut, juce::PathStrokeType (1.6f));
        }

        // Notas
        const float noteH = noteHeight();
        for (int i = 0; i < (int) notes.size(); ++i)
        {
            const auto& nt = notes[(size_t) i];
            const float x0 = xOfSec ((double) nt.startFrame * secPerFrame);
            const float x1 = xOfSec ((double) (nt.endFrame + 1) * secPerFrame);
            if (x1 < 0.0f || x0 > W)
                continue;
            const float w = juce::jmax (4.0f, x1 - x0);

            if (nt.edited)
            {
                g.setColour (juce::Colour (0x60ffffff));
                g.drawRoundedRectangle (x0, yOfMidi (nt.medianMidi) - noteH * 0.5f, w, noteH, 3.0f, 1.0f);
            }

            const juce::Rectangle<float> rc (x0, yOfMidi (nt.targetMidi) - noteH * 0.5f, w, noteH);
            g.setColour (nt.edited ? accent.withAlpha (0.85f) : cyan.withAlpha (0.55f));
            g.fillRoundedRectangle (rc, 3.0f);
            if (i == selected)
            {
                g.setColour (juce::Colours::white);
                g.drawRoundedRectangle (rc, 3.0f, 2.0f);
            }
            if (w > 34.0f)
            {
                g.setColour (juce::Colours::black);
                g.setFont (11.0f);
                g.drawText (afina::noteWithOctave ((int) std::lround (nt.targetMidi)), rc.toNearestInt(),
                            juce::Justification::centred);
            }
        }

        // Cursor de reproducción
        const float px = xOfSec (proc.getPositionSeconds());
        if (px >= 0.0f && px <= W)
        {
            g.setColour (juce::Colours::white.withAlpha (0.8f));
            g.drawVerticalLine ((int) px, laneH, H);
        }

        if (notes.empty())
        {
            g.setColour (juce::Colours::grey);
            g.setFont (14.0f);
            g.drawText (es ("Pulsa \"Capturar notas\" y reproduce tu voz una vez: aquí aparecerán tus notas."),
                        getLocalBounds().withTrimmedTop ((int) laneH), juce::Justification::centred);
        }
    }

    void mouseDown (const juce::MouseEvent& e) override
    {
        const int hit = hitTest (e.position);
        if (hit >= 0)
        {
            selected = hit;
            if (onSelect)
                onSelect();
            repaint();
            return;
        }
        dragging = true;
        dragStartX = e.position.x;
        dragStartView = viewStart;
        autoFit = false;
    }

    void mouseDrag (const juce::MouseEvent& e) override
    {
        if (! dragging)
            return;
        viewStart = dragStartView - (double) (e.position.x - dragStartX) / (double) juce::jmax (1, getWidth()) * viewLen;
        viewStart = juce::jmax (-1.0, viewStart);
        repaint();
    }

    void mouseUp (const juce::MouseEvent&) override { dragging = false; }

    void mouseDoubleClick (const juce::MouseEvent& e) override
    {
        if (hitTest (e.position) < 0)
        {
            autoFit = true;   // doble clic en el fondo = ver todo
            repaint();
        }
    }

    void mouseWheelMove (const juce::MouseEvent& e, const juce::MouseWheelDetails& wheel) override
    {
        autoFit = false;
        const double pivot = secOfX (e.position.x);
        const double factor = wheel.deltaY > 0.0f ? 0.8 : 1.25;
        viewLen = juce::jlimit (1.0, 900.0, viewLen * factor);
        viewStart = juce::jmax (-1.0, pivot - (double) e.position.x / (double) juce::jmax (1, getWidth()) * viewLen);
        repaint();
    }

private:
    static constexpr float laneH = 20.0f;

    float xOfSec (double sec) const { return (float) ((sec - viewStart) / viewLen * (double) getWidth()); }
    double secOfX (float x) const { return viewStart + (double) x / (double) juce::jmax (1, getWidth()) * viewLen; }
    float yOfMidi (float midi) const
    {
        const float lo = centerMidi - spanMidi * 0.5f;
        return laneH + ((float) getHeight() - laneH) * (1.0f - (midi - lo) / spanMidi);
    }
    float noteHeight() const { return juce::jmax (7.0f, ((float) getHeight() - laneH) / spanMidi * 0.8f); }

    int hitTest (juce::Point<float> p) const
    {
        const auto& notes = proc.getNotes();
        const double secPerFrame = (double) proc.getFrameSize() / proc.getSampleRate();
        const float noteH = noteHeight();
        for (int i = (int) notes.size() - 1; i >= 0; --i)
        {
            const auto& nt = notes[(size_t) i];
            const float x0 = xOfSec ((double) nt.startFrame * secPerFrame);
            const float x1 = xOfSec ((double) (nt.endFrame + 1) * secPerFrame);
            const juce::Rectangle<float> rc (x0, yOfMidi (nt.targetMidi) - noteH * 0.5f, juce::jmax (4.0f, x1 - x0), noteH);
            if (rc.expanded (2.0f).contains (p))
                return i;
        }
        return -1;
    }

    MilagrosNoteProcessor& proc;
    double viewStart = 0.0, viewLen = 8.0, dragStartView = 0.0;
    float dragStartX = 0.0f, centerMidi = 60.0f, spanMidi = 24.0f;
    bool autoFit = true, dragging = false;
    int selected = -1, lastVersion = -1;
};

//==============================================================================
// PÁGINA 1: notas
class MilagrosNoteEditor::MainPage : public juce::Component
{
public:
    explicit MainPage (MilagrosNoteProcessor& p) : proc (p), roll (p)
    {
        setupCombo (modeBox, modeAtt, "mode");
        setupCombo (keyBox, keyAtt, "key");
        setupCombo (scaleBox, scaleAtt, "scale");
        setupCombo (guideBox, guideAtt, "guide");

        captureBtn.setButtonText (es ("● Capturar notas"));
        captureBtn.setClickingTogglesState (true);
        captureBtn.setColour (juce::TextButton::buttonOnColourId, juce::Colour (0xffc0392b));
        autoBtn.setButtonText (es ("Corrección total (1 clic)"));
        restoreBtn.setButtonText (es ("Restaurar original"));
        beatKeyBtn.setButtonText (es ("Usar tonalidad del beat"));
        clearBtn.setButtonText (es ("Borrar captura"));
        for (auto* b : { &captureBtn, &autoBtn, &restoreBtn, &beatKeyBtn, &clearBtn })
            addAndMakeVisible (*b);

        upBtn.setButtonText (es ("↑ Subir 1"));
        downBtn.setButtonText (es ("↓ Bajar 1"));
        snapBtn.setButtonText (es ("Nota que pide el beat"));
        origBtn.setButtonText (es ("Original"));
        for (auto* b : { &upBtn, &downBtn, &snapBtn, &origBtn })
            addAndMakeVisible (*b);

        selLabel.setColour (juce::Label::textColourId, juce::Colours::white);
        selLabel.setJustificationType (juce::Justification::centredLeft);
        addAndMakeVisible (selLabel);
        status.setColour (juce::Label::textColourId, juce::Colours::lightgrey);
        status.setJustificationType (juce::Justification::centredLeft);
        addAndMakeVisible (status);
        addAndMakeVisible (roll);

        liveBtn.setButtonText (es ("Escucharme afinado al cantar"));
        liveBtn.setColour (juce::ToggleButton::textColourId, juce::Colours::white);
        liveBtn.setColour (juce::ToggleButton::tickColourId, accent);
        addAndMakeVisible (liveBtn);
        liveAtt = std::make_unique<juce::AudioProcessorValueTreeState::ButtonAttachment> (proc.apvts, "live", liveBtn);

        setupKnob (stabilize,  "stabilize",  es ("Estabilizar"));
        setupKnob (transition, "transition", es ("Transición"));
        setupKnob (refA,       "refA",       es ("Afinación base"));
        setupKnob (thresh,     "thresh",     es ("Umbral de voz"));
        stabilize.slider.setColour (juce::Slider::rotarySliderFillColourId, accent);
        stabilize.slider.onValueChange = [this]
        {
            proc.applyAllShifts ((float) stabilize.slider.getValue());
            roll.repaint();
        };

        roll.onSelect = [this] { updateSelection(); };

        captureBtn.onClick = [this]
        {
            const bool on = captureBtn.getToggleState();
            proc.setCapturing (on);
            captureBtn.setButtonText (on ? es ("■ Detener captura") : es ("● Capturar notas"));
            if (! on)
            {
                proc.rebuildNotes();
                roll.refresh();
            }
        };
        autoBtn.onClick = [this]
        {
            proc.autoCorrectAll();
            roll.repaint();
            updateSelection();
        };
        restoreBtn.onClick = [this]
        {
            proc.restoreAll();
            roll.repaint();
            updateSelection();
        };
        clearBtn.onClick = [this]
        {
            proc.clearCapture();
            captureBtn.setToggleState (false, juce::dontSendNotification);
            captureBtn.setButtonText (es ("● Capturar notas"));
            roll.refresh();
        };
        beatKeyBtn.onClick = [this]
        {
            const auto bc = proc.getBeatChroma();
            float sum = 0.0f;
            for (float v : bc)
                sum += v;
            if (sum > 1.0f)
            {
                const auto k = KeyAnalyzer::estimate (bc);
                if (k.root >= 0)
                {
                    setChoice (proc.apvts, "key", k.root);
                    setChoice (proc.apvts, "scale", k.minor ? 2 : 1);
                }
            }
        };
        upBtn.onClick = [this] { editSelected ([this] (int i) { proc.moveNote (i, +1); }); };
        downBtn.onClick = [this] { editSelected ([this] (int i) { proc.moveNote (i, -1); }); };
        snapBtn.onClick = [this] { editSelected ([this] (int i) { proc.snapNote (i); }); };
        origBtn.onClick = [this] { editSelected ([this] (int i) { proc.restoreNote (i); }); };

        updateSelection();
    }

    void refresh()
    {
        if (proc.isCapturing() && ++capTick >= 8)
        {
            capTick = 0;
            proc.rebuildNotes();
        }
        roll.refresh();

        const double capSec = (double) proc.getCapturedFrames() * (double) proc.getFrameSize() / proc.getSampleRate();
        juce::String s = proc.hasHostPosition() ? es ("Posición del tema: la da el DAW")
                                                : es ("Posición del tema: interna (el DAW no la informa)");
        s += es ("   ·   Capturado: ") + juce::String (capSec, 1) + es (" s   ·   Notas: ") + juce::String ((int) proc.getNotes().size());
        if (proc.isCapturing())
            s += es ("   ·   ● CAPTURANDO");
        if (status.getText() != s)
            status.setText (s, juce::dontSendNotification);
    }

    void paint (juce::Graphics& g) override
    {
        g.setColour (juce::Colours::grey);
        g.setFont (12.0f);
        g.drawText (es ("MilagrosNote v0.1 · gratis y de código abierto · rueda del ratón = zoom · arrastrar el fondo = mover · doble clic = ver todo"),
                    getLocalBounds().removeFromBottom (18), juce::Justification::centred);
    }

    void resized() override
    {
        auto r = getLocalBounds().reduced (10);
        r.removeFromBottom (18);

        auto row1 = r.removeFromTop (28);
        const int w4 = row1.getWidth() / 4;
        modeBox.setBounds (row1.removeFromLeft (w4).reduced (2, 0));
        keyBox.setBounds (row1.removeFromLeft (w4).reduced (2, 0));
        scaleBox.setBounds (row1.removeFromLeft (w4).reduced (2, 0));
        guideBox.setBounds (row1.reduced (2, 0));

        r.removeFromTop (6);
        auto row2 = r.removeFromTop (30);
        const int w5 = row2.getWidth() / 5;
        for (auto* b : { &captureBtn, &autoBtn, &restoreBtn, &beatKeyBtn, &clearBtn })
            b->setBounds (row2.removeFromLeft (w5).reduced (2, 0));

        r.removeFromTop (4);
        status.setBounds (r.removeFromTop (18));
        r.removeFromTop (4);

        auto bottom = r.removeFromBottom (170);
        roll.setBounds (r);

        bottom.removeFromTop (6);
        auto selRow = bottom.removeFromTop (30);
        selLabel.setBounds (selRow.removeFromLeft (selRow.getWidth() * 4 / 10));
        const int wb = selRow.getWidth() / 4;
        for (auto* b : { &upBtn, &downBtn, &snapBtn, &origBtn })
            b->setBounds (selRow.removeFromLeft (wb).reduced (2, 0));

        bottom.removeFromTop (4);
        const int wk = bottom.getWidth() / 5;
        for (auto* k : { &stabilize, &transition, &refA, &thresh })
        {
            auto cell = bottom.removeFromLeft (wk).reduced (4);
            k->label.setBounds (cell.removeFromTop (20));
            k->slider.setBounds (cell);
        }
        liveBtn.setBounds (bottom.reduced (4, 40));
    }

private:
    struct Knob
    {
        juce::Slider slider { juce::Slider::RotaryHorizontalVerticalDrag, juce::Slider::TextBoxBelow };
        juce::Label label;
        std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> attachment;
    };

    void setupKnob (Knob& k, const char* id, const juce::String& text)
    {
        k.slider.setTextBoxStyle (juce::Slider::TextBoxBelow, false, 90, 18);
        k.slider.setColour (juce::Slider::rotarySliderFillColourId, cyan);
        k.label.setText (text, juce::dontSendNotification);
        k.label.setJustificationType (juce::Justification::centred);
        k.label.setColour (juce::Label::textColourId, juce::Colours::white);
        addAndMakeVisible (k.slider);
        addAndMakeVisible (k.label);
        k.attachment = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment> (proc.apvts, id, k.slider);
    }

    void setupCombo (juce::ComboBox& box,
                     std::unique_ptr<juce::AudioProcessorValueTreeState::ComboBoxAttachment>& att,
                     const char* id)
    {
        if (auto* p = dynamic_cast<juce::AudioParameterChoice*> (proc.apvts.getParameter (id)))
            box.addItemList (p->choices, 1);
        addAndMakeVisible (box);
        att = std::make_unique<juce::AudioProcessorValueTreeState::ComboBoxAttachment> (proc.apvts, id, box);
    }

    template <typename Fn>
    void editSelected (Fn fn)
    {
        const int sel = roll.getSelected();
        if (sel < 0 || sel >= (int) proc.getNotes().size())
            return;
        fn (sel);
        roll.repaint();
        updateSelection();
    }

    void updateSelection()
    {
        const int sel = roll.getSelected();
        const auto& notes = proc.getNotes();
        const bool ok = sel >= 0 && sel < (int) notes.size();
        for (auto* b : { &upBtn, &downBtn, &snapBtn, &origBtn })
            b->setEnabled (ok);

        if (! ok)
        {
            selLabel.setText (es ("Haz clic en una nota para moverla."), juce::dontSendNotification);
            return;
        }
        const auto& nt = notes[(size_t) sel];
        const int chord = proc.chordForNote (nt);
        const int sug = proc.suggestTarget (nt);
        const auto name = [] (float m) { return afina::noteWithOctave ((int) std::lround (m)); };

        selLabel.setText (es ("Nota: ") + name (nt.medianMidi) + es (" → ") + name (nt.targetMidi)
                              + es ("   ·   Acorde del beat: ") + (chord >= 0 ? notechords::chordName (chord) : es ("no detectado"))
                              + es ("   ·   El beat pide: ") + afina::noteWithOctave (sug),
                          juce::dontSendNotification);
    }

    MilagrosNoteProcessor& proc;
    PianoRoll roll;
    juce::ComboBox modeBox, keyBox, scaleBox, guideBox;
    juce::TextButton captureBtn, autoBtn, restoreBtn, beatKeyBtn, clearBtn, upBtn, downBtn, snapBtn, origBtn;
    juce::ToggleButton liveBtn;
    juce::Label selLabel, status;
    Knob stabilize, transition, refA, thresh;
    int capTick = 0;
    std::unique_ptr<juce::AudioProcessorValueTreeState::ComboBoxAttachment> modeAtt, keyAtt, scaleAtt, guideAtt;
    std::unique_ptr<juce::AudioProcessorValueTreeState::ButtonAttachment> liveAtt;
};

//==============================================================================
// PÁGINA 2: hoja de consejos
namespace
{
    struct Advice
    {
        juce::String text;
        int voiceRoot = -1, beatRoot = -1;
        bool voiceMinor = false, beatMinor = false;
    };

    Advice buildAdvice (const MilagrosNoteProcessor& proc)
    {
        Advice r;
        juce::StringArray lines;
        const auto& notes = proc.getNotes();

        // Lo que se sabe del beat
        const auto bc = proc.getBeatChroma();
        float bsum = 0.0f;
        for (float v : bc)
            bsum += v;
        if (bsum > 1.0f)
        {
            const auto k = KeyAnalyzer::estimate (bc);
            if (k.root >= 0)
            {
                r.beatRoot = k.root;
                r.beatMinor = k.minor;
            }
        }

        juce::String chordsTxt;
        {
            std::array<int, 24> cnt {};
            auto& sh = SharedChords::get();
            const int mf = juce::jmin (sh.maxFrame.load(), SharedChords::maxFrames - 1);
            for (int c = 0; c <= mf; ++c)
            {
                const int id = sh.chord[c].load();
                if (id >= 0 && id < 24)
                    ++cnt[(size_t) id];
            }
            for (int k = 0; k < 4; ++k)
            {
                int best = -1, bv = 0;
                for (int i = 0; i < 24; ++i)
                {
                    if (cnt[(size_t) i] > bv)
                    {
                        bv = cnt[(size_t) i];
                        best = i;
                    }
                }
                if (best < 0)
                    break;
                chordsTxt += (chordsTxt.isEmpty() ? juce::String() : juce::String (", ")) + notechords::chordName (best);
                cnt[(size_t) best] = 0;
            }
        }

        if (notes.empty())
        {
            lines.add (es ("CÓMO USARLO\n1. Pon MilagrosNote en la pista de tu voz.\n2. Pulsa \"Capturar notas\".\n"
                           "3. Reproduce tu voz (o canta) de principio a fin, una sola vez.\n"
                           "4. Pulsa \"Detener captura\": verás tus notas como bloques.\n"
                           "5. Pulsa \"Corrección total\", o haz clic en una nota y muévela.\n"
                           "6. Reproduce el tema: escucharás tu voz corregida."));
        }
        else
        {
            const int root = (int) proc.apvts.getRawParameterValue ("key")->load();
            const int scale = (int) proc.apvts.getRawParameterValue ("scale")->load();

            std::array<int, 128> hist {};
            std::array<float, 12> vchroma {};
            int totalFrames = 0, inTune = 0, outScale = 0, edited = 0;
            for (const auto& nt : notes)
            {
                const int len = nt.endFrame - nt.startFrame + 1;
                const int mi = juce::jlimit (0, 127, (int) std::lround (nt.medianMidi));
                hist[(size_t) mi] += len;
                vchroma[(size_t) (mi % 12)] += (float) len;
                totalFrames += len;

                const int nearest = afina::nearestAllowed (nt.medianMidi, scale, root);
                const float d = std::abs (nt.medianMidi - (float) nearest);
                if (d <= 0.25f)
                    ++inTune;
                if (d > 0.5f)
                    ++outScale;
                if (nt.edited)
                    ++edited;
            }
            auto pct = [&] (float p)
            {
                const int goal = (int) ((float) totalFrames * p);
                int acc = 0;
                for (int m = 0; m < 128; ++m)
                {
                    acc += hist[(size_t) m];
                    if (acc >= goal)
                        return m;
                }
                return 0;
            };

            lines.add (es ("• TUS NOTAS: se detectaron ") + juce::String ((int) notes.size()) + es (" notas (")
                       + juce::String ((int) edited) + es (" ya corregidas). Cantas entre ") + afina::noteWithOctave (pct (0.05f))
                       + es (" y ") + afina::noteWithOctave (pct (0.95f)) + es (", y tu nota más cómoda es ")
                       + afina::noteWithOctave (pct (0.5f)) + ".");

            const auto vk = KeyAnalyzer::estimate (vchroma);
            if (vk.root >= 0)
            {
                r.voiceRoot = vk.root;
                r.voiceMinor = vk.minor;
                lines.add (es ("• TONALIDAD DE TU VOZ: parece ") + keyText (vk.root, vk.minor)
                           + es (" (seguridad ") + juce::String (juce::jlimit (0, 100, juce::roundToInt (vk.score * 100.0f)))
                           + es (" %). La tonalidad son las notas \"permitidas\" de la canción."));
            }

            const float inPct = 100.0f * (float) inTune / (float) juce::jmax (1, (int) notes.size());
            lines.add (es ("• AFINACIÓN: el ") + juce::String (juce::roundToInt (inPct))
                       + es (" % de tus notas ya cae a menos de 25 centésimas de la nota correcta de la escala elegida (100 centésimas = un semitono). ")
                       + (outScale > 0 ? juce::String (outScale) + es (" notas quedan fuera de la escala. ") : es ("Todas están dentro de la escala. "))
                       + es ("Pulsa \"Corrección total\" para llevarlas a la nota correcta."));
        }

        // Beat y acordes
        if (r.beatRoot >= 0)
        {
            lines.add (es ("• BEAT: parece estar en ") + keyText (r.beatRoot, r.beatMinor)
                       + (chordsTxt.isNotEmpty() ? es (". Acordes más frecuentes: ") + chordsTxt : juce::String())
                       + es (". Pulsa \"Usar tonalidad del beat\" para que tu voz se ajuste a él. Los acordes son una aproximación: "
                             "con 808 muy fuertes o acordes complejos puede equivocarse."));
            if (r.voiceRoot >= 0)
            {
                const bool same = r.voiceRoot == r.beatRoot && r.voiceMinor == r.beatMinor;
                const bool relative = (! r.beatMinor && r.voiceMinor && r.voiceRoot == (r.beatRoot + 9) % 12)
                                   || (r.beatMinor && ! r.voiceMinor && r.voiceRoot == (r.beatRoot + 3) % 12);
                lines.add (same || relative ? es ("• Tu voz y el beat están en la misma familia de notas. ¡Buen encaje!")
                                            : es ("• Tu voz y el beat parecen estar en tonalidades distintas. Si suena raro, usa la tonalidad del beat "
                                                  "y la corrección total."));
            }
        }
        else
        {
            lines.add (es ("• BEAT: aún no se ha analizado. Dos formas: (1) envía el beat al sidechain de este plugin (si tu versión "
                           "de FL Studio lo permite), o (2) pon otra copia de MilagrosNote en el canal del beat, cámbiala al modo "
                           "\"Instrumental\" y reproduce el tema. Si no, elige la tonalidad a mano arriba."));
        }

        if (! proc.hasHostPosition())
            lines.add (es ("• AVISO: tu DAW no informa la posición del tema. Para que la corrección suene en el lugar correcto, "
                           "reproduce siempre desde el inicio del tema."));

        lines.add (es ("\nGLOSARIO\nNota (bloque): cada sonido de tu voz con una altura.\nTonalidad: las notas que \"suenan bien\" en la canción.\n"
                       "Estabilizar: cuánto se aplana el vibrato dentro de cada nota.\nTransición: qué tan suave es el paso de una nota a otra.\n"
                       "Mover una nota muchos semitonos puede sonar artificial: úsalo con moderación."));

        r.text = lines.joinIntoString ("\n\n");
        return r;
    }
}

class MilagrosNoteEditor::AdvisorPage : public juce::Component
{
public:
    explicit AdvisorPage (MilagrosNoteProcessor& p) : proc (p)
    {
        text.setMultiLine (true);
        text.setReadOnly (true);
        text.setCaretVisible (false);
        text.setColour (juce::TextEditor::backgroundColourId, panelColour);
        text.setColour (juce::TextEditor::textColourId, juce::Colours::white);
        text.setColour (juce::TextEditor::outlineColourId, juce::Colours::transparentBlack);
        addAndMakeVisible (text);

        useVoiceBtn.setButtonText (es ("Usar tonalidad de mi voz"));
        useBeatBtn.setButtonText (es ("Usar tonalidad del beat"));
        autoBtn.setButtonText (es ("Corrección total (1 clic)"));
        resetBeatBtn.setButtonText (es ("Reiniciar análisis del beat"));
        for (auto* b : { &useVoiceBtn, &useBeatBtn, &autoBtn, &resetBeatBtn })
            addAndMakeVisible (*b);

        useVoiceBtn.onClick = [this]
        {
            if (advice.voiceRoot >= 0)
            {
                setChoice (proc.apvts, "key", advice.voiceRoot);
                setChoice (proc.apvts, "scale", advice.voiceMinor ? 2 : 1);
            }
        };
        useBeatBtn.onClick = [this]
        {
            if (advice.beatRoot >= 0)
            {
                setChoice (proc.apvts, "key", advice.beatRoot);
                setChoice (proc.apvts, "scale", advice.beatMinor ? 2 : 1);
            }
        };
        autoBtn.onClick = [this] { proc.autoCorrectAll(); };
        resetBeatBtn.onClick = [this] { SharedChords::get().clear(); refresh(); };
        refresh();
    }

    void resized() override
    {
        auto r = getLocalBounds().reduced (12);
        auto row2 = r.removeFromBottom (34);
        r.removeFromBottom (6);
        auto row1 = r.removeFromBottom (34);
        r.removeFromBottom (8);

        useVoiceBtn.setBounds (row1.removeFromLeft (row1.getWidth() / 2 - 4));
        row1.removeFromLeft (8);
        useBeatBtn.setBounds (row1);
        autoBtn.setBounds (row2.removeFromLeft (row2.getWidth() / 2 - 4));
        row2.removeFromLeft (8);
        resetBeatBtn.setBounds (row2);
        text.setBounds (r);
    }

    void refresh()
    {
        advice = buildAdvice (proc);
        if (text.getText() != advice.text)
            text.setText (advice.text, false);
        useVoiceBtn.setEnabled (advice.voiceRoot >= 0);
        useBeatBtn.setEnabled (advice.beatRoot >= 0);
    }

private:
    MilagrosNoteProcessor& proc;
    juce::TextEditor text;
    juce::TextButton useVoiceBtn, useBeatBtn, autoBtn, resetBeatBtn;
    Advice advice;
};

//==============================================================================
MilagrosNoteEditor::MilagrosNoteEditor (MilagrosNoteProcessor& p)
    : AudioProcessorEditor (&p), proc (p)
{
    mainPage = std::make_unique<MainPage> (proc);
    advisorPage = std::make_unique<AdvisorPage> (proc);

    tabs.setTabBarDepth (32);
    tabs.setOutline (0);
    tabs.addTab (es ("Notas"), panelColour, mainPage.get(), false);
    tabs.addTab (es ("Asesor (hoja de consejos)"), panelColour, advisorPage.get(), false);
    addAndMakeVisible (tabs);

    setSize (960, 740);
    startTimerHz (20);
}

MilagrosNoteEditor::~MilagrosNoteEditor()
{
    stopTimer();
}

void MilagrosNoteEditor::paint (juce::Graphics& g)
{
    g.fillAll (bgColour);
}

void MilagrosNoteEditor::resized()
{
    tabs.setBounds (getLocalBounds());
}

void MilagrosNoteEditor::timerCallback()
{
    if (tabs.getCurrentTabIndex() == 0)
    {
        mainPage->refresh();
    }
    else if (++tick >= 10)
    {
        tick = 0;
        advisorPage->refresh();
    }
}
