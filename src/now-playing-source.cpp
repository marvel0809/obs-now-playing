#include "now-playing-source.h"

#include <graphics/graphics.h>

NowPlayingSource::NowPlayingSource(obs_data_t *settings, obs_source_t *source) : source_(source)
{
	update(settings);
}

NowPlayingSource::~NowPlayingSource()
{
	if (texture_) {
		obs_enter_graphics();
		gs_texture_destroy(texture_);
		obs_leave_graphics();
		texture_ = nullptr;
	}
}

void NowPlayingSource::update(obs_data_t *settings)
{
	settings_.show_album_art = obs_data_get_bool(settings, "show_album_art");
	settings_.show_progress_bar = obs_data_get_bool(settings, "show_progress_bar");
	settings_.show_status_icon = obs_data_get_bool(settings, "show_status_icon");

	settings_.canvas_width = (int)obs_data_get_int(settings, "canvas_width");
	settings_.canvas_height = (int)obs_data_get_int(settings, "canvas_height");
	settings_.album_art_size = (int)obs_data_get_int(settings, "album_art_size");

	settings_.title_font_size = (int)obs_data_get_int(settings, "title_font_size");
	settings_.artist_font_size = (int)obs_data_get_int(settings, "artist_font_size");

	obs_data_t *font_obj = obs_data_get_obj(settings, "font");
	if (font_obj) {
		const char *face = obs_data_get_string(font_obj, "face");
		if (face && *face)
			settings_.font_face = face;
		settings_.font_flags = (int)obs_data_get_int(font_obj, "flags");
		obs_data_release(font_obj);
	}

	settings_.text_color = (uint32_t)obs_data_get_int(settings, "text_color");
	settings_.bg_color = (uint32_t)obs_data_get_int(settings, "bg_color");
	settings_.accent_color = (uint32_t)obs_data_get_int(settings, "accent_color");

	const char *idle = obs_data_get_string(settings, "idle_text");
	if (idle && *idle)
		settings_.idle_text = idle;

	const char *filter = obs_data_get_string(settings, "source_filter");
	settings_.source_filter = filter ? filter : "";

	if (!watcher_)
		watcher_ = std::make_unique<MediaSessionWatcher>(settings_.source_filter);
	else
		watcher_->set_source_filter(settings_.source_filter);

	// Settings that affect canvas size / layout invalidate the cached
	// texture so the very next tick redraws at the new dimensions.
	last_data_version_ = 0;
	last_art_version_ = 0;
}

void NowPlayingSource::rebuild_texture_if_needed()
{
	MediaSnapshot snap = watcher_->get_snapshot();

	auto pixels = renderer_.render(snap, settings_, settings_.canvas_width, settings_.canvas_height);
	if (pixels.empty())
		return;

	obs_enter_graphics();
	if (!texture_ || texture_w_ != settings_.canvas_width || texture_h_ != settings_.canvas_height) {
		if (texture_)
			gs_texture_destroy(texture_);
		const uint8_t *data = pixels.data();
		// GS_DYNAMIC is required for later gs_texture_set_image() calls
		// (subsequent frames reuse this texture instead of recreating it).
		texture_ = gs_texture_create((uint32_t)settings_.canvas_width, (uint32_t)settings_.canvas_height, GS_BGRA, 1, &data,
					      GS_DYNAMIC);
		texture_w_ = settings_.canvas_width;
		texture_h_ = settings_.canvas_height;
	} else {
		gs_texture_set_image(texture_, pixels.data(), (uint32_t)settings_.canvas_width * 4, false);
	}
	obs_leave_graphics();

	last_data_version_ = snap.data_version;
	last_art_version_ = snap.art_version;
}

void NowPlayingSource::video_tick(float seconds)
{
	if (!watcher_)
		return;

	refresh_accum_ += seconds;

	MediaVersions v = watcher_->get_versions();
	bool changed = v.data_version != last_data_version_ || v.art_version != last_art_version_;
	// While something is playing, force a redraw twice a second so the
	// progress bar keeps advancing even though data_version itself only
	// bumps on a real SMTC event.
	bool periodic = v.has_session && v.status == PlaybackStatus::Playing && refresh_accum_ >= 0.5f;

	if (!texture_ || changed || periodic) {
		rebuild_texture_if_needed();
		refresh_accum_ = 0.0f;
	}
}

void NowPlayingSource::video_render(gs_effect_t *)
{
	if (!texture_)
		return;

	gs_effect_t *default_effect = obs_get_base_effect(OBS_EFFECT_DEFAULT);
	gs_eparam_t *image = gs_effect_get_param_by_name(default_effect, "image");
	gs_effect_set_texture(image, texture_);

	gs_blend_state_push();
	gs_blend_function(GS_BLEND_SRCALPHA, GS_BLEND_INVSRCALPHA);

	while (gs_effect_loop(default_effect, "Draw")) {
		gs_draw_sprite(texture_, 0, (uint32_t)settings_.canvas_width, (uint32_t)settings_.canvas_height);
	}

	gs_blend_state_pop();
}

uint32_t NowPlayingSource::get_width() const
{
	return (uint32_t)settings_.canvas_width;
}

uint32_t NowPlayingSource::get_height() const
{
	return (uint32_t)settings_.canvas_height;
}

// ---- static libobs callback trampolines ----------------------------------

const char *NowPlayingSource::get_name(void *)
{
	return obs_module_text("NowPlayingSource.Name");
}

void *NowPlayingSource::create(obs_data_t *settings, obs_source_t *source)
{
	return new NowPlayingSource(settings, source);
}

void NowPlayingSource::destroy(void *data)
{
	delete static_cast<NowPlayingSource *>(data);
}

void NowPlayingSource::update_cb(void *data, obs_data_t *settings)
{
	static_cast<NowPlayingSource *>(data)->update(settings);
}

void NowPlayingSource::video_tick_cb(void *data, float seconds)
{
	static_cast<NowPlayingSource *>(data)->video_tick(seconds);
}

void NowPlayingSource::video_render_cb(void *data, gs_effect_t *effect)
{
	static_cast<NowPlayingSource *>(data)->video_render(effect);
}

uint32_t NowPlayingSource::get_width_cb(void *data)
{
	return static_cast<NowPlayingSource *>(data)->get_width();
}

uint32_t NowPlayingSource::get_height_cb(void *data)
{
	return static_cast<NowPlayingSource *>(data)->get_height();
}

void NowPlayingSource::get_defaults(obs_data_t *settings)
{
	now_playing_settings defaults;

	obs_data_set_default_bool(settings, "show_album_art", defaults.show_album_art);
	obs_data_set_default_bool(settings, "show_progress_bar", defaults.show_progress_bar);
	obs_data_set_default_bool(settings, "show_status_icon", defaults.show_status_icon);

	obs_data_set_default_int(settings, "canvas_width", defaults.canvas_width);
	obs_data_set_default_int(settings, "canvas_height", defaults.canvas_height);
	obs_data_set_default_int(settings, "album_art_size", defaults.album_art_size);

	obs_data_set_default_int(settings, "title_font_size", defaults.title_font_size);
	obs_data_set_default_int(settings, "artist_font_size", defaults.artist_font_size);

	obs_data_t *font_obj = obs_data_create();
	obs_data_set_string(font_obj, "face", defaults.font_face.c_str());
	obs_data_set_int(font_obj, "flags", defaults.font_flags);
	obs_data_set_int(font_obj, "size", defaults.title_font_size);
	obs_data_set_default_obj(settings, "font", font_obj);
	obs_data_release(font_obj);

	obs_data_set_default_int(settings, "text_color", (long long)defaults.text_color);
	obs_data_set_default_int(settings, "bg_color", (long long)defaults.bg_color);
	obs_data_set_default_int(settings, "accent_color", (long long)defaults.accent_color);

	obs_data_set_default_string(settings, "idle_text", defaults.idle_text.c_str());
	obs_data_set_default_string(settings, "source_filter", defaults.source_filter.c_str());
}

obs_properties_t *NowPlayingSource::get_properties(void *)
{
	obs_properties_t *props = obs_properties_create();

	obs_properties_add_int(props, "canvas_width", obs_module_text("NowPlayingSource.CanvasWidth"), 200, 3000, 10);
	obs_properties_add_int(props, "canvas_height", obs_module_text("NowPlayingSource.CanvasHeight"), 80, 1200, 10);

	obs_properties_add_bool(props, "show_album_art", obs_module_text("NowPlayingSource.ShowAlbumArt"));
	obs_properties_add_int(props, "album_art_size", obs_module_text("NowPlayingSource.AlbumArtSize"), 40, 1000, 10);

	obs_properties_add_font(props, "font", obs_module_text("NowPlayingSource.Font"));
	obs_properties_add_int(props, "title_font_size", obs_module_text("NowPlayingSource.TitleFontSize"), 8, 200, 1);
	obs_properties_add_int(props, "artist_font_size", obs_module_text("NowPlayingSource.ArtistFontSize"), 8, 200, 1);

	obs_properties_add_color_alpha(props, "text_color", obs_module_text("NowPlayingSource.TextColor"));
	obs_properties_add_color_alpha(props, "bg_color", obs_module_text("NowPlayingSource.BgColor"));
	obs_properties_add_color_alpha(props, "accent_color", obs_module_text("NowPlayingSource.AccentColor"));

	obs_properties_add_bool(props, "show_progress_bar", obs_module_text("NowPlayingSource.ShowProgressBar"));
	obs_properties_add_bool(props, "show_status_icon", obs_module_text("NowPlayingSource.ShowStatusIcon"));

	obs_properties_add_text(props, "idle_text", obs_module_text("NowPlayingSource.IdleText"), OBS_TEXT_DEFAULT);
	obs_properties_add_text(props, "source_filter", obs_module_text("NowPlayingSource.SourceFilter"), OBS_TEXT_DEFAULT);

	return props;
}
