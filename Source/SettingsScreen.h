#pragma once
// Οθόνη ρυθμίσεων μέσα στο Flow: πλέγμα από πλακάκια και μία καρτέλα για το καθένα (χωρίς λίστες και χωρίς παράθυρα).
// Το Main δίνει τις λειτουργίες μέσω του Host· εδώ υπάρχει μόνο η εμφάνιση και τα κλικ.
#include <juce_gui_basics/juce_gui_basics.h>
#include "HobbyStyle.h"
#include "QrCode.h"
#include <functional>
#include <vector>

class SettingsScreen : public juce::Component, private juce::Timer
{
public:
    struct SetInfo { juce::String name, date; juce::Image thumb; bool current = false; juce::File file; };

    struct Host
    {
        std::function<void()> openAudio;
        std::function<juce::String()> audioSummary;
        std::function<juce::String()> currentSetName;
        std::function<std::vector<SetInfo>()> listSets;
        std::function<void (const juce::String&)> saveSetNamed;
        std::function<void()> saveElsewhere, loadElsewhere;
        std::function<void (const juce::File&)> loadSet;
        std::function<juce::Image()> orbImage;
        std::function<juce::String()> sessionInfo;   // κενό = δεν υπάρχει
        std::function<void()> restoreSession;
        std::function<juce::File()> recFolder;
        std::function<void (const juce::File&)> setRecFolder;
        std::function<void()> chooseRecFolder;
        std::function<float()> masterGet;
        std::function<void (float)> masterSet;
        std::function<bool()> limiterGet;
        std::function<void (bool)> limiterSet;
        std::function<juce::String()> masterCcText;   // κενό = δεν έχει ανατεθεί
        std::function<void()> masterAssign, masterRemove;
        std::function<bool()> linkAvailable, linkGet;
        std::function<void()> linkToggle;
        std::function<void()> panicNow, panicAssign, panicRemove;
        std::function<juce::String()> panicCodeText;   // κενό = δεν έχει ανατεθεί
        std::function<bool()> ipadGet;
        std::function<void()> ipadToggle;
        std::function<juce::String()> ipadUrl;
        std::function<int()> ipadClients;
        std::function<juce::String()> ipadOtherIps;
        std::function<juce::String()> orbFolderText;
        std::function<void()> chooseOrbFolder;
        std::function<int()> startQGet, lengthQGet, tapMsGet, snapQGet;
        std::function<void (int)> startQSet, lengthQSet, tapMsSet, snapQSet;
        std::function<int()> autoLoopGet;
        std::function<void (int)> autoLoopSet;
        std::function<void (int)> learnStart;   // 1 = ανάθεση, 2 = αφαίρεση· η οθόνη ρυθμίσεων κλείνει
    };

    explicit SettingsScreen (juce::Typeface::Ptr face) : typeface (face)
    {
        addChildComponent (nameEditor);
        nameEditor.setMultiLine (false);
        nameEditor.setReturnKeyStartsNewLine (false);
        nameEditor.setFont (hobby::makeFont (typeface, 16.0f));
        nameEditor.setColour (juce::TextEditor::backgroundColourId, juce::Colour (0xff36322f));
        nameEditor.setColour (juce::TextEditor::textColourId, juce::Colour (0xffe6e0d8));
        nameEditor.setColour (juce::TextEditor::outlineColourId, juce::Colour (0xff3e3a37));
        nameEditor.setColour (juce::TextEditor::focusedOutlineColourId, hobby::kOn);
        nameEditor.setColour (juce::CaretComponent::caretColourId, hobby::kOn);
        nameEditor.setIndents (14, 10);
        nameEditor.onReturnKey = [this] { doSave(); };
    }
    ~SettingsScreen() override { stopTimer(); }

    Host host;
    std::function<void()> onChanged, onClose;

    void open()
    {
        page = Home; loadScroll = 0; saved = false; waitPanic = false;
        nameEditor.setVisible (false);
        setVisible (true); toFront (false);
        startTimerHz (4);
        repaint();
    }
    void close()
    {
        stopTimer();
        nameEditor.setVisible (false);
        setVisible (false);
        if (onClose) onClose();
    }
    void hideForPanel() { stopTimer(); nameEditor.setVisible (false); setVisible (false); }   // χωρίς onClose: επιστρέφει με open()
    // Esc: πίσω στο πλέγμα, και από το πλέγμα κλείνει
    bool goBack() { if (page == Home) { close(); return true; } showPage (Home); return true; }

    void resized() override { layoutEditor(); }

    void paint (juce::Graphics& g) override
    {
        using namespace juce;
        hits.clear();
        g.setColour (hobby::kScreen);
        g.fillRoundedRectangle (getLocalBounds().toFloat(), 14.0f);

        static const char* titles[] = { "SETTINGS", "SAVE SET", "LOAD SET", "RESTORE LAST SESSION", "REC AUDIO FOLDER", "MASTER OUTPUT", "PANIC", "IPAD ORB", "ORB IMAGES", "LOOP RECORDING", "MIDI LEARN" };
        g.setColour (dim()); g.setFont (font (13.0f, true));
        g.drawText (titles[(int) page], 40, 26, 500, 20, Justification::centredLeft, false);
        {
            const Rectangle<int> r (getWidth() - 40 - 26, 22, 26, 26);
            g.setColour (hov (r) ? Colour (0xff4a4440) : Colour (0xff36322f));
            g.fillEllipse (r.toFloat());
            g.setColour (txt());
            g.drawLine ((float) r.getX() + 8.0f, (float) r.getY() + 8.0f, (float) r.getRight() - 8.0f, (float) r.getBottom() - 8.0f, 2.0f);
            g.drawLine ((float) r.getX() + 8.0f, (float) r.getBottom() - 8.0f, (float) r.getRight() - 8.0f, (float) r.getY() + 8.0f, 2.0f);
            hits.push_back ({ r, [this] { if (page == Home) close(); else showPage (Home); } });
        }

        switch (page)
        {
            case Home:   paintHome (g); break;
            case Save:   paintSave (g); break;
            case Load:   paintLoad (g); break;
            case Restore: paintRestore (g); break;
            case RecFolder: paintRecFolder (g); break;
            case Master: paintMaster (g); break;
            case Panic:  paintPanic (g); break;
            case Ipad:   paintIpad (g); break;
            case OrbImages: paintOrbImages (g); break;
            case LoopRec: paintLoopRec (g); break;
            case Learn:  paintLearn (g); break;
        }
    }

    void mouseMove (const juce::MouseEvent& e) override { mouse = e.getPosition(); repaint(); }
    void mouseExit (const juce::MouseEvent&) override { mouse = { -1, -1 }; repaint(); }
    void mouseDown (const juce::MouseEvent& e) override
    {
        mouse = e.getPosition();
    }
    void mouseUp (const juce::MouseEvent& e) override
    {
        for (int i = (int) hits.size(); --i >= 0;)
            if (hits[(size_t) i].r.contains (e.getPosition()))
            {
                auto fn = hits[(size_t) i].fn;
                if (fn) fn();
                return;
            }
    }
    void mouseWheelMove (const juce::MouseEvent&, const juce::MouseWheelDetails& w) override
    {
        if (page != Load) return;
        loadScroll = juce::jlimit (0, juce::jmax (0, loadMaxScroll), loadScroll - (int) (w.deltaY * 220.0f));
        repaint();
    }

private:
    enum Page { Home = 0, Save, Load, Restore, RecFolder, Master, Panic, Ipad, OrbImages, LoopRec, Learn };
    struct Hit { juce::Rectangle<int> r; std::function<void()> fn; };

    static juce::Colour dim()  { return juce::Colour (0xff968e86); }
    static juce::Colour txt()  { return juce::Colour (0xffe6e0d8); }
    static juce::Colour card() { return juce::Colour (0xff2e2a28); }
    static juce::Colour lineC(){ return juce::Colour (0xff3e3a37); }
    juce::Font font (float h, bool bold = false) const { auto f = hobby::makeFont (typeface, h); return bold ? f.boldened() : f; }
    bool hov (const juce::Rectangle<int>& r) const { return r.contains (mouse); }

    void showPage (Page p)
    {
        page = p; loadScroll = 0; saved = false; waitPanic = false;
        nameEditor.setVisible (p == Save);
        if (p == Save)
        {
            nameEditor.setText (host.currentSetName ? host.currentSetName() : juce::String(), false);
            if (nameEditor.getText().trim().isEmpty() || nameEditor.getText() == juce::String::fromUTF8 ("\xe2\x80\x93")) nameEditor.setText ("Set", false);
            layoutEditor();
            nameEditor.grabKeyboardFocus();
            nameEditor.selectAll();
        }
        repaint();
    }
    void layoutEditor() { nameEditor.setBounds (40, 98, juce::jmin (520, getWidth() - 80 - 200), 42); }

    // ---------- στοιχεία ----------
    void pill (juce::Graphics& g, juce::Rectangle<int> r, const juce::String& t, bool main, bool enabled, std::function<void()> fn)
    {
        using namespace juce;
        const bool h = enabled && hov (r);
        g.setColour (main ? Colour (h ? 0xff315040 : 0xff284234) : Colour (h ? 0xff423d39 : 0xff36322f));
        g.fillRoundedRectangle (r.toFloat(), (float) r.getHeight() * 0.5f);
        g.setColour (main ? hobby::kOn : lineC());
        g.drawRoundedRectangle (r.toFloat().reduced (1.0f), (float) r.getHeight() * 0.5f, main ? 2.0f : 1.0f);
        g.setColour (! enabled ? dim().withAlpha (0.5f) : main ? hobby::kOn : txt());
        g.setFont (font (14.0f, true));
        g.drawText (t, r, Justification::centred, false);
        if (enabled) hits.push_back ({ r, std::move (fn) });
    }
    void note (juce::Graphics& g, const juce::String& t, int y)
    {
        g.setColour (juce::Colour (0xff786f67)); g.setFont (font (11.0f));
        g.drawText (t, 40, y, getWidth() - 80, 16, juce::Justification::centredLeft, false);
    }
    void toggleSwitch (juce::Graphics& g, juce::Rectangle<float> sw, bool on)
    {
        g.setColour (on ? juce::Colour (0xff284234) : juce::Colour (0xff3e3a37));
        g.fillRoundedRectangle (sw, sw.getHeight() * 0.5f);
        g.setColour (on ? hobby::kOn : dim());
        const float d = sw.getHeight() - 4.0f;
        g.fillEllipse (on ? sw.getRight() - d - 2.0f : sw.getX() + 2.0f, sw.getY() + 2.0f, d, d);
    }

    void drawIcon (juce::Graphics& g, int kind, juce::Point<float> c, juce::Colour col)
    {
        using namespace juce;
        g.setColour (col);
        Path p;
        const PathStrokeType st (2.2f, PathStrokeType::curved, PathStrokeType::rounded);
        switch (kind)
        {
            case 0: drawSymbol (g, Symbol::Sliders, c, 30.0f, col); break;
            case 1: // save: βέλος κάτω προς δίσκο
                p.startNewSubPath (c.x, c.y - 11.0f); p.lineTo (c.x, c.y + 3.0f);
                p.startNewSubPath (c.x - 7.0f, c.y - 3.0f); p.lineTo (c.x, c.y + 5.0f); p.lineTo (c.x + 7.0f, c.y - 3.0f);
                p.startNewSubPath (c.x - 11.0f, c.y + 11.0f); p.lineTo (c.x + 11.0f, c.y + 11.0f);
                g.strokePath (p, st); break;
            case 2: // load: βέλος πάνω
                p.startNewSubPath (c.x, c.y + 4.0f); p.lineTo (c.x, c.y - 9.0f);
                p.startNewSubPath (c.x - 7.0f, c.y - 3.0f); p.lineTo (c.x, c.y - 11.0f); p.lineTo (c.x + 7.0f, c.y - 3.0f);
                p.startNewSubPath (c.x - 11.0f, c.y + 11.0f); p.lineTo (c.x + 11.0f, c.y + 11.0f);
                g.strokePath (p, st); break;
            case 3: // restore: κυκλικό βέλος
                p.addCentredArc (c.x, c.y, 10.0f, 10.0f, 0.0f, degreesToRadians (40.0f), degreesToRadians (330.0f), true);
                g.strokePath (p, st);
                { Path a; a.startNewSubPath (c.x + 10.0f, c.y - 11.0f); a.lineTo (c.x + 10.0f, c.y - 2.0f); a.lineTo (c.x + 2.0f, c.y - 6.0f); a.closeSubPath(); g.fillPath (a); }
                break;
            case 4: drawSymbol (g, Symbol::Folder, c, 30.0f, col); break;
            case 5: drawSymbol (g, Symbol::Speaker, c, 30.0f, col); break;
            case 6: g.setFont (font (30.0f, true)); g.drawText (juce::String::fromUTF8 ("\xe2\x88\x9e"), Rectangle<float> (c.x - 20.0f, c.y - 18.0f, 40.0f, 36.0f), Justification::centred, false); break;
            case 7: g.setColour (Colour (0xfff26b6b)); g.setFont (font (30.0f, true)); g.drawText ("!", Rectangle<float> (c.x - 20.0f, c.y - 18.0f, 40.0f, 36.0f), Justification::centred, false); break;
            case 9: g.setFont (font (22.0f, true)); g.drawText (juce::String::fromUTF8 ("\xe2\x96\xb6|"), Rectangle<float> (c.x - 24.0f, c.y - 16.0f, 48.0f, 32.0f), Justification::centred, false); break;
            case 10: g.setFont (font (28.0f, true)); g.drawText (juce::String::fromUTF8 ("\xe2\x97\x8e"), Rectangle<float> (c.x - 20.0f, c.y - 18.0f, 40.0f, 36.0f), Justification::centred, false); break;
            case 8: drawSymbol (g, Symbol::Window, c, 30.0f, col); break;
            default: break;
        }
    }

    // ---------- αρχική: πλέγμα ----------
    void paintHome (juce::Graphics& g)
    {
        using namespace juce;
        struct Tile { int icon; String title; bool hot, dimmed; std::function<void()> fn; };
        const bool linkAv = host.linkAvailable && host.linkAvailable();
        const bool linkOn = host.linkGet && host.linkGet();
        std::vector<Tile> tiles {
            { 0, "Audio & MIDI", false, false, [this] { if (host.openAudio) host.openAudio(); } },
            { 1, "Save set", false, false, [this] { showPage (Save); } },
            { 2, "Load set", false, false, [this] { showPage (Load); } },
            { 3, "Restore session", false, false, [this] { showPage (Restore); } },
            { 4, "Rec Audio folder", false, false, [this] { showPage (RecFolder); } },
            { 5, "Master Output", false, false, [this] { showPage (Master); } },
            { 6, "Ableton Link", linkOn, ! linkAv, [this, linkAv] { if (linkAv && host.linkToggle) host.linkToggle(); } },
            { 7, "Panic", false, false, [this] { showPage (Panic); } },
            { 4, "Orb images", false, false, [this] { showPage (OrbImages); } },
            { 9, "Loop recording", false, false, [this] { showPage (LoopRec); } },
            { 10, "MIDI learn", false, false, [this] { showPage (Learn); } },
        };
        const int cols = 5, gap = 16, x0 = 40, y0 = 74;
        const int rows = ((int) tiles.size() + cols - 1) / cols;
        const int tw = (getWidth() - 2 * x0 - (cols - 1) * gap) / cols;
        const int th = jlimit (84, 150, (getHeight() - y0 - 30 - (rows - 1) * gap) / rows);
        for (int i = 0; i < (int) tiles.size(); ++i)
        {
            auto& t = tiles[(size_t) i];
            const Rectangle<int> r (x0 + (i % cols) * (tw + gap), y0 + (i / cols) * (th + gap), tw, th);
            const bool h = hov (r);
            g.setColour (h ? Colour (0xff36322f) : card());
            g.fillRoundedRectangle (r.toFloat(), 16.0f);
            g.setColour (t.hot ? hobby::kOn : lineC());
            g.drawRoundedRectangle (r.toFloat().reduced (1.0f), 16.0f, t.hot ? 2.0f : 1.0f);
            const Colour ink = t.dimmed ? dim().withAlpha (0.5f) : t.hot ? hobby::kOn : txt();
            drawIcon (g, t.icon, { r.getCentreX() + 0.0f, (float) r.getY() + (float) th * 0.38f }, ink);
            g.setColour (t.dimmed ? dim().withAlpha (0.6f) : txt()); g.setFont (font (14.0f, true));
            g.drawText (t.title, r.getX() + 6, r.getBottom() - 32, tw - 12, 20, Justification::centred, true);
            hits.push_back ({ r, t.fn });
        }
    }

    // ---------- save ----------
    void doSave()
    {
        const auto name = nameEditor.getText().trim();
        if (name.isEmpty()) return;
        if (host.saveSetNamed) host.saveSetNamed (name);
        saved = true;
        repaint();
    }
    void paintSave (juce::Graphics& g)
    {
        using namespace juce;
        g.setColour (dim()); g.setFont (font (12.0f, true));
        g.drawText ("Name", 40, 78, 300, 16, Justification::centredLeft, false);
        g.drawText ("Replace an existing set", 40, 166, 400, 16, Justification::centredLeft, false);
        const auto sets = host.listSets ? host.listSets() : std::vector<SetInfo>();
        int x = 40, y = 188;
        for (int i = 0; i < (int) sets.size() && i < 12; ++i)
        {
            const Rectangle<int> r (x, y, 150, 40);
            if (x + 150 > 40 + juce::jmin (520, getWidth() - 280)) { x = 40; y += 48; }
            const Rectangle<int> rr (x, y, 150, 40);
            g.setColour (hov (rr) ? Colour (0xff36322f) : card());
            g.fillRoundedRectangle (rr.toFloat(), 10.0f);
            g.setColour (lineC()); g.drawRoundedRectangle (rr.toFloat().reduced (0.5f), 10.0f, 1.0f);
            g.setColour (txt()); g.setFont (font (13.0f));
            g.drawText (sets[(size_t) i].name, rr.reduced (12, 0), Justification::centredLeft, true);
            const String n = sets[(size_t) i].name;
            hits.push_back ({ rr, [this, n] { nameEditor.setText (n, false); repaint(); } });
            x += 158; (void) r;
        }
        const int by = juce::jmax (y + 70, 300);
        if (saved)
        {
            g.setColour (hobby::kOn); g.setFont (font (14.0f, true));
            g.drawText (juce::String::fromUTF8 ("Saved \xe2\x9c\x93"), 580, by, 140, 46, Justification::centredLeft, false);
        }
        pill (g, { 40, by, 170, 46 }, "Save", true, nameEditor.getText().trim().isNotEmpty(), [this] { doSave(); });
        pill (g, { 226, by, 130, 46 }, "Cancel", false, true, [this] { showPage (Home); });
        pill (g, { 372, by, 190, 46 }, "Save elsewhere...", false, true, [this] { if (host.saveElsewhere) host.saveElsewhere(); close(); });
        // μικρογραφία
        const int s = 190, ox = getWidth() - 40 - s - 20, oy = 100;
        const auto img = host.orbImage ? host.orbImage() : Image();
        g.saveState();
        Path c; c.addEllipse ((float) ox, (float) oy, (float) s, (float) s);
        g.reduceClipRegion (c);
        if (img.isValid()) g.drawImage (img, Rectangle<float> ((float) ox, (float) oy, (float) s, (float) s), RectanglePlacement::centred | RectanglePlacement::fillDestination);
        else { g.setColour (Colour (0xff1e1b19)); g.fillEllipse ((float) ox, (float) oy, (float) s, (float) s); }
        g.restoreState();
        g.setColour (Colour (0xff4a423b)); g.drawEllipse ((float) ox, (float) oy, (float) s, (float) s, 2.0f);
        g.setColour (dim()); g.setFont (font (11.0f));
        g.drawText ("The orb image is the thumbnail", ox - 20, oy + s + 8, s + 40, 16, Justification::centred, false);
        note (g, "Saves into the Flow sets folder. \"Save elsewhere...\" opens the system file window.", getHeight() - 28);
    }

    // ---------- load ----------
    void paintLoad (juce::Graphics& g)
    {
        using namespace juce;
        const auto sets = host.listSets ? host.listSets() : std::vector<SetInfo>();
        const int cols = 5, gap = 16, x0 = 40, top = 70, bottom = getHeight() - 74;
        const int cw = (getWidth() - 2 * x0 - (cols - 1) * gap) / cols, ch = 150;
        const int rows = ((int) sets.size() + cols - 1) / cols;
        loadMaxScroll = jmax (0, rows * (ch + gap) - (bottom - top));
        loadScroll = jlimit (0, loadMaxScroll, loadScroll);
        g.saveState();
        g.reduceClipRegion (Rectangle<int> (0, top, getWidth(), bottom - top));
        if (sets.empty())
        {
            g.setColour (dim()); g.setFont (font (14.0f));
            g.drawText ("No saved sets yet.", x0, top + 10, 400, 24, Justification::centredLeft, false);
        }
        for (int i = 0; i < (int) sets.size(); ++i)
        {
            const auto& s = sets[(size_t) i];
            const Rectangle<int> r (x0 + (i % cols) * (cw + gap), top + (i / cols) * (ch + gap) - loadScroll, cw, ch);
            if (r.getBottom() < top || r.getY() > bottom) continue;
            const bool h = hov (r) && mouse.y >= top && mouse.y <= bottom;
            g.setColour (h ? Colour (0xff36322f) : card());
            g.fillRoundedRectangle (r.toFloat(), 14.0f);
            g.setColour (s.current ? hobby::kOn : lineC());
            g.drawRoundedRectangle (r.toFloat().reduced (1.0f), 14.0f, s.current ? 2.0f : 1.0f);
            const float d = 76.0f;
            const Rectangle<float> th ((float) r.getCentreX() - d * 0.5f, (float) r.getY() + 14.0f, d, d);
            g.saveState();
            Path c; c.addEllipse (th); g.reduceClipRegion (c);
            if (s.thumb.isValid()) g.drawImage (s.thumb, th, RectanglePlacement::centred | RectanglePlacement::fillDestination);
            else { g.setColour (Colour (0xff1e1b19)); g.fillEllipse (th); }
            g.restoreState();
            g.setColour (Colour (0xffe8e0d6)); g.drawEllipse (th, 2.0f);
            g.setColour (txt()); g.setFont (font (13.0f, true));
            g.drawText (s.name, r.getX() + 8, r.getY() + 98, cw - 16, 18, Justification::centred, true);
            g.setColour (dim()); g.setFont (font (11.0f));
            g.drawText (s.current ? String ("now") : s.date, r.getX() + 8, r.getY() + 120, cw - 16, 16, Justification::centred, false);
            if (mouse.y >= top && mouse.y <= bottom) {}
            const auto f = s.file;
            hits.push_back ({ r.getIntersection (Rectangle<int> (0, top, getWidth(), bottom - top)), [this, f] { if (host.loadSet) host.loadSet (f); close(); } });
        }
        g.restoreState();
        pill (g, { getWidth() - 40 - 190, getHeight() - 64, 190, 40 }, "Other file...", false, true, [this] { if (host.loadElsewhere) host.loadElsewhere(); close(); });
        note (g, "Press a set to load it. Scroll for more.", getHeight() - 52);
    }

    // ---------- restore ----------
    void paintRestore (juce::Graphics& g)
    {
        using namespace juce;
        const String info = host.sessionInfo ? host.sessionInfo() : String();
        const Rectangle<int> r (40, 78, juce::jmin (560, getWidth() - 80), 150);
        g.setColour (card()); g.fillRoundedRectangle (r.toFloat(), 16.0f);
        g.setColour (lineC()); g.drawRoundedRectangle (r.toFloat().reduced (0.5f), 16.0f, 1.0f);
        drawIcon (g, 3, { (float) r.getX() + 62.0f, (float) r.getCentreY() }, info.isEmpty() ? dim() : txt());
        g.setColour (dim()); g.setFont (font (13.0f, true));
        g.drawText ("Last session", r.getX() + 120, r.getY() + 34, 380, 20, Justification::centredLeft, false);
        g.setColour (txt()); g.setFont (font (info.isEmpty() ? 16.0f : 17.0f, true));
        g.drawText (info.isEmpty() ? String ("There is no previous session") : info.upToFirstOccurrenceOf ("  ", false, false), r.getX() + 120, r.getY() + 62, 400, 26, Justification::centredLeft, true);
        g.setColour (dim()); g.setFont (font (12.0f));
        g.drawText (info.contains ("  ") ? info.fromFirstOccurrenceOf ("  ", false, false).trim() : String(), r.getX() + 120, r.getY() + 94, 400, 20, Justification::centredLeft, true);
        pill (g, { 40, 250, 200, 48 }, "Restore", true, info.isNotEmpty(), [this] { if (host.restoreSession) host.restoreSession(); close(); });
        pill (g, { 256, 250, 130, 48 }, "Cancel", false, true, [this] { showPage (Home); });
        note (g, "Restores the state Flow had the last time it was running.", getHeight() - 28);
    }

    // ---------- rec folder ----------
    void paintRecFolder (juce::Graphics& g)
    {
        using namespace juce;
        const File cur = host.recFolder ? host.recFolder() : File();
        g.setColour (dim()); g.setFont (font (12.0f, true));
        g.drawText ("Now", 40, 78, 300, 16, Justification::centredLeft, false);
        const Rectangle<int> r (40, 98, juce::jmin (620, getWidth() - 80), 42);
        g.setColour (Colour (0xff36322f)); g.fillRoundedRectangle (r.toFloat(), 10.0f);
        g.setColour (lineC()); g.drawRoundedRectangle (r.toFloat().reduced (0.5f), 10.0f, 1.0f);
        drawSymbol (g, Symbol::Folder, { (float) r.getX() + 26.0f, (float) r.getCentreY() }, 20.0f, txt());
        String path = cur.getFullPathName();
        const auto home = File::getSpecialLocation (File::userHomeDirectory).getFullPathName();
        if (home.isNotEmpty() && path.startsWith (home)) path = "~" + path.substring (home.length());
        g.setColour (txt()); g.setFont (font (14.0f));
        g.drawText (path, r.withTrimmedLeft (48).withTrimmedRight (12), Justification::centredLeft, true);
        g.setColour (dim()); g.setFont (font (12.0f, true));
        g.drawText ("Quick choice", 40, 164, 300, 16, Justification::centredLeft, false);
        struct Q { String name; File f; };
        const File home2 = File::getSpecialLocation (File::userHomeDirectory);
        std::vector<Q> q {
            { "Music/Flow", File::getSpecialLocation (File::userMusicDirectory).getChildFile ("Flow") },
            { "Desktop", File::getSpecialLocation (File::userDesktopDirectory) },
            { "Documents", File::getSpecialLocation (File::userDocumentsDirectory) },
            { "Downloads", home2.getChildFile ("Downloads") },
        };
        int x = 40;
        for (auto& e : q)
        {
            const bool on = e.f == cur;
            pill (g, { x, 186, 140, 44 }, e.name, on, true, [this, f = e.f] { if (host.setRecFolder) host.setRecFolder (f); if (onChanged) onChanged(); repaint(); });
            x += 150;
        }
        pill (g, { 40, 252, 230, 46 }, "Another folder...", false, true, [this] { if (host.chooseRecFolder) host.chooseRecFolder(); });
        note (g, "The quick choices change immediately. Any other folder opens the macOS folder window.", getHeight() - 28);
    }

    // ---------- master output ----------
    void paintMaster (juce::Graphics& g)
    {
        using namespace juce;
        const Rectangle<int> r (40, 74, 340, 64);
        const bool on = host.limiterGet && host.limiterGet();
        g.setColour (hov (r) ? Colour (0xff36322f) : card()); g.fillRoundedRectangle (r.toFloat(), 14.0f);
        g.setColour (lineC()); g.drawRoundedRectangle (r.toFloat().reduced (0.5f), 14.0f, 1.0f);
        g.setColour (txt()); g.setFont (font (15.0f, true)); g.drawText ("Safety limiter", r.getX() + 22, r.getY() + 12, 200, 20, Justification::centredLeft, false);
        g.setColour (dim()); g.setFont (font (11.0f)); g.drawText ("protects against loud peaks", r.getX() + 22, r.getY() + 34, 220, 16, Justification::centredLeft, false);
        toggleSwitch (g, { (float) r.getRight() - 64.0f, (float) r.getY() + 20.0f, 42.0f, 24.0f }, on);
        hits.push_back ({ r, [this, on] { if (host.limiterSet) host.limiterSet (! on); if (onChanged) onChanged(); repaint(); } });
    }

    // ---------- panic ----------
    void paintPanic (juce::Graphics& g)
    {
        using namespace juce;
        const String cc = host.panicCodeText ? host.panicCodeText() : String();
        pill (g, { 40, 84, 320, 56 }, "Silence stuck notes (Esc)", true, true, [this] { if (host.panicNow) host.panicNow(); });
        g.setColour (dim()); g.setFont (font (12.0f, true));
        g.drawText ("Assigned pedal or key", 40, 172, 300, 16, Justification::centredLeft, false);
        g.setColour (txt()); g.setFont (font (16.0f, true));
        if (waitPanic && cc != ccAtAssign) waitPanic = false;
        g.drawText (waitPanic ? String ("Press the pedal or key now...") : cc.isEmpty() ? String ("none") : cc, 40, 192, 400, 26, Justification::centredLeft, false);
        pill (g, { 40, 234, 300, 46 }, "Assign (MIDI learn)", false, true, [this, cc] { if (host.panicAssign) host.panicAssign(); ccAtAssign = cc; waitPanic = true; repaint(); });
        pill (g, { 356, 234, 200, 46 }, "Remove assignment", false, cc.isNotEmpty(), [this] { if (host.panicRemove) host.panicRemove(); if (onChanged) onChanged(); repaint(); });
        note (g, "After Assign, press the pedal or key you want to use for Panic.", getHeight() - 28);
    }

    // ---------- iPad orb ----------
    void paintIpad (juce::Graphics& g)
    {
        using namespace juce;
        const bool on = host.ipadGet && host.ipadGet();
        {
            const Rectangle<int> r (40, 78, 360, 64);
            g.setColour (hov (r) ? Colour (0xff36322f) : card()); g.fillRoundedRectangle (r.toFloat(), 14.0f);
            g.setColour (on ? hobby::kOn : lineC()); g.drawRoundedRectangle (r.toFloat().reduced (1.0f), 14.0f, on ? 2.0f : 1.0f);
            g.setColour (txt()); g.setFont (font (15.0f, true)); g.drawText ("iPad orb (Safari)", r.getX() + 22, r.getY() + 12, 220, 20, Justification::centredLeft, false);
            g.setColour (dim()); g.setFont (font (11.0f));
            g.drawText (on ? String (host.ipadClients ? host.ipadClients() : 0) + " connected" : String ("off"), r.getX() + 22, r.getY() + 34, 220, 16, Justification::centredLeft, false);
            toggleSwitch (g, { (float) r.getRight() - 66.0f, (float) r.getY() + 20.0f, 44.0f, 24.0f }, on);
            hits.push_back ({ r, [this] { if (host.ipadToggle) host.ipadToggle(); repaint(); } });
        }
        if (on)
        {
            const String url = host.ipadUrl ? host.ipadUrl() : String();
            const auto code = qr::encode (url.toStdString());
            if (code.size > 0)
            {
                const int quiet = 3, avail = getHeight() - 170;
                const int px = jmax (2, avail / (code.size + 2 * quiet));
                const int side = px * (code.size + 2 * quiet);
                const int x0 = 40, y0 = 160;
                g.setColour (Colours::white); g.fillRoundedRectangle ((float) x0, (float) y0, (float) side, (float) side, 10.0f);
                g.setColour (Colours::black);
                for (int y = 0; y < code.size; ++y)
                    for (int x = 0; x < code.size; ++x)
                        if (code.dark (x, y)) g.fillRect (x0 + (x + quiet) * px, y0 + (y + quiet) * px, px, px);
                g.setColour (txt()); g.setFont (font (15.0f, true));
                g.drawText ("Scan with the iPad camera", x0 + side + 28, y0 + 6, 420, 22, Justification::centredLeft, false);
                g.setColour (dim()); g.setFont (font (12.0f));
                g.drawFittedText (url, x0 + side + 28, y0 + 34, getWidth() - (x0 + side + 28) - 40, 40, Justification::topLeft, 2);
                const auto others = host.ipadOtherIps ? host.ipadOtherIps() : juce::String();
                if (others.isNotEmpty())
                {
                    g.setColour (dim()); g.setFont (font (11.0f));
                    g.drawFittedText ("Other addresses of this Mac: " + others, x0 + side + 28, y0 + 160, getWidth() - (x0 + side + 28) - 40, 40, Justification::topLeft, 2);
                }
                g.setColour (dim()); g.setFont (font (12.0f));
                g.drawFittedText ("Same Wi-Fi as the Mac. In Safari, tap \"Tap to start\" for full screen. macOS may ask once to allow incoming connections.",
                                  x0 + side + 28, y0 + 84, getWidth() - (x0 + side + 28) - 40, 60, Justification::topLeft, 4);
            }
        }
        else note (g, "Turn it on to get a QR code for the iPad.", getHeight() - 28);
    }

    // ---------- orb images ----------
    void paintOrbImages (juce::Graphics& g)
    {
        using namespace juce;
        g.setColour (dim()); g.setFont (font (12.0f, true));
        g.drawText ("Folder", 40, 78, 300, 16, Justification::centredLeft, false);
        const Rectangle<int> r (40, 98, juce::jmin (620, getWidth() - 80), 42);
        g.setColour (Colour (0xff36322f)); g.fillRoundedRectangle (r.toFloat(), 10.0f);
        g.setColour (lineC()); g.drawRoundedRectangle (r.toFloat().reduced (0.5f), 10.0f, 1.0f);
        drawSymbol (g, Symbol::Folder, { (float) r.getX() + 26.0f, (float) r.getCentreY() }, 20.0f, txt());
        g.setColour (txt()); g.setFont (font (14.0f));
        g.drawText (host.orbFolderText ? host.orbFolderText() : String(), r.withTrimmedLeft (48).withTrimmedRight (12), Justification::centredLeft, true);
        pill (g, { 40, 164, 230, 46 }, "Choose folder...", false, true, [this] { if (host.chooseOrbFolder) host.chooseOrbFolder(); });
        note (g, "At every start Flow picks a random image from this folder (jpg, png) and shows it in the orb.", getHeight() - 28);
    }

    // ---------- loop recording ----------
    void chipRow (juce::Graphics& g, int x, int y, const juce::String& label, const juce::String& desc,
                  const std::vector<juce::String>& opts, int sel, std::function<void (int)> setFn)
    {
        using namespace juce;
        g.setColour (txt()); g.setFont (font (14.0f, true));
        g.drawText (label, x, y, 360, 20, Justification::centredLeft, false);
        g.setColour (dim()); g.setFont (font (11.0f));
        g.drawText (desc, x, y + 22, 400, 16, Justification::centredLeft, false);
        for (int i = 0; i < (int) opts.size(); ++i)
        {
            const Rectangle<int> r (x + i * 118, y + 46, 108, 40);
            const bool on = i == sel;
            g.setColour (on ? Colour (0xff284234) : (hov (r) ? Colour (0xff423d39) : Colour (0xff36322f)));
            g.fillRoundedRectangle (r.toFloat(), 20.0f);
            g.setColour (on ? hobby::kOn : lineC());
            g.drawRoundedRectangle (r.toFloat().reduced (1.0f), 20.0f, on ? 2.0f : 1.0f);
            g.setColour (on ? hobby::kOn : txt()); g.setFont (font (14.0f, true));
            g.drawText (opts[(size_t) i], r, Justification::centred, false);
            hits.push_back ({ r, [this, setFn, i] { setFn (i); if (onChanged) onChanged(); repaint(); } });
        }
    }
    void paintLoopRec (juce::Graphics& g)
    {
        const int x1 = 40, x2 = juce::jmax (440, getWidth() / 2 + 10);
        const int tapMs = host.tapMsGet ? host.tapMsGet() : 500;
        chipRow (g, x1, 76, "Start quantize", "Where the loop starts when you press Rec MIDI loop", { "Off", "Beat", "Bar" },
                 host.startQGet ? host.startQGet() : 0, [this] (int v) { if (host.startQSet) host.startQSet (v); });
        chipRow (g, x2, 76, "Length quantize", "The loop length snaps to whole beats or bars", { "Off", "Beat", "Bar" },
                 host.lengthQGet ? host.lengthQGet() : 0, [this] (int v) { if (host.lengthQSet) host.lengthQSet (v); });
        chipRow (g, x1, 200, "Pedal press wait", "Time window for repeated pedal presses", { "0.5 s", "0.7 s", "1.0 s" },
                 tapMs < 600 ? 0 : tapMs < 900 ? 1 : 2, [this] (int v) { if (host.tapMsSet) host.tapMsSet (v == 0 ? 500 : v == 1 ? 700 : 1000); });
        chipRow (g, x1, 324, "Auto loop", "Detects a repeat: starts by itself, or when you press Rec MIDI loop", { "Off", "Auto", "On Rec" },
                 host.autoLoopGet ? host.autoLoopGet() : 0, [this] (int v) { if (host.autoLoopSet) host.autoLoopSet (v); });
        chipRow (g, x2, 200, "Note quantize", "The notes you play snap to the grid", { "Off", "1/8", "1/16" },
                 host.snapQGet ? host.snapQGet() : 0, [this] (int v) { if (host.snapQSet) host.snapQSet (v); });
    }

    // ---------- midi learn ----------
    void paintLearn (juce::Graphics& g)
    {
        using namespace juce;
        pill (g, { 40, 84, 330, 56 }, "Assign a pedal or key", true, true, [this] { if (host.learnStart) host.learnStart (1); close(); });
        {
            const Rectangle<int> r (390, 84, 300, 56);
            const bool h = hov (r);
            g.setColour (h ? Colour (0xff5a3a3a) : Colour (0xff4a3232)); g.fillRoundedRectangle (r.toFloat(), 28.0f);
            g.setColour (Colour (0xfff26b6b)); g.drawRoundedRectangle (r.toFloat().reduced (1.0f), 28.0f, 2.0f);
            g.setFont (font (14.0f, true));
            g.drawText ("Remove an assignment", r, Justification::centred, false);
            hits.push_back ({ r, [this] { if (host.learnStart) host.learnStart (2); close(); } });
        }
        g.setColour (dim()); g.setFont (font (12.0f));
        g.drawFittedText ("After you press one of them, this screen closes. Press the button on the screen you want to control, then the pedal or key. Esc cancels.",
                          40, 168, juce::jmin (640, getWidth() - 80), 40, Justification::topLeft, 3);
    }

    void timerCallback() override { if (isVisible()) repaint(); }

    juce::Typeface::Ptr typeface;
    juce::TextEditor nameEditor;
    std::vector<Hit> hits;
    juce::Point<int> mouse { -1, -1 };
    Page page = Home;
    int loadScroll = 0, loadMaxScroll = 0;
    bool saved = false, waitPanic = false;
    juce::String ccAtAssign;
};
