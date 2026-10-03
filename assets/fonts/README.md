# Sodium React

Original rounded sans-serif for Magnesium Client, version 1.0.

All five weights are bundled as TTF files: Light (300), Regular (400), Medium
(500), Semibold (600), and Bold (700), each with a matching slanted italic.
The app uses Regular for controls and chat text, and Semibold for headings.
Fonts load from assets/fonts/ beside the executable; CMake copies this directory
when building. Keep this folder with the executable when distributing the app.

The font covers 306 Western Latin letters, punctuation, and symbols. Italics
are slanted companions; outlines are unhinted. Emoji and other scripts need a
separate font/fallback implementation. All available codepoints are loaded into
both active font atlases. If a font is missing, the app uses raylib's default font.

See LICENSE-Sodium-React.txt for the MIT license; include it when redistributing.
