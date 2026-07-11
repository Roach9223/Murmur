#include "theme.h"

#include <nlohmann/json.hpp>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <cmath>
#include <filesystem>
#include <fstream>

namespace fs = std::filesystem;

Theme g_theme;  // filled by SetActiveTheme() at startup

// --- color math -------------------------------------------------------------

static ImVec4 Lighten(const ImVec4& c, float amt)
{
    return ImVec4(c.x + (1.0f - c.x) * amt,
                  c.y + (1.0f - c.y) * amt,
                  c.z + (1.0f - c.z) * amt, c.w);
}

static ImVec4 Darken(const ImVec4& c, float amt)
{
    return ImVec4(c.x * (1.0f - amt), c.y * (1.0f - amt), c.z * (1.0f - amt), c.w);
}

static ImVec4 WithAlpha(const ImVec4& c, float a)
{
    return ImVec4(c.x, c.y, c.z, a);
}

static ButtonStyle Derive(const ImVec4& base)
{
    return { base, Lighten(base, 0.12f), Darken(base, 0.25f) };
}

ImU32 Theme::U32(const ImVec4& c, float alphaMul)
{
    return ImGui::ColorConvertFloat4ToU32(ImVec4(c.x, c.y, c.z, c.w * alphaMul));
}

// --- shared meter values (meaning-bearing in both themes) --------------------

static void FillSharedMeters(Theme& t)
{
    t.VuGreen         = IM_COL32(40, 200, 60, 220);
    t.VuYellow        = IM_COL32(230, 200, 40, 220);
    t.VuRed           = IM_COL32(220, 50, 40, 220);
    t.ThresholdStrong = IM_COL32(255, 220, 50, 220);
    t.ThresholdWeak   = IM_COL32(255, 220, 50, 100);
    t.PeakLine        = IM_COL32(255, 255, 255, 180);
    t.PanelBg         = IM_COL32(18, 18, 20, 255);
    t.PanelInset      = IM_COL32(30, 30, 30, 255);
    t.GridLine        = IM_COL32(255, 255, 255, 15);
    t.GridLabel       = IM_COL32(255, 255, 255, 45);
}

static void FillSharedPhases(Theme& t)
{
    t.PhaseColors[Theme::P_Idle]         = ImVec4(0.42f, 0.42f, 0.46f, 1.0f);
    t.PhaseColors[Theme::P_Listening]    = ImVec4(0.30f, 0.50f, 1.00f, 1.0f);
    t.PhaseColors[Theme::P_Recording]    = ImVec4(1.00f, 0.60f, 0.20f, 1.0f);
    t.PhaseColors[Theme::P_Transcribing] = ImVec4(0.70f, 0.40f, 1.00f, 1.0f);
    t.PhaseColors[Theme::P_Cleaning]     = ImVec4(0.20f, 0.90f, 0.90f, 1.0f);
    t.PhaseColors[Theme::P_Typing]       = ImVec4(0.20f, 0.90f, 0.30f, 1.0f);
    t.PhaseColors[Theme::P_Pending]      = ImVec4(1.00f, 0.80f, 0.10f, 1.0f);
    t.PhaseColors[Theme::P_Error]        = ImVec4(1.00f, 0.30f, 0.30f, 1.0f);
}

// --- presets -----------------------------------------------------------------

Theme MakeClassicTheme()
{
    Theme t;
    t.name = "classic";

    t.Bg0     = ImVec4(0.06f, 0.06f, 0.07f, 1.0f);
    t.Bg1     = ImVec4(0.07f, 0.07f, 0.08f, 1.0f);
    t.Bg2     = ImVec4(0.11f, 0.12f, 0.14f, 1.0f);
    t.Border  = ImVec4(0.18f, 0.19f, 0.22f, 0.60f);
    t.Text    = ImVec4(0.92f, 0.93f, 0.94f, 1.0f);
    t.TextDim = ImVec4(0.42f, 0.44f, 0.47f, 1.0f);

    t.Success = ImVec4(0.20f, 0.90f, 0.30f, 1.0f);
    t.Danger  = ImVec4(1.00f, 0.35f, 0.35f, 1.0f);
    t.Warning = ImVec4(0.95f, 0.75f, 0.20f, 1.0f);
    t.Info    = ImVec4(0.40f, 0.80f, 1.00f, 1.0f);
    t.CmdText = ImVec4(0.95f, 0.75f, 0.30f, 1.0f);

    // Classic accent = the teal that was always half-present in the base style
    t.Accent     = ImVec4(0.24f, 0.78f, 0.78f, 1.0f);
    t.AccentDim  = Darken(t.Accent, 0.35f);
    t.AccentWash = WithAlpha(t.Accent, 0.14f);

    // Original per-category button hues, preserved
    t.BtnAccent  = { ImVec4(0.15f, 0.35f, 0.55f, 1.0f),   // the blues
                     ImVec4(0.20f, 0.45f, 0.65f, 1.0f),
                     ImVec4(0.10f, 0.25f, 0.45f, 1.0f) };
    t.BtnAlt     = { ImVec4(0.55f, 0.25f, 0.60f, 1.0f),   // the purples
                     ImVec4(0.65f, 0.35f, 0.70f, 1.0f),
                     ImVec4(0.45f, 0.15f, 0.50f, 1.0f) };
    t.BtnNeutral = { ImVec4(0.25f, 0.25f, 0.25f, 1.0f),
                     ImVec4(0.35f, 0.35f, 0.35f, 1.0f),
                     ImVec4(0.20f, 0.20f, 0.20f, 1.0f) };
    t.BtnSuccess = { ImVec4(0.15f, 0.55f, 0.15f, 1.0f),
                     ImVec4(0.20f, 0.65f, 0.20f, 1.0f),
                     ImVec4(0.10f, 0.45f, 0.10f, 1.0f) };
    t.BtnDanger  = { ImVec4(0.65f, 0.08f, 0.08f, 1.0f),
                     ImVec4(0.70f, 0.10f, 0.10f, 1.0f),
                     ImVec4(0.40f, 0.05f, 0.05f, 1.0f) };
    t.BtnWarning = { ImVec4(0.55f, 0.42f, 0.12f, 1.0f),
                     ImVec4(0.65f, 0.52f, 0.18f, 1.0f),
                     ImVec4(0.45f, 0.34f, 0.08f, 1.0f) };

    FillSharedMeters(t);
    FillSharedPhases(t);

    t.FancyBanner = false;
    t.PillMode    = false;
    t.OutputCard  = false;
    t.Rounding    = 3.0f;
    return t;
}

Theme MakeStudioTheme()
{
    Theme t = MakeClassicTheme();
    t.name = "studio";

    // Slightly deeper, bluer base for contrast with the teal
    t.Bg0    = ImVec4(0.055f, 0.060f, 0.068f, 1.0f);
    t.Bg1    = ImVec4(0.080f, 0.086f, 0.096f, 1.0f);
    t.Bg2    = ImVec4(0.115f, 0.125f, 0.140f, 1.0f);
    t.Border = ImVec4(0.20f, 0.22f, 0.25f, 0.55f);

    // One accent to rule them all: teal #3DC8C8
    t.Accent     = ImVec4(0.24f, 0.78f, 0.78f, 1.0f);
    t.AccentDim  = Darken(t.Accent, 0.35f);
    t.AccentWash = WithAlpha(t.Accent, 0.14f);

    // All interactive buttons derive from the accent (darkened base so white
    // text stays readable; hover brightens toward the pure accent)
    ImVec4 accentBtn = ImVec4(0.13f, 0.42f, 0.42f, 1.0f);
    t.BtnAccent = { accentBtn, Lighten(accentBtn, 0.18f), Darken(accentBtn, 0.25f) };
    t.BtnAlt    = t.BtnAccent;
    t.BtnNeutral = { ImVec4(0.16f, 0.17f, 0.20f, 1.0f),
                     ImVec4(0.22f, 0.24f, 0.28f, 1.0f),
                     ImVec4(0.12f, 0.13f, 0.16f, 1.0f) };
    t.BtnSuccess = Derive(ImVec4(0.14f, 0.52f, 0.24f, 1.0f));
    t.BtnDanger  = Derive(ImVec4(0.62f, 0.14f, 0.14f, 1.0f));
    t.BtnWarning = Derive(ImVec4(0.55f, 0.42f, 0.12f, 1.0f));

    // Listening phase adopts the brand accent (was blue)
    t.PhaseColors[Theme::P_Listening] = t.Accent;
    // Cleaning keeps a distinguishable cyan-white so it doesn't vanish into accent
    t.PhaseColors[Theme::P_Cleaning]  = ImVec4(0.55f, 0.95f, 0.95f, 1.0f);

    t.FancyBanner = true;
    t.PillMode    = true;
    t.OutputCard  = true;
    t.ModernFX    = true;
    t.Rounding    = 5.0f;
    return t;
}

// --- ImGui style application --------------------------------------------------

void Theme::Apply(ImGuiStyle& s) const
{
    // Geometry
    s.WindowRounding    = Rounding + 1.0f;
    s.ChildRounding     = Rounding;
    s.FrameRounding     = Rounding;
    s.PopupRounding     = Rounding + 1.0f;
    s.ScrollbarRounding = 6.0f;
    s.GrabRounding      = Rounding;
    s.TabRounding       = Rounding;

    s.WindowPadding     = ImVec2(10, 10);
    s.FramePadding      = FancyBanner ? ImVec2(10, 5) : ImVec2(8, 4);
    s.ItemSpacing       = FancyBanner ? ImVec2(8, 6) : ImVec2(8, 5);
    s.ItemInnerSpacing  = ImVec2(6, 4);
    s.ScrollbarSize     = 12.0f;
    s.GrabMinSize       = 8.0f;

    s.WindowBorderSize  = 1.0f;
    s.ChildBorderSize   = 1.0f;
    s.PopupBorderSize   = 1.0f;
    s.FrameBorderSize   = 0.0f;
    s.TabBorderSize     = 0.0f;
    s.SeparatorTextBorderSize = 2.0f;

    ImVec4* c = s.Colors;
    ImVec4 frameHover = Lighten(Bg2, 0.06f);
    ImVec4 frameActive = Lighten(Bg2, 0.12f);

    c[ImGuiCol_Text]                 = Text;
    c[ImGuiCol_TextDisabled]         = TextDim;
    c[ImGuiCol_WindowBg]             = Bg0;
    c[ImGuiCol_ChildBg]              = Bg1;
    c[ImGuiCol_PopupBg]              = WithAlpha(Bg1, 0.98f);
    c[ImGuiCol_Border]               = Border;
    c[ImGuiCol_BorderShadow]         = ImVec4(0, 0, 0, 0);
    c[ImGuiCol_FrameBg]              = Bg2;
    c[ImGuiCol_FrameBgHovered]       = frameHover;
    c[ImGuiCol_FrameBgActive]        = frameActive;
    c[ImGuiCol_TitleBg]              = Darken(Bg0, 0.15f);
    c[ImGuiCol_TitleBgActive]        = Bg1;
    c[ImGuiCol_TitleBgCollapsed]     = WithAlpha(Darken(Bg0, 0.15f), 0.5f);
    c[ImGuiCol_MenuBarBg]            = Lighten(Bg0, 0.02f);
    c[ImGuiCol_ScrollbarBg]          = WithAlpha(Bg0, 0.5f);
    c[ImGuiCol_ScrollbarGrab]        = ImVec4(0.22f, 0.23f, 0.26f, 1.0f);
    c[ImGuiCol_ScrollbarGrabHovered] = ImVec4(0.30f, 0.32f, 0.36f, 1.0f);
    c[ImGuiCol_ScrollbarGrabActive]  = ImVec4(0.38f, 0.40f, 0.45f, 1.0f);
    c[ImGuiCol_CheckMark]            = Accent;
    c[ImGuiCol_SliderGrab]           = Darken(Accent, 0.10f);
    c[ImGuiCol_SliderGrabActive]     = Lighten(Accent, 0.10f);
    c[ImGuiCol_Button]               = BtnNeutral.base;
    c[ImGuiCol_ButtonHovered]        = BtnNeutral.hover;
    c[ImGuiCol_ButtonActive]         = WithAlpha(Accent, 0.60f);
    c[ImGuiCol_Header]               = Bg2;
    c[ImGuiCol_HeaderHovered]        = frameHover;
    c[ImGuiCol_HeaderActive]         = frameActive;
    c[ImGuiCol_Separator]            = Border;
    c[ImGuiCol_SeparatorHovered]     = WithAlpha(Accent, 0.60f);
    c[ImGuiCol_SeparatorActive]      = Accent;
    c[ImGuiCol_ResizeGrip]           = WithAlpha(ImVec4(0.22f, 0.23f, 0.26f, 1.0f), 0.5f);
    c[ImGuiCol_ResizeGripHovered]    = WithAlpha(Accent, 0.60f);
    c[ImGuiCol_ResizeGripActive]     = Accent;
    c[ImGuiCol_Tab]                  = Bg2;
    c[ImGuiCol_TabHovered]           = WithAlpha(Accent, 0.40f);
    c[ImGuiCol_TabSelected]          = Darken(Accent, 0.35f);
    c[ImGuiCol_TableHeaderBg]        = Lighten(Bg1, 0.03f);
    c[ImGuiCol_TableBorderStrong]    = WithAlpha(Border, 1.0f);
    c[ImGuiCol_TableBorderLight]     = ImVec4(0.14f, 0.15f, 0.17f, 1.0f);
    c[ImGuiCol_TableRowBg]           = ImVec4(0, 0, 0, 0);
    c[ImGuiCol_TableRowBgAlt]        = ImVec4(0.10f, 0.10f, 0.12f, 0.40f);
    c[ImGuiCol_TextSelectedBg]       = WithAlpha(Accent, 0.30f);
    c[ImGuiCol_NavHighlight]         = Accent;
    c[ImGuiCol_ModalWindowDimBg]     = ImVec4(0, 0, 0, 0.60f);
    if (ModernFX) {
        // Progress bars + plots pick up the brand accent in Studio
        c[ImGuiCol_PlotHistogram]        = Darken(Accent, 0.10f);
        c[ImGuiCol_PlotHistogramHovered] = Accent;
    }
}

// --- phases -------------------------------------------------------------------

Theme::Phase Theme::PhaseFromString(const std::string& s)
{
    if (s == "listening")        return P_Listening;
    if (s == "recording")        return P_Recording;
    if (s == "transcribing")     return P_Transcribing;
    if (s == "cleaning")         return P_Cleaning;
    if (s == "typing")           return P_Typing;
    if (s == "pending_approval") return P_Pending;
    if (s == "error")            return P_Error;
    return P_Idle;
}

Theme::PhaseDraw Theme::PhaseDrawColors(const std::string& s) const
{
    ImVec4 base = PhaseColors[PhaseFromString(s)];
    PhaseDraw d;
    d.line = U32(base, 0.85f);
    d.fill = U32(base, 0.13f);
    d.peak = U32(Lighten(base, 0.35f), 0.50f);
    d.glow = U32(base, 0.25f);
    return d;
}

// --- button helpers -------------------------------------------------------------

void Theme::PushButton(const ButtonStyle& b) const
{
    ImGui::PushStyleColor(ImGuiCol_Button, b.base);
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, b.hover);
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, b.active);
}

void Theme::PushPulseButton(const ImVec4& c) const
{
    ImGui::PushStyleColor(ImGuiCol_Button, c);
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, c);
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, c);
}

void Theme::PopButton()
{
    ImGui::PopStyleColor(3);
}

// --- persistence ------------------------------------------------------------------

static fs::path SettingsPath()
{
    wchar_t exePath[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, exePath, MAX_PATH);
    return fs::path(exePath).parent_path() / "ui_settings.json";
}

std::string LoadThemePreference()
{
    try {
        std::ifstream f(SettingsPath());
        if (f) {
            nlohmann::json j = nlohmann::json::parse(f, nullptr, false);
            if (j.is_object() && j.value("theme", "") == "classic")
                return "classic";  // explicitly chosen backup look
        }
    } catch (...) {}
    return "studio";  // the default Murmur identity
}

void SaveThemePreference(const std::string& name)
{
    try {
        nlohmann::json j;
        std::ifstream in(SettingsPath());
        if (in) {
            j = nlohmann::json::parse(in, nullptr, false);
            if (!j.is_object()) j = nlohmann::json::object();
            in.close();
        }
        j["theme"] = name;
        std::ofstream out(SettingsPath(), std::ios::trunc);
        out << j.dump(2) << "\n";
    } catch (...) {}
}

void SetActiveTheme(const std::string& name)
{
    g_theme = (name == "studio") ? MakeStudioTheme() : MakeClassicTheme();
    g_theme.Apply(ImGui::GetStyle());
}
