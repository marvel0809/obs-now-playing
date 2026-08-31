#include <obs-module.h>

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <objidl.h>
#include <gdiplus.h>
#pragma comment(lib, "gdiplus.lib")

#include "now-playing-source.h"

OBS_DECLARE_MODULE()
OBS_MODULE_USE_DEFAULT_LOCALE("obs-now-playing", "en-US")

// GDI+ must be started/shut down exactly once per process; the actual text
// and album-art compositing happens in overlay-renderer.cpp.
static ULONG_PTR gdiplus_token = 0;

static obs_source_info now_playing_source_info = {};

MODULE_EXPORT bool obs_module_load(void)
{
	Gdiplus::GdiplusStartupInput input;
	if (Gdiplus::GdiplusStartup(&gdiplus_token, &input, nullptr) != Gdiplus::Ok) {
		blog(LOG_ERROR, "[obs-now-playing] GdiplusStartup failed");
		return false;
	}

	now_playing_source_info.id = "now_playing_source";
	now_playing_source_info.type = OBS_SOURCE_TYPE_INPUT;
	now_playing_source_info.output_flags = OBS_SOURCE_VIDEO | OBS_SOURCE_CUSTOM_DRAW;
	now_playing_source_info.icon_type = OBS_ICON_TYPE_MEDIA;
	now_playing_source_info.get_name = NowPlayingSource::get_name;
	now_playing_source_info.create = NowPlayingSource::create;
	now_playing_source_info.destroy = NowPlayingSource::destroy;
	now_playing_source_info.update = NowPlayingSource::update_cb;
	now_playing_source_info.video_tick = NowPlayingSource::video_tick_cb;
	now_playing_source_info.video_render = NowPlayingSource::video_render_cb;
	now_playing_source_info.get_width = NowPlayingSource::get_width_cb;
	now_playing_source_info.get_height = NowPlayingSource::get_height_cb;
	now_playing_source_info.get_defaults = NowPlayingSource::get_defaults;
	now_playing_source_info.get_properties = NowPlayingSource::get_properties;

	obs_register_source(&now_playing_source_info);

	blog(LOG_INFO, "[obs-now-playing] plugin loaded (native source, no browser/HTML)");
	return true;
}

MODULE_EXPORT void obs_module_unload(void)
{
	if (gdiplus_token) {
		Gdiplus::GdiplusShutdown(gdiplus_token);
		gdiplus_token = 0;
	}
}
