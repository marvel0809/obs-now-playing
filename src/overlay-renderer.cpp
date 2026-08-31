#include "overlay-renderer.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <objidl.h>
#include <gdiplus.h>
#pragma comment(lib, "gdiplus.lib")

#include <obs-module.h>

#include <algorithm>
#include <cstring>

namespace {

Gdiplus::Color ToGdiColor(uint32_t c)
{
	// OBS color properties pack as 0xAABBGGRR (matches vec4_from_rgba()).
	BYTE r = c & 0xFF;
	BYTE g = (c >> 8) & 0xFF;
	BYTE b = (c >> 16) & 0xFF;
	BYTE a = (c >> 24) & 0xFF;
	return Gdiplus::Color(a, r, g, b);
}

std::wstring Utf8ToWide(const std::string &s)
{
	if (s.empty())
		return {};
	int len = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0);
	if (len <= 0)
		return {};
	std::wstring out(len, L'\0');
	MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), out.data(), len);
	return out;
}

std::wstring FormatTime(double seconds)
{
	if (seconds < 0)
		seconds = 0;
	int total = (int)(seconds + 0.5);
	int m = total / 60;
	int s = total % 60;
	wchar_t buf[32];
	swprintf(buf, 32, L"%d:%02d", m, s);
	return buf;
}

void RoundedRectPath(Gdiplus::GraphicsPath &path, int x, int y, int w, int h, int radius)
{
	if (w <= 0 || h <= 0)
		return;
	if (radius <= 0) {
		path.AddRectangle(Gdiplus::Rect(x, y, w, h));
		return;
	}
	radius = std::min(radius, std::min(w, h) / 2);
	int d = radius * 2;
	path.AddArc(x, y, d, d, 180, 90);
	path.AddArc(x + w - d, y, d, d, 270, 90);
	path.AddArc(x + w - d, y + h - d, d, d, 0, 90);
	path.AddArc(x, y + h - d, d, d, 90, 90);
	path.CloseFigure();
}

} // namespace

std::vector<uint8_t> OverlayRenderer::render(const MediaSnapshot &snapshot, const now_playing_settings &settings, int width, int height)
{
	if (width <= 0 || height <= 0)
		return {};

	Gdiplus::Bitmap bitmap(width, height, PixelFormat32bppARGB);
	Gdiplus::Graphics gfx(&bitmap);
	gfx.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
	gfx.SetTextRenderingHint(Gdiplus::TextRenderingHintAntiAliasGridFit);
	gfx.SetInterpolationMode(Gdiplus::InterpolationModeHighQualityBicubic);
	gfx.Clear(Gdiplus::Color(0, 0, 0, 0));

	const int padding = 18;

	{
		Gdiplus::SolidBrush brush(ToGdiColor(settings.bg_color));
		Gdiplus::GraphicsPath path;
		RoundedRectPath(path, 0, 0, width, height, 14);
		gfx.FillPath(&brush, &path);
	}

	int art_size = settings.show_album_art ? std::max(0, std::min(settings.album_art_size, height - padding * 2)) : 0;
	int art_x = padding;
	int art_y = (height - art_size) / 2;

	if (settings.show_album_art && art_size > 0) {
		Gdiplus::GraphicsPath clip;
		RoundedRectPath(clip, art_x, art_y, art_size, art_size, 10);
		Gdiplus::Region oldClip;
		gfx.GetClip(&oldClip);
		gfx.SetClip(&clip);

		if (!snapshot.art_pixels.empty() && snapshot.art_width > 0 && snapshot.art_height > 0) {
			Gdiplus::Bitmap art(snapshot.art_width, snapshot.art_height, snapshot.art_width * 4, PixelFormat32bppARGB,
					     const_cast<BYTE *>(snapshot.art_pixels.data()));
			gfx.DrawImage(&art, Gdiplus::Rect(art_x, art_y, art_size, art_size));
		} else {
			Gdiplus::SolidBrush placeholder(Gdiplus::Color(60, 255, 255, 255));
			gfx.FillRectangle(&placeholder, art_x, art_y, art_size, art_size);
		}

		gfx.SetClip(&oldClip);
	}

	int text_left = (settings.show_album_art && art_size > 0) ? (art_x + art_size + padding) : padding;
	int text_width = std::max(10, width - text_left - padding);

	// FontFamily has no accessible copy-assignment operator, so resolve the
	// name we're going to use *before* constructing the real object.
	std::wstring requestedFamily = Utf8ToWide(settings.font_face);
	{
		Gdiplus::FontFamily probe(requestedFamily.c_str());
		if (probe.GetLastStatus() != Gdiplus::Ok)
			requestedFamily = L"Segoe UI";
	}
	Gdiplus::FontFamily family(requestedFamily.c_str());

	int gdiStyle = Gdiplus::FontStyleRegular;
	if (settings.font_flags & OBS_FONT_BOLD)
		gdiStyle |= Gdiplus::FontStyleBold;
	if (settings.font_flags & OBS_FONT_ITALIC)
		gdiStyle |= Gdiplus::FontStyleItalic;

	Gdiplus::StringFormat format;
	format.SetTrimming(Gdiplus::StringTrimmingEllipsisCharacter);
	format.SetFormatFlags(Gdiplus::StringFormatFlagsNoWrap);

	if (!snapshot.has_session) {
		Gdiplus::Font font(&family, (Gdiplus::REAL)settings.title_font_size, gdiStyle, Gdiplus::UnitPixel);
		Gdiplus::SolidBrush brush(ToGdiColor(settings.text_color));
		// StringFormat's copy constructor is protected (must use Clone()),
		// so just mutate the shared `format` in place -- safe here because
		// this branch is the only one that runs (mutually exclusive with
		// the title/artist branch below) and nothing reads `format` after.
		format.SetLineAlignment(Gdiplus::StringAlignmentCenter);
		Gdiplus::RectF rect((Gdiplus::REAL)text_left, 0.0f, (Gdiplus::REAL)text_width, (Gdiplus::REAL)height);
		gfx.DrawString(Utf8ToWide(settings.idle_text).c_str(), -1, &font, rect, &format, &brush);
	} else {
		int y = padding + 4;

		Gdiplus::Font titleFont(&family, (Gdiplus::REAL)settings.title_font_size, gdiStyle, Gdiplus::UnitPixel);
		Gdiplus::SolidBrush textBrush(ToGdiColor(settings.text_color));
		Gdiplus::RectF titleRect((Gdiplus::REAL)text_left, (Gdiplus::REAL)y, (Gdiplus::REAL)text_width,
					   (Gdiplus::REAL)(settings.title_font_size * 1.4f));
		std::wstring title = snapshot.title.empty() ? L"(Unknown Title)" : snapshot.title;
		gfx.DrawString(title.c_str(), -1, &titleFont, titleRect, &format, &textBrush);
		y += (int)(settings.title_font_size * 1.4f) + 4;

		Gdiplus::Font artistFont(&family, (Gdiplus::REAL)settings.artist_font_size, gdiStyle, Gdiplus::UnitPixel);
		Gdiplus::Color titleColor = ToGdiColor(settings.text_color);
		Gdiplus::SolidBrush artistBrush(
			Gdiplus::Color((BYTE)(titleColor.GetA() * 0.75f), titleColor.GetR(), titleColor.GetG(), titleColor.GetB()));
		Gdiplus::RectF artistRect((Gdiplus::REAL)text_left, (Gdiplus::REAL)y, (Gdiplus::REAL)text_width,
					    (Gdiplus::REAL)(settings.artist_font_size * 1.4f));
		gfx.DrawString(snapshot.artist.c_str(), -1, &artistFont, artistRect, &format, &artistBrush);
		y += (int)(settings.artist_font_size * 1.4f) + 10;

		Gdiplus::Color accent = ToGdiColor(settings.accent_color);

		if (settings.show_progress_bar && snapshot.duration_seconds > 0.5) {
			int bar_h = 8;
			int bar_w = text_width;

			Gdiplus::SolidBrush track(Gdiplus::Color(70, 255, 255, 255));
			Gdiplus::GraphicsPath trackPath;
			RoundedRectPath(trackPath, text_left, y, bar_w, bar_h, bar_h / 2);
			gfx.FillPath(&track, &trackPath);

			double frac = std::clamp(snapshot.position_seconds / snapshot.duration_seconds, 0.0, 1.0);
			int fill_w = std::max(bar_h, (int)(bar_w * frac));
			Gdiplus::SolidBrush fillBrush(accent);
			Gdiplus::GraphicsPath fillPath;
			RoundedRectPath(fillPath, text_left, y, fill_w, bar_h, bar_h / 2);
			gfx.FillPath(&fillBrush, &fillPath);

			y += bar_h + 6;

			Gdiplus::Font timeFont(&family, (Gdiplus::REAL)std::max(12, settings.artist_font_size - 4), Gdiplus::FontStyleRegular,
						 Gdiplus::UnitPixel);
			Gdiplus::SolidBrush timeBrush(Gdiplus::Color(180, 255, 255, 255));
			std::wstring timeStr = FormatTime(snapshot.position_seconds) + L" / " + FormatTime(snapshot.duration_seconds);
			Gdiplus::RectF timeRect((Gdiplus::REAL)text_left, (Gdiplus::REAL)y, (Gdiplus::REAL)text_width, 20.0f);
			gfx.DrawString(timeStr.c_str(), -1, &timeFont, timeRect, &format, &timeBrush);
		}

		if (settings.show_status_icon) {
			int badge = 34;
			int bx, by;
			if (settings.show_album_art && art_size > 0) {
				bx = art_x + art_size - badge + 6;
				by = art_y + art_size - badge + 6;
			} else {
				bx = text_left;
				by = height - padding - badge;
			}

			Gdiplus::SolidBrush circle(Gdiplus::Color(220, accent.GetR(), accent.GetG(), accent.GetB()));
			gfx.FillEllipse(&circle, bx, by, badge, badge);

			Gdiplus::SolidBrush iconBrush(Gdiplus::Color(255, 255, 255, 255));
			if (snapshot.status == PlaybackStatus::Playing) {
				Gdiplus::PointF tri[3] = {
					{(Gdiplus::REAL)(bx + badge * 0.36f), (Gdiplus::REAL)(by + badge * 0.28f)},
					{(Gdiplus::REAL)(bx + badge * 0.36f), (Gdiplus::REAL)(by + badge * 0.72f)},
					{(Gdiplus::REAL)(bx + badge * 0.74f), (Gdiplus::REAL)(by + badge * 0.5f)},
				};
				gfx.FillPolygon(&iconBrush, tri, 3);
			} else {
				Gdiplus::REAL bw = badge * 0.16f;
				gfx.FillRectangle(&iconBrush, (Gdiplus::REAL)(bx + badge * 0.32f), (Gdiplus::REAL)(by + badge * 0.26f), bw,
						    badge * 0.48f);
				gfx.FillRectangle(&iconBrush, (Gdiplus::REAL)(bx + badge * 0.56f), (Gdiplus::REAL)(by + badge * 0.26f), bw,
						    badge * 0.48f);
			}
		}
	}

	Gdiplus::BitmapData data{};
	Gdiplus::Rect full(0, 0, width, height);
	if (bitmap.LockBits(&full, Gdiplus::ImageLockModeRead, PixelFormat32bppARGB, &data) != Gdiplus::Ok)
		return {};

	std::vector<uint8_t> out(size_t(width) * size_t(height) * 4);
	const uint8_t *src_base = static_cast<const uint8_t *>(data.Scan0);
	int stride = data.Stride; // may be negative for bottom-up layouts
	for (int y = 0; y < height; ++y) {
		const uint8_t *src_row = src_base + ptrdiff_t(y) * stride;
		uint8_t *dst_row = out.data() + size_t(y) * size_t(width) * 4;
		std::memcpy(dst_row, src_row, size_t(width) * 4);
	}
	bitmap.UnlockBits(&data);

	return out;
}
