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
#include <string>
#include <thread>
#include <vector>

static std::atomic<int> g_got{0};
static std::atomic<int> g_seen{0};
static std::string g_out;

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

static bool set_cmd(void* h, scap_level_t lvl, const char* feat)
{
	bool ok = scap_set_command(h, lvl, feat);
	std::printf("cmd %s -> %s\n", feat, ok ? "ok" : "FAIL");
	if (!ok)
		print_err(feat);
	return ok;
}

static void get_str(void* h, scap_level_t lvl, const char* feat)
{
	char buf[256];
	size_t n = sizeof(buf);
	std::memset(buf, 0, sizeof(buf));
	bool ok = scap_get_string(h, lvl, feat, buf, &n);
	if (ok)
		std::printf("  %s = %s\n", feat, buf);
	else
		std::printf("  %s = <unreadable>\n", feat);
}

static void save_frame(const void* base, size_t size, const cxp_buf_info_t& info, const char* fmt)
{
	const int64_t w = info.width;
	const int64_t h = info.height;
	std::printf("frame id=%llu %lldx%lld fmt=%lld (%s) bytes=%zu\n",
		(unsigned long long)info.frame_id,
		(long long)w, (long long)h, (long long)info.format,
		fmt ? fmt : "", size);

	std::string raw = g_out + ".raw";
	{
		std::ofstream f(raw, std::ios::binary);
		f.write(static_cast<const char*>(base), static_cast<std::streamsize>(size));
	}
	std::printf("wrote %s\n", raw.c_str());

	if (w > 0 && h > 0 && size >= static_cast<size_t>(w * h)) {
		std::string pgm = g_out + ".pgm";
		std::ofstream f(pgm, std::ios::binary);
		f << "P5\n" << w << " " << h << "\n255\n";
		const auto* p = static_cast<const unsigned char*>(base);
		if (std::strcmp(fmt ? fmt : "", "Mono8") == 0 || size == static_cast<size_t>(w * h)) {
			f.write(reinterpret_cast<const char*>(p), static_cast<std::streamsize>(w * h));
		} else {
			std::vector<unsigned char> gray(static_cast<size_t>(w * h));
			size_t step = size / static_cast<size_t>(w * h);
			if (step < 2)
				step = 2;
			for (int64_t i = 0; i < w * h; ++i)
				gray[static_cast<size_t>(i)] = p[static_cast<size_t>(i) * step + (step - 1)];
			f.write(reinterpret_cast<const char*>(gray.data()), static_cast<std::streamsize>(gray.size()));
		}
		std::printf("wrote %s\n", pgm.c_str());
	}
}

static void on_frame(void* ctx, GenTL::BUFFER_HANDLE h_buf, void* base, size_t size)
{
	if (!base)
		return;
	if (g_seen.fetch_add(1) < 2)
		return;
	if (g_got.exchange(1))
		return;
	auto* cap = static_cast<SCAP_HANDLE>(ctx);
	cxp_buf_info_t info;
	std::memset(&info, 0, sizeof(info));
	scap_get_buf_info(cap, h_buf, &info, true);
	char fmt[128];
	size_t n = sizeof(fmt);
	std::memset(fmt, 0, sizeof(fmt));
	scap_get_string(cap, scap_camera, "PixelFormat", fmt, &n);
	save_frame(base, size, info, fmt);
}

int main(int argc, char** argv)
{
	g_out = argc > 1 ? argv[1] : "/home/user/work/GraberCam/frame";
	setenv("GENICAM_GENTL64_PATH", "/opt/magewell/usr/lib", 0);

	if (!scap_init()) {
		std::printf("scap_init failed\n");
		print_err("scap_init");
		return 1;
	}
	std::printf("cards: %u\n", scap_get_card_num());

	CARD_HANDLE card = scap_create_card(0);
	if (!card) {
		std::printf("scap_create_card failed\n");
		print_err("create_card");
		scap_done();
		return 1;
	}
	std::printf("card open\n");
	get_str(card, scap_interface, "FirmwareMode");
	get_str(card, scap_interface, "CxpLinkConfiguration");

	set_str(card, scap_interface, "FirmwareMode", "AAAA");
	scap_set_integer(card, scap_interface, "DeviceSelector", 0);
	if (!set_str(card, scap_interface, "CxpLinkConfiguration", "CXP12_X4"))
		set_str(card, scap_interface, "CxpLinkConfiguration", "Auto");
	set_cmd(card, scap_interface, "CxpPoCxpAuto");

	std::printf("waiting for link...\n");
	std::this_thread::sleep_for(std::chrono::seconds(3));
	get_str(card, scap_interface, "CxpLinkConfiguration");

	SCAP_HANDLE cap = scap_create_by_camera(card, 0);
	if (!cap) {
		std::printf("scap_create_by_camera failed\n");
		print_err("create_camera");
		scap_close_card(card);
		scap_done();
		return 2;
	}
	std::printf("camera open\n");

	cxp_std_features_t cam;
	std::memset(&cam, 0, sizeof(cam));
	if (scap_get_camera_info(cap, &cam, true)) {
		std::printf("vendor=%s model=%s sn=%s %lldx%lld %s\n",
			cam.device_vendor_name, cam.device_model_name, cam.device_serial_number,
			(long long)cam.width, (long long)cam.height, cam.format_str);
	} else {
		print_err("camera_info");
	}

	set_str(cap, scap_camera, "TriggerMode", "Off");
	set_str(cap, scap_camera, "AcquisitionMode", "Continuous");
	set_str(cap, scap_camera, "PixelFormat", "Mono8");
	set_str(cap, scap_camera, "ExposureAuto", "Off");
	get_str(cap, scap_camera, "ExposureTime");
	get_str(cap, scap_camera, "Gain");
	if (!scap_set_float(cap, scap_camera, "ExposureTime", 80000.0)) {
		print_err("ExposureTime float");
		if (!scap_set_integer(cap, scap_camera, "ExposureTime", 500000))
			print_err("ExposureTime int");
	}
	for (const char* g : {"Gain", "GainRaw", "AnalogGain"}) {
		if (scap_set_float(cap, scap_camera, g, 2.0))
			std::printf("set %s=2 ok\n", g);
	}
	get_str(cap, scap_camera, "ExposureTime");
	get_str(cap, scap_camera, "Gain");

	if (!scap_start_capture(cap, on_frame, cap, 8, 4, false)) {
		std::printf("start_capture failed\n");
		print_err("start");
		scap_close_by_camera(cap);
		scap_close_card(card);
		scap_done();
		return 3;
	}

	for (int i = 0; i < 200 && !g_got.load(); ++i)
		std::this_thread::sleep_for(std::chrono::milliseconds(100));

	scap_stop_capture(cap);
	scap_close_by_camera(cap);
	scap_close_card(card);
	scap_done();

	if (!g_got.load()) {
		std::printf("no frame\n");
		return 4;
	}
	std::printf("done\n");
	return 0;
}
