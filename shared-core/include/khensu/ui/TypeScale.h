#pragma once

namespace khensu::type {

/// UXR-8: the one named type scale (px), shared by the desktop app
/// (KhensuTheme), the plugin editor and the shared-core widgets (ui::Palette).
/// Every font size is one of these, passed to a font helper
/// (`KhensuTheme::uiFont(type::body)`, `ui::Palette::uiFont(type::caption)`);
/// scripts/check_doc_drift.py fails on a literal font height outside the theme
/// files and on any literal under the 11 px floor. Buttons keep their own
/// 12 px floor (UX-4, KhensuLookAndFeel::getTextButtonFont).
inline constexpr float caption = 11.0f;   // labels, axes, captions - the floor
inline constexpr float secondary = 12.0f; // secondary text, chips, table cells
inline constexpr float body = 13.0f;      // body copy
inline constexpr float large = 14.0f;     // emphasised body, list titles
inline constexpr float subtitle = 15.0f;  // card / overlay titles
inline constexpr float title = 17.0f;     // screen and card titles, small readouts
inline constexpr float heading = 20.0f;   // screen headers, secondary readouts
inline constexpr float headline = 24.0f;  // large readouts
inline constexpr float display = 28.0f;   // hero numeric readouts
inline constexpr float hero = 40.0f;      // the one score / verdict figure

} // namespace khensu::type
