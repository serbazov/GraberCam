#pragma once

#include "CEasyCapS.h"

#include <cstdint>
#include <string>
#include <vector>

struct Opt {
	uint32_t card = 0;
	uint32_t camera = 0;
	std::string link = "CXP12_X4";
	std::string firmware = "AAAA";
	double link_wait = 2.0;
	double exposure_us = 80000.0;
	bool exposure_set = false;
	double gain = 2.0;
	bool gain_set = false;
	bool bytes_set = false;
	std::string format = "Mono8";
	bool format_set = false;
	uint32_t bufs = 8;
	uint64_t timeout_ms = 1000;
	std::string level = "camera";
	std::string output;
	uint64_t frames = 0;
	double seconds = 0;
	std::string mode;
	uint32_t warmup = 8;
	uint32_t skip = 2;
	uint64_t bytes = 65408000;
	std::string json;
	bool json_set = false;
	bool no_json = false;
	std::string gentl = "/opt/magewell/usr/lib";
	std::vector<std::string> pos;
};

enum class OpenMode {
	CardOnly,
	Camera,
	CameraConfigured
};

struct Session {
	CARD_HANDLE card = nullptr;
	SCAP_HANDLE cap = nullptr;
	bool sdk = false;
	bool grabbing = false;

	std::string card_name;
	std::string vendor;
	std::string model;
	std::string serial;
	std::string format;
	std::string exposure;
	std::string gain_raw;
	std::string analog_gain;
	std::string trigger;
	std::string acquisition;
	int64_t width = 0;
	int64_t height = 0;
	uint64_t tick_hz = 0;

	~Session();
	bool open(const Opt& opt, OpenMode mode);
	bool configure(const Opt& opt);
	void refresh();
	bool start(uint32_t bufs);
	void stop();
	void close();
};

const char* last_err();
bool parse_level(const std::string& name, scap_level_t& level);
void* feature_handle(const Session& session, scap_level_t level);
bool get_feature(void* handle, scap_level_t level, const char* name, std::string& out);
bool set_feature(void* handle, scap_level_t level, const char* name, const char* value);
