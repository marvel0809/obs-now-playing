#pragma once

#include <cstdint>
#include <vector>

#include "media-session-watcher.h"
#include "now-playing-settings.h"

// Rasterizes one frame of the "now playing" widget (background panel,
// album art, title/artist text, progress bar, status icon) into a tightly
// packed, top-down, straight-alpha BGRA8 buffer ready for gs_texture_create.
//
// This is only called a few times a second (on data change, or on a small
// timer while something is playing so the progress bar can advance) -- not
// once per video frame -- so doing the work with GDI+ is plenty fast.
class OverlayRenderer {
public:
	OverlayRenderer() = default;
	~OverlayRenderer() = default;

	// Returns width*height*4 bytes (BGRA8) on success, empty vector on failure.
	std::vector<uint8_t> render(const MediaSnapshot &snapshot, const now_playing_settings &settings, int width, int height);
};
