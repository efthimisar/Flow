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
    struct SmartBlock
    {
        bool active = false;
        juce::String name;
        int type = 0;                                   // 0 Other, 1 Bass, 2 Keys, 3 Pad, 4 Lead, 5 Strings
        float low = 0, mid = 0, high = 0, width = 0;    // μετρήσεις 0..1
        float lim[4] = { 1, 1, 1, 1 };                  // όρια του τύπου {Low, Mid, High, πλάτος}
        float needLow = 0, needW = 1, appLow = 0, appW = 1;
    };
    struct FollowBlock { bool active = false; juce::String name; bool on = false; };
    struct SetInfo { juce::String name, date; juce::Image thumb; bool current = false; juce::File file; };

    struct ToolItem { juce::String glyph, label, tip; juce::Colour colour; bool enabled = true, armed = false; std::function<void()> fn; };
    struct OrbKnob { bool bound = false; juce::String name; float value = 0.0f; };

    struct Host
    {
        std::function<std::vector<ToolItem>()> tools;          // εργαλεία του επιλεγμένου block
        std::function<std::vector<OrbKnob>()> orbKnobs;        // 16 αναθέσεις του orb
        std::function<void (int)> orbAssign, orbClear;         // θέση 0..15
        std::function<void()> openAudio;
        std::function<juce::String()> audioSummary;
        std::function<juce::String()> currentSetName;
        std::function<std::vector<SetInfo>()> listSets;
        std::function<void (const juce::String&)> saveSetNamed;
        std::function<void()> saveElsewhere, loadElsewhere;
        std::function<void (const juce::File&)> loadSet;
        std::function<void (const juce::File&)> deleteSet;   // στον κάδο
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
        std::function<std::vector<FollowBlock>()> followBlocks;
        std::function<int()> followOnGet, followRootGet, followTypeGet;
        std::function<void (int)> followOnSet, followRootSet, followTypeSet;
        std::function<void (int, bool)> followBlockSet;
        std::function<std::vector<SmartBlock>()> smartBlocks;
        std::function<int()> smartOnGet, smartModeGet;
        std::function<float()> smartAcceptGet;
        std::function<void (int)> smartOnSet, smartModeSet;
        std::function<void (float)> smartAcceptSet;
        std::function<void (int, int)> smartTypeSet;    // block, τύπος
        std::function<void (int)> smartApply;           // ✓ (Ask): εφαρμογή / αναίρεση της πρότασης
        std::function<int()> windowSizeGet;      // 0 Compact, 1 Default, 2 Large, 3 Free
        std::function<void (int)> windowSizeSet;
        std::function<bool (int)> windowSizeFits;
        std::function<bool()> helpGet;           // μπαλονάκια βοήθειας on/off
        std::function<void()> helpToggle;
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

    // η οθόνη Smart master ανοίγει απευθείας από το δικό της κουμπί (όχι από το πλέγμα των ρυθμίσεων)
    void openSmart()
    {
        open();
        direct = true; page = Smart;
        repaint();
    }
    void openFollow()
    {
        open();
        direct = true; page = Follow;
        repaint();
    }
    bool isSmartDirect() const { return isVisible() && direct && page == Smart; }
    bool isFollowDirect() const { return isVisible() && direct && page == Follow; }
    void open()
    {
        direct = false;
        page = Home; loadScroll = 0; saved = false; waitPanic = false;
        nameEditor.setVisible (false);
        setVisible (true); toFront (false);
        startTimerHz (12);
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
    bool goBack() { if (page == Home || direct) { close(); return true; } showPage (Home); return true; }

    void resized() override { layoutEditor(); }

    void paint (juce::Graphics& g) override
    {
        using namespace juce;
        hits.clear();
        g.setColour (hobby::kScreen);
        g.fillRoundedRectangle (getLocalBounds().toFloat(), 14.0f);

        static const char* titles[] = { "SETTINGS", "SAVE SET", "LOAD SET", "RESTORE LAST SESSION", "REC AUDIO FOLDER", "MASTER OUTPUT", "PANIC", "IPAD ORB", "ORB IMAGES", "LOOP RECORDING", "MIDI LEARN", "WINDOW SIZE", "SMART MASTER", "FOLLOW", "BLOCK TOOLS", "ORB KNOBS" };
        g.setColour (dim()); g.setFont (font (13.0f, true));
        g.drawText (titles[(int) page], 40, 26, 500, 20, Justification::centredLeft, false);
        {
            const Rectangle<int> r (getWidth() - 40 - 26, 22, 26, 26);
            g.setColour (hov (r) ? Colour (0xff4a4440) : Colour (0xff36322f));
            g.fillEllipse (r.toFloat());
            g.setColour (txt());
            g.drawLine ((float) r.getX() + 8.0f, (float) r.getY() + 8.0f, (float) r.getRight() - 8.0f, (float) r.getBottom() - 8.0f, 2.0f);
            g.drawLine ((float) r.getX() + 8.0f, (float) r.getBottom() - 8.0f, (float) r.getRight() - 8.0f, (float) r.getY() + 8.0f, 2.0f);
            hits.push_back ({ r, [this] { if (page == Home || direct) close(); else showPage (Home); } });
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
            case WindowSize: paintWindowSize (g); break;
            case Smart: paintSmart (g); break;
            case Follow: paintFollow (g); break;
            case Tools:  paintTools (g); break;
            case OrbKnobs: paintOrbKnobs (g); break;
        }
    }

    void mouseMove (const juce::MouseEvent& e) override { mouse = e.getPosition(); repaint(); }
    void mouseExit (const juce::MouseEvent&) override { mouse = { -1, -1 }; repaint(); }
    void mouseDown (const juce::MouseEvent& e) override
    {
        mouse = e.getPosition();
        dialDrag = (page == Smart || (page == Home && selTile == 11)) && dialRect.contains (mouse) && host.smartAcceptSet != nullptr;
        dialY0 = e.getPosition().y; dialV0 = host.smartAcceptGet ? host.smartAcceptGet() : 0.0f; dialMoved = false;
    }
    void mouseDrag (const juce::MouseEvent& e) override
    {
        if (! dialDrag) return;
        const int dy = dialY0 - e.getPosition().y;
        if (std::abs (dy) > 2) dialMoved = true;
        host.smartAcceptSet (juce::jlimit (0.0f, 1.0f, std::round ((dialV0 + (float) dy / 150.0f) * 20.0f) / 20.0f));
        repaint();
    }
    void mouseUp (const juce::MouseEvent& e) override
    {
        if (dialDrag) { dialDrag = false; if (dialMoved) return; }
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
    enum Page { Home = 0, Save, Load, Restore, RecFolder, Master, Panic, Ipad, OrbImages, LoopRec, Learn, WindowSize, Smart, Follow, Tools, OrbKnobs };
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
            case 11: // smart master: τρεις ομόκεντροι δακτύλιοι
                for (int k = 0; k < 3; ++k)
                {
                    Path a; a.addCentredArc (c.x, c.y, 6.0f + 5.0f * (float) k, 6.0f + 5.0f * (float) k, 0.0f, degreesToRadians (-130.0f), degreesToRadians (130.0f - 30.0f * (float) k), true);
                    g.strokePath (a, st);
                }
                break;
            case 12: g.setFont (font (24.0f, true)); g.drawText (juce::String::fromUTF8 ("\xe2\x86\x92"), Rectangle<float> (c.x - 20.0f, c.y - 16.0f, 40.0f, 32.0f), Justification::centred, false); break;   // Follow
            case 13: g.setFont (font (22.0f, true)); g.drawText ("iPad", Rectangle<float> (c.x - 26.0f, c.y - 14.0f, 52.0f, 28.0f), Justification::centred, false); break;
            case 16: g.setFont (font (28.0f, true)); g.drawText (juce::String::fromUTF8 ("\xe2\x86\xbb"), Rectangle<float> (c.x - 20.0f, c.y - 18.0f, 40.0f, 36.0f), Justification::centred, false); break;
            case 14: g.setFont (font (24.0f, true)); g.drawText (juce::String::fromUTF8 ("\xe2\x87\x84"), Rectangle<float> (c.x - 22.0f, c.y - 16.0f, 44.0f, 32.0f), Justification::centred, false); break;
            case 17: g.setFont (font (26.0f, true)); g.drawText ("?", Rectangle<float> (c.x - 20.0f, c.y - 18.0f, 40.0f, 36.0f), Justification::centred, false); break;
            case 15: g.drawEllipse (c.x - 11.0f, c.y - 11.0f, 22.0f, 22.0f, 2.2f); g.fillEllipse (c.x - 3.0f, c.y - 3.0f, 6.0f, 6.0f); break;
            default: break;
        }
    }

    // ---------- αρχική: πλέγμα ----------
    void paintHome (juce::Graphics& g)
    {
        using namespace juce;
        struct Tile { int icon; String title; bool hot, dimmed; std::function<void()> fn; bool card = false; };
        const bool linkAv = host.linkAvailable && host.linkAvailable();
        const bool linkOn = host.linkGet && host.linkGet();
        std::vector<Tile> tiles {
            { 0, "Audio & MIDI", false, false, [this] { if (host.openAudio) host.openAudio(); } },
            { 1, "Save set", false, false, [this] { showPage (Save); } },
            { 16, "Update set", false, ! (host.currentSetName && host.currentSetName().isNotEmpty() && host.currentSetName() != juce::String::fromUTF8 ("\xe2\x80\x93")),
              [this] { const auto n = host.currentSetName ? host.currentSetName() : juce::String(); if (n.isNotEmpty() && n != juce::String::fromUTF8 ("\xe2\x80\x93") && host.saveSetNamed) { host.saveSetNamed (n); close(); } } },
            { 2, "Load set", false, false, [this] { showPage (Load); } },
            { 3, "Restore session", false, false, [this] { showPage (Restore); } },
            { 4, "Rec Audio folder", false, false, [this] { showPage (RecFolder); } },
            { 5, "Master Output", false, false, [this] { showPage (Master); } },
            { 6, "Ableton Link", linkOn, ! linkAv, [this, linkAv] { if (linkAv && host.linkToggle) host.linkToggle(); } },
            { 7, "Panic", false, false, [this] { showPage (Panic); } },
            { 9, "Loop recording", false, false, [this] { showPage (LoopRec); } },
            { 10, "MIDI learn", false, false, [this] { showPage (Learn); } },
            { 11, "Smart master", host.smartOnGet && host.smartOnGet() != 0, false, [this] { showPage (Smart); }, true },
            { 12, "Follow", host.followOnGet && host.followOnGet() != 0, false, [this] { showPage (Follow); }, true },
            { 15, "Orb knobs", false, false, [this] { showPage (OrbKnobs); }, true },
            { 4, "Orb images", false, false, [this] { showPage (OrbImages); } },
            { 13, "iPad", host.ipadGet && host.ipadGet(), false, [this] { showPage (Ipad); } },
            { 8, "Window size", false, false, [this] { showPage (WindowSize); } },
            { 17, "Help", host.helpGet && host.helpGet(), false, [this] { if (host.helpToggle) host.helpToggle(); repaint(); } },
        };
        // κύκλοι στη μέση· το περιεχόμενο του επιλεγμένου ανοίγει αριστερά και δεξιά
        const int W = getWidth(), H = getHeight();
        dialRect = {};
        const int cols = W >= 1000 ? 5 : 4;
        const int pitch = 68;
        const int rows = ((int) tiles.size() + cols - 1) / cols;
        const int rp = jlimit (62, 96, (H - 90) / rows);
        const int d = jmin (56, rp - 24);
        const int cardW = jlimit (210, 340, (W - 80 - cols * pitch - 28) / 2);
        const int gx0 = W / 2 - (cols * pitch) / 2;
        for (int i = 0; i < (int) tiles.size(); ++i)
        {
            auto& t = tiles[(size_t) i];
            const int ccx = gx0 + (i % cols) * pitch + pitch / 2, ccy = 74 + (i / cols) * rp;
            const Rectangle<int> r (ccx - d / 2, ccy - d / 2, d, d);
            const bool isSel = i == selTile && t.card;
            const bool h = hov (r);
            g.setColour (isSel ? Colour (0xff3c5a46) : h ? Colour (0xff36322f) : card());
            g.fillEllipse (r.toFloat());
            g.setColour (isSel || t.hot ? hobby::kOn : lineC());
            g.drawEllipse (r.toFloat().reduced (1.0f), isSel ? 2.4f : t.hot ? 2.0f : 1.2f);
            const Colour ink = t.dimmed ? dim().withAlpha (0.5f) : (t.hot || isSel) ? hobby::kOn : txt();
            drawIcon (g, t.icon, { (float) ccx, (float) ccy }, ink);
            g.setColour (isSel ? txt() : t.dimmed ? dim().withAlpha (0.6f) : dim()); g.setFont (font (11.0f, true));
            g.drawText (t.title, ccx - pitch / 2, r.getBottom() + 3, pitch, 16, Justification::centred, true);
            hits.push_back ({ r.expanded (4), t.card ? std::function<void()> ([this, i] { selTile = i; repaint(); }) : t.fn });
        }
        const Rectangle<int> L (40, 44, cardW, H - 44 - 24), R (W - 40 - cardW, 44, cardW, H - 44 - 24);
        for (auto c : { L, R })
        {
            g.setColour (card()); g.fillRoundedRectangle (c.toFloat(), 18.0f);
            g.setColour (lineC()); g.drawRoundedRectangle (c.toFloat().reduced (0.5f), 18.0f, 1.0f);
        }
        if (selTile == 11) cardsSmart (g, L, R);
        else if (selTile == 12) cardsFollow (g, L, R);
        else if (selTile == 13) cardsOrb (g, L, R);
    }

    juce::Rectangle<int> cardHeader (juce::Graphics& g, juce::Rectangle<int> c, const juce::String& t)
    {
        g.setColour (txt()); g.setFont (font (16.0f, true));
        g.drawText (t, c.getX() + 20, c.getY() + 14, c.getWidth() - 40, 22, juce::Justification::centredLeft, false);
        return c.reduced (20, 0).withTrimmedTop (46).withTrimmedBottom (14);
    }
    void smallPill (juce::Graphics& g, juce::Rectangle<int> r, const juce::String& t, bool sel, std::function<void()> fn)
    {
        using namespace juce;
        const bool hv = hov (r);
        g.setColour (sel ? Colour (0xff3c5a46) : hv ? Colour (0xff423d39) : Colour (0xff36322f)); g.fillRoundedRectangle (r.toFloat(), (float) r.getHeight() * 0.5f);
        g.setColour (sel ? hobby::kOn : lineC()); g.drawRoundedRectangle (r.toFloat().reduced (0.5f), (float) r.getHeight() * 0.5f, sel ? 1.8f : 1.0f);
        g.setColour (sel ? txt() : dim()); g.setFont (font (13.0f, true));
        g.drawText (t, r, Justification::centred, true);
        hits.push_back ({ r, std::move (fn) });
    }

    void cardsSmart (juce::Graphics& g, juce::Rectangle<int> L, juce::Rectangle<int> R)
    {
        using namespace juce;
        const bool on = host.smartOnGet && host.smartOnGet() != 0;
        const int mode = host.smartModeGet ? host.smartModeGet() : 0;
        const float acc = host.smartAcceptGet ? host.smartAcceptGet() : 0.6f;
        auto c = cardHeader (g, L, "Smart master");
        const int w3 = (c.getWidth() - 12) / 3;
        g.setColour (dim()); g.setFont (font (12.0f)); g.drawText ("Mode", c.getX(), c.getY(), c.getWidth(), 16, Justification::centredLeft, false);
        smallPill (g, { c.getX(), c.getY() + 20, w3, 34 }, "Off", ! on, [this] { if (host.smartOnSet) host.smartOnSet (0); repaint(); });
        smallPill (g, { c.getX() + w3 + 6, c.getY() + 20, w3, 34 }, "Ask", on && mode == 0, [this] { if (host.smartOnSet) host.smartOnSet (1); if (host.smartModeSet) host.smartModeSet (0); repaint(); });
        smallPill (g, { c.getX() + 2 * (w3 + 6), c.getY() + 20, w3, 34 }, "Auto", on && mode == 1, [this] { if (host.smartOnSet) host.smartOnSet (1); if (host.smartModeSet) host.smartModeSet (1); repaint(); });
        g.setColour (dim()); g.drawText ("Accept corrections (drag the dial)", c.getX(), c.getY() + 76, c.getWidth(), 16, Justification::centredLeft, false);
        const int D = jlimit (70, 120, c.getHeight() - 150);
        dialRect = { c.getCentreX() - D / 2, c.getY() + 100, D, D };
        {
            Path tr, vp; const float cx = (float) dialRect.getCentreX(), cy = (float) dialRect.getCentreY(), r = (float) D * 0.5f - 6.0f;
            const float st0 = degreesToRadians (-135.0f), full = degreesToRadians (270.0f);
            tr.addCentredArc (cx, cy, r, r, 0.0f, st0, st0 + full, true);
            vp.addCentredArc (cx, cy, r, r, 0.0f, st0, st0 + full * jmax (0.003f, acc), true);
            g.setColour (Colour (0xff3c3835)); g.strokePath (tr, PathStrokeType (7.0f, PathStrokeType::curved, PathStrokeType::rounded));
            g.setColour (hobby::kOn); g.strokePath (vp, PathStrokeType (7.0f, PathStrokeType::curved, PathStrokeType::rounded));
            g.setColour (txt()); g.setFont (font (20.0f, true));
            g.drawText (String ((int) std::round (acc * 100.0f)) + "%", dialRect, Justification::centred, false);
        }
        g.setColour (dim()); g.setFont (font (11.0f));
        g.drawFittedText ("0 % = measure only", c.getX(), dialRect.getBottom() + 4, c.getWidth(), 16, Justification::centred, 1);

        // δεξιά: ανά block
        auto rc = cardHeader (g, R, "Per block");
        const auto blocks = host.smartBlocks ? host.smartBlocks() : std::vector<SmartBlock>();
        static const char* typeNames[] = { "Other", "Bass", "Keys", "Pad", "Lead", "Strings", "Drums", "FX" };
        int y = rc.getY();
        const int rowH = 40;
        bool any = false;
        for (int b = 0; b < (int) blocks.size(); ++b)
        {
            const auto& B = blocks[(size_t) b];
            if (! B.active || y + rowH > rc.getBottom() + 6) continue;
            any = true;
            g.setColour (Colour (0xff3a3632)); g.fillEllipse ((float) rc.getX(), (float) y + 5.0f, 28.0f, 28.0f);
            g.setColour (txt()); g.setFont (font (13.0f, true)); g.drawText (String (b + 1), rc.getX(), y + 5, 28, 28, Justification::centred, false);
            smallPill (g, { rc.getX() + 36, y + 5, 86, 28 }, typeNames[jlimit (0, 7, B.type)], B.type != 0, [this, b, t = B.type] { if (host.smartTypeSet) host.smartTypeSet (b, (t + 1) % 8); repaint(); });
            const bool needLow = on && B.type != 0 && B.needLow > 0.5f, needW = on && B.type != 0 && B.needW < 0.97f;
            const bool applied = B.appLow > 0.2f || B.appW < 0.98f, need = needLow || needW;
            String msg = ! on ? "off" : B.type == 0 ? "pick a type" : need ? ((needLow ? "Lows -" + String (B.needLow * acc, 1) + " dB " : String()) + (needW ? "Width " + String ((int) std::round (100.0f - (1.0f - B.needW) * acc * 100.0f)) + "%" : String())) : applied ? "correcting" : "in range";
            const bool showCheck = on && mode == 0 && B.type != 0 && (need || applied) && acc > 0.0f;
            g.setColour (need ? Colour (0xffebb45a) : dim()); g.setFont (font (11.5f));
            g.drawText (msg, rc.getX() + 130, y + 5, rc.getWidth() - 130 - (showCheck ? 40 : 0), 28, Justification::centredLeft, true);
            if (showCheck)
            {
                const Rectangle<int> k (rc.getRight() - 32, y + 5, 28, 28);
                g.setColour (applied ? Colour (0xff3c5a46) : Colour (0xff35493c)); g.fillEllipse (k.toFloat());
                g.setColour (hobby::kOn); g.drawEllipse (k.toFloat().reduced (1.0f), applied ? 2.4f : 1.6f);
                Path ck; const auto cp = k.getCentre().toFloat();
                ck.startNewSubPath (cp.x - 6.0f, cp.y); ck.lineTo (cp.x - 2.0f, cp.y + 5.0f); ck.lineTo (cp.x + 7.0f, cp.y - 5.0f);
                g.setColour (txt()); g.strokePath (ck, PathStrokeType (2.4f, PathStrokeType::curved, PathStrokeType::rounded));
                hits.push_back ({ k.expanded (3), [this, b] { if (host.smartApply) host.smartApply (b); repaint(); } });
            }
            y += rowH;
        }
        if (! any) { g.setColour (dim()); g.setFont (font (13.0f)); g.drawText ("No block has an instrument yet.", rc.getX(), rc.getY(), rc.getWidth(), 20, Justification::centredLeft, false); }
    }

    void cardsFollow (juce::Graphics& g, juce::Rectangle<int> L, juce::Rectangle<int> R)
    {
        using namespace juce;
        const bool on = host.followOnGet && host.followOnGet() != 0;
        const int root = host.followRootGet ? host.followRootGet() : 9;
        const int type = host.followTypeGet ? host.followTypeGet() : 1;
        static const char* notes[] = { "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B" };
        auto c = cardHeader (g, L, "Follow");
        const int w2 = (c.getWidth() - 8) / 2;
        smallPill (g, { c.getX(), c.getY(), w2, 34 }, "Off", ! on, [this] { if (host.followOnSet) host.followOnSet (0); repaint(); });
        smallPill (g, { c.getX() + w2 + 8, c.getY(), w2, 34 }, "On", on, [this] { if (host.followOnSet) host.followOnSet (1); repaint(); });
        g.setColour (dim()); g.setFont (font (12.0f)); g.drawText ("Scale of the loops you record", c.getX(), c.getY() + 46, c.getWidth(), 16, Justification::centredLeft, false);
        const int cw = c.getWidth() / 4;
        const int rd = jmin (36, cw - 6);
        for (int i = 0; i < 12; ++i)
        {
            const Rectangle<int> r (c.getX() + (i % 4) * cw + (cw - rd) / 2, c.getY() + 68 + (i / 4) * (rd + 6), rd, rd);
            const bool sel = i == root;
            g.setColour (sel ? Colour (0xff3c5a46) : Colour (0xff36322f)); g.fillEllipse (r.toFloat());
            g.setColour (sel ? hobby::kOn : lineC()); g.drawEllipse (r.toFloat().reduced (0.5f), sel ? 1.8f : 1.0f);
            g.setColour (sel ? txt() : dim()); g.setFont (font (13.0f, sel));
            g.drawText (notes[i], r, Justification::centred, false);
            hits.push_back ({ r, [this, i] { if (host.followRootSet) host.followRootSet (i); repaint(); } });
        }
        const int ty = c.getY() + 68 + 3 * (rd + 6) + 6;
        smallPill (g, { c.getX(), ty, w2, 34 }, "Major", type == 0, [this] { if (host.followTypeSet) host.followTypeSet (0); repaint(); });
        smallPill (g, { c.getX() + w2 + 8, ty, w2, 34 }, "Minor", type == 1, [this] { if (host.followTypeSet) host.followTypeSet (1); repaint(); });

        auto rc = cardHeader (g, R, "Blocks that follow");
        const auto blocks = host.followBlocks ? host.followBlocks() : std::vector<FollowBlock>();
        int y = rc.getY();
        bool any = false;
        for (int b = 0; b < (int) blocks.size(); ++b)
        {
            const auto& B = blocks[(size_t) b];
            if (! B.active || y + 40 > rc.getBottom() + 6) continue;
            any = true;
            g.setColour (Colour (0xff3a3632)); g.fillEllipse ((float) rc.getX(), (float) y + 5.0f, 28.0f, 28.0f);
            g.setColour (txt()); g.setFont (font (13.0f, true)); g.drawText (String (b + 1), rc.getX(), y + 5, 28, 28, Justification::centred, false);
            g.setFont (font (13.0f)); g.drawText (B.name, rc.getX() + 36, y + 5, rc.getWidth() - 36 - 70, 28, Justification::centredLeft, true);
            const Rectangle<int> sw (rc.getRight() - 58, y + 8, 54, 24);
            g.setColour (B.on ? Colour (0xff3c5a46) : Colour (0xff242220)); g.fillRoundedRectangle (sw.toFloat(), 12.0f);
            g.setColour (B.on ? hobby::kOn : lineC()); g.drawRoundedRectangle (sw.toFloat().reduced (0.5f), 12.0f, 1.5f);
            g.setColour (B.on ? txt() : dim());
            g.fillEllipse ((float) (B.on ? sw.getRight() - 22 : sw.getX() + 3), (float) sw.getY() + 3.0f, 18.0f, 18.0f);
            hits.push_back ({ sw.expanded (4), [this, b, on2 = B.on] { if (host.followBlockSet) host.followBlockSet (b, ! on2); repaint(); } });
            y += 40;
        }
        if (! any) { g.setColour (dim()); g.setFont (font (13.0f)); g.drawText ("No block has an instrument yet.", rc.getX(), rc.getY(), rc.getWidth(), 20, Justification::centredLeft, false); }
        else
        {
            g.setColour (dim()); g.setFont (font (11.0f));
            g.drawFittedText ("Hold a chord of 3+ notes. Key B0 turns Follow on and off.", rc.getX(), rc.getBottom() - 30, rc.getWidth(), 30, Justification::centredLeft, 2);
        }
    }

    void toolCircles (juce::Graphics& g, juce::Rectangle<int> c, const std::vector<ToolItem>& items, int from, int to)
    {
        using namespace juce;
        const int d = 58, cols = jmax (1, c.getWidth() / (d + 14));
        const int gx = c.getX() + (c.getWidth() - (jmin (cols, to - from) * (d + 14) - 14)) / 2;
        for (int i = from; i < to; ++i)
        {
            const auto& it = items[(size_t) i];
            const int k = i - from;
            const Rectangle<int> r (gx + (k % cols) * (d + 14), c.getY() + (k / cols) * (d + 40), d, d);
            const bool hv = it.enabled && hov (r);
            g.setColour (hv ? Colour (0xff423d39) : Colour (0xff36322f)); g.fillEllipse (r.toFloat());
            g.setColour (it.armed ? hobby::kRed : it.enabled ? it.colour : lineC()); g.drawEllipse (r.toFloat().reduced (1.0f), it.armed ? 3.0f : 1.6f);
            g.setColour (it.enabled ? (it.armed ? hobby::kRed : it.colour) : dim().withAlpha (0.4f)); g.setFont (font (22.0f, true));
            g.drawText (it.glyph, r, Justification::centred, false);
            g.setColour (dim()); g.setFont (font (11.0f));
            g.drawFittedText (it.armed ? String ("press again") : it.label, r.getX() - 10, r.getBottom() + 3, d + 20, 30, Justification::centredTop, 2);
            if (it.enabled) hits.push_back ({ r, [this, fn = it.fn] { if (fn) fn(); repaint(); } });
        }
    }
    void cardsTools (juce::Graphics& g, juce::Rectangle<int> L, juce::Rectangle<int> R)
    {
        const auto items = host.tools ? host.tools() : std::vector<ToolItem>();
        auto c = cardHeader (g, L, "Block tools");
        auto rc = cardHeader (g, R, "Remove");
        if (items.empty()) { g.setColour (dim()); g.setFont (font (13.0f)); g.drawText ("Select a block first.", c.getX(), c.getY(), c.getWidth(), 20, juce::Justification::centredLeft, false); return; }
        const int n = (int) items.size();
        toolCircles (g, c, items, 0, jmin (n, 4));        // ⇄ VST, ⇄ MIDI FX, ✕ MIDI FX, + FX
        toolCircles (g, rc, items, jmin (n, 4), n);       // ✕ FX, SP, ✕ block
        g.setColour (dim()); g.setFont (font (11.0f));
        g.drawFittedText ("Removing needs two presses.", rc.getX(), rc.getBottom() - 20, rc.getWidth(), 18, juce::Justification::centredLeft, 1);
    }

    void cardsOrb (juce::Graphics& g, juce::Rectangle<int> L, juce::Rectangle<int> R)
    {
        using namespace juce;
        const auto ks = host.orbKnobs ? host.orbKnobs() : std::vector<OrbKnob>();
        if (ks.size() < 16) return;
        for (int side = 0; side < 2; ++side)
        {
            auto c = cardHeader (g, side == 0 ? L : R, side == 0 ? "Orb rings 1-2" : "Orb rings 3-4");
            const int rowH = jmax (22, jmin (28, (c.getHeight() - 40) / 8));
            int y = c.getY();
            for (int cc = side * 2; cc < side * 2 + 2; ++cc)
            {
                g.setColour (Colour (0xff9a948b)); g.drawEllipse ((float) c.getX(), (float) y + 3.0f, 12.0f, 12.0f, 2.2f);
                g.setColour (dim()); g.setFont (font (12.0f, true)); g.drawText ("Ring " + String (cc + 1), c.getX() + 20, y, 100, 18, Justification::centredLeft, false);
                y += 20;
                for (int r = 0; r < 4; ++r)
                {
                    const int slot = cc * 4 + r;
                    const Rectangle<int> row (c.getX(), y, c.getWidth(), rowH - 3);
                    const auto& k = ks[(size_t) slot];
                    g.setColour (hov (row) ? Colour (0xff423d39) : Colour (0xff36322f)); g.fillRoundedRectangle (row.toFloat(), 8.0f);
                    g.setFont (font (12.0f));
                    if (k.bound)
                    {
                        g.setColour (txt()); g.drawText (k.name, row.withTrimmedLeft (8).withTrimmedRight (74), Justification::centredLeft, true);
                        g.setColour (dim()); g.drawText (String (roundToInt (jlimit (0.0f, 1.0f, k.value) * 100.0f)) + "%", row.withTrimmedRight (30), Justification::centredRight, false);
                        g.setColour (hov (row) ? hobby::kRed : dim()); g.drawText (String::fromUTF8 ("\xe2\x9c\x95"), row.withTrimmedRight (8), Justification::centredRight, false);
                        hits.push_back ({ row, [this, slot] { if (host.orbClear) host.orbClear (slot); repaint(); } });
                    }
                    else
                    {
                        g.setColour (dim().withAlpha (0.7f)); g.drawText (String::fromUTF8 ("+  learn"), row.withTrimmedLeft (8), Justification::centredLeft, false);
                        hits.push_back ({ row, [this, slot] { if (host.orbAssign) host.orbAssign (slot); close(); } });
                    }
                    y += rowH;
                }
                y += 6;
            }
        }
    }

    // ---------- εργαλεία του block ----------
    void paintTools (juce::Graphics& g)
    {
        using namespace juce;
        const auto items = host.tools ? host.tools() : std::vector<ToolItem>();
        if (items.empty()) { note (g, "Select a block first.", 80); return; }
        const int d = 78, gap = 34, x0 = 40, y0 = 90;
        int x = x0, y = y0;
        for (auto& it : items)
        {
            if (x + d > getWidth() - 40) { x = x0; y += d + 46; }
            const Rectangle<int> r (x, y, d, d);
            const bool hv = it.enabled && hov (r);
            g.setColour (hv ? Colour (0xff36322f) : card()); g.fillEllipse (r.toFloat());
            g.setColour (it.armed ? hobby::kRed : it.enabled ? it.colour : lineC()); g.drawEllipse (r.toFloat().reduced (1.0f), it.armed ? 3.0f : 1.6f);
            g.setColour (it.enabled ? (it.armed ? hobby::kRed : it.colour) : dim().withAlpha (0.4f)); g.setFont (font (28.0f, true));
            g.drawText (it.glyph, r, Justification::centred, false);
            g.setColour (dim()); g.setFont (font (12.0f));
            g.drawFittedText (it.armed ? String ("press again") : it.label, x - 14, r.getBottom() + 6, d + 28, 32, Justification::centredTop, 2);
            if (it.enabled) hits.push_back ({ r, [this, fn = it.fn] { if (fn) fn(); repaint(); } });
            x += d + gap;
        }
        note (g, "Instrument, MIDI FX and FX of the selected block. Removing needs two presses.", getHeight() - 28);
    }

    // ---------- αναθέσεις orb ----------
    void paintOrbKnobs (juce::Graphics& g)
    {
        using namespace juce;
        const auto ks = host.orbKnobs ? host.orbKnobs() : std::vector<OrbKnob>();
        if (ks.size() < 16) return;
        static const Colour ringC[4] { Colour (0xff9a948b), Colour (0xff9a948b), Colour (0xff9a948b), Colour (0xff9a948b) };
        const int colW = (getWidth() - 80) / 2, rowH = 24;
        for (int c = 0; c < 4; ++c)
        {
            const int bx = 40 + (c % 2) * colW, by = 76 + (c / 2) * (4 * rowH + 34);
            g.setColour (ringC[c]); g.drawEllipse ((float) bx, (float) by + 4.0f, 14.0f, 14.0f, 2.4f);
            g.setColour (dim()); g.setFont (font (12.0f, true));
            g.drawText ("Ring " + String (c + 1), bx + 22, by, 120, 22, Justification::centredLeft, false);
            for (int r = 0; r < 4; ++r)
            {
                const int slot = c * 4 + r;
                const Rectangle<int> row (bx, by + 26 + r * rowH, colW - 24, rowH - 2);
                const auto& k = ks[(size_t) slot];
                g.setColour (hov (row) ? Colour (0xff36322f) : card()); g.fillRoundedRectangle (row.toFloat(), 8.0f);
                g.setFont (font (13.0f));
                if (k.bound)
                {
                    g.setColour (txt()); g.drawText (k.name, row.withTrimmedLeft (10).withTrimmedRight (110), Justification::centredLeft, true);
                    g.setColour (dim()); g.drawText (String (roundToInt (jlimit (0.0f, 1.0f, k.value) * 100.0f)) + "%", row.withTrimmedRight (44), Justification::centredRight, false);
                    g.setColour (hov (row) ? hobby::kRed : dim()); g.drawText (String::fromUTF8 ("\xe2\x9c\x95"), row.withTrimmedRight (12), Justification::centredRight, false);
                    hits.push_back ({ row, [this, slot] { if (host.orbClear) host.orbClear (slot); repaint(); } });
                }
                else
                {
                    g.setColour (dim().withAlpha (0.7f)); g.drawText (String::fromUTF8 ("+  learn"), row.withTrimmedLeft (10), Justification::centredLeft, false);
                    hits.push_back ({ row, [this, slot] { if (host.orbAssign) host.orbAssign (slot); close(); } });
                }
            }
        }
        note (g, "Press + learn, then touch a parameter of the plugin. The ring around the orb does the same.", getHeight() - 28);
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
            if (h)   // ✕ πάνω δεξιά όταν πας το ποντίκι στην κάρτα: 1ο πάτημα οπλίζει (κόκκινο), 2ο το στέλνει στον κάδο
            {
                const Rectangle<int> xr (r.getRight() - 34, r.getY() + 8, 26, 26);
                const bool armed = delArmed == f.getFullPathName() && Time::getMillisecondCounter() < delArmedAt;
                g.setColour (armed ? Colour (0xff6a2f2f) : hov (xr) ? Colour (0xff4a4440) : Colour (0xff3a3632)); g.fillEllipse (xr.toFloat());
                g.setColour (armed ? hobby::kRed : txt());
                g.drawLine ((float) xr.getX() + 8.0f, (float) xr.getY() + 8.0f, (float) xr.getRight() - 8.0f, (float) xr.getBottom() - 8.0f, 2.0f);
                g.drawLine ((float) xr.getX() + 8.0f, (float) xr.getBottom() - 8.0f, (float) xr.getRight() - 8.0f, (float) xr.getY() + 8.0f, 2.0f);
                if (armed) { g.setFont (font (11.0f, true)); g.setColour (hobby::kRed); g.drawText ("press again to delete", r.getX() + 8, r.getY() + 136, cw - 16, 14, Justification::centred, false); }
                hits.push_back ({ xr.expanded (3), [this, f, armed]
                {
                    if (armed) { delArmed = {}; if (host.deleteSet) host.deleteSet (f); }
                    else { delArmed = f.getFullPathName(); delArmedAt = juce::Time::getMillisecondCounter() + 3000; }
                    repaint();
                } });
            }
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

    // ---------- window size ----------
    void paintWindowSize (juce::Graphics& g)
    {
        using namespace juce;
        struct Opt { const char* name; const char* dim; };
        const Opt opts[] = { { "Compact", "880 x 690" }, { "Default", "1097 x 810" }, { "Large", "1280 x 940" }, { "Free", "drag the edges" } };
        const int cur = host.windowSizeGet ? host.windowSizeGet() : 1;
        const int gap = 16, x0 = 40, y0 = 84;
        const int tw = juce::jmin (230, (getWidth() - 2 * x0 - 3 * gap) / 4);
        for (int i = 0; i < 4; ++i)
        {
            const bool fits = ! host.windowSizeFits || host.windowSizeFits (i);
            const bool on = i == cur;
            const Rectangle<int> r (x0 + i * (tw + gap), y0, tw, 110);
            g.setColour (hov (r) && fits ? Colour (0xff36322f) : card());
            g.fillRoundedRectangle (r.toFloat(), 16.0f);
            g.setColour (on ? hobby::kOn : lineC());
            g.drawRoundedRectangle (r.toFloat().reduced (1.0f), 16.0f, on ? 2.0f : 1.0f);
            g.setColour (fits ? (on ? hobby::kOn : txt()) : dim().withAlpha (0.5f));
            g.setFont (font (16.0f, true));
            g.drawText (opts[i].name, r.getX(), r.getY() + 26, r.getWidth(), 24, Justification::centred, false);
            g.setColour (dim().withAlpha (fits ? 1.0f : 0.5f)); g.setFont (font (13.0f));
            g.drawText (fits ? String (opts[i].dim) : String ("does not fit this display"), r.getX() + 6, r.getY() + 58, r.getWidth() - 12, 20, Justification::centred, false);
            if (fits) hits.push_back ({ r, [this, i] { if (host.windowSizeSet) host.windowSizeSet (i); repaint(); } });
        }
        g.setColour (dim()); g.setFont (font (12.0f));
        g.drawFittedText ("Outer window size, including the title bar. Only Free lets you resize the window by dragging.",
                          40, y0 + 130, juce::jmin (720, getWidth() - 80), 40, Justification::topLeft, 2);
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

    // ---------- smart master ----------
    static void ringArc (juce::Graphics& g, float cx, float cy, float r, float thick, float v, juce::Colour c, float lim, bool over)
    {
        using namespace juce;
        const float full = degreesToRadians (270.0f);
        // το addCentredArc μετράει τις γωνίες από την κορυφή (12:00), δεξιόστροφα· 135° από τη θέση «κάτω αριστερά»
        const float start = degreesToRadians (-135.0f);
        auto pth = [&] (float frac)
        {
            Path p; p.addCentredArc (cx, cy, r, r, 0.0f, start, start + full * frac, true); return p;
        };
        PathStrokeType st (thick, PathStrokeType::curved, PathStrokeType::rounded);
        g.setColour (Colour (0xff343130));
        g.strokePath (pth (1.0f), st);
        if (v > 0.003f) { g.setColour (over ? Colour (0xffebb45a) : c); g.strokePath (pth (juce::jlimit (0.0f, 1.0f, v)), st); }
        if (lim < 0.999f)
        {
            const float a = start + full * lim;
            const float dx = std::sin (a), dy = -std::cos (a);
            g.setColour (Colour (0xffe6e0d8));
            g.drawLine (cx + dx * (r - thick * 0.5f - 2.0f), cy + dy * (r - thick * 0.5f - 2.0f), cx + dx * (r + thick * 0.5f + 2.0f), cy + dy * (r + thick * 0.5f + 2.0f), 1.6f);
        }
    }

    void paintFollow (juce::Graphics& g)
    {
        using namespace juce;
        const Colour ok = hobby::kOn;
        const int W = getWidth(), H = getHeight();
        const bool on = host.followOnGet && host.followOnGet() != 0;
        const int root = host.followRootGet ? host.followRootGet() : 9;
        const int type = host.followTypeGet ? host.followTypeGet() : 1;
        static const char* notes[] = { "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B" };
        static const char* types[] = { "Major", "Minor", "Dorian", "Mixolydian", "Phrygian", "Lydian" };
        g.setColour (txt()); g.setFont (font (14.0f, true));
        g.drawText ("Scale of the loops you record", 40, 52, 400, 20, Justification::centredLeft, false);
        const int cwid = jmin (52, (W - 80 - 280) / 12);
        for (int i = 0; i < 12; ++i)
        {
            const Rectangle<int> r (40 + i * cwid, 78, cwid - 6, 30);
            const bool sel = i == root;
            g.setColour (sel ? Colour (0xff3c5a46) : Colour (0xff242220)); g.fillRoundedRectangle (r.toFloat(), 15.0f);
            g.setColour (sel ? ok : lineC()); g.drawRoundedRectangle (r.toFloat().reduced (0.5f), 15.0f, sel ? 1.6f : 1.0f);
            g.setColour (sel ? txt() : dim()); g.setFont (font (12.5f, sel));
            g.drawText (notes[i], r, Justification::centred, false);
            hits.push_back ({ r, [this, i] { if (host.followRootSet) host.followRootSet (i); repaint(); } });
        }
        const int tw = jmin (104, (W - 80 - 280) / 6);
        for (int i = 0; i < 6; ++i)
        {
            const Rectangle<int> r (40 + i * tw, 116, tw - 6, 30);
            const bool sel = i == type;
            g.setColour (sel ? Colour (0xff3c5a46) : Colour (0xff242220)); g.fillRoundedRectangle (r.toFloat(), 15.0f);
            g.setColour (sel ? ok : lineC()); g.drawRoundedRectangle (r.toFloat().reduced (0.5f), 15.0f, sel ? 1.6f : 1.0f);
            g.setColour (sel ? txt() : dim()); g.setFont (font (12.0f, sel));
            g.drawText (types[i], r, Justification::centred, false);
            hits.push_back ({ r, [this, i] { if (host.followTypeSet) host.followTypeSet (i); repaint(); } });
        }
        g.setColour (ok); g.setFont (font (22.0f, true));
        g.drawText (String (notes[jlimit (0, 11, root)]) + " " + String (types[jlimit (0, 5, type)]).toLowerCase(), W - 40 - 230, 52, 230, 30, Justification::centredRight, false);
        {   // γενικός διακόπτης (το ίδιο κάνει το πλήκτρο B7)
            const Rectangle<int> o (W - 40 - 150, 96, 150, 40);
            g.setColour (on ? Colour (0xff3c5a46) : Colour (0xff2a2725)); g.fillRoundedRectangle (o.toFloat(), 20.0f);
            g.setColour (on ? ok : lineC()); g.drawRoundedRectangle (o.toFloat().reduced (1.0f), 20.0f, on ? 2.0f : 1.0f);
            g.setColour (on ? txt() : dim()); g.setFont (font (14.0f, true));
            g.drawText (on ? "Follow On" : "Follow Off", o, Justification::centred, false);
            hits.push_back ({ o, [this, on] { if (host.followOnSet) host.followOnSet (on ? 0 : 1); repaint(); } });
        }
        // κάρτες block
        const auto blocks = host.followBlocks ? host.followBlocks() : std::vector<FollowBlock>();
        std::vector<int> idx;
        for (int b = 0; b < (int) blocks.size(); ++b) if (blocks[(size_t) b].active) idx.push_back (b);
        const int top = 166, bottom = H - 30;
        if (idx.empty())
        {
            g.setColour (dim()); g.setFont (font (14.0f));
            g.drawText ("No block has an instrument yet.", 40, top + 10, W - 80, 24, Justification::centredLeft, false);
        }
        else
        {
            const int n = (int) idx.size(), gap = 12;
            const int cw = jmin (260, (W - 80 - (n - 1) * gap) / n), ch = jlimit (70, 130, bottom - top);
            for (int q = 0; q < n; ++q)
            {
                const int b = idx[(size_t) q];
                const auto& B = blocks[(size_t) b];
                const Rectangle<int> c (40 + q * (cw + gap), top, cw, ch);
                g.setColour (card()); g.fillRoundedRectangle (c.toFloat(), 14.0f);
                g.setColour (B.on ? ok : lineC()); g.drawRoundedRectangle (c.toFloat().reduced (1.0f), 14.0f, B.on ? 2.0f : 1.0f);
                g.setColour (dim()); g.setFont (font (11.0f, true));
                g.drawText ("Block " + String (b + 1), c.getX() + 14, c.getY() + 10, cw - 28, 14, Justification::centredLeft, false);
                g.setColour (txt()); g.setFont (font (15.0f, true));
                g.drawText (B.name, c.getX() + 14, c.getY() + 26, cw - 100, 20, Justification::centredLeft, true);
                const Rectangle<int> sw (c.getRight() - 14 - 62, c.getY() + 18, 62, 28);
                g.setColour (B.on ? Colour (0xff3c5a46) : Colour (0xff242220)); g.fillRoundedRectangle (sw.toFloat(), 14.0f);
                g.setColour (B.on ? ok : lineC()); g.drawRoundedRectangle (sw.toFloat().reduced (0.5f), 14.0f, 1.5f);
                g.setColour (B.on ? txt() : dim());
                g.fillEllipse ((float) (B.on ? sw.getRight() - 24 : sw.getX() + 4), (float) sw.getY() + 4.0f, 20.0f, 20.0f);
                hits.push_back ({ sw.expanded (4), [this, b, B] { if (host.followBlockSet) host.followBlockSet (b, ! B.on); repaint(); } });
                g.setColour (B.on ? ok : dim()); g.setFont (font (11.5f));
                if (ch >= 90) g.drawText (B.on ? "loops follow the chord" : "loops stay as recorded", c.getX() + 14, c.getBottom() - 34, cw - 28, 18, Justification::centredLeft, true);
            }
        }
        g.setColour (dim()); g.setFont (font (11.0f));
        g.drawFittedText ("Hold a chord of 3 or more notes: its lowest note and third set the new scale. Key B7 turns Follow on and off.",
                          40, H - 24, W - 80, 16, Justification::centredLeft, 1);
    }

    void paintSmart (juce::Graphics& g)
    {
        using namespace juce;
        const Colour lowC (0xff6ea0eb), midC (0xff78c88c), highC (0xffdc96cd), amber (0xffebb45a), ok = hobby::kOn;
        const int W = getWidth(), H = getHeight();
        const bool on = host.smartOnGet && host.smartOnGet() != 0;
        const int mode = host.smartModeGet ? host.smartModeGet() : 0;
        const float acc = host.smartAcceptGet ? host.smartAcceptGet() : 0.6f;
        // πάνω λωρίδα: δείκτης ποσοστού, Ask / Auto, On
        const int sh = H < 420 ? 54 : 70;
        const Rectangle<int> strip (40, 56, W - 80, sh);
        g.setColour (card()); g.fillRoundedRectangle (strip.toFloat(), 16.0f);
        g.setColour (lineC()); g.drawRoundedRectangle (strip.toFloat().reduced (0.5f), 16.0f, 1.0f);
        dialRect = { strip.getX() + 10, strip.getY() + 6, sh - 12, sh - 12 };
        {
            Path tr, vp; const float cx = (float) dialRect.getCentreX(), cy = (float) dialRect.getCentreY(), r = (float) dialRect.getWidth() * 0.5f - 5.0f;
            const float st0 = degreesToRadians (-135.0f), full = degreesToRadians (270.0f);
            tr.addCentredArc (cx, cy, r, r, 0.0f, st0, st0 + full, true);
            vp.addCentredArc (cx, cy, r, r, 0.0f, st0, st0 + full * jmax (0.003f, acc), true);
            g.setColour (Colour (0xff3c3835)); g.strokePath (tr, PathStrokeType (5.0f, PathStrokeType::curved, PathStrokeType::rounded));
            g.setColour (ok); g.strokePath (vp, PathStrokeType (5.0f, PathStrokeType::curved, PathStrokeType::rounded));
        }
        g.setColour (txt()); g.setFont (font (14.0f, true));
        g.drawText ("Accept corrections", strip.getX() + sh + 14, strip.getY() + 8, 260, 20, Justification::centredLeft, false);
        g.setColour (dim()); g.setFont (font (11.0f));
        g.drawText ("Drag the dial. 0 % = measure only", strip.getX() + sh + 14, strip.getY() + 28, 300, 16, Justification::centredLeft, false);
        g.setColour (ok); g.setFont (font (22.0f, true));
        g.drawText (String ((int) std::round (acc * 100.0f)) + " %", strip.getX() + sh + 330, strip.getY(), 100, sh, Justification::centredLeft, false);
        {   // Ask / Auto
            const Rectangle<int> r (strip.getRight() - 330, strip.getY() + (sh - 40) / 2, 150, 40);
            g.setColour (Colour (0xff242220)); g.fillRoundedRectangle (r.toFloat(), 20.0f);
            g.setColour (lineC()); g.drawRoundedRectangle (r.toFloat().reduced (0.5f), 20.0f, 1.0f);
            for (int k = 0; k < 2; ++k)
            {
                const Rectangle<int> h (r.getX() + 2 + k * 73, r.getY() + 2, 73, 36);
                if (mode == k) { g.setColour (Colour (0xff3c5a46)); g.fillRoundedRectangle (h.toFloat(), 18.0f); }
                g.setColour (mode == k ? txt() : dim()); g.setFont (font (13.0f, true));
                g.drawText (k == 0 ? "Ask" : "Auto", h, Justification::centred, false);
                hits.push_back ({ h, [this, k] { if (host.smartModeSet) host.smartModeSet (k); repaint(); } });
            }
            const Rectangle<int> o (strip.getRight() - 160, strip.getY() + (sh - 40) / 2, 150, 40);
            g.setColour (on ? Colour (0xff3c5a46) : Colour (0xff2a2725)); g.fillRoundedRectangle (o.toFloat(), 20.0f);
            g.setColour (on ? ok : lineC()); g.drawRoundedRectangle (o.toFloat().reduced (1.0f), 20.0f, on ? 2.0f : 1.0f);
            g.setColour (on ? txt() : dim()); g.setFont (font (14.0f, true));
            g.drawText (on ? "On" : "Off", o, Justification::centred, false);
            hits.push_back ({ o, [this, on] { if (host.smartOnSet) host.smartOnSet (on ? 0 : 1); repaint(); } });
        }

        // κάρτες block (μόνο όσα έχουν plugin)
        const auto blocks = host.smartBlocks ? host.smartBlocks() : std::vector<SmartBlock>();
        std::vector<int> idx;
        for (int b = 0; b < (int) blocks.size(); ++b) if (blocks[(size_t) b].active) idx.push_back (b);
        const int top = strip.getBottom() + 10, bottom = H - 26;
        if (idx.empty())
        {
            g.setColour (dim()); g.setFont (font (14.0f));
            g.drawText ("No block has an instrument yet.", 40, top + 20, W - 80, 24, Justification::centredLeft, false);
            return;
        }
        const int n = (int) idx.size(), gap = 12;
        const int cw = jmin (260, (W - 80 - (n - 1) * gap) / n), ch = bottom - top;
        static const char* typeNames[] = { "Other", "Bass", "Keys", "Pad", "Lead", "Strings", "Drums", "FX" };
        const bool compact = ch < 300;
        for (int q = 0; q < n; ++q)
        {
            const int b = idx[(size_t) q];
            const auto& B = blocks[(size_t) b];
            const Rectangle<int> c (40 + q * (cw + gap), top, cw, ch);
            g.setColour (card()); g.fillRoundedRectangle (c.toFloat(), 16.0f);
            g.setColour (lineC()); g.drawRoundedRectangle (c.toFloat().reduced (0.5f), 16.0f, 1.0f);
            int y = c.getY() + 8;
            if (! compact)
            {
                g.setColour (dim()); g.setFont (font (11.0f, true));
                g.drawText ("Block " + String (b + 1), c.getX() + 12, y, cw - 24, 14, Justification::centredLeft, false);
                g.setColour (txt()); g.setFont (font (15.0f, true));
                g.drawText (B.name, c.getX() + 12, y + 14, cw - 24, 20, Justification::centredLeft, true);
                y += 38;
            }
            // τύπος ήχου: ένα κουμπί που αλλάζει με κάθε πάτημα
            const Rectangle<int> tp (c.getX() + 10, y, cw - 20, 26);
            const bool typed = B.type != 0;
            g.setColour (typed ? Colour (0xff3c5a46) : Colour (0xff242220)); g.fillRoundedRectangle (tp.toFloat(), 13.0f);
            g.setColour (typed ? ok : lineC()); g.drawRoundedRectangle (tp.toFloat().reduced (0.5f), 13.0f, typed ? 1.6f : 1.0f);
            g.setColour (typed ? txt() : dim()); g.setFont (font (12.5f, true));
            g.drawText (String (b + 1) + "  " + String (typeNames[jlimit (0, 7, B.type)]), tp, Justification::centred, false);
            hits.push_back ({ tp, [this, b, B] { if (host.smartTypeSet) host.smartTypeSet (b, (B.type + 1) % 8); repaint(); } });
            y += 34;

            // κάτω μέρος: πλάτος stereo + πρόταση
            const int sugH = compact ? 40 : 52, stH = compact ? 34 : 40;
            const int ringTop = y, ringBottom = c.getBottom() - sugH - stH - 14;
            const int D = jlimit (48, cw - 20, ringBottom - ringTop);
            const float rcx = (float) c.getCentreX(), rcy = (float) ringTop + (float) (ringBottom - ringTop) * 0.5f;
            const float R0 = (float) D * 0.5f, th = jmax (3.5f, R0 * 0.16f);
            const bool dim0 = ! on || B.type == 0;
            auto over = [&] (float v, float lm) { return on && B.type != 0 && v > lm; };
            ringArc (g, rcx, rcy, R0 - th * 0.5f,              th, B.low,  lowC,  dim0 ? 1.0f : B.lim[0], over (B.low,  B.lim[0]));
            ringArc (g, rcx, rcy, R0 - th * 1.7f,              th, B.mid,  midC,  dim0 ? 1.0f : B.lim[1], over (B.mid,  B.lim[1]));
            ringArc (g, rcx, rcy, R0 - th * 2.9f,              th, B.high, highC, dim0 ? 1.0f : B.lim[2], over (B.high, B.lim[2]));

            // πλάτος: κύκλος με γεμάτο σχήμα
            const float sr = (float) (stH - 12) * 0.5f + 2.0f;
            const float scx = (float) c.getX() + 12.0f + sr, scy = (float) (ringBottom + 6 + stH / 2);
            const bool wOver = over (B.width, B.lim[3]);
            g.setColour (Colour (0xff4a4642)); g.drawEllipse (scx - sr, scy - sr, sr * 2.0f, sr * 2.0f, 1.6f);
            {
                const float ew = jmax (1.5f, B.width * sr);
                g.setColour (wOver ? amber : midC);
                g.fillEllipse (scx - ew, scy - sr + 2.0f, ew * 2.0f, sr * 2.0f - 4.0f);
                if (on && B.type != 0 && B.lim[3] < 0.999f)
                {
                    const float lw = B.lim[3] * sr;
                    g.setColour (txt());
                    g.drawLine (scx - lw, scy - sr - 2.0f, scx - lw, scy + sr + 2.0f, 1.4f);
                    g.drawLine (scx + lw, scy - sr - 2.0f, scx + lw, scy + sr + 2.0f, 1.4f);
                }
            }
            g.setColour (wOver ? amber : txt()); g.setFont (font (compact ? 14.0f : 17.0f, true));
            g.drawText (String ((int) std::round (B.width * 100.0f)) + " %", (int) (scx + sr + 8.0f), (int) (scy - 12.0f), cw - (int) (sr * 2.0f) - 34, 24, Justification::centredLeft, false);

            // πρόταση / κατάσταση
            const Rectangle<int> sg (c.getX() + 8, c.getBottom() - sugH - 8, cw - 16, sugH);
            const bool needLow = on && B.type != 0 && B.needLow > 0.5f, needW = on && B.type != 0 && B.needW < 0.97f;
            const bool applied = B.appLow > 0.2f || B.appW < 0.98f;
            const bool need = needLow || needW;
            g.setColour (Colour (0xff242220)); g.fillRoundedRectangle (sg.toFloat(), 12.0f);
            g.setColour (need ? amber : lineC()); g.drawRoundedRectangle (sg.toFloat().reduced (0.5f), 12.0f, need ? 1.5f : 1.0f);
            g.setFont (font (11.5f, true));
            String l1, l2;
            if (! on || B.type == 0) { l1 = B.type == 0 ? "Pick a sound type" : "Smart master is off"; }
            else if (need)
            {
                if (needLow) l2 = "Lows -" + String (B.needLow * acc, 1) + " dB";
                if (needW) l2 += String (l2.isEmpty() ? "" : "   ") + "Width " + String ((int) std::round (100.0f - (1.0f - B.needW) * acc * 100.0f)) + " %";
                l1 = String (typeNames[B.type]) + ": over the limit";
            }
            else l1 = applied ? "Correcting" : "In range";
            g.setColour (need ? amber : dim());
            g.drawText (l1, sg.getX() + 10, sg.getY() + 5, sg.getWidth() - 56, 16, Justification::centredLeft, true);
            if (l2.isNotEmpty()) { g.setColour (txt()); g.setFont (font (11.5f)); g.drawText (l2, sg.getX() + 10, sg.getY() + 22, sg.getWidth() - 56, 16, Justification::centredLeft, true); }
            if (on && mode == 0 && B.type != 0 && (need || applied) && acc > 0.0f)
            {
                const Point<float> bc ((float) sg.getRight() - 24.0f, (float) sg.getCentreY());
                g.setColour (applied ? Colour (0xff3c5a46) : Colour (0xff35493c)); g.fillEllipse (bc.x - 15.0f, bc.y - 15.0f, 30.0f, 30.0f);
                g.setColour (ok); g.drawEllipse (bc.x - 15.0f, bc.y - 15.0f, 30.0f, 30.0f, applied ? 2.4f : 1.6f);
                Path ck; ck.startNewSubPath (bc.x - 6.0f, bc.y); ck.lineTo (bc.x - 2.0f, bc.y + 5.0f); ck.lineTo (bc.x + 7.0f, bc.y - 5.0f);
                g.setColour (txt()); g.strokePath (ck, PathStrokeType (2.6f, PathStrokeType::curved, PathStrokeType::rounded));
                hits.push_back ({ Rectangle<int> ((int) bc.x - 18, (int) bc.y - 18, 36, 36), [this, b] { if (host.smartApply) host.smartApply (b); repaint(); } });
            }
        }
        // υπόμνημα
        {
            int x = 40; const int y = H - 20;
            const char* nm[] = { "Low", "Mid", "High" }; const Colour cc[] = { lowC, midC, highC };
            g.setFont (font (11.0f));
            for (int k = 0; k < 3; ++k)
            {
                g.setColour (cc[k]); g.fillEllipse ((float) x, (float) y + 3.0f, 9.0f, 9.0f);
                g.setColour (dim()); g.drawText (nm[k], x + 14, y, 40, 15, Justification::centredLeft, false);
                x += 62;
            }
            g.setColour (dim());
            g.drawText ("Ring = level. White tick = limit of the sound type. Amber = over the limit. Circle = stereo width.", x + 10, y, W - x - 50, 15, Justification::centredLeft, true);
        }
    }

    void timerCallback() override { if (isVisible()) repaint(); }

    juce::Typeface::Ptr typeface;
    juce::TextEditor nameEditor;
    std::vector<Hit> hits;
    juce::Point<int> mouse { -1, -1 };
    bool direct = false;
    juce::Rectangle<int> dialRect;
    bool dialDrag = false, dialMoved = false;
    int dialY0 = 0;
    float dialV0 = 0.0f;
    Page page = Home;
    int loadScroll = 0, loadMaxScroll = 0;
    int selTile = 11;
    juce::String delArmed; juce::uint32 delArmedAt = 0;   // ✕ διαγραφής set: οπλισμένο για 3 δλ
    bool saved = false, waitPanic = false;
    juce::String ccAtAssign;
};
