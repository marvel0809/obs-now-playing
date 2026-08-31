#pragma once

#include <cstdint>
#include <string>

// All user-configurable options for the "Now Playing" source.
// Colors are stored as OBS's packed 0xAABBGGRR format (same as
// obs_data_get_int() returns for an OBS_PROPERTY_COLOR_ALPHA field).
struct now_playing_settings {
	bool show_album_art = true;
	bool show_progress_bar = true;
	bool show_status_icon = true;

	int canvas_width = 720;
	int canvas_height = 200;
	int album_art_size = 160;

	std::string font_face = "Segoe UI";
	int font_flags = 0; // OBS_FONT_BOLD / OBS_FONT_ITALIC bitmask, see obs-properties.h
	int title_font_size = 28;
	int artist_font_size = 20;

	uint32_t text_color = 0xFFFFFFFF;   // AABBGGRR, opaque white
	uint32_t bg_color = 0xCC1A1A1A;     // semi-transparent dark panel
	uint32_t accent_color = 0xFF57B95F; // progress bar / status icon

	// NOTE: this file is UTF-8. Make sure the project is built with the
	// /utf-8 flag on MSVC (already added in the CMakeLists snippet in
	// README.md) or this literal will get mangled by the build's code page.
	std::string idle_text = "\xE5\xB0\x9A\xE6\x9C\xAA\xE5\x81\xB5\xE6\xB8\xAC\xE5\x88\xB0\xE6\x92\xAD\xE6\x94\xBE\xE4\xB8\xAD\xE7\x9A\x84\xE9\x9F\xB3\xE6\xA8\x82"; // "尚未偵測到播放中的音樂"

	// Optional substring filter matched (case-insensitive) against the
	// media session's SourceAppUserModelId, e.g. "spotify" or "chrome".
	// Empty = follow whichever app Windows currently reports as active.
	std::string source_filter;
};
