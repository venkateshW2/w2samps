#pragma once
#include <juce_gui_basics/juce_gui_basics.h>
#include <juce_audio_processors/juce_audio_processors.h>
#include "PluginProcessor.h"
#include "WaveformDisplay.h"
#include "FuncGen.h"

//==============================================================================
// FuncGenCanvas — drawable Catmull-Rom curve editor for one FuncGen.
//
// Click anywhere on canvas   → add control point
// Drag a control point       → move it (sort order maintained)
// Double-click a point       → delete it
//==============================================================================
class FuncGenCanvas : public juce::Component
{
public:
    std::function<void()> onChange;

    void setFuncGen (FuncGen* fg) { funcGen_ = fg; repaint(); }
    FuncGen* getFuncGen() const   { return funcGen_; }

    /** Call from timer with current FG phase [0,1] — draws a vertical playhead. */
    void setPlayhead (float phase)
    {
        playheadPhase_ = phase;
        repaint();
    }

    void paint (juce::Graphics& g) override
    {
        auto b  = getLocalBounds();
        int  bw = b.getWidth(), bh = b.getHeight();

        // Dark background
        g.setColour (juce::Colour (0xff1C1C1E));
        g.fillRoundedRectangle (b.toFloat(), 3.0f);

        // Subtle grid (4 columns, 2 rows)
        g.setColour (juce::Colour (0xff3A3A3C));
        for (int i = 1; i < 4; ++i)
            g.drawVerticalLine   (b.getX() + bw * i / 4, (float) b.getY(), (float) b.getBottom());
        g.drawHorizontalLine (b.getCentreY(), (float) b.getX(), (float) b.getRight());

        if (funcGen_ == nullptr) { g.drawRect (b, 1); return; }

        // Curve (sample LUT across full width)
        juce::Path curve;
        bool started = false;
        for (int px = 0; px < bw; ++px)
        {
            float t   = (float) px / (float) std::max (1, bw - 1);
            float val = funcGen_->evaluate (t);
            float fy  = (float) b.getBottom() - val * (float) bh;
            if (!started) { curve.startNewSubPath ((float)(b.getX() + px), fy); started = true; }
            else          { curve.lineTo           ((float)(b.getX() + px), fy); }
        }
        g.setColour (juce::Colour (0xff30D158));  // kActive green
        g.strokePath (curve, juce::PathStrokeType (1.5f));

        // Control points
        const auto& pts = funcGen_->getPoints();
        for (int i = 0; i < (int) pts.size(); ++i)
        {
            float px = (float) b.getX() + pts[(size_t)i].x * (float) bw;
            float py = (float) b.getBottom() - pts[(size_t)i].y * (float) bh;
            bool sel = (i == selectedPt_);
            g.setColour (sel ? juce::Colours::white : juce::Colour (0xff30D158));
            g.fillEllipse (px - 5.f, py - 5.f, 10.f, 10.f);
            g.setColour (juce::Colour (0xff1C1C1E));
            g.drawEllipse (px - 5.f, py - 5.f, 10.f, 10.f, 1.0f);
        }

        // Playhead — vertical line at current phase position
        if (playheadPhase_ >= 0.f)
        {
            int phX = b.getX() + (int) (playheadPhase_ * (float) bw);
            g.setColour (juce::Colour (0xffFFD60A).withAlpha (0.75f));  // gold
            g.drawVerticalLine (phX, (float) b.getY(), (float) b.getBottom());
            // Small triangle indicator at top
            juce::Path tri;
            tri.addTriangle ((float) phX, (float) b.getY(),
                             (float) phX - 4.f, (float) b.getY() - 6.f,
                             (float) phX + 4.f, (float) b.getY() - 6.f);
            g.fillPath (tri);
        }

        g.setColour (juce::Colour (0xff636366));
        g.drawRoundedRectangle (b.toFloat().reduced (0.5f), 3.0f, 1.0f);
    }

    void mouseDown (const juce::MouseEvent& e) override
    {
        if (funcGen_ == nullptr) return;
        selectedPt_ = findNearest (e.x, e.y);
        if (selectedPt_ == -1)
        {
            float nx = (float) e.x / (float) std::max (1, getWidth());
            float ny = 1.0f - (float) e.y / (float) std::max (1, getHeight());
            funcGen_->addPoint (nx, ny);
            selectedPt_ = findNearest (e.x, e.y);
            if (onChange) onChange();
        }
        repaint();
    }

    void mouseDrag (const juce::MouseEvent& e) override
    {
        if (funcGen_ == nullptr || selectedPt_ == -1) return;
        float nx = (float) e.x / (float) std::max (1, getWidth());
        float ny = 1.0f - (float) e.y / (float) std::max (1, getHeight());
        funcGen_->movePoint (selectedPt_, nx, ny);
        selectedPt_ = findNearest (e.x, e.y);
        if (onChange) onChange();
        repaint();
    }

    void mouseUp (const juce::MouseEvent&) override { selectedPt_ = -1; repaint(); }

    void mouseDoubleClick (const juce::MouseEvent& e) override
    {
        if (funcGen_ == nullptr) return;
        int idx = findNearest (e.x, e.y);
        if (idx != -1)
        {
            funcGen_->removePoint (idx);
            selectedPt_ = -1;
            if (onChange) onChange();
            repaint();
        }
    }

private:
    int findNearest (int mx, int my) const
    {
        if (funcGen_ == nullptr) return -1;
        constexpr int kR = 12;
        const auto& pts = funcGen_->getPoints();
        for (int i = 0; i < (int) pts.size(); ++i)
        {
            int px = (int) (pts[(size_t)i].x * getWidth());
            int py = getHeight() - (int) (pts[(size_t)i].y * getHeight());
            int dx = mx - px, dy = my - py;
            if (dx*dx + dy*dy < kR*kR) return i;
        }
        return -1;
    }

    FuncGen* funcGen_      = nullptr;
    int      selectedPt_   = -1;
    float    playheadPhase_ = -1.f;  // -1 = hidden
};

//==============================================================================
// W2LookAndFeel — dark theme
//==============================================================================
class W2LookAndFeel : public juce::LookAndFeel_V4
{
public:
    // Dark theme colours
    static constexpr uint32_t kBg       = 0xff1C1C1E;  // near-black bg
    static constexpr uint32_t kPanel    = 0xff2C2C2E;  // raised panel
    static constexpr uint32_t kElevated = 0xff3A3A3C;  // button bg
    static constexpr uint32_t kActive   = 0xff30D158;  // green highlight (on/active)
    static constexpr uint32_t kText     = 0xffF2F2F7;  // near-white text
    static constexpr uint32_t kTextDim  = 0xff8E8E93;  // dimmed text
    static constexpr uint32_t kTrack    = 0xff48484A;  // slider track bg
    static constexpr uint32_t kAccent   = 0xff636366;  // borders, separators
    static constexpr uint32_t kMute     = 0xffFF453A;  // mute red
    static constexpr uint32_t kSolo     = 0xffFFD60A;  // solo yellow
    // Per-voice accent colours
    static constexpr uint32_t kV0       = 0xff0A84FF;  // V1 blue
    static constexpr uint32_t kV1       = 0xffFF9F0A;  // V2 amber
    static constexpr uint32_t kV2       = 0xffBF5AF2;  // V3 purple

    W2LookAndFeel()
    {
        setColour (juce::ResizableWindow::backgroundColourId, juce::Colour(kBg));
        setColour (juce::Label::textColourId,                 juce::Colour(kText));
        setColour (juce::Slider::backgroundColourId,          juce::Colour(kTrack));
        setColour (juce::Slider::trackColourId,               juce::Colour(kActive));
        setColour (juce::Slider::thumbColourId,               juce::Colour(kText));
        setColour (juce::TextButton::buttonColourId,          juce::Colour(kElevated));
        setColour (juce::TextButton::buttonOnColourId,        juce::Colour(kActive));
        setColour (juce::TextButton::textColourOffId,         juce::Colour(kText));
        setColour (juce::TextButton::textColourOnId,          juce::Colour(kBg));
    }

    void drawLinearSlider (juce::Graphics& g, int x, int y, int w, int h,
                           float sliderPos, float /*minSliderPos*/, float /*maxSliderPos*/,
                           juce::Slider::SliderStyle style, juce::Slider& slider) override
    {
        // BPM drag-number display — no track, no thumb
        if (slider.getName() == "bpm_drag")
        {
            auto b = juce::Rectangle<float> ((float)x, (float)y, (float)w, (float)h);
            g.setColour (juce::Colour (kElevated));
            g.fillRoundedRectangle (b, 5.0f);
            g.setColour (juce::Colour (kActive));
            g.drawRoundedRectangle (b.reduced (0.5f), 5.0f, 1.0f);
            g.setColour (juce::Colour (kText));
            g.setFont (juce::Font (juce::FontOptions{}.withName ("Menlo").withHeight (15.0f)));
            g.drawText (juce::String ((int)slider.getValue()),
                        (int)b.getX(), (int)b.getY(), (int)b.getWidth(), (int)b.getHeight(),
                        juce::Justification::centred);
            return;
        }

        bool horiz = (style == juce::Slider::LinearHorizontal ||
                      style == juce::Slider::LinearBar);
        bool vert  = (style == juce::Slider::LinearVertical);

        const float trackH = 2.0f;
        float cx = x + w * 0.5f;
        float cy = y + h * 0.5f;

        g.setColour (slider.findColour (juce::Slider::backgroundColourId));
        if (horiz)
            g.fillRoundedRectangle ((float)x, cy - trackH/2, (float)w, trackH, trackH/2);
        else if (vert)
            g.fillRoundedRectangle (cx - trackH/2, (float)y, trackH, (float)h, trackH/2);

        g.setColour (slider.findColour (juce::Slider::trackColourId));
        if (horiz)
            g.fillRoundedRectangle ((float)x, cy - trackH/2, sliderPos - (float)x, trackH, trackH/2);
        else if (vert)
        {
            float fillH = (float)(y + h) - sliderPos;
            g.fillRoundedRectangle (cx - trackH/2, sliderPos, trackH, fillH, trackH/2);
        }

        const float thumbR = 5.0f;
        g.setColour (slider.findColour (juce::Slider::thumbColourId));
        if (horiz)
            g.fillEllipse (sliderPos - thumbR, cy - thumbR, thumbR*2, thumbR*2);
        else if (vert)
            g.fillEllipse (cx - thumbR, sliderPos - thumbR, thumbR*2, thumbR*2);
    }

    void drawButtonBackground (juce::Graphics& g, juce::Button& btn,
                                const juce::Colour& bgColour,
                                bool highlighted, bool down) override
    {
        auto b = btn.getLocalBounds().toFloat().reduced (0.5f);
        bool toggled = btn.getToggleState() || down;

        // bgColour is buttonColourId — check if app has set a custom fill
        bool hasFill = toggled
            || (bgColour.getARGB() != (uint32_t)kElevated
                && bgColour.getARGB() != (uint32_t)kBg
                && bgColour.getARGB() != (uint32_t)kPanel
                && bgColour.getAlpha() > 0);

        if (hasFill)
        {
            juce::Colour fillCol = toggled ? juce::Colour (kActive) : bgColour;
            g.setColour (fillCol);
            g.fillRoundedRectangle (b, 4.0f);
        }
        else
        {
            // Outline-only when inactive
            if (highlighted)
            {
                g.setColour (juce::Colour (kActive).withAlpha (0.10f));
                g.fillRoundedRectangle (b, 4.0f);
            }
            g.setColour (juce::Colour (highlighted ? kActive : kAccent).withAlpha (0.6f));
            g.drawRoundedRectangle (b, 4.0f, 1.0f);
        }
    }

    void drawButtonText (juce::Graphics& g, juce::TextButton& btn,
                         bool /*highlighted*/, bool /*down*/) override
    {
        auto col = btn.findColour (juce::TextButton::buttonColourId);
        bool hasFill = btn.getToggleState()
            || (col.getARGB() != (uint32_t)kElevated
                && col.getARGB() != (uint32_t)kBg
                && col.getARGB() != (uint32_t)kPanel
                && col.getAlpha() > 0);

        // On a filled button use dark text, on outline use bright text
        g.setColour (hasFill ? juce::Colour (kBg) : juce::Colour (kText));
        g.setFont (juce::Font (juce::FontOptions{}.withName ("Menlo").withHeight (10.5f)));
        g.drawText (btn.getButtonText(), btn.getLocalBounds().reduced (2, 0),
                    juce::Justification::centred, true);
    }

    juce::Font getLabelFont (juce::Label&) override
    {
        return juce::Font (juce::FontOptions{}.withName("Menlo").withHeight(11.0f));
    }
};

//==============================================================================
/**
 * TimelineView — scrollable multi-lane macro-envelope editor.
 *
 * 8 lanes (one per TimelineEnv). Each lane has:
 *   - Left sidebar (120px): ON/OFF button, LOOP button, duration label (editable),
 *     +Dest button, Clear button, destination chips showing V1→Pitch etc.
 *   - Right content: FuncGenCanvas (click to add/drag points, double-click to delete),
 *     gold playhead, green progress fill.
 * Lanes scroll vertically inside a Viewport when they exceed the visible area.
 */
class TimelineView : public juce::Component
{
public:
    //==========================================================================
    // Inner scrollable content — holds all per-lane child components
    struct LanesContent : public juce::Component
    {
        void paint (juce::Graphics& g) override
        {
            auto b = getLocalBounds();
            g.setColour (juce::Colour (0xff1C1C1E));
            g.fillRect (b);
            // Draw lane separators (each 120px tall)
            g.setColour (juce::Colour (0xff3A3A3C));
            for (int i = 1; i <= 8; ++i)
                g.drawHorizontalLine (i * 120 - 1, 0.f, (float)b.getWidth());
            // Sidebar background (120px wide)
            g.setColour (juce::Colour (0xff242426));
            g.fillRect (0, 0, 120, b.getHeight());
            // Sidebar right border
            g.setColour (juce::Colour (0xff48484A));
            g.drawVerticalLine (120, 0.f, (float)b.getHeight());
        }
    };

    //==========================================================================
    TimelineView()
    {
        lanesViewport_.setScrollBarsShown (true, false);
        lanesViewport_.setViewedComponent (&lanesContent_, false);
        addAndMakeVisible (lanesViewport_);
    }

    void setProcessor (W2SamplerProcessor* p)
    {
        proc_ = p;
        buildLaneControls();
        resized();
        repaint();
    }

    void refreshPlayheads()
    {
        // Refresh loop/active button colours and repaint lanes
        if (proc_)
        {
            for (int i = 0; i < 8; ++i)
            {
                bool act  = proc_->getTimeline(i).isActive();
                bool loop = proc_->getTimeline(i).looping.load (std::memory_order_relaxed);
                activeBtn_[i].setColour (juce::TextButton::buttonColourId,
                    juce::Colour (act ? 0xff30D158u : 0xff3A3A3Cu));
                loopBtn_[i].setColour (juce::TextButton::buttonColourId,
                    juce::Colour (loop ? 0xff30D158u : 0xff3A3A3Cu));
            }
        }
        lanesContent_.repaint();
    }

    //==========================================================================
    void resized() override
    {
        auto b = getLocalBounds();

        // Ruler row at top
        zoomSlider_.setBounds (b.getRight() - 100, b.getY() + 3, 96, kRulerH - 6);

        // Viewport fills rest below ruler
        lanesViewport_.setBounds (b.getX(), b.getY() + kRulerH,
                                  b.getWidth(), b.getHeight() - kRulerH);

        // Content size: always 8 lanes tall, full width
        lanesContent_.setSize (b.getWidth(), 8 * kLaneH);

        // Per-lane children layout (coordinates relative to lanesContent_)
        const int cw = b.getWidth() - kSideW - 2;   // curve area width
        for (int i = 0; i < 8; ++i)
        {
            int ly = i * kLaneH;
            // Sidebar controls
            activeBtn_[i].setBounds   (3,        ly + 3,  46, 20);
            loopBtn_[i].setBounds     (53,       ly + 3,  46, 20);
            durationLabel_[i].setBounds (3,      ly + 27, 110, 18);
            addDestBtn_[i].setBounds  (3,        ly + kLaneH - 46, 54, 18);
            clearDestBtn_[i].setBounds (61,      ly + kLaneH - 46, 52, 18);
            // Curve canvas — takes up the right content area, leaving 22px at bottom for playhead label
            fgCanvas_[i].setBounds (kSideW + 2, ly + 3, cw - 4, kLaneH - 28);
        }
    }

    //==========================================================================
    void paint (juce::Graphics& g) override
    {
        auto b = getLocalBounds();

        // Overall background
        g.setColour (juce::Colour (0xff1C1C1E));
        g.fillRect (b);

        // ── Ruler bar ────────────────────────────────────────────────────────
        {
            juce::Rectangle<int> ruler (b.getX(), b.getY(), b.getWidth(), kRulerH);
            g.setColour (juce::Colour (0xff2C2C2E));
            g.fillRect (ruler);
            g.setColour (juce::Colour (0xff48484A));
            g.drawHorizontalLine (ruler.getBottom(), 0.f, (float)b.getWidth());

            // "TIMELINE" label in sidebar zone
            g.setColour (juce::Colour (0xff8E8E93));
            g.setFont (juce::Font (juce::FontOptions{}.withName("Menlo").withHeight(10.0f)));
            g.drawText ("TIMELINE", ruler.getX() + 4, ruler.getY(),
                        kSideW - 4, kRulerH, juce::Justification::centredLeft);

            // Tick marks in curve zone
            const int curveX = b.getX() + kSideW + 2;
            float maxSec  = getMaxDurationSec();
            float tickStep = getTickStep();
            g.setColour (juce::Colour (0xff636366));
            g.setFont (juce::Font (juce::FontOptions{}.withName("Menlo").withHeight(9.0f)));
            for (float t = 0.f; t <= maxSec + tickStep * 0.5f; t += tickStep)
            {
                int tx = curveX + (int)(t * kDefaultPxPerSec);
                if (tx > b.getRight()) break;
                bool major = (std::fmod (t, tickStep * 2.f) < 0.01f) || tickStep >= 10.f;
                g.setColour (major ? juce::Colour (0xff636366) : juce::Colour (0xff3A3A3C));
                g.drawVerticalLine (tx, (float)ruler.getY() + (major ? 4 : 10), (float)ruler.getBottom());
                if (major)
                {
                    g.setColour (juce::Colour (0xff8E8E93));
                    g.drawText (formatTime (t), tx + 2, ruler.getY() + 3, 40, 14,
                                juce::Justification::centredLeft);
                }
            }
        }

        // ── Per-lane overlays (drawn on top of lanesContent_ area) ────────────
        // (Lane background is in lanesContent_.paint; we draw destination chips and
        //  playheads here because they need to account for viewport scroll offset.)
        if (proc_ == nullptr) return;

        int scrollY = lanesViewport_.getViewPositionY();
        const int curveX = b.getX() + kSideW + 2;
        const int curveW = b.getWidth() - kSideW - 2;

        for (int i = 0; i < 8; ++i)
        {
            auto& tl = proc_->getTimeline (i);
            // Lane y in this component's coordinate space
            int laneTopInContent = i * kLaneH;
            int ly = b.getY() + kRulerH + laneTopInContent - scrollY;
            if (ly + kLaneH < b.getY() + kRulerH) continue;   // scrolled above
            if (ly >= b.getBottom()) break;                    // scrolled below

            bool  active  = tl.isActive();
            float phase   = tl.getCurrentPhase();
            float dur     = tl.durationSec.load (std::memory_order_relaxed);

            // Sidebar background per-lane (alt shading)
            g.setColour (i % 2 == 0 ? juce::Colour (0xff242426) : juce::Colour (0xff222224));
            g.fillRect (b.getX(), ly, kSideW, kLaneH - 1);

            // Lane number
            g.setColour (juce::Colour (active ? 0xffF2F2F7u : 0xff636366u));
            g.setFont (juce::Font (juce::FontOptions{}.withName("Menlo").withHeight(10.0f)));
            g.drawText ("TL" + juce::String(i + 1), b.getX() + 3, ly + 50, 40, 14,
                        juce::Justification::centredLeft);

            // Destination chips — bottom of sidebar
            static const char* destNames[] = { "None","Pitch","Att","Dec","Sus","Rel",
                                               "FltHz","FltQ","Drive","RvbMix","RvbSz","LoopMs" };
            static const char* vCols[] = { "V1", "V2", "V3" };
            const auto& dests = tl.getPendingDests();
            int chipY = ly + kLaneH - 24;
            int chipX = b.getX() + 3;
            for (int d = 0; d < (int)dests.size() && d < 4; ++d)
            {
                int v  = dests[(size_t)d].voice;
                int dt = dests[(size_t)d].dest;
                juce::String label = (v >= 0 && v < 3 && dt >= 0 && dt < 12)
                    ? (juce::String(vCols[v]) + " " + juce::String(destNames[dt]))
                    : "?";
                int chipW = juce::jmin (kSideW - 6, 108);
                g.setColour (juce::Colour (0xff3A3A3C));
                g.fillRoundedRectangle ((float)chipX, (float)chipY, (float)chipW, 14.f, 3.f);
                // Voice colour accent bar
                const uint32_t vcol[] = { 0xff0A84FF, 0xffFF9F0A, 0xffBF5AF2 };
                if (v >= 0 && v < 3)
                {
                    g.setColour (juce::Colour (vcol[v]));
                    g.fillRect (chipX, chipY, 3, 14);
                }
                g.setColour (juce::Colour (0xffF2F2F7));
                g.setFont (juce::Font (juce::FontOptions{}.withName("Menlo").withHeight(9.0f)));
                g.drawText (label, chipX + 5, chipY, chipW - 5, 14, juce::Justification::centredLeft);
                chipX += chipW + 2;
                if (chipX + 40 > b.getX() + kSideW) { chipY -= 16; chipX = b.getX() + 3; }
            }

            if (!active) continue;

            // ── Playhead in curve area ────────────────────────────────────────
            int playheadX = curveX + (int)(phase * (float)curveW);
            g.setColour (juce::Colour (0xff30D158).withAlpha (0.07f));
            g.fillRect (curveX, ly, (int)(phase * curveW), kLaneH - 1);
            g.setColour (juce::Colour (0xffFFD60A).withAlpha (0.9f));
            g.drawVerticalLine (playheadX, (float)ly, (float)(ly + kLaneH - 25));

            // Duration indicator — right edge of full duration
            g.setColour (juce::Colour (0xff636366));
            g.drawVerticalLine (curveX + curveW - 1, (float)ly, (float)(ly + kLaneH - 1));

            // Phase label
            g.setColour (juce::Colour (0xff8E8E93));
            g.setFont (juce::Font (juce::FontOptions{}.withName("Menlo").withHeight(8.5f)));
            float elapsedSec = phase * dur;
            g.drawText (formatTime(elapsedSec) + " / " + formatTime(dur),
                        curveX + 4, ly + kLaneH - 24, 140, 14,
                        juce::Justification::centredLeft);
        }
    }

    //==========================================================================
    void mouseDown (const juce::MouseEvent& e) override
    {
        // Double-click on a duration label area to edit duration
        (void) e;
    }

private:
    static constexpr int   kRulerH        = 28;
    static constexpr int   kLaneH         = 120;
    static constexpr int   kSideW         = 120;
    static constexpr float kDefaultPxPerSec = 3.0f;

    W2SamplerProcessor* proc_ = nullptr;

    // Scrollable lane area
    juce::Viewport  lanesViewport_;
    LanesContent    lanesContent_;

    // Zoom slider (in ruler row)
    juce::Slider    zoomSlider_;

    // Per-lane components (children of lanesContent_)
    juce::TextButton activeBtn_[8];
    juce::TextButton loopBtn_[8];
    juce::TextButton addDestBtn_[8];
    juce::TextButton clearDestBtn_[8];
    juce::Label      durationLabel_[8];
    FuncGenCanvas    fgCanvas_[8];

    //==========================================================================
    void buildLaneControls()
    {
        // Zoom slider
        zoomSlider_.setSliderStyle (juce::Slider::LinearHorizontal);
        zoomSlider_.setTextBoxStyle (juce::Slider::NoTextBox, true, 0, 0);
        zoomSlider_.setRange (0.2, 8.0, 0.0);
        zoomSlider_.setValue (1.0, juce::dontSendNotification);
        zoomSlider_.setColour (juce::Slider::backgroundColourId, juce::Colour (0xff48484A));
        zoomSlider_.setColour (juce::Slider::trackColourId,      juce::Colour (0xff30D158));
        zoomSlider_.setColour (juce::Slider::thumbColourId,      juce::Colour (0xffF2F2F7));
        zoomSlider_.setPopupDisplayEnabled (false, false, nullptr);
        zoomSlider_.onValueChange = [this] { repaint(); };
        addAndMakeVisible (zoomSlider_);

        for (int i = 0; i < 8; ++i)
        {
            auto& tl = proc_->getTimeline (i);

            // Active toggle — "ON" or "OFF"
            bool act = tl.isActive();
            activeBtn_[i].setButtonText (act ? "ON" : "OFF");
            activeBtn_[i].setColour (juce::TextButton::buttonColourId,
                juce::Colour (act ? 0xff30D158u : 0xff3A3A3Cu));
            activeBtn_[i].setColour (juce::TextButton::textColourOffId, juce::Colour (0xffF2F2F7));
            activeBtn_[i].onClick = [this, i] {
                if (!proc_) return;
                bool now = !proc_->getTimeline(i).isActive();
                proc_->getTimeline(i).setActive (now);
                activeBtn_[i].setButtonText (now ? "ON" : "OFF");
                activeBtn_[i].setColour (juce::TextButton::buttonColourId,
                    juce::Colour (now ? 0xff30D158u : 0xff3A3A3Cu));
                repaint();
            };
            lanesContent_.addAndMakeVisible (activeBtn_[i]);

            // Loop toggle
            bool loop = tl.looping.load (std::memory_order_relaxed);
            loopBtn_[i].setButtonText ("LOOP");
            loopBtn_[i].setColour (juce::TextButton::buttonColourId,
                juce::Colour (loop ? 0xff30D158u : 0xff3A3A3Cu));
            loopBtn_[i].setColour (juce::TextButton::textColourOffId, juce::Colour (0xffF2F2F7));
            loopBtn_[i].onClick = [this, i] {
                if (!proc_) return;
                bool now = !proc_->getTimeline(i).looping.load (std::memory_order_relaxed);
                proc_->getTimeline(i).looping.store (now, std::memory_order_relaxed);
                loopBtn_[i].setColour (juce::TextButton::buttonColourId,
                    juce::Colour (now ? 0xff30D158u : 0xff3A3A3Cu));
                repaint();
            };
            lanesContent_.addAndMakeVisible (loopBtn_[i]);

            // Duration label — shows "60s", double-click to edit
            float dur = tl.durationSec.load (std::memory_order_relaxed);
            durationLabel_[i].setText (formatDurForEdit (dur), juce::dontSendNotification);
            durationLabel_[i].setFont (juce::Font (juce::FontOptions{}.withName("Menlo").withHeight(11.0f)));
            durationLabel_[i].setColour (juce::Label::textColourId,       juce::Colour (0xffF2F2F7));
            durationLabel_[i].setColour (juce::Label::backgroundColourId, juce::Colour (0xff3A3A3C));
            durationLabel_[i].setColour (juce::Label::textWhenEditingColourId, juce::Colour (0xffF2F2F7));
            durationLabel_[i].setColour (juce::Label::backgroundWhenEditingColourId, juce::Colour (0xff1C1C1E));
            durationLabel_[i].setJustificationType (juce::Justification::centred);
            durationLabel_[i].setEditable (false, true, false); // double-click to edit
            durationLabel_[i].onTextChange = [this, i] {
                if (!proc_) return;
                float sec = parseDuration (durationLabel_[i].getText());
                if (sec > 0.f)
                    proc_->getTimeline(i).durationSec.store (sec, std::memory_order_relaxed);
            };
            lanesContent_.addAndMakeVisible (durationLabel_[i]);

            // + Dest button
            addDestBtn_[i].setButtonText ("+ Dest");
            addDestBtn_[i].setColour (juce::TextButton::buttonColourId, juce::Colour (0xff3A3A3Cu));
            addDestBtn_[i].setColour (juce::TextButton::textColourOffId, juce::Colour (0xffF2F2F7));
            addDestBtn_[i].onClick = [this, i] { showAddDestMenu (i); };
            lanesContent_.addAndMakeVisible (addDestBtn_[i]);

            // Clear dests button
            clearDestBtn_[i].setButtonText ("Clear");
            clearDestBtn_[i].setColour (juce::TextButton::buttonColourId, juce::Colour (0xff3A3A3Cu));
            clearDestBtn_[i].setColour (juce::TextButton::textColourOffId, juce::Colour (0xffF2F2F7));
            clearDestBtn_[i].onClick = [this, i] {
                if (proc_) { proc_->getTimeline(i).clearDests(); repaint(); }
            };
            lanesContent_.addAndMakeVisible (clearDestBtn_[i]);

            // FuncGenCanvas — linked to the timeline's curve
            fgCanvas_[i].setFuncGen (&tl.curve);
            fgCanvas_[i].onChange = [this] { repaint(); };
            lanesContent_.addAndMakeVisible (fgCanvas_[i]);
        }
    }

    float getMaxDurationSec() const
    {
        float mx = 30.f;
        if (proc_)
            for (int i = 0; i < 8; ++i)
                mx = std::max (mx, proc_->getTimeline(i).durationSec.load (std::memory_order_relaxed));
        return mx;
    }

    float getTickStep() const
    {
        const float steps[] = { 1.f, 2.f, 5.f, 10.f, 15.f, 30.f, 60.f, 120.f, 300.f };
        for (float s : steps)
            if (s * kDefaultPxPerSec >= 30.f) return s;
        return 60.f;
    }

    static juce::String formatTime (float secs)
    {
        if (secs < 60.f) return juce::String ((int)secs) + "s";
        int m = (int)(secs / 60.f);
        int s = (int)std::fmod (secs, 60.f);
        return juce::String(m) + "m" + (s > 0 ? juce::String(s) + "s" : "");
    }

    static juce::String formatDurForEdit (float secs)
    {
        // e.g. "60s" or "2m30s" — used in duration label
        if (secs < 60.f) return juce::String ((int)secs) + "s";
        int m = (int)(secs / 60.f);
        int s = (int)std::fmod (secs, 60.f);
        return juce::String(m) + "m" + juce::String(s) + "s";
    }

    // Parse "60s", "2m", "2m30s", "90" (bare number = seconds)
    static float parseDuration (const juce::String& text)
    {
        juce::String t = text.trim().toLowerCase();
        float total = 0.f;
        // parse minutes
        int mIdx = t.indexOfChar ('m');
        if (mIdx >= 0)
        {
            total += t.substring (0, mIdx).getFloatValue() * 60.f;
            t = t.substring (mIdx + 1);
        }
        // parse seconds
        int sIdx = t.indexOfChar ('s');
        if (sIdx >= 0) total += t.substring (0, sIdx).getFloatValue();
        else if (t.isNotEmpty()) total += t.getFloatValue();
        return total > 0.f ? total : -1.f;
    }

    void showAddDestMenu (int lane)
    {
        if (!proc_) return;
        static const char* destNames[] = { "None","Pitch","Attack","Decay","Sustain","Release",
                                           "Filter Hz","Filter Q","Drive","Reverb Mix","Reverb Size","Loop Ms" };
        juce::PopupMenu menu;
        for (int v = 0; v < 3; ++v)
        {
            juce::PopupMenu sub;
            for (int d = 1; d < 12; ++d)
                sub.addItem (v * 100 + d, juce::String(destNames[d]));
            menu.addSubMenu ("Voice " + juce::String(v + 1), sub);
        }
        menu.showMenuAsync (juce::PopupMenu::Options{}, [this, lane] (int result) {
            if (!proc_ || result <= 0) return;
            int v = result / 100;
            int d = result % 100;
            if (v < 0 || v > 2 || d < 1 || d > 11) return;
            TimelineDest td;
            td.voice = v; td.dest = d;
            td.depth = 1.0f; td.min = 0.0f; td.max = 1.0f;
            proc_->getTimeline(lane).addDest (td);
            repaint();
        });
    }

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (TimelineView)
};

//==============================================================================
/**
 * W2SamplerEditor — resizable (940×720 default), dark theme.
 *
 * Layout:
 *   y=0..44         Transport bar (full width)
 *   y=44..H-90      Content
 *     x=0..320      Left column — voice selector + accordion voice controls
 *     x=320..W-120  Center — concentric rings
 *     x=W-120..W    Right column — master meter + gain fader
 *   y=H-90..H       Bottom bar — FX/Presets for selected voice
 */
class W2SamplerEditor : public juce::AudioProcessorEditor,
                        public juce::Timer
{
public:
    explicit W2SamplerEditor (W2SamplerProcessor&);
    ~W2SamplerEditor() override;

    void paint    (juce::Graphics&) override;
    void resized  () override;
    void timerCallback() override;
    void mouseDown (const juce::MouseEvent&) override;

private:
    W2SamplerProcessor& proc;

    // LookAndFeel instance — must outlive all child components
    W2LookAndFeel laf_;

    // Mirror LookAndFeel colour constants as local aliases
    static constexpr uint32_t kBg       = W2LookAndFeel::kBg;
    static constexpr uint32_t kPanel    = W2LookAndFeel::kPanel;
    static constexpr uint32_t kElevated = W2LookAndFeel::kElevated;
    static constexpr uint32_t kActive   = W2LookAndFeel::kActive;
    static constexpr uint32_t kText     = W2LookAndFeel::kText;
    static constexpr uint32_t kTextDim  = W2LookAndFeel::kTextDim;
    static constexpr uint32_t kTrack    = W2LookAndFeel::kTrack;
    static constexpr uint32_t kAccent   = W2LookAndFeel::kAccent;
    static constexpr uint32_t kMute     = W2LookAndFeel::kMute;
    static constexpr uint32_t kSolo     = W2LookAndFeel::kSolo;
    static constexpr uint32_t kV0       = W2LookAndFeel::kV0;
    static constexpr uint32_t kV1       = W2LookAndFeel::kV1;
    static constexpr uint32_t kV2       = W2LookAndFeel::kV2;

    // Fixed layout constants
    static constexpr int kTransportH = 38;
    static constexpr int kLeftW      = 320;
    static constexpr int kRightW     = 120;
    static constexpr int kBottomH    = 90;

    // Selected voice
    int selectedVoice = 0;

    // Ring geometry (computed in resized)
    float ringCX_ = 0.0f, ringCY_ = 0.0f;
    float ringR_[3] = { 175.0f, 120.0f, 65.0f };

    // Scrollable left panel
    juce::Viewport   leftViewport_;
    juce::Component  leftContent_;

    //==========================================================================
    // Transport bar (y=0..38)
    juce::TextButton  playBtn    { "Play" };
    juce::Label       bpmLabel   { "", "BPM" };
    juce::Slider      bpmSlider;
    static constexpr int kNumClkDivs = 4;
    static constexpr int kClkDivVals[kNumClkDivs] = { 1, 2, 4, 8 };
    juce::TextButton  clkDivBtns[kNumClkDivs];
    juce::TextButton  muteBtn[3];
    juce::TextButton  soloBtn[3];

    // Voice selector row (top of left column)
    juce::TextButton  voiceSelectBtn[3];

    // Right column master controls
    juce::Rectangle<int> masterColumnRect_;
    juce::Slider      masterGainSlider;

    // Timeline view (toggles with center rings)
    bool         showTimeline_ = false;
    juce::TextButton tlToggleBtn { "TL" };
    TimelineView timelineView_;

    //==========================================================================
    // Per-voice UI (all three built; only selectedVoice shown in left panel)
    struct VoiceUI
    {
        // ── Section accordion (left panel: SAMPLE/SEQ/PHASE/SOUND/MOD; FX/PRESETS bottom) ──
        // 0=SAMPLE 1=SEQUENCE 2=PHASE 3=SOUND 4=FX/PRESETS(bottom) 5=MODULATION
        juce::TextButton sectionBtn[6];
        bool sectionOpen[6] = { true, true, false, false, false, false };

        // Nav row
        juce::TextButton loadBtn  { "Load" };
        juce::TextButton prevBtn  { "< Prev" };
        juce::TextButton nextBtn  { "Next >" };
        juce::TextButton rndBtn   { "Rnd" };
        juce::Label      nameLabel;

        // Onset sensitivity
        juce::Slider     onsetSensSlider;
        juce::Label      onsetSensLabel { "", "Sens" };

        // Waveform
        WaveformDisplay  waveform;

        // Sequence section
        juce::Slider     stepsSlider, hitsSlider, rotSlider;
        juce::Label      stepsLabel { "", "Steps" };
        juce::Label      hitsLabel  { "", "Hits" };
        juce::Label      rotLabel   { "", "Rot" };
        juce::TextButton smpAdvBtn  { "Hold" };
        juce::TextButton loopModeBtn { "Off" };
        juce::Slider     loopMsSlider;
        juce::Label      loopMsLabel { "", "Loop ms" };
        juce::TextButton loopLockBtn { "Lock" };
        juce::TextButton freezeBtn   { "Freeze" };

        // Phase section
        juce::Slider     offsetSlider, warpSlider, quantSlider;
        juce::Label      offsetLabel { "", "Offset" };
        juce::Label      warpLabel   { "", "Warp" };
        juce::Label      quantLabel  { "", "Grid" };
        juce::TextButton revBtn      { "Rev: Off" };
        juce::TextButton phaseSrcBtn { "Master" };
        juce::TextButton ratePresetBtns[kNumRatePresets];
        juce::Slider     rateSlider;
        juce::Label      rateLabel { "", "Rate" };

        // Sound section (linear sliders)
        juce::Slider     pitchSlider, attSlider, decSlider, susSlider, relSlider;
        juce::Label      pitchLabel { "", "Pitch" };
        juce::Label      attLabel   { "", "Attack" };
        juce::Label      decLabel   { "", "Decay" };
        juce::Label      susLabel   { "", "Sustain" };
        juce::Label      relLabel   { "", "Release" };
        juce::Slider     fFreqSlider, fResSlider, driveSlider, rvbMixSlider, rvbSzSlider;
        juce::Label      fFreqLabel  { "", "Filter Hz" };
        juce::Label      fResLabel   { "", "Filter Q" };
        juce::Label      driveLabel  { "", "Drive" };
        juce::Label      rvbMixLabel { "", "Rvb Mix" };
        juce::Label      rvbSzLabel  { "", "Rvb Size" };
        juce::Slider     gainSlider, preGainSlider, limitSlider;
        juce::Label      gainLabel     { "", "Level" };
        juce::Label      preGainLabel  { "", "Pre Gain" };
        juce::Label      limitLabel    { "", "Limit dB" };
        juce::Slider     smoothSlider;
        juce::Label      smoothLabel  { "", "Smooth ms" };

        // ── Modulation section (section 5, left panel) ───────────────────────
        static constexpr int kNumFg = 4;
        FuncGenCanvas    fgCanvas[kNumFg];
        juce::TextButton fgSyncBtn[kNumFg];   // SYNC / FREE toggle
        juce::Slider     fgRateSlider[kNumFg]; // continuous rate (mult or Hz)
        juce::Label      fgRateLabel[kNumFg];
        juce::ComboBox   fgDestBox[kNumFg];    // dropdown for mod destination
        juce::Slider     fgDepthSlider[kNumFg];
        juce::Slider     fgMinSlider[kNumFg];
        juce::Slider     fgMaxSlider[kNumFg];
        juce::Label      fgDepthLabel[kNumFg];
        juce::Label      fgMinLabel[kNumFg];
        juce::Label      fgMaxLabel[kNumFg];

        // ── Mod indicator bars (one per ModDest, shown below SOUND sliders) ──
        // A thin accent-coloured strip below each modulated slider.
        // Width = destModNorm × slider width. Hidden when no FG targets that dest.
        struct ModBar : juce::Component
        {
            float normValue = 0.f;
            juce::Colour barColour;
            void paint (juce::Graphics& g) override
            {
                if (normValue < 0.001f) return;
                g.setColour (barColour.withAlpha (0.85f));
                g.fillRect (0, 0, (int)(normValue * (float)getWidth()), getHeight());
                // Dim fill for remaining range
                g.setColour (barColour.withAlpha (0.15f));
                g.fillRect ((int)(normValue * (float)getWidth()), 0,
                            getWidth() - (int)(normValue * (float)getWidth()), getHeight());
            }
        };
        ModBar destModBars[kNumModDests];

        // FX/Presets (shown in bottom bar)
        bool             rndLocked[10] = {};
        juce::TextButton rndLockBtns[10];
        juce::Slider     rndFxSlider;
        juce::Label      rndFxLabel  { "", "Rnd FX%" };
        juce::TextButton rndFxFireBtn { "Rnd FX Now" };
        juce::TextButton resetFxBtn   { "Reset FX" };
        juce::TextButton presetBtns[8];
        juce::TextButton presetSaveBtn { "Save" };
        bool             presetSaveMode = false;
    };

    VoiceUI voiceUI[3];

    std::shared_ptr<juce::FileChooser> fileChooser_;

    //==========================================================================
    void buildTransportBar();
    void buildVoiceUI   (int v);
    void layoutTransportBar();
    void layoutMasterColumn();
    void layoutVoicePanel   (int v);
    void layoutBottomBar    (int v);
    void hideVoiceAll       ();

    void styleSlider    (juce::Slider& s, float mn, float mx, float def, bool isInt = false);
    void styleButton    (juce::TextButton& b);
    void styleLabel     (juce::Label& l, bool bright = false);

    void syncVoiceFromParams  (int v);
    void updateCycleBtns      (int v);

    void drawRings         (juce::Graphics& g);
    void drawMasterColumn  (juce::Graphics& g);

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (W2SamplerEditor)
};
