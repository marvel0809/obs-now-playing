#include "media-session-watcher.h"

// Windows SDK ships pre-generated C++/WinRT projection headers for the OS
// API surface (including Windows.Media.Control), so no cppwinrt.exe / MIDL
// generation step is required -- just #include and link windowsapp.lib.
// Collections.h must come first: it defines the begin()/end() support that
// range-based for needs for IVectorView<T> (GetSessions() below) -- without
// it (or included after first use) MSVC fails with C3779/C3536 on the
// range-for over manager.GetSessions().
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Graphics.Imaging.h>
#include <winrt/Windows.Media.Control.h>
#include <winrt/Windows.Storage.Streams.h>

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstring>
#include <mutex>
#include <thread>

using namespace winrt::Windows::Foundation;
using namespace winrt::Windows::Media::Control;
using namespace winrt::Windows::Storage::Streams;
using namespace winrt::Windows::Graphics::Imaging;

// SoftwareBitmap's pixel buffer is exposed through this classic COM
// interface rather than a WinRT-projected one. It isn't declared in the
// projection headers, so we declare it ourselves -- this is the standard
// pattern used throughout Microsoft's own C++/WinRT SoftwareBitmap samples.
struct __declspec(uuid("5B0D3235-4DBA-4D44-865E-8F1D0E4FD04D")) IMemoryBufferByteAccess : ::IUnknown {
	virtual HRESULT __stdcall GetBuffer(uint8_t **value, uint32_t *capacity) = 0;
};

namespace {
std::string to_lower(std::string s)
{
	std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return (char)std::tolower(c); });
	return s;
}
} // namespace

struct MediaSessionWatcher::Impl {
	std::thread worker;
	std::atomic<bool> stop{false};
	std::atomic<bool> filter_dirty{false};

	std::mutex filter_mutex;
	std::string filter; // already lower-cased

	std::mutex snapshot_mutex;
	MediaSnapshot snapshot;
	uint64_t next_data_version = 1;
	uint64_t next_art_version = 1;

	GlobalSystemMediaTransportControlsSessionManager manager{nullptr};
	GlobalSystemMediaTransportControlsSession current_session{nullptr};

	winrt::event_token manager_sessions_changed_token{};
	winrt::event_token session_media_props_token{};
	winrt::event_token session_playback_token{};
	winrt::event_token session_timeline_token{};

	void run();
	GlobalSystemMediaTransportControlsSession pick_session();
	void attach_to_best_session();
	void detach_session_events();
	void refresh_from_session();
	void mark_no_session();
};

void MediaSessionWatcher::Impl::run()
{
	winrt::init_apartment(winrt::apartment_type::multi_threaded);

	try {
		manager = GlobalSystemMediaTransportControlsSessionManager::RequestAsync().get();
	} catch (...) {
		mark_no_session();
		winrt::uninit_apartment();
		return;
	}

	manager_sessions_changed_token = manager.SessionsChanged([this](auto &&, auto &&) { attach_to_best_session(); });

	attach_to_best_session();

	// MTA background thread: WinRT's threadpool delivers the events above
	// without needing a message pump here. We just idle until told to stop,
	// waking periodically to notice a source_filter change.
	while (!stop.load()) {
		if (filter_dirty.exchange(false))
			attach_to_best_session();
		std::this_thread::sleep_for(std::chrono::milliseconds(250));
	}

	detach_session_events();
	if (manager)
		manager.SessionsChanged(manager_sessions_changed_token);
	manager = nullptr;

	winrt::uninit_apartment();
}

GlobalSystemMediaTransportControlsSession MediaSessionWatcher::Impl::pick_session()
{
	if (!manager)
		return nullptr;

	std::string want;
	{
		std::lock_guard<std::mutex> lock(filter_mutex);
		want = filter;
	}

	if (!want.empty()) {
		for (auto const &s : manager.GetSessions()) {
			std::string aumid = to_lower(winrt::to_string(s.SourceAppUserModelId()));
			if (aumid.find(want) != std::string::npos)
				return s;
		}
		// No session matched the filter; fall through to the OS default
		// below rather than showing nothing, since the filter is meant
		// to be a *preference*, not a hard requirement.
	}

	return manager.GetCurrentSession();
}

void MediaSessionWatcher::Impl::attach_to_best_session()
{
	detach_session_events();

	current_session = pick_session();
	if (!current_session) {
		mark_no_session();
		return;
	}

	session_media_props_token = current_session.MediaPropertiesChanged([this](auto &&, auto &&) { refresh_from_session(); });
	session_playback_token = current_session.PlaybackInfoChanged([this](auto &&, auto &&) { refresh_from_session(); });
	session_timeline_token = current_session.TimelinePropertiesChanged([this](auto &&, auto &&) { refresh_from_session(); });

	refresh_from_session();
}

void MediaSessionWatcher::Impl::detach_session_events()
{
	if (current_session) {
		current_session.MediaPropertiesChanged(session_media_props_token);
		current_session.PlaybackInfoChanged(session_playback_token);
		current_session.TimelinePropertiesChanged(session_timeline_token);
	}
	current_session = nullptr;
}

void MediaSessionWatcher::Impl::mark_no_session()
{
	std::lock_guard<std::mutex> lock(snapshot_mutex);
	if (snapshot.has_session) {
		snapshot = MediaSnapshot{};
		snapshot.data_version = next_data_version++;
	}
}

void MediaSessionWatcher::Impl::refresh_from_session()
{
	auto session = current_session; // local strong ref
	if (!session) {
		mark_no_session();
		return;
	}

	MediaSnapshot next;
	next.has_session = true;

	try {
		auto props = session.TryGetMediaPropertiesAsync().get();
		next.title = std::wstring(props.Title());

		std::wstring artist(props.Artist());
		if (artist.empty())
			artist = std::wstring(props.AlbumArtist());
		next.artist = artist;

		auto playback = session.GetPlaybackInfo();
		switch (playback.PlaybackStatus()) {
		case GlobalSystemMediaTransportControlsSessionPlaybackStatus::Playing:
			next.status = PlaybackStatus::Playing;
			break;
		case GlobalSystemMediaTransportControlsSessionPlaybackStatus::Paused:
			next.status = PlaybackStatus::Paused;
			break;
		case GlobalSystemMediaTransportControlsSessionPlaybackStatus::Stopped:
			next.status = PlaybackStatus::Stopped;
			break;
		case GlobalSystemMediaTransportControlsSessionPlaybackStatus::Changing:
			next.status = PlaybackStatus::Changing;
			break;
		case GlobalSystemMediaTransportControlsSessionPlaybackStatus::Opened:
			next.status = PlaybackStatus::Opened;
			break;
		default:
			next.status = PlaybackStatus::Closed;
			break;
		}

		auto timeline = session.GetTimelineProperties();
		auto to_seconds = [](TimeSpan ts) { return static_cast<double>(ts.count()) / 10000000.0; };
		double start = to_seconds(timeline.StartTime());
		double end = to_seconds(timeline.EndTime());
		next.position_seconds = to_seconds(timeline.Position());
		next.duration_seconds = (end > start) ? (end - start) : 0.0;

		// Thumbnail is best-effort: not every app supplies one.
		if (auto thumbRef = props.Thumbnail()) {
			try {
				auto stream = thumbRef.OpenReadAsync().get();
				auto decoder = BitmapDecoder::CreateAsync(stream).get();
				auto bitmap = decoder.GetSoftwareBitmapAsync().get();
				bitmap = SoftwareBitmap::Convert(bitmap, BitmapPixelFormat::Bgra8, BitmapAlphaMode::Straight);

				auto buffer = bitmap.LockBuffer(BitmapBufferAccessMode::Read);
				auto reference = buffer.CreateReference();
				auto byteAccess = reference.as<IMemoryBufferByteAccess>();

				uint8_t *data = nullptr;
				uint32_t capacity = 0;
				byteAccess->GetBuffer(&data, &capacity);

				auto desc = buffer.GetPlaneDescription(0);
				next.art_width = bitmap.PixelWidth();
				next.art_height = bitmap.PixelHeight();
				next.art_pixels.resize(size_t(next.art_width) * size_t(next.art_height) * 4);

				for (int y = 0; y < next.art_height; ++y) {
					const uint8_t *src = data + size_t(desc.StartIndex) + size_t(y) * size_t(desc.Stride);
					uint8_t *dst = next.art_pixels.data() + size_t(y) * size_t(next.art_width) * 4;
					std::memcpy(dst, src, size_t(next.art_width) * 4);
				}
			} catch (...) {
				next.art_pixels.clear();
				next.art_width = 0;
				next.art_height = 0;
			}
		}
	} catch (...) {
		// The session likely just closed out from under us; treat as idle
		// rather than propagating the exception to the caller's thread.
		mark_no_session();
		return;
	}

	std::lock_guard<std::mutex> lock(snapshot_mutex);

	bool art_changed = next.art_width != snapshot.art_width || next.art_height != snapshot.art_height ||
			    next.art_pixels != snapshot.art_pixels;
	bool data_changed = !snapshot.has_session || next.title != snapshot.title || next.artist != snapshot.artist ||
			     next.status != snapshot.status ||
			     std::abs(next.position_seconds - snapshot.position_seconds) > 0.5 ||
			     next.duration_seconds != snapshot.duration_seconds;

	uint64_t keep_data_version = snapshot.data_version;
	uint64_t keep_art_version = snapshot.art_version;

	snapshot = next;
	snapshot.data_version = data_changed ? next_data_version++ : keep_data_version;
	snapshot.art_version = art_changed ? next_art_version++ : keep_art_version;
}

MediaSessionWatcher::MediaSessionWatcher(std::string source_filter) : impl_(std::make_unique<Impl>())
{
	impl_->filter = to_lower(std::move(source_filter));
	impl_->worker = std::thread([this] { impl_->run(); });
}

MediaSessionWatcher::~MediaSessionWatcher()
{
	impl_->stop = true;
	if (impl_->worker.joinable())
		impl_->worker.join();
}

MediaSnapshot MediaSessionWatcher::get_snapshot()
{
	std::lock_guard<std::mutex> lock(impl_->snapshot_mutex);
	return impl_->snapshot;
}

MediaVersions MediaSessionWatcher::get_versions()
{
	std::lock_guard<std::mutex> lock(impl_->snapshot_mutex);
	return {impl_->snapshot.has_session, impl_->snapshot.status, impl_->snapshot.data_version, impl_->snapshot.art_version};
}

void MediaSessionWatcher::set_source_filter(std::string filter)
{
	filter = to_lower(std::move(filter));
	{
		std::lock_guard<std::mutex> lock(impl_->filter_mutex);
		if (impl_->filter == filter)
			return;
		impl_->filter = std::move(filter);
	}
	impl_->filter_dirty = true;
}
