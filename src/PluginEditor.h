#pragma once
#include <juce_gui_basics/juce_gui_basics.h>
#include <juce_audio_processors/juce_audio_processors.h>
#include "PluginProcessor.h"
#include "WaveformDisplay.h"
#include "FuncGen.h"
#include "SoundBrowser.h"

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

    /** Set the visible phase sub-range for DAW-style zoom. Default 0,1 = full range. */
    void setViewRange (float start, float end)
    {
        viewStart_ = std::max (0.f, start);
        viewEnd_   = std::min (1.f, std::max (viewStart_ + 0.001f, end));
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
            float t   = viewStart_ + (float) px / (float) std::max (1, bw - 1) * (viewEnd_ - viewStart_);
            float val = funcGen_->evaluateSmooth (t);
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
            float normInView = (pts[(size_t)i].x - viewStart_) / (viewEnd_ - viewStart_);
            if (normInView < -0.05f || normInView > 1.05f) continue;  // out of view
            float px = (float) b.getX() + normInView * (float) bw;
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
            float nx = viewStart_ + (float) e.x / (float) std::max (1, getWidth()) * (viewEnd_ - viewStart_);
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
        float nx = viewStart_ + (float) e.x / (float) std::max (1, getWidth()) * (viewEnd_ - viewStart_);
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
            int px = (int) ((pts[(size_t)i].x - viewStart_) / (viewEnd_ - viewStart_) * getWidth());
            int py = getHeight() - (int) (pts[(size_t)i].y * getHeight());
            int dx = mx - px, dy = my - py;
            if (dx*dx + dy*dy < kR*kR) return i;
        }
        return -1;
    }

    FuncGen* funcGen_      = nullptr;
    int      selectedPt_   = -1;
    float    playheadPhase_ = -1.f;  // -1 = hidden
    float    viewStart_ = 0.f;   // visible phase range start [0,1]
    float    viewEnd_   = 1.f;   // visible phase range end   [0,1]
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
 * PlayheadOverlay — transparent overlay that draws the timeline playhead on top
 * of FuncGenCanvas. Uses setInterceptsMouseClicks(false,false) so all mouse
 * events pass through to the canvas beneath.
 */
class PlayheadOverlay : public juce::Component
{
public:
    PlayheadOverlay()  { setInterceptsMouseClicks (false, false); }

    void setPhase (float p) { phase_ = p; repaint(); }

    void paint (juce::Graphics& g) override
    {
        if (phase_ < 0.f) return;
        auto b = getLocalBounds();
        int x = (int) (phase_ * (float) b.getWidth());
        g.setColour (juce::Colour (0xffFFD60A).withAlpha (0.9f));
        g.drawVerticalLine (x, 0.f, (float) b.getHeight());
        // Triangle notch at top
        juce::Path tri;
        tri.addTriangle ((float)x, 0.f, (float)(x-4), -6.f, (float)(x+4), -6.f);
        g.fillPath (tri);
    }

private:
    float phase_ = -1.f;
};

//==============================================================================
/**
 * TimelineView — DAW-style fixed-window envelope editor.
 *
 * Fixed-width canvas; zoom changes the visible time range (not canvas size).
 * Ruler click/drag = seek. Canvas click/drag = curve edit.
 * Starts with 2 lanes; + Add Lane button adds up to 8.
 * Inspector strip at bottom shows selected lane's destinations with full editing.
 */
class TimelineView : public juce::Component
{
public:
    //==========================================================================
    struct LanesContent : public juce::Component
    {
        TimelineView* owner = nullptr;
        void paint (juce::Graphics& g) override
        {
            if (owner) owner->paintLanes (g, getLocalBounds());
        }
        void mouseDown  (const juce::MouseEvent& e) override
        {
            if (owner) owner->lanesMouseDown (e);
        }
        void mouseDrag  (const juce::MouseEvent& e) override
        {
            if (owner) owner->lanesMouseDrag (e);
        }
    };

    //==========================================================================
    TimelineView()
    {
        lanesVP_.setScrollBarsShown (true, false);   // vertical only (canvas is fixed width)
        lanesVP_.setViewedComponent (&lanesCt_, false);
        lanesCt_.owner = this;
        addAndMakeVisible (lanesVP_);
    }

    void setProcessor (W2SamplerProcessor* p)
    {
        proc_ = p;
        buildControls();
        updateViewRange();
        resized();
    }

    void refreshPlayheads()
    {
        if (!proc_) return;
        // Update overlays
        for (int i = 0; i < numLanes_; ++i)
        {
            auto& tl = proc_->getTimeline (i);
            float ph  = tl.getCurrentPhase();
            // Map timeline phase to visible canvas fraction
            float maxDur = getMaxDurSec();
            float tlDur  = tl.durationSec.load (std::memory_order_relaxed);
            // Phase of this lane in absolute seconds
            float absSec = ph * tlDur;
            // Map absSec to canvas fraction within the current view window
            float canvasFrac = (maxDur > 0.f) ? (absSec / maxDur) : 0.f;
            float viewFrac = (canvasFrac - viewStartFrac_) / viewRangeFrac_;
            phOverlay_[i].setPhase (juce::jlimit (0.f, 1.f, viewFrac));
        }
        lanesCt_.repaint();
    }

    //==========================================================================
    void resized() override
    {
        auto b  = getLocalBounds();
        int  bw = b.getWidth();
        int  bh = b.getHeight();

        // Header controls
        int hx = 110;
        addLaneBtn_.setBounds  (bw - 90, 4, 84, kHeaderH - 8);
        zoomOutBtn_.setBounds  (hx,       4, 24, kHeaderH - 8);  hx += 28;
        zoomInBtn_.setBounds   (hx,       4, 24, kHeaderH - 8);  hx += 28;
        fitBtn_.setBounds      (hx,       4, 30, kHeaderH - 8);  hx += 34;
        zoomSlider_.setBounds  (hx,       4, bw - 90 - hx - 8, kHeaderH - 8);

        // Inspector at bottom
        int inspH = (selectedLane_ >= 0) ? kInspH : 28;
        int vpH   = bh - kHeaderH - inspH;

        lanesVP_.setBounds (0, kHeaderH, bw, vpH);
        lanesCt_.setSize   (bw, kRulerH + numLanes_ * kLaneH + 30);

        layoutInspector (b.getX(), bh - inspH, bw, inspH);
        layoutLaneControls (bw);
    }

    //==========================================================================
    void paint (juce::Graphics& g) override
    {
        auto b = getLocalBounds();
        // Header background
        g.setColour (juce::Colour (0xff2C2C2E));
        g.fillRect (b.getX(), b.getY(), b.getWidth(), kHeaderH);
        g.setColour (juce::Colour (0xff48484A));
        g.drawHorizontalLine (kHeaderH, 0.f, (float) b.getWidth());
        // "TIMELINE" label
        g.setColour (juce::Colour (0xff8E8E93));
        g.setFont (juce::Font (juce::FontOptions{}.withName("Menlo").withHeight(10.5f)));
        g.drawText ("TIMELINE", 6, 0, 100, kHeaderH, juce::Justification::centredLeft);
        // Inspector background (when visible)
        if (selectedLane_ >= 0)
        {
            int inspY = b.getBottom() - kInspH;
            g.setColour (juce::Colour (0xff242426));
            g.fillRect (0, inspY, b.getWidth(), kInspH);
            g.setColour (juce::Colour (0xff48484A));
            g.drawHorizontalLine (inspY, 0.f, (float) b.getWidth());
        }
        else
        {
            int inspY = b.getBottom() - 28;
            g.setColour (juce::Colour (0xff1E1E20));
            g.fillRect (0, inspY, b.getWidth(), 28);
            g.setColour (juce::Colour (0xff48484A));
            g.drawHorizontalLine (inspY, 0.f, (float) b.getWidth());
            g.setColour (juce::Colour (0xff636366));
            g.setFont (juce::Font (juce::FontOptions{}.withName("Menlo").withHeight(9.5f)));
            g.drawText ("Click a lane sidebar to edit its destinations",
                        6, inspY, b.getWidth() - 12, 28, juce::Justification::centredLeft);
        }
    }

    //==========================================================================
    // Drawn inside LanesContent
    void paintLanes (juce::Graphics& g, juce::Rectangle<int> b)
    {
        if (!proc_) return;
        const int W  = b.getWidth();
        const int cX = kSideW;   // curve start x
        const int cW = W - kSideW;

        // Overall bg
        g.setColour (juce::Colour (0xff1A1A1C));
        g.fillRect (b);
        // Sidebar bg
        g.setColour (juce::Colour (0xff242426));
        g.fillRect (0, 0, kSideW, b.getHeight());
        g.setColour (juce::Colour (0xff3A3A3C));
        g.drawVerticalLine (cX, 0.f, (float) b.getHeight());

        float bpm       = proc_->bpm ? proc_->bpm->get() : 120.f;
        float secPerBeat = 60.f / bpm;
        float secPerBar  = secPerBeat * 4.f;
        float maxDur    = getMaxDurSec();
        float viewStart = viewStartFrac_ * maxDur;         // seconds
        float viewEnd   = (viewStartFrac_ + viewRangeFrac_) * maxDur;
        float pps       = (float) cW / (viewEnd - viewStart + 0.001f);  // pixels per second

        // ── Ruler ───────────────────────────────────────────────────────────
        {
            g.setColour (juce::Colour (0xff2C2C2E));
            g.fillRect (cX, 0, cW, kRulerH);
            g.setColour (juce::Colour (0xff3A3A3C));
            g.drawHorizontalLine (kRulerH, 0.f, (float) W);

            // Sec ticks
            float tickStep = getSecTickStep (pps);
            g.setFont (juce::Font (juce::FontOptions{}.withName("Menlo").withHeight(8.5f)));
            float tStart = std::floor (viewStart / tickStep) * tickStep;
            for (float t = tStart; t <= viewEnd + 0.001f; t += tickStep)
            {
                int tx = cX + (int)((t - viewStart) * pps);
                if (tx < cX || tx > cX + cW) continue;
                bool isBeat = (secPerBeat > 0.f && std::fmod(t + 0.001f, secPerBeat) < 0.01f);
                bool isBar  = (secPerBar  > 0.f && std::fmod(t + 0.001f, secPerBar)  < 0.01f);
                g.setColour (isBar  ? juce::Colour (0xff5A5A5C) :
                             isBeat ? juce::Colour (0xff4A4A4C) :
                                      juce::Colour (0xff3A3A3C));
                g.drawVerticalLine (tx, isBar ? 0.f : (float)(kRulerH / 2), (float) kRulerH);

                if (isBar || (pps * tickStep >= 30.f))
                {
                    g.setColour (isBar ? juce::Colour (0xff8E8E93) : juce::Colour (0xff636366));
                    juce::String label;
                    if (isBar && secPerBar > 0.f)
                        label = "|" + juce::String ((int)(t / secPerBar) + 1);
                    else
                        label = formatSec (t);
                    g.drawText (label, tx + 2, 2, 48, kRulerH - 4,
                                juce::Justification::centredLeft);
                }
            }
            // Sidebar ruler label
            g.setColour (juce::Colour (0xff636366));
            g.setFont (juce::Font (juce::FontOptions{}.withName("Menlo").withHeight(9.0f)));
            g.drawText ("ON  LOOP  SPD", 3, 2, kSideW - 4, kRulerH - 4,
                        juce::Justification::centredLeft);
        }

        // ── Lanes ────────────────────────────────────────────────────────────
        for (int i = 0; i < numLanes_; ++i)
        {
            auto& tl  = proc_->getTimeline (i);
            int   ly  = kRulerH + i * kLaneH;
            bool  act = tl.isActive();
            bool  sel = (i == selectedLane_);
            float dur = tl.durationSec.load (std::memory_order_relaxed);
            float rate = tl.rateMultiplier.load (std::memory_order_relaxed);

            // Lane bg
            g.setColour (i % 2 == 0 ? juce::Colour (0xff1E1E20) : juce::Colour (0xff1A1A1C));
            g.fillRect (cX, ly, cW, kLaneH);

            // Active/selected strip on left edge
            g.setColour (sel ? juce::Colour (0xffFFD60A) :
                         act ? juce::Colour (0xff30D158) :
                               juce::Colour (0xff3A3A3C));
            g.fillRect (0, ly, 4, kLaneH - 1);

            // Lane separator
            g.setColour (juce::Colour (0xff2C2C2E));
            g.drawHorizontalLine (ly + kLaneH - 1, 0.f, (float) W);

            // Lane number + dur + rate in sidebar
            g.setColour (juce::Colour (act ? 0xffF2F2F7u : 0xff636366u));
            g.setFont (juce::Font (juce::FontOptions{}.withName("Menlo").withHeight(10.5f)));
            g.drawText ("TL" + juce::String(i+1), 5, ly + 46, 32, 14,
                        juce::Justification::centredLeft);
            g.setColour (juce::Colour (0xff636366));
            g.setFont (juce::Font (juce::FontOptions{}.withName("Menlo").withHeight(8.5f)));
            g.drawText (formatSec(dur), 38, ly + 46, 56, 12,
                        juce::Justification::centredLeft);
            // Rate badge (if not 1x)
            if (std::abs(rate - 1.0f) > 0.05f)
            {
                g.setColour (juce::Colour (0xff30D158));
                g.drawText (juce::String(rate, 1) + "x", 5, ly + 62, kSideW - 10, 11,
                            juce::Justification::centredLeft);
            }

            // Dest count badge
            int ndests = (int) tl.getPendingDests().size();
            if (ndests > 0)
            {
                g.setColour (juce::Colour (0xff3A3A3C));
                g.fillRoundedRectangle (4.f, (float)(ly + 78), (float)(kSideW - 8), 14.f, 3.f);
                g.setColour (juce::Colour (0xffF2F2F7));
                g.setFont (juce::Font (juce::FontOptions{}.withName("Menlo").withHeight(9.0f)));
                g.drawText (juce::String(ndests) + " dest" + (ndests > 1 ? "s" : ""),
                            6, ly + 78, kSideW - 10, 14, juce::Justification::centredLeft);
            }

            if (!act) continue;

            // Curve grid
            g.setColour (juce::Colour (0xff242424));
            for (int row = 1; row < 4; ++row)
            {
                int gy = ly + row * (kLaneH / 4);
                g.drawHorizontalLine (gy, (float)cX, (float)(cX + cW));
            }

            // Progress fill
            float tlDurInView = juce::jlimit (0.f, 1.f, dur / (maxDur + 0.001f));
            float endFrac     = (tlDurInView - viewStartFrac_) / viewRangeFrac_;
            float phaseFrac   = tl.getCurrentPhase();
            float phAbsSec    = phaseFrac * dur;
            float phCanvasFrac = (phAbsSec / (maxDur + 0.001f) - viewStartFrac_) / viewRangeFrac_;
            int   playX = cX + juce::jlimit (0, cW, (int)(phCanvasFrac * cW));

            g.setColour (juce::Colour (0xff30D158).withAlpha (0.06f));
            int fillW = juce::jlimit (0, cW, playX - cX);
            g.fillRect (cX, ly, fillW, kLaneH - 1);

            // Duration end marker
            int endX = cX + juce::jlimit (0, cW, (int)(endFrac * cW));
            if (endX > cX && endX < cX + cW)
            {
                g.setColour (juce::Colour (0xff636366));
                g.drawVerticalLine (endX, (float)ly, (float)(ly + kLaneH - 1));
            }

            // Time label at bottom
            g.setColour (juce::Colour (0xff636366));
            g.setFont (juce::Font (juce::FontOptions{}.withName("Menlo").withHeight(8.5f)));
            g.drawText (formatSec (phAbsSec) + " / " + formatSec (dur),
                        cX + 4, ly + kLaneH - 16, 130, 13,
                        juce::Justification::centredLeft);
        }

        // Add-lane button row
        {
            int by = kRulerH + numLanes_ * kLaneH + 4;
            g.setColour (juce::Colour (0xff2C2C2E));
            g.fillRect (0, by - 2, W, 26);
        }
    }

    //==========================================================================
    void lanesMouseDown (const juce::MouseEvent& e)
    {
        if (!proc_) return;
        int y = e.y;

        // Ruler area → seek
        if (y < kRulerH)
        {
            int cX = kSideW, cW = lanesCt_.getWidth() - kSideW;
            if (e.x < cX) return;
            float frac = (float)(e.x - cX) / (float)std::max(1, cW);
            float seekFrac = viewStartFrac_ + frac * viewRangeFrac_;
            float maxDur = getMaxDurSec();
            float seekSec = seekFrac * maxDur;
            // Seek all active lanes to the same absolute time
            for (int i = 0; i < numLanes_; ++i)
            {
                auto& tl = proc_->getTimeline(i);
                float dur = tl.durationSec.load (std::memory_order_relaxed);
                if (dur > 0.f)
                    tl.seekToPhase (juce::jlimit (0.f, 1.f, seekSec / dur));
            }
            rulerDragging_ = true;
            repaint();
            return;
        }

        // Lane sidebar area → select lane
        if (e.x < kSideW)
        {
            int lane = (y - kRulerH) / kLaneH;
            if (lane >= 0 && lane < numLanes_)
            {
                selectedLane_ = (selectedLane_ == lane) ? -1 : lane;
                updateInspector();
                if (auto* parent = getParentComponent())
                    parent->resized();
                repaint();
                lanesCt_.repaint();
            }
        }
    }

    void lanesMouseDrag (const juce::MouseEvent& e)
    {
        if (!proc_ || !rulerDragging_) return;
        if (e.y >= kRulerH) { rulerDragging_ = false; return; }
        int cX = kSideW, cW = lanesCt_.getWidth() - kSideW;
        if (e.x < cX) return;
        float frac    = (float)(e.x - cX) / (float)std::max(1, cW);
        float seekFrac = viewStartFrac_ + frac * viewRangeFrac_;
        float maxDur  = getMaxDurSec();
        float seekSec = seekFrac * maxDur;
        for (int i = 0; i < numLanes_; ++i)
        {
            auto& tl = proc_->getTimeline(i);
            float dur = tl.durationSec.load (std::memory_order_relaxed);
            if (dur > 0.f)
                tl.seekToPhase (juce::jlimit (0.f, 1.f, seekSec / dur));
        }
    }

private:
    //==========================================================================
    static constexpr int   kHeaderH = 28;
    static constexpr int   kRulerH  = 24;
    static constexpr int   kLaneH   = 96;
    static constexpr int   kSideW   = 100;
    static constexpr int   kInspH   = 144;

    W2SamplerProcessor* proc_         = nullptr;
    int                 numLanes_     = 2;
    int                 selectedLane_ = -1;
    bool                rulerDragging_ = false;

    // DAW-style view window (fractions of max duration, 0..1)
    float viewStartFrac_ = 0.f;
    float viewRangeFrac_ = 1.f;   // 1.0 = show everything, 0.1 = show 10% of total

    juce::Viewport  lanesVP_;
    LanesContent    lanesCt_;

    // Header controls
    juce::TextButton addLaneBtn_ { "+ Lane" };
    juce::TextButton zoomInBtn_  { "+" };
    juce::TextButton zoomOutBtn_ { "-" };
    juce::TextButton fitBtn_     { "Fit" };
    juce::Slider     zoomSlider_;

    // Per-lane controls (children of lanesCt_)
    juce::TextButton activeBtn_[8];
    juce::TextButton loopBtn_[8];
    juce::Slider     rateSlider_[8];
    juce::Label      durationLabel_[8];
    FuncGenCanvas    fgCanvas_[8];
    PlayheadOverlay  phOverlay_[8];

    // Inspector (direct children of TimelineView)
    struct DestRow
    {
        juce::ComboBox   voiceBox;
        juce::ComboBox   destBox;
        juce::Slider     depthSlider;
        juce::Slider     minSlider;
        juce::Slider     maxSlider;
        juce::TextButton deleteBtn { "x" };
    };
    DestRow          destRows_[8];
    juce::TextButton addDestInspBtn_ { "+ Add Dest" };
    juce::Label      inspTitle_;
    juce::Label      inspDepthHdr_;
    juce::Label      inspMinHdr_;
    juce::Label      inspMaxHdr_;

    //==========================================================================
    float getMaxDurSec() const
    {
        float mx = 30.f;
        if (proc_)
            for (int i = 0; i < numLanes_; ++i)
                mx = std::max (mx, proc_->getTimeline(i).durationSec.load (std::memory_order_relaxed));
        return mx;
    }

    void updateViewRange()
    {
        // Clamp view start so end doesn't exceed 1.0
        if (viewStartFrac_ + viewRangeFrac_ > 1.0f)
            viewStartFrac_ = std::max (0.f, 1.0f - viewRangeFrac_);

        // Update FuncGenCanvas view ranges
        for (int i = 0; i < 8; ++i)
            fgCanvas_[i].setViewRange (viewStartFrac_, viewStartFrac_ + viewRangeFrac_);

        lanesCt_.repaint();
    }

    void layoutLaneControls (int totalW)
    {
        int cW = totalW - kSideW;
        for (int i = 0; i < 8; ++i)
        {
            int ly = kRulerH + i * kLaneH;
            bool vis = (i < numLanes_);
            activeBtn_[i].setVisible   (vis);
            loopBtn_[i].setVisible     (vis);
            rateSlider_[i].setVisible  (vis);
            durationLabel_[i].setVisible (vis);
            fgCanvas_[i].setVisible    (vis);
            phOverlay_[i].setVisible   (vis);

            if (!vis) continue;

            activeBtn_[i].setBounds    (3,        ly + 3,  44, 20);
            loopBtn_[i].setBounds      (51,       ly + 3,  44, 20);
            rateSlider_[i].setBounds   (3,        ly + 27, kSideW - 6, 15);
            durationLabel_[i].setBounds (3,       ly + 44, kSideW - 6, 16);
            fgCanvas_[i].setBounds     (kSideW+1, ly + 2,  cW - 2, kLaneH - 20);
            phOverlay_[i].setBounds    (kSideW+1, ly + 2,  cW - 2, kLaneH - 20);
        }
        // Add-lane button row is painted inside paintLanes — just repaint
        lanesCt_.repaint();
    }

    void layoutInspector (int x, int y, int w, int /*h*/)
    {
        if (selectedLane_ < 0)
            return;

        // Title + column header
        inspTitle_.setBounds (x + 4, y + 2, w - 50, 18);

        // Column positions
        // Layout: [4] [voice:46] [4] [dest:90] [4] [depth:slW] [4] [min:slW] [4] [max:slW] [4] [del:22] [4]
        // Fixed total: 4+46+4+90+4+4+4+4+22+4 = 186  →  slW = (w-186) / 3
        int rx  = x + 4;
        int slW = std::max (40, (w - 186) / 3);
        int slX = rx + 144;   // depth slider x
        // Column header labels — each centred over its slider column
        inspDepthHdr_.setBounds (slX,             y + 20, slW, 13);
        inspMinHdr_.setBounds   (slX + slW + 4,   y + 20, slW, 13);
        inspMaxHdr_.setBounds   (slX + 2*(slW+4), y + 20, slW, 13);

        const int rowH = 22;
        int iy = y + 36;   // below title + column header

        for (int j = 0; j < 8; ++j)
        {
            bool vis = (selectedLane_ >= 0 && proc_ &&
                        j < (int)proc_->getTimeline(selectedLane_).getPendingDests().size());
            destRows_[j].voiceBox.setVisible    (vis);
            destRows_[j].destBox.setVisible     (vis);
            destRows_[j].depthSlider.setVisible  (vis);
            destRows_[j].minSlider.setVisible    (vis);
            destRows_[j].maxSlider.setVisible    (vis);
            destRows_[j].deleteBtn.setVisible    (vis);
            if (!vis) continue;

            destRows_[j].voiceBox.setBounds    (rx,              iy + j*rowH, 46,  rowH - 3);
            destRows_[j].destBox.setBounds     (rx + 50,         iy + j*rowH, 90,  rowH - 3);
            destRows_[j].depthSlider.setBounds (slX,             iy + j*rowH, slW, rowH - 3);
            destRows_[j].minSlider.setBounds   (slX + slW + 4,   iy + j*rowH, slW, rowH - 3);
            destRows_[j].maxSlider.setBounds   (slX + 2*(slW+4), iy + j*rowH, slW, rowH - 3);
            destRows_[j].deleteBtn.setBounds   (x + w - 26,      iy + j*rowH, 22,  rowH - 3);
        }

        int nDests = (selectedLane_ >= 0 && proc_)
            ? (int)proc_->getTimeline(selectedLane_).getPendingDests().size() : 0;
        addDestInspBtn_.setVisible (selectedLane_ >= 0 && nDests < 8);
        if (selectedLane_ >= 0 && nDests < 8)
        {
            int addY2 = iy + nDests * rowH;
            addDestInspBtn_.setBounds (x + 4, addY2, 80, rowH - 3);
        }
    }

    //==========================================================================
    void buildControls()
    {
        // ── Header controls ───────────────────────────────────────────────────
        styleHeaderBtn (addLaneBtn_);
        addLaneBtn_.onClick = [this] {
            if (numLanes_ < 8) { numLanes_++; resized(); lanesCt_.repaint(); }
        };
        addAndMakeVisible (addLaneBtn_);

        styleHeaderBtn (zoomInBtn_);
        zoomInBtn_.onClick  = [this] { viewRangeFrac_ = std::max (0.05f, viewRangeFrac_ * 0.6f); updateViewRange(); };
        addAndMakeVisible (zoomInBtn_);

        styleHeaderBtn (zoomOutBtn_);
        zoomOutBtn_.onClick = [this] { viewRangeFrac_ = std::min (1.0f, viewRangeFrac_ / 0.6f); updateViewRange(); };
        addAndMakeVisible (zoomOutBtn_);

        styleHeaderBtn (fitBtn_);
        fitBtn_.onClick = [this] { viewStartFrac_ = 0.f; viewRangeFrac_ = 1.f; updateViewRange(); };
        addAndMakeVisible (fitBtn_);

        zoomSlider_.setSliderStyle (juce::Slider::LinearHorizontal);
        zoomSlider_.setTextBoxStyle (juce::Slider::NoTextBox, true, 0, 0);
        // Slider position represents viewRangeFrac (left=zoomed out/1.0, right=zoomed in/0.05)
        zoomSlider_.setRange (0.0, 1.0, 0.0);
        zoomSlider_.setValue (0.0, juce::dontSendNotification);
        zoomSlider_.setColour (juce::Slider::backgroundColourId, juce::Colour (0xff48484A));
        zoomSlider_.setColour (juce::Slider::trackColourId,      juce::Colour (0xff30D158));
        zoomSlider_.setColour (juce::Slider::thumbColourId,      juce::Colour (0xffF2F2F7));
        zoomSlider_.setPopupDisplayEnabled (false, false, nullptr);
        zoomSlider_.onValueChange = [this] {
            // Map slider 0..1 → viewRangeFrac_ 1.0..0.05 (inverted: left=all, right=zoomed in)
            float sl = (float) zoomSlider_.getValue();
            viewRangeFrac_ = 1.0f - sl * 0.95f;
            updateViewRange();
        };
        addAndMakeVisible (zoomSlider_);

        // ── Per-lane controls (added to lanesCt_) ─────────────────────────────
        for (int i = 0; i < 8; ++i)
        {
            auto& tl = proc_->getTimeline (i);

            // Active
            activeBtn_[i].setButtonText (tl.isActive() ? "ON" : "OFF");
            styleActivBtn (activeBtn_[i], tl.isActive());
            activeBtn_[i].onClick = [this, i] {
                if (!proc_) return;
                bool now = !proc_->getTimeline(i).isActive();
                proc_->getTimeline(i).setActive (now);
                activeBtn_[i].setButtonText (now ? "ON" : "OFF");
                styleActivBtn (activeBtn_[i], now);
                lanesCt_.repaint();
            };
            lanesCt_.addAndMakeVisible (activeBtn_[i]);

            // Loop
            loopBtn_[i].setButtonText ("LOOP");
            bool loop = tl.looping.load (std::memory_order_relaxed);
            styleActivBtn (loopBtn_[i], loop);
            loopBtn_[i].onClick = [this, i] {
                if (!proc_) return;
                bool now = !proc_->getTimeline(i).looping.load (std::memory_order_relaxed);
                proc_->getTimeline(i).looping.store (now, std::memory_order_relaxed);
                styleActivBtn (loopBtn_[i], now);
                lanesCt_.repaint();
            };
            lanesCt_.addAndMakeVisible (loopBtn_[i]);

            // Rate slider
            float rate = tl.rateMultiplier.load (std::memory_order_relaxed);
            rateSlider_[i].setSliderStyle (juce::Slider::LinearHorizontal);
            rateSlider_[i].setTextBoxStyle (juce::Slider::NoTextBox, true, 0, 0);
            rateSlider_[i].setRange (0.1, 10.0, 0.0);
            rateSlider_[i].setValue (rate, juce::dontSendNotification);
            rateSlider_[i].setDoubleClickReturnValue (true, 1.0);
            rateSlider_[i].setColour (juce::Slider::backgroundColourId, juce::Colour (0xff3A3A3C));
            rateSlider_[i].setColour (juce::Slider::trackColourId,      juce::Colour (0xff30D158));
            rateSlider_[i].setColour (juce::Slider::thumbColourId,      juce::Colour (0xffF2F2F7));
            rateSlider_[i].setPopupDisplayEnabled (true, true, nullptr);
            rateSlider_[i].onValueChange = [this, i] {
                if (proc_) proc_->getTimeline(i).rateMultiplier.store (
                    (float) rateSlider_[i].getValue(), std::memory_order_relaxed);
                lanesCt_.repaint();
            };
            lanesCt_.addAndMakeVisible (rateSlider_[i]);

            // Duration label
            float dur = tl.durationSec.load (std::memory_order_relaxed);
            durationLabel_[i].setText (formatSec(dur), juce::dontSendNotification);
            durationLabel_[i].setFont (juce::Font (juce::FontOptions{}.withName("Menlo").withHeight(9.5f)));
            durationLabel_[i].setColour (juce::Label::textColourId,       juce::Colour (0xffF2F2F7));
            durationLabel_[i].setColour (juce::Label::backgroundColourId, juce::Colour (0xff3A3A3C));
            durationLabel_[i].setColour (juce::Label::textWhenEditingColourId, juce::Colour (0xffF2F2F7));
            durationLabel_[i].setColour (juce::Label::backgroundWhenEditingColourId, juce::Colour (0xff1C1C1E));
            durationLabel_[i].setJustificationType (juce::Justification::centred);
            durationLabel_[i].setEditable (false, true, false);
            durationLabel_[i].onTextChange = [this, i] {
                if (!proc_) return;
                float sec = parseDuration (durationLabel_[i].getText());
                if (sec > 0.f)
                {
                    proc_->getTimeline(i).durationSec.store (sec, std::memory_order_relaxed);
                    durationLabel_[i].setText (formatSec(sec), juce::dontSendNotification);
                    lanesCt_.repaint();
                }
            };
            lanesCt_.addAndMakeVisible (durationLabel_[i]);

            // FuncGenCanvas
            fgCanvas_[i].setFuncGen (&tl.curve);
            fgCanvas_[i].onChange = [this] { lanesCt_.repaint(); };
            lanesCt_.addAndMakeVisible (fgCanvas_[i]);

            // PlayheadOverlay (added AFTER canvas so it's on top)
            lanesCt_.addAndMakeVisible (phOverlay_[i]);
        }

        // ── Inspector controls ─────────────────────────────────────────────────
        inspTitle_.setFont (juce::Font (juce::FontOptions{}.withName("Menlo").withHeight(10.0f)));
        inspTitle_.setColour (juce::Label::textColourId, juce::Colour (0xffF2F2F7));
        addAndMakeVisible (inspTitle_);

        auto styleHdr = [](juce::Label& lbl, const char* txt) {
            lbl.setText (txt, juce::dontSendNotification);
            lbl.setFont (juce::Font (juce::FontOptions{}.withName("Menlo").withHeight(8.5f)));
            lbl.setColour (juce::Label::textColourId, juce::Colour (0xff8E8E93));
            lbl.setJustificationType (juce::Justification::centred);
        };
        styleHdr (inspDepthHdr_, "Depth");
        styleHdr (inspMinHdr_,   "Min");
        styleHdr (inspMaxHdr_,   "Max");
        addAndMakeVisible (inspDepthHdr_);
        addAndMakeVisible (inspMinHdr_);
        addAndMakeVisible (inspMaxHdr_);

        static const char* destNames[] = {
            "None","Pitch","Attack","Decay","Sustain","Release",
            "Flt Hz","Flt Q","Drive","Rvb Mix","Rvb Sz","Loop ms",
            "Rate","Ph Offset","Warp","Seq Steps","Seq Hits","Seq Rot"
        };

        for (int j = 0; j < 8; ++j)
        {
            auto& row = destRows_[j];

            // Voice ComboBox
            row.voiceBox.addItem ("V1", 1);
            row.voiceBox.addItem ("V2", 2);
            row.voiceBox.addItem ("V3", 3);
            row.voiceBox.setTextWhenNothingSelected ("V1");
            row.voiceBox.setSelectedId (1, juce::dontSendNotification);
            row.voiceBox.setColour (juce::ComboBox::backgroundColourId, juce::Colour (0xff3A3A3C));
            row.voiceBox.setColour (juce::ComboBox::textColourId,       juce::Colour (0xffF2F2F7));
            row.voiceBox.setColour (juce::ComboBox::outlineColourId,    juce::Colour (0xff636366));
            row.voiceBox.onChange = [this, j] { applyInspectorRowChange (j); };
            addAndMakeVisible (row.voiceBox);

            // Dest ComboBox
            for (int d = 1; d < 18; ++d)
                row.destBox.addItem (destNames[d], d);
            row.destBox.setColour (juce::ComboBox::backgroundColourId, juce::Colour (0xff3A3A3C));
            row.destBox.setColour (juce::ComboBox::textColourId,       juce::Colour (0xffF2F2F7));
            row.destBox.setColour (juce::ComboBox::outlineColourId,    juce::Colour (0xff636366));
            row.destBox.onChange = [this, j] { applyInspectorRowChange (j); };
            addAndMakeVisible (row.destBox);

            // Depth slider
            row.depthSlider.setSliderStyle (juce::Slider::LinearHorizontal);
            row.depthSlider.setTextBoxStyle (juce::Slider::NoTextBox, true, 0, 0);
            row.depthSlider.setRange (-1.0, 1.0, 0.0);
            row.depthSlider.setValue (1.0, juce::dontSendNotification);
            row.depthSlider.setDoubleClickReturnValue (true, 1.0);
            row.depthSlider.setColour (juce::Slider::backgroundColourId, juce::Colour (0xff3A3A3C));
            row.depthSlider.setColour (juce::Slider::trackColourId,      juce::Colour (0xff30D158));
            row.depthSlider.setColour (juce::Slider::thumbColourId,      juce::Colour (0xffF2F2F7));
            row.depthSlider.setPopupDisplayEnabled (true, true, nullptr);
            row.depthSlider.onValueChange = [this, j] { applyInspectorRowChange (j); };
            addAndMakeVisible (row.depthSlider);

            // Min slider (normalised range lo)
            row.minSlider.setSliderStyle (juce::Slider::LinearHorizontal);
            row.minSlider.setTextBoxStyle (juce::Slider::NoTextBox, true, 0, 0);
            row.minSlider.setRange (0.0, 1.0, 0.0);
            row.minSlider.setValue (0.0, juce::dontSendNotification);
            row.minSlider.setDoubleClickReturnValue (true, 0.0);
            row.minSlider.setColour (juce::Slider::backgroundColourId, juce::Colour (0xff3A3A3C));
            row.minSlider.setColour (juce::Slider::trackColourId,      juce::Colour (0xff5E5CE6));
            row.minSlider.setColour (juce::Slider::thumbColourId,      juce::Colour (0xffF2F2F7));
            row.minSlider.setPopupDisplayEnabled (true, true, nullptr);
            row.minSlider.onValueChange = [this, j] { applyInspectorRowChange (j); };
            addAndMakeVisible (row.minSlider);

            // Max slider (normalised range hi)
            row.maxSlider.setSliderStyle (juce::Slider::LinearHorizontal);
            row.maxSlider.setTextBoxStyle (juce::Slider::NoTextBox, true, 0, 0);
            row.maxSlider.setRange (0.0, 1.0, 0.0);
            row.maxSlider.setValue (1.0, juce::dontSendNotification);
            row.maxSlider.setDoubleClickReturnValue (true, 1.0);
            row.maxSlider.setColour (juce::Slider::backgroundColourId, juce::Colour (0xff3A3A3C));
            row.maxSlider.setColour (juce::Slider::trackColourId,      juce::Colour (0xffFF9F0A));
            row.maxSlider.setColour (juce::Slider::thumbColourId,      juce::Colour (0xffF2F2F7));
            row.maxSlider.setPopupDisplayEnabled (true, true, nullptr);
            row.maxSlider.onValueChange = [this, j] { applyInspectorRowChange (j); };
            addAndMakeVisible (row.maxSlider);

            // Delete button
            row.deleteBtn.setColour (juce::TextButton::buttonColourId, juce::Colour (0xff3A3A3Cu));
            row.deleteBtn.setColour (juce::TextButton::textColourOffId, juce::Colour (0xffFF453A));
            row.deleteBtn.onClick = [this, j] {
                if (!proc_ || selectedLane_ < 0) return;
                proc_->getTimeline(selectedLane_).removeDest (j);
                updateInspector();
                lanesCt_.repaint();
                repaint();
            };
            addAndMakeVisible (row.deleteBtn);
        }

        // Add dest button in inspector
        addDestInspBtn_.setColour (juce::TextButton::buttonColourId, juce::Colour (0xff3A3A3Cu));
        addDestInspBtn_.setColour (juce::TextButton::textColourOffId, juce::Colour (0xffF2F2F7));
        addDestInspBtn_.onClick = [this] {
            if (!proc_ || selectedLane_ < 0) return;
            TimelineDest td; td.voice=0; td.dest=1; td.depth=1.f; td.min=0.f; td.max=1.f;
            proc_->getTimeline(selectedLane_).addDest (td);
            updateInspector();
            lanesCt_.repaint();
            repaint();
        };
        addAndMakeVisible (addDestInspBtn_);
    }

    void updateInspector()
    {
        if (selectedLane_ < 0 || !proc_) return;
        auto& tl = proc_->getTimeline (selectedLane_);
        const auto& dests = tl.getPendingDests();
        inspTitle_.setText ("TL" + juce::String(selectedLane_+1) + "  —  "
                            + juce::String((int)dests.size()) + " destination"
                            + ((int)dests.size() != 1 ? "s" : ""),
                            juce::dontSendNotification);
        for (int j = 0; j < 8; ++j)
        {
            bool vis = (j < (int)dests.size());
            if (vis)
            {
                destRows_[j].voiceBox.setSelectedId  (dests[(size_t)j].voice + 1, juce::dontSendNotification);
                destRows_[j].destBox.setSelectedId   (dests[(size_t)j].dest,      juce::dontSendNotification);
                destRows_[j].depthSlider.setValue    (dests[(size_t)j].depth,     juce::dontSendNotification);
                destRows_[j].minSlider.setValue      (dests[(size_t)j].min,       juce::dontSendNotification);
                destRows_[j].maxSlider.setValue      (dests[(size_t)j].max,       juce::dontSendNotification);
            }
            destRows_[j].voiceBox.setVisible    (vis);
            destRows_[j].destBox.setVisible     (vis);
            destRows_[j].depthSlider.setVisible  (vis);
            destRows_[j].minSlider.setVisible    (vis);
            destRows_[j].maxSlider.setVisible    (vis);
            destRows_[j].deleteBtn.setVisible    (vis);
        }
        resized();
        repaint();
    }

    void applyInspectorRowChange (int j)
    {
        if (!proc_ || selectedLane_ < 0) return;
        auto& tl = proc_->getTimeline (selectedLane_);
        const auto& dests = tl.getPendingDests();
        if (j >= (int)dests.size()) return;
        auto updated = dests;
        updated[(size_t)j].voice  = destRows_[j].voiceBox.getSelectedId() - 1;
        updated[(size_t)j].dest   = destRows_[j].destBox.getSelectedId();
        updated[(size_t)j].depth  = (float) destRows_[j].depthSlider.getValue();
        updated[(size_t)j].min    = (float) destRows_[j].minSlider.getValue();
        updated[(size_t)j].max    = (float) destRows_[j].maxSlider.getValue();
        tl.setDests (updated);
    }

    //==========================================================================
    float getSecTickStep (float pps) const
    {
        const float steps[] = { 0.1f, 0.25f, 0.5f, 1.f, 2.f, 5.f, 10.f, 15.f, 30.f, 60.f, 120.f, 300.f };
        for (float s : steps)
            if (s * pps >= 28.f) return s;
        return 300.f;
    }

    static juce::String formatSec (float secs)
    {
        if (secs < 0.f) secs = 0.f;
        if (secs < 60.f)
        {
            int s  = (int) secs;
            int ms = (int)((secs - (float)s) * 10.f);
            return juce::String(s) + (ms > 0 ? "." + juce::String(ms) : "") + "s";
        }
        int m = (int)(secs / 60.f);
        int s = (int)std::fmod (secs, 60.f);
        return juce::String(m) + "m" + (s > 0 ? juce::String(s) + "s" : "");
    }

    static float parseDuration (const juce::String& text)
    {
        juce::String t = text.trim().toLowerCase();
        float total = 0.f;
        int mIdx = t.indexOfChar ('m');
        if (mIdx >= 0) { total += t.substring(0,mIdx).getFloatValue()*60.f; t = t.substring(mIdx+1); }
        int sIdx = t.indexOfChar ('s');
        if (sIdx >= 0) total += t.substring(0,sIdx).getFloatValue();
        else if (t.isNotEmpty()) total += t.getFloatValue();
        return total > 0.f ? total : -1.f;
    }

    static void styleHeaderBtn (juce::TextButton& b)
    {
        b.setColour (juce::TextButton::buttonColourId, juce::Colour (0xff3A3A3Cu));
        b.setColour (juce::TextButton::textColourOffId, juce::Colour (0xffF2F2F7));
    }

    static void styleActivBtn (juce::TextButton& b, bool on)
    {
        b.setColour (juce::TextButton::buttonColourId,
            juce::Colour (on ? 0xff30D158u : 0xff3A3A3Cu));
        b.setColour (juce::TextButton::textColourOffId, juce::Colour (0xffF2F2F7));
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
    juce::Label       bpmDisplay;           // click to edit inline
    juce::TextButton  tapBtn     { "TAP" };
    // Tap tempo state
    juce::int64       tapTimes_[4] = {};
    int               tapCount_    = 0;
    // Beat flash state
    bool              beatFlash_   = false;
    juce::int64       beatFlashMs_ = 0;
    float             prevClockPhase_ = 0.f;
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
    juce::TextButton tlToggleBtn  { "TL" };
    juce::TextButton browseBtn    { "Browse" };
    TimelineView timelineView_;
    std::unique_ptr<SoundBrowser> soundBrowser_;

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

        // Playlist row
        juce::ComboBox   playlistCombo;
        juce::TextButton loadPlaylistBtn { "Load PL" };

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
        juce::TextButton bungeeBtn    { "RAW" };   // RAW = raw mode, STCH = Bungee mode

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
    static void refreshPlaylistCombo (juce::ComboBox& cb);
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
