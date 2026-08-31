#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

// Mirrors GlobalSystemMediaTransportControlsSessionPlaybackStatus without
// leaking any WinRT types into this header, so the rest of the plugin
// (and the .h file itself) stays free of <winrt/...> includes.
enum class PlaybackStatus { Closed, Opened, Changing, Stopped, Playing, Paused };

// A snapshot of "what Windows currently thinks is playing", safe to copy
// and read from any thread. Produced by MediaSessionWatcher's background
// worker, consumed by the OBS render/tick thread.
struct MediaSnapshot {
	bool has_session = false;

	std::wstring title;
	std::wstring artist;
	PlaybackStatus status = PlaybackStatus::Closed;

	double position_seconds = 0.0;
	double duration_seconds = 0.0;

	// Album art, decoded to straight-alpha, tightly packed BGRA8, top-down.
	std::vector<uint8_t> art_pixels;
	int art_width = 0;
	int art_height = 0;

	// Bumped whenever title/artist/status/position changes.
	uint64_t data_version = 0;
	// Bumped only when art_pixels/art_width/art_height changes, so the
	// (rare) album-art texture upload can be skipped most frames.
	uint64_t art_version = 0;
};

// Cheap poll target: just the two version counters (+status), safe to call
// every video_tick without paying for a full MediaSnapshot copy (which can
// carry a few hundred KB of album-art pixels).
struct MediaVersions {
	bool has_session = false;
	PlaybackStatus status = PlaybackStatus::Closed;
	uint64_t data_version = 0;
	uint64_t art_version = 0;
};

// Owns a background thread that talks to
// Windows.Media.Control.GlobalSystemMediaTransportControlsSessionManager
// (the same OS facility that drives the volume flyout's "now playing"
// widget) and keeps a thread-safe MediaSnapshot up to date via its
// change events. Nothing here blocks the OBS graphics/render thread.
class MediaSessionWatcher {
public:
	explicit MediaSessionWatcher(std::string source_filter = {});
	~MediaSessionWatcher();

	MediaSessionWatcher(const MediaSessionWatcher &) = delete;
	MediaSessionWatcher &operator=(const MediaSessionWatcher &) = delete;

	// Cheap: just locks a mutex and copies the latest snapshot. Prefer
	// get_versions() for a per-frame "did anything change" check.
	MediaSnapshot get_snapshot();

	// Cheap version/status poll -- no pixel data copied.
	MediaVersions get_versions();

	// Case-insensitive substring match against SourceAppUserModelId(),
	// e.g. "spotify" or "chrome". Empty = no filtering. Safe to call from
	// the OBS thread whenever the user changes the setting.
	void set_source_filter(std::string filter);

private:
	struct Impl;
	std::unique_ptr<Impl> impl_;
};
