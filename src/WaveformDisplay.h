#pragma once
#include <juce_gui_basics/juce_gui_basics.h>
#include <juce_audio_basics/juce_audio_basics.h>
#include <functional>
#include <cmath>

/**
 * WaveformDisplay — waveform with draggable region + loop handles.
 *
 * Visual layers (bottom to top):
 *   White background
 *   Grey waveform
 *   50% black overlay outside region (red handles)
 *   Subtle grey fill over current loop window (dark grey handles)
 *   Bright white line = seq loop anchor (moves with Seq/Rnd mode)
 *   Gold line = actual sample read position (playhead)
 *   Length overlay text at bottom
 *
 * Red handles  : region start / end   (playable boundary)
 * Dark handles : loop start / end     (the loop window, set by handles)
 * White marker : active seq anchor    (where the next trigger will play from)
 * Gold cursor  : sample read position (where GranularVoice is reading)
 */
class WaveformDisplay : public juce::Component
{
public:
    std::function<void(float)> onRegionStart;
    std::function<void(float)> onRegionEnd;
    std::function<void(float)> onLoopStart;
    std::function<void(float)> onLoopEnd;

    //==========================================================================
    void setBuffer (const juce::AudioBuffer<float>* buf)
    {
        buffer_ = buf;
        repaint();
    }

    /** Call from timerCallback with current parameter values. */
    void setState (float rgnSt, float rgnEn,
                   float lpSt,  float lpEn,
                   int loopMode, bool loopSizeLock,
                   float loopSizeMs,
                   float sampleReadPos,     // actual playhead [0,1] in full buffer
                   float seqLoopAnchor)     // seq/rnd loop anchor [0,1] in full buffer
    {
        regionStart_    = rgnSt;  regionEnd_     = rgnEn;
        loopStart_      = lpSt;   loopEnd_       = lpEn;
        loopMode_       = loopMode;
        loopSizeLock_   = loopSizeLock;
        loopSizeMs_     = loopSizeMs;
        sampleReadPos_  = sampleReadPos;
        seqLoopAnchor_  = seqLoopAnchor;
    }

    void setSampleInfo (double sampleRateHz, int numSamples)
    {
        sampleRateHz_    = sampleRateHz;
        numSamplesTotal_ = numSamples;
    }

    //==========================================================================
    void paint (juce::Graphics& g) override
    {
        auto b  = getLocalBounds();
        int  bw = b.getWidth();
        int  bh = b.getHeight();

        // White background
        g.setColour (juce::Colours::white);
        g.fillRect (b);

        if (buffer_ == nullptr || buffer_->getNumSamples() == 0)
        {
            g.setColour (juce::Colour (0xff888888));
            g.drawText ("No sample  \xe2\x80\x94  click Load Folder", b,
                        juce::Justification::centred, true);
            return;
        }

        // ── Waveform (grey) ───────────────────────────────────────────────────
        int numSamples = buffer_->getNumSamples();
        int numCh      = buffer_->getNumChannels();
        g.setColour (juce::Colour (0xff888888));

        for (int px = 0; px < bw; ++px)
        {
            int s0 = (int)((double) px       / bw * numSamples);
            int s1 = (int)((double)(px + 1)  / bw * numSamples);
            s1 = std::min (s1, numSamples - 1);

            float mn = 0.0f, mx = 0.0f;
            for (int s = s0; s <= s1; ++s)
            {
                float v = 0.0f;
                for (int c = 0; c < numCh; ++c) v += buffer_->getSample (c, s);
                v /= (float) numCh;
                mn = std::min (mn, v);
                mx = std::max (mx, v);
            }
            int mid = bh / 2;
            int y1  = mid - (int)(mx * mid);
            int y2  = mid - (int)(mn * mid);
            if (y1 == y2) ++y2;
            g.drawVerticalLine (px, (float) y1, (float) y2);
        }

        // ── Region shading (darken outside region) ────────────────────────────
        int rxs = normToX (regionStart_);
        int rxe = normToX (regionEnd_);
        g.setColour (juce::Colours::black.withAlpha (0.30f));
        if (rxs > 0)   g.fillRect (0, 0, rxs,      bh);
        if (rxe < bw)  g.fillRect (rxe, 0, bw - rxe, bh);

        // ── Loop window shading (always visible) ─────────────────────────────
        {
            // Off/Fixed: window is exactly the handle positions.
            // Seq/Rnd:   window follows the moving seq anchor (same width as handles).
            bool isMoving = (loopMode_ == 2 || loopMode_ == 3);
            float winStart = isMoving ? seqLoopAnchor_ : loopStart_;
            float winEnd;
            if (loopSizeLock_ && sampleRateHz_ > 0.0 && numSamplesTotal_ > 0)
                winEnd = winStart + (float)(loopSizeMs_ / 1000.0 * sampleRateHz_ / numSamplesTotal_);
            else
                winEnd = isMoving ? winStart + (loopEnd_ - loopStart_) : loopEnd_;
            winEnd = std::min (winEnd, regionEnd_);

            int lxs = normToX (winStart);
            int lxe = normToX (winEnd);
            g.setColour (juce::Colour (0xff4488CC).withAlpha (0.18f));
            g.fillRect (lxs, 0, std::max (1, lxe - lxs), bh);
        }

        // ── Seq anchor marker — only shown when window is moving ─────────────
        if (loopMode_ == 2 || loopMode_ == 3)
        {
            int ax = normToX (seqLoopAnchor_);
            g.setColour (juce::Colour (0xff4488CC).withAlpha (0.9f));
            g.fillRect (ax - 1, 0, 3, bh);
        }

        // ── Sample read cursor (playhead, dark) ───────────────────────────────
        if (sampleReadPos_ >= 0.0f && sampleReadPos_ <= 1.0f)
        {
            int px = normToX (sampleReadPos_);
            g.setColour (juce::Colour (0xff222222).withAlpha (0.8f));
            g.fillRect (px - 1, 0, 2, bh);
        }

        // ── Region handles (black) ────────────────────────────────────────────
        drawHandle (g, regionStart_, juce::Colour (0xff111111), bh);
        drawHandle (g, regionEnd_,   juce::Colour (0xff111111), bh);

        // ── Loop handles (blue, always visible) ───────────────────────────────
        drawHandle (g, loopStart_, juce::Colour (0xff2266AA), bh);
        if (!loopSizeLock_)
            drawHandle (g, loopEnd_, juce::Colour (0xff2266AA), bh);

        // ── Length info overlay ───────────────────────────────────────────────
        if (sampleRateHz_ > 0.0 && numSamplesTotal_ > 0)
        {
            double totalMs = numSamplesTotal_ / sampleRateHz_ * 1000.0;
            double rgnMs   = (double)(regionEnd_ - regionStart_) * totalMs;

            juce::String info = "Total: " + msStr (totalMs)
                              + "  Region: " + msStr (rgnMs);

            if (loopMode_ != 0)
            {
                double lpMs = loopSizeLock_
                    ? (double)loopSizeMs_
                    : (double)(loopEnd_ - loopStart_)
                      * (double)(regionEnd_ - regionStart_) * totalMs;
                info += "  Loop: " + msStr (lpMs);
            }

            g.setFont (juce::Font (juce::FontOptions{}.withName ("Menlo").withHeight (10.0f)));
            g.setColour (juce::Colour (0xff555555));
            g.drawText (info, 4, bh - 14, bw - 8, 12,
                        juce::Justification::centredLeft, false);
        }

        // ── Border ────────────────────────────────────────────────────────────
        g.setColour (juce::Colour (0xffCCCCCC));
        g.drawRect (b, 1);
    }

    void mouseDown (const juce::MouseEvent& e) override
    {
        drag_ = findHandle (e.x, e.y);
    }

    void mouseDrag (const juce::MouseEvent& e) override
    {
        float v = juce::jlimit (0.0f, 1.0f, xToNorm (e.x));
        switch (drag_)
        {
            case Handle::RegionStart: regionStart_ = v; if (onRegionStart) onRegionStart (v); break;
            case Handle::RegionEnd:   regionEnd_   = v; if (onRegionEnd)   onRegionEnd   (v); break;
            case Handle::LoopStart:   loopStart_   = v; if (onLoopStart)   onLoopStart   (v); break;
            case Handle::LoopEnd:     loopEnd_     = v; if (onLoopEnd)     onLoopEnd     (v); break;
            case Handle::None: break;
        }
        repaint();
    }

    void mouseUp (const juce::MouseEvent&) override { drag_ = Handle::None; }

private:
    enum class Handle { None, RegionStart, RegionEnd, LoopStart, LoopEnd };

    int   normToX (float n) const { return (int)(n * (float) getWidth()); }
    float xToNorm (int x)   const { return (float) x / (float) std::max (1, getWidth()); }

    Handle findHandle (int x, int /*y*/) const
    {
        constexpr int kR = 8;
        auto d = [&](float n) { return std::abs (normToX (n) - x); };
        if (d (regionStart_) < kR) return Handle::RegionStart;
        if (d (regionEnd_)   < kR) return Handle::RegionEnd;
        // Loop handles always draggable
        if (d (loopStart_) < kR) return Handle::LoopStart;
        if (!loopSizeLock_ && d (loopEnd_) < kR) return Handle::LoopEnd;
        return Handle::None;
    }

    void drawHandle (juce::Graphics& g, float norm, juce::Colour col, int h)
    {
        int x = normToX (norm);
        g.setColour (col);
        g.drawVerticalLine (x, 0.0f, (float) h);
        juce::Path tri;
        tri.addTriangle ((float)(x - 5), 0.0f,
                         (float)(x + 5), 0.0f,
                         (float) x,       10.0f);
        g.fillPath (tri);
    }

    static juce::String msStr (double ms)
    {
        if (ms >= 1000.0) return juce::String (ms / 1000.0, 2) + "s";
        return juce::String ((int) ms) + "ms";
    }

    const juce::AudioBuffer<float>* buffer_       = nullptr;
    float  regionStart_    = 0.0f, regionEnd_     = 1.0f;
    float  loopStart_      = 0.0f, loopEnd_       = 1.0f;
    int    loopMode_       = 0;
    bool   loopSizeLock_   = false;
    float  loopSizeMs_     = 100.0f;
    float  sampleReadPos_  = 0.0f;
    float  seqLoopAnchor_  = 0.0f;
    double sampleRateHz_   = 0.0;
    int    numSamplesTotal_ = 0;
    Handle drag_           = Handle::None;
};
