#pragma once

#include <memory>

#include <obs-module.h>

#include "media-session-watcher.h"
#include "now-playing-settings.h"
#include "overlay-renderer.h"

// A native OBS video source (no browser source, no HTML) that reads
// Windows' system-wide "now playing" info (Windows.Media.Control /
// SMTC -- the same thing that drives the volume flyout widget, and picks
// up Spotify's desktop app as well as YouTube / YouTube Music playing in
// Chrome) and draws it as a title/artist/album-art/progress-bar overlay.
class NowPlayingSource {
public:
	NowPlayingSource(obs_data_t *settings, obs_source_t *source);
	~NowPlayingSource();

	void update(obs_data_t *settings);
	void video_tick(float seconds);
	void video_render(gs_effect_t *effect);
	uint32_t get_width() const;
	uint32_t get_height() const;

	static const char *get_name(void *type_data);
	static void *create(obs_data_t *settings, obs_source_t *source);
	static void destroy(void *data);
	static void update_cb(void *data, obs_data_t *settings);
	static void video_tick_cb(void *data, float seconds);
	static void video_render_cb(void *data, gs_effect_t *effect);
	static uint32_t get_width_cb(void *data);
	static uint32_t get_height_cb(void *data);
	static void get_defaults(obs_data_t *settings);
	static obs_properties_t *get_properties(void *data);

private:
	void rebuild_texture_if_needed();

	obs_source_t *source_ = nullptr;
	now_playing_settings settings_;

	std::unique_ptr<MediaSessionWatcher> watcher_;
	OverlayRenderer renderer_;

	gs_texture_t *texture_ = nullptr;
	int texture_w_ = 0;
	int texture_h_ = 0;

	uint64_t last_data_version_ = 0;
	uint64_t last_art_version_ = 0;
	float refresh_accum_ = 0.0f;
};
