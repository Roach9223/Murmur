#pragma once

#include "imgui.h"
#include <string>

// Dual-theme system.
//
// "Classic"  — the original Murmur look, preserved deliberately: multi-hued
//              buttons (blue / purple / green), 3px rounding, flat banner.
// "Studio"   — the designed identity: one teal accent for everything
//              interactive, semantic red/green/amber reserved for meaning,
//              softer rounding, gradient banner, pill-shaped mode buttons.
//
// Switch live via View → Theme; the choice persists in ui_settings.json
// next to Murmur.exe.

struct ButtonStyle {
    ImVec4 base, hover, active;
};

struct Theme {
    const char* name = "classic";

    // Background layers (darkest → most elevated) + chrome
    ImVec4 Bg0, Bg1, Bg2, Border;
    ImVec4 Text, TextDim;

    // Semantic status colors — meaning-bearing in BOTH themes
    ImVec4 Success, Danger, Warning, Info, CmdText;

    // Brand accent scalars (selected pill fill, section ticks, washes)
    ImVec4 Accent, AccentDim, AccentWash;

    // Button trios. Studio derives all interactive ones from Accent;
    // Classic keeps its original per-category variety.
    ButtonStyle BtnAccent;    // primary actions: Update, A2T, Edit, Download
    ButtonStyle BtnAlt;       // secondary toggles: media key, system audio
    ButtonStyle BtnNeutral;   // idle/off buttons
    ButtonStyle BtnSuccess;   // approve, restart&update, unlocked
    ButtonStyle BtnDanger;    // reject, stop-rec
    ButtonStyle BtnWarning;   // locked sliders

    // Meter / draw-list colors (meaning-bearing, shared values in both themes)
    ImU32 VuGreen, VuYellow, VuRed;
    ImU32 ThresholdStrong, ThresholdWeak;
    ImU32 PeakLine;
    ImU32 PanelBg, PanelInset;
    ImU32 GridLine, GridLabel;

    // Engine phase palette (spectrum, status indicator, banner tint)
    enum Phase { P_Idle, P_Listening, P_Recording, P_Transcribing,
                 P_Cleaning, P_Typing, P_Pending, P_Error, P_COUNT };
    ImVec4 PhaseColors[P_COUNT];

    // Studio-only structural polish (false in Classic → renders as today)
    bool  FancyBanner = false;   // gradient banner + recording glow border
    bool  PillMode    = false;   // rounded pill mode buttons
    bool  OutputCard  = false;   // Heard/Typed in a bordered card
    float Rounding    = 3.0f;

    // --- methods ---
    void Apply(ImGuiStyle& style) const;

    static Phase PhaseFromString(const std::string& s);
    ImVec4 PhaseColor(Phase p) const { return PhaseColors[p]; }
    ImVec4 PhaseColor(const std::string& s) const { return PhaseColors[PhaseFromString(s)]; }

    struct PhaseDraw { ImU32 line, fill, peak, glow; };
    PhaseDraw PhaseDrawColors(const std::string& s) const;

    // Button helpers — each pushes exactly 3 colors; pair with PopButton()
    void PushButton(const ButtonStyle& b) const;
    void PushAccentButton() const  { PushButton(BtnAccent); }
    void PushAltButton() const     { PushButton(BtnAlt); }
    void PushNeutralButton() const { PushButton(BtnNeutral); }
    void PushSuccessButton() const { PushButton(BtnSuccess); }
    void PushDangerButton() const  { PushButton(BtnDanger); }
    void PushWarningButton() const { PushButton(BtnWarning); }
    void PushToggleButton(bool on) const { PushButton(on ? BtnAccent : BtnNeutral); }
    // Pulsing button (capture modes, banner states): same color all 3 states
    void PushPulseButton(const ImVec4& c) const;
    static void PopButton();  // PopStyleColor(3)

    // Draw-list helper
    static ImU32 U32(const ImVec4& c, float alphaMul = 1.0f);
};

Theme MakeClassicTheme();
Theme MakeStudioTheme();

// Active theme — UI thread only
extern Theme g_theme;

// ui_settings.json persistence (file lives next to Murmur.exe)
std::string LoadThemePreference();                 // "classic" | "studio"
void        SaveThemePreference(const std::string& name);
void        SetActiveTheme(const std::string& name);  // fills g_theme + Apply()
