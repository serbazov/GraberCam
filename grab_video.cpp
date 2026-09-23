#include "CEasyCapS.h"
#include "CEasyCapSEx.h"
#include "scap_gentl_error.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iostream>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

static constexpr int kFrames = 12;

struct Shot {
	uint64_t id = 0;
	int64_t w = 0;
	int64_t h = 0;
	std::chrono::steady_clock::time_point t;
	std::vector<unsigned char> mono;
};

static std::mutex g_mu;
static std::vector<Shot> g_shots;
static std::atomic<int> g_count{0};

static void print_err(const char* where)
{
	char text[512];
	size_t n = sizeof(text);
	std::memset(text, 0, sizeof(text));
	GC_ERROR e = scap_get_last_error_info(text, &n);
	std::printf("  %s failed, gc=%d %s\n", where, (int)e, text);
}

static bool set_str(void* h, scap_level_t lvl, const char* feat, const char* val)
{
	bool ok = scap_set_string(h, lvl, feat, val);
	std::printf("set %s=%s -> %s\n", feat, val, ok ? "ok" : "FAIL");
	if (!ok)
		print_err(feat);
	return ok;
}

static void on_frame(void* ctx, GenTL::BUFFER_HANDLE h_buf, void* base, size_t size)
{
	if (!base || size == 0)
		return;
	int n = g_count.load();
	if (n >= kFrames)
		return;

	auto* cap = static_cast<SCAP_HANDLE>(ctx);
	cxp_buf_info_t info;
	std::memset(&info, 0, sizeof(info));
	scap_get_buf_info(cap, h_buf, &info, true);

	Shot shot;
	shot.id = info.frame_id;
	shot.w = info.width;
	shot.h = info.height;
	shot.t = std::chrono::steady_clock::now();
	shot.mono.resize(size);
	std::memcpy(shot.mono.data(), base, size);

	{
		std::lock_guard<std::mutex> lock(g_mu);
		if (static_cast<int>(g_shots.size()) >= kFrames)
			return;
		g_shots.push_back(std::move(shot));
		g_count.store(static_cast<int>(g_shots.size()));
	}
}

int main()
{
	setenv("GENICAM_GENTL64_PATH", "/opt/magewell/usr/lib", 0);
	if (!scap_init()) {
		std::printf("scap_init failed\n");
		print_err("scap_init");
		return 1;
	}

	CARD_HANDLE card = scap_create_card(0);
	if (!card) {
		print_err("create_card");
		scap_done();
		return 1;
	}
	set_str(card, scap_interface, "FirmwareMode", "AAAA");
	scap_set_integer(card, scap_interface, "DeviceSelector", 0);
	if (!set_str(card, scap_interface, "CxpLinkConfiguration", "CXP12_X4"))
		set_str(card, scap_interface, "CxpLinkConfiguration", "Auto");
	scap_set_command(card, scap_interface, "CxpPoCxpAuto");
	std::this_thread::sleep_for(std::chrono::seconds(2));

	SCAP_HANDLE cap = scap_create_by_camera(card, 0);
	if (!cap) {
		print_err("create_camera");
		scap_close_card(card);
		scap_done();
		return 2;
	}

	set_str(cap, scap_camera, "TriggerMode", "Off");
	set_str(cap, scap_camera, "AcquisitionMode", "Continuous");
	set_str(cap, scap_camera, "PixelFormat", "Mono8");
	if (!scap_set_float(cap, scap_camera, "ExposureTime", 80000.0))
		print_err("ExposureTime");
	scap_set_float(cap, scap_camera, "GainRaw", 2.0);
	scap_set_float(cap, scap_camera, "AnalogGain", 2.0);

	auto t0 = std::chrono::steady_clock::now();
	if (!scap_start_capture(cap, on_frame, cap, 16, kFrames, false)) {
		print_err("start");
		scap_close_by_camera(cap);
		scap_close_card(card);
		scap_done();
		return 3;
	}
	for (int i = 0; i < 400 && g_count.load() < kFrames; ++i)
		std::this_thread::sleep_for(std::chrono::milliseconds(50));
	auto t1 = std::chrono::steady_clock::now();
	scap_stop_capture(cap);
	scap_close_by_camera(cap);
	scap_close_card(card);
	scap_done();

	std::vector<Shot> shots;
	{
		std::lock_guard<std::mutex> lock(g_mu);
		shots.swap(g_shots);
	}
	if (shots.size() < 2) {
		std::printf("captured only %zu frames\n", shots.size());
		return 4;
	}

	double sec = std::chrono::duration<double>(t1 - t0).count();
	double span = std::chrono::duration<double>(shots.back().t - shots.front().t).count();
	double fps = span > 0 ? (shots.size() - 1) / span : 0;
	std::printf("captured %zu frames, %lldx%lld, ids %llu..%llu, %.2f fps over %.2fs (wall %.2fs)\n",
		shots.size(), (long long)shots[0].w, (long long)shots[0].h,
		(unsigned long long)shots.front().id, (unsigned long long)shots.back().id,
		fps, span, sec);
	bool sequential = true;
	for (size_t i = 1; i < shots.size(); ++i) {
		if (shots[i].id != shots[i - 1].id + 1)
			sequential = false;
	}
	std::printf("frame ids consecutive: %s\n", sequential ? "yes" : "no");

	int play_fps = fps < 1 ? 1 : static_cast<int>(fps + 0.5);
	const std::string raw_path = "/home/user/work/GraberCam/clip_frames.raw";
	const std::string meta_path = "/home/user/work/GraberCam/clip_frames.txt";
	{
		std::ofstream meta(meta_path);
		meta << shots.size() << " " << shots[0].w << " " << shots[0].h << " " << play_fps << "\n";
		std::ofstream raw(raw_path, std::ios::binary);
		for (const auto& s : shots)
			raw.write(reinterpret_cast<const char*>(s.mono.data()), static_cast<std::streamsize>(s.mono.size()));
		if (!raw || !meta) {
			std::printf("failed to write frame dump\n");
			return 5;
		}
	}
	std::printf("wrote %s (%d fps)\n", raw_path.c_str(), play_fps);
	return 0;
}
