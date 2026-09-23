#include "session.hpp"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <thread>

const char* last_err()
{
	static char text[640];
	char detail[512];
	size_t n = sizeof(detail);
	std::memset(detail, 0, sizeof(detail));
	GC_ERROR e = scap_get_last_error_info(detail, &n);
	std::snprintf(text, sizeof(text), "gc=%d %s", (int)e, detail);
	return text;
}

bool parse_level(const std::string& name, scap_level_t& level)
{
	if (name == "interface" || name == "card") {
		level = scap_interface;
		return true;
	}
	if (name == "device") {
		level = scap_device;
		return true;
	}
	if (name == "camera") {
		level = scap_camera;
		return true;
	}
	if (name == "stream" || name == "data_stream") {
		level = scap_data_stream;
		return true;
	}
	return false;
}

void* feature_handle(const Session& session, scap_level_t level)
{
	if (level == scap_interface)
		return session.card;
	return session.cap;
}

bool get_feature(void* handle, scap_level_t level, const char* name, std::string& out)
{
	char buf[1024];
	size_t n = sizeof(buf);
	std::memset(buf, 0, sizeof(buf));
	if (!handle || !scap_get_string(handle, level, name, buf, &n))
		return false;
	out.assign(buf);
	return true;
}

bool set_feature(void* handle, scap_level_t level, const char* name, const char* value)
{
	if (!handle)
		return false;
	if (!value || value[0] == '\0')
		return scap_set_command(handle, level, name);
	if (scap_set_string(handle, level, name, value))
		return true;
	char* end = nullptr;
	double number = std::strtod(value, &end);
	if (end != value && end && *end == '\0') {
		if (scap_set_float(handle, level, name, number))
			return true;
		if (scap_set_integer(handle, level, name, static_cast<int64_t>(number)))
			return true;
	}
	return false;
}

Session::~Session()
{
	close();
}

void Session::stop()
{
	if (grabbing && cap) {
		scap_stop(cap);
		grabbing = false;
	}
}

void Session::close()
{
	stop();
	if (cap) {
		scap_close_by_camera(cap);
		cap = nullptr;
	}
	if (card) {
		scap_close_card(card);
		card = nullptr;
	}
	if (sdk) {
		scap_done();
		sdk = false;
	}
}

static void remember(SCAP_HANDLE cap, const char* name, std::string& dest)
{
	std::string value;
	if (get_feature(cap, scap_camera, name, value))
		dest = value;
}

static bool apply_camera(SCAP_HANDLE cap, const Opt& opt)
{
	if (opt.format_set && !set_feature(cap, scap_camera, "PixelFormat", opt.format.c_str())) {
		std::fprintf(stderr, "PixelFormat=%s: %s\n", opt.format.c_str(), last_err());
		return false;
	}
	if (opt.exposure_set) {
		char exposure[64];
		std::snprintf(exposure, sizeof(exposure), "%.0f", opt.exposure_us);
		if (!set_feature(cap, scap_camera, "ExposureTime", exposure))
			std::fprintf(stderr, "ExposureTime: %s\n", last_err());
	}
	if (opt.gain_set) {
		char gain[64];
		std::snprintf(gain, sizeof(gain), "%.6g", opt.gain);
		if (!set_feature(cap, scap_camera, "GainRaw", gain))
			std::fprintf(stderr, "GainRaw: %s\n", last_err());
		if (!set_feature(cap, scap_camera, "AnalogGain", gain))
			std::fprintf(stderr, "AnalogGain: %s\n", last_err());
	}
	return true;
}

void Session::refresh()
{
	if (!cap)
		return;
	cxp_std_features_t cam;
	std::memset(&cam, 0, sizeof(cam));
	if (scap_get_camera_info(cap, &cam, true)) {
		vendor = cam.device_vendor_name;
		model = cam.device_model_name;
		serial = cam.device_serial_number;
		format = cam.format_str;
		width = cam.width;
		height = cam.height;
	} else {
		std::fprintf(stderr, "scap_get_camera_info: %s\n", last_err());
	}
	remember(cap, "PixelFormat", format);
	remember(cap, "ExposureTime", exposure);
	remember(cap, "GainRaw", gain_raw);
	remember(cap, "AnalogGain", analog_gain);
	remember(cap, "TriggerMode", trigger);
	remember(cap, "AcquisitionMode", acquisition);
}

bool Session::configure(const Opt& opt)
{
	if (!cap)
		return false;
	if (!apply_camera(cap, opt))
		return false;
	refresh();
	return true;
}

static void read_tick(Session& session)
{
	const char* names[] = {
		"TimestampTickFrequency",
		"GevTimestampTickFrequency",
		"TimestampFrequency"
	};
	for (const char* name : names) {
		int64_t hz = 0;
		if (session.cap && scap_get_integer(session.cap, scap_device, name, &hz) && hz > 0) {
			session.tick_hz = static_cast<uint64_t>(hz);
			return;
		}
		if (session.cap && scap_get_integer(session.cap, scap_camera, name, &hz) && hz > 0) {
			session.tick_hz = static_cast<uint64_t>(hz);
			return;
		}
	}
}

bool Session::open(const Opt& opt, OpenMode mode)
{
	close();
	if (!opt.gentl.empty())
		setenv("GENICAM_GENTL64_PATH", opt.gentl.c_str(), 0);
	if (!scap_init()) {
		std::fprintf(stderr, "scap_init: %s\n", last_err());
		return false;
	}
	sdk = true;

	uint32_t cards = scap_get_card_num();
	std::fprintf(stderr, "карт: %u\n", cards);
	if (opt.card >= cards) {
		std::fprintf(stderr, "нет карты %u\n", opt.card);
		close();
		return false;
	}

	gentl_if_info_t info;
	std::memset(&info, 0, sizeof(info));
	if (scap_preread_card_info(opt.card, &info))
		card_name = info.if_display_name;

	card = scap_create_card(opt.card);
	if (!card) {
		std::fprintf(stderr, "scap_create_card: %s\n", last_err());
		close();
		return false;
	}

	if (!opt.firmware.empty()) {
		if (!set_feature(card, scap_interface, "FirmwareMode", opt.firmware.c_str()))
			std::fprintf(stderr, "FirmwareMode: %s\n", last_err());
	}
	scap_set_integer(card, scap_interface, "DeviceSelector", static_cast<int64_t>(opt.camera));
	if (!set_feature(card, scap_interface, "CxpLinkConfiguration", opt.link.c_str())) {
		std::fprintf(stderr, "линк %s не принят (%s), пробую Auto\n", opt.link.c_str(), last_err());
		if (!set_feature(card, scap_interface, "CxpLinkConfiguration", "Auto")) {
			std::fprintf(stderr, "CxpLinkConfiguration: %s\n", last_err());
			close();
			return false;
		}
	}
	if (!set_feature(card, scap_interface, "CxpPoCxpAuto", nullptr))
		std::fprintf(stderr, "CxpPoCxpAuto: %s\n", last_err());

	if (opt.link_wait > 0) {
		std::fprintf(stderr, "ожидание линка %.1f с\n", opt.link_wait);
		std::this_thread::sleep_for(std::chrono::duration<double>(opt.link_wait));
	}

	gentl_dev_info_t dev;
	std::memset(&dev, 0, sizeof(dev));
	if (scap_preread_device_info(card, opt.camera, &dev) && dev.TsFrequency > 0)
		tick_hz = dev.TsFrequency;

	if (mode == OpenMode::CardOnly)
		return true;

	cap = scap_create_by_camera(card, opt.camera);
	if (!cap) {
		std::fprintf(stderr, "scap_create_by_camera: %s\n", last_err());
		close();
		return false;
	}

	read_tick(*this);

	if (mode == OpenMode::CameraConfigured && !apply_camera(cap, opt)) {
		close();
		return false;
	}
	refresh();
	std::fprintf(stderr, "камера %s %s sn=%s %lldx%lld %s\n",
		vendor.c_str(), model.c_str(), serial.c_str(),
		(long long)width, (long long)height, format.c_str());
	if (!exposure.empty())
		std::fprintf(stderr, "ExposureTime = %s мкс\n", exposure.c_str());
	if (!gain_raw.empty() || !analog_gain.empty())
		std::fprintf(stderr, "GainRaw = %s, AnalogGain = %s\n",
			gain_raw.empty() ? "-" : gain_raw.c_str(),
			analog_gain.empty() ? "-" : analog_gain.c_str());
	if (!trigger.empty() || !acquisition.empty())
		std::fprintf(stderr, "TriggerMode = %s, AcquisitionMode = %s\n",
			trigger.empty() ? "-" : trigger.c_str(),
			acquisition.empty() ? "-" : acquisition.c_str());
	if (mode == OpenMode::CameraConfigured && trigger != "Off" && !trigger.empty())
		std::fprintf(stderr, "триггер включён: поток сам не пойдёт, пока нет импульса\n");
	return true;
}

bool Session::start(uint32_t bufs)
{
	if (!cap)
		return false;
	if (grabbing)
		stop();
	if (bufs == 0)
		bufs = 1;
	std::fprintf(stderr, "старт захвата, буферов %u\n", bufs);
	if (!scap_start(cap, bufs)) {
		std::fprintf(stderr, "scap_start: %s\n", last_err());
		return false;
	}
	grabbing = true;
	return true;
}
