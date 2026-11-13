#include "session.hpp"

#include <atomic>
#include <chrono>
#include <cerrno>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <mutex>
#include <string>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <thread>
#include <unistd.h>
#include <vector>

// Захват в одном потоке: буфер карты возвращается сразу после прореживания.
// На экран уходит только уменьшенная RGB-копия в shared memory.
// Команды GUI исполняются в том же потоке, между кадрами.

namespace {

constexpr uint32_t kMagic = 0x31565047u;
constexpr int kMaxW = 1600;
constexpr int kMaxH = 1100;
constexpr size_t kPixOff = 512;
constexpr size_t kShmSize = kPixOff + static_cast<size_t>(kMaxW) * kMaxH * 3;
const char* kShmName = "/grabercam-preview";
const char* kSockPath = "/tmp/grabercam-view.sock";

struct Head {
	uint32_t magic;
	uint32_t seq;
	uint32_t width;
	uint32_t height;
	uint32_t cam_w;
	uint32_t cam_h;
	float grab_fps;
	float preview_fps;
	char format[48];
	char model[80];
	char serial[48];
	char exposure[32];
	char gain[32];
	char analog[32];
	char trigger[32];
};

struct Job {
	std::string line;
	std::string reply;
	bool ready = false;
};

std::atomic<bool> g_stop{false};
std::mutex g_mu;
std::condition_variable g_cv;
Job* g_job = nullptr;

Head* g_head = nullptr;
uint8_t* g_pix = nullptr;
int g_shm_fd = -1;

void put_text(char* dst, size_t n, const std::string& s)
{
	if (n == 0)
		return;
	std::snprintf(dst, n, "%s", s.c_str());
}

bool needs_restart(const std::string& name)
{
	return name == "PixelFormat" || name == "Width" || name == "Height"
		|| name == "OffsetX" || name == "OffsetY" || name == "Binning"
		|| name == "BinningMode" || name == "ReverseX" || name == "ReverseY";
}

int bytes_per_pixel(size_t size, int64_t w, int64_t h)
{
	if (w <= 0 || h <= 0)
		return 1;
	size_t pixels = static_cast<size_t>(w) * static_cast<size_t>(h);
	if (pixels == 0)
		return 1;
	if (size >= pixels * 2)
		return 2;
	return 1;
}

uint8_t gray_from_pixel(const uint8_t* p, int bpp, const std::string& format)
{
	if (bpp <= 1)
		return p[0];
	uint16_t v = static_cast<uint16_t>(p[0] | (p[1] << 8));
	if (format.find("12") != std::string::npos)
		return static_cast<uint8_t>(v >> 4);
	if (format.find("10") != std::string::npos)
		return static_cast<uint8_t>(v >> 2);
	return static_cast<uint8_t>(v >> 8);
}

void publish(const uint8_t* src, size_t size, int64_t cam_w, int64_t cam_h, const Session& session, float grab_fps, float preview_fps)
{
	if (!g_head || cam_w <= 0 || cam_h <= 0 || !src)
		return;
	int sw = static_cast<int>(cam_w);
	int sh = static_cast<int>(cam_h);
	int bpp = bytes_per_pixel(size, cam_w, cam_h);
	if (static_cast<size_t>(sw) * static_cast<size_t>(sh) * static_cast<size_t>(bpp) > size)
		return;
	int step_x = (sw + kMaxW - 1) / kMaxW;
	int step_y = (sh + kMaxH - 1) / kMaxH;
	int step = step_x > step_y ? step_x : step_y;
	if (step < 1)
		step = 1;
	int dw = sw / step;
	int dh = sh / step;
	if (dw < 1)
		dw = 1;
	if (dh < 1)
		dh = 1;
	if (dw > kMaxW)
		dw = kMaxW;
	if (dh > kMaxH)
		dh = kMaxH;

	uint32_t seq = __atomic_load_n(&g_head->seq, __ATOMIC_RELAXED);
	__atomic_store_n(&g_head->seq, seq + 1, __ATOMIC_RELEASE);

	uint8_t* dst = g_pix;
	for (int y = 0; y < dh; ++y) {
		int sy = y * step;
		if (sy >= sh)
			sy = sh - 1;
		const uint8_t* row = src + static_cast<size_t>(sy) * static_cast<size_t>(sw) * static_cast<size_t>(bpp);
		uint8_t* out = dst + static_cast<size_t>(y) * static_cast<size_t>(dw) * 3;
		for (int x = 0; x < dw; ++x) {
			int sx = x * step;
			if (sx >= sw)
				sx = sw - 1;
			uint8_t g = gray_from_pixel(row + static_cast<size_t>(sx) * static_cast<size_t>(bpp), bpp, session.format);
			out[0] = g;
			out[1] = g;
			out[2] = g;
			out += 3;
		}
	}
	g_head->width = static_cast<uint32_t>(dw);
	g_head->height = static_cast<uint32_t>(dh);
	g_head->cam_w = static_cast<uint32_t>(sw);
	g_head->cam_h = static_cast<uint32_t>(sh);
	g_head->grab_fps = grab_fps;
	g_head->preview_fps = preview_fps;
	put_text(g_head->format, sizeof(g_head->format), session.format);
	put_text(g_head->model, sizeof(g_head->model), session.model);
	put_text(g_head->serial, sizeof(g_head->serial), session.serial);
	put_text(g_head->exposure, sizeof(g_head->exposure), session.exposure);
	put_text(g_head->gain, sizeof(g_head->gain), session.gain_raw);
	put_text(g_head->analog, sizeof(g_head->analog), session.analog_gain);
	put_text(g_head->trigger, sizeof(g_head->trigger), session.trigger);
	__atomic_store_n(&g_head->seq, seq + 2, __ATOMIC_RELEASE);
}

bool save_frame(const std::string& path, const void* base, size_t size, int64_t w, int64_t h, int bpp)
{
	FILE* file = std::fopen(path.c_str(), "wb");
	if (!file)
		return false;
	bool pgm = path.size() >= 4 && path.compare(path.size() - 4, 4, ".pgm") == 0;
	bool ok = true;
	if (pgm && bpp == 1 && w > 0 && h > 0 && size >= static_cast<size_t>(w * h)) {
		std::fprintf(file, "P5\n%lld %lld\n255\n", (long long)w, (long long)h);
		ok = std::fwrite(base, 1, static_cast<size_t>(w * h), file) == static_cast<size_t>(w * h);
	} else {
		ok = std::fwrite(base, 1, size, file) == size;
	}
	std::fclose(file);
	return ok;
}

std::string json_escape(const std::string& text)
{
	std::string out;
	out.reserve(text.size() + 8);
	for (unsigned char c : text) {
		if (c == '"' || c == '\\') {
			out.push_back('\\');
			out.push_back(static_cast<char>(c));
		} else if (c < 0x20) {
			continue;
		} else {
			out.push_back(static_cast<char>(c));
		}
	}
	return out;
}

std::string exec_line(Session& session, uint32_t bufs, const std::string& line)
{
	if (line == "QUIT") {
		g_stop.store(true);
		return "OK bye";
	}
	if (line == "INFO") {
		char buf[640];
		std::snprintf(buf, sizeof(buf),
			"OK {\"model\":\"%s\",\"serial\":\"%s\",\"format\":\"%s\",\"w\":%lld,\"h\":%lld,"
			"\"exposure\":\"%s\",\"gain\":\"%s\",\"analog\":\"%s\",\"trigger\":\"%s\",\"acq\":\"%s\"}",
			json_escape(session.model).c_str(),
			json_escape(session.serial).c_str(),
			json_escape(session.format).c_str(),
			(long long)session.width,
			(long long)session.height,
			json_escape(session.exposure).c_str(),
			json_escape(session.gain_raw).c_str(),
			json_escape(session.analog_gain).c_str(),
			json_escape(session.trigger).c_str(),
			json_escape(session.acquisition).c_str());
		return buf;
	}
	if (line.compare(0, 4, "GET ") == 0) {
		std::string name = line.substr(4);
		std::string value;
		if (!get_feature(session.cap, scap_camera, name.c_str(), value))
			return std::string("ERR ") + last_err();
		return "OK " + value;
	}
	auto restart_and_set = [&](const std::string& name, const char* value) -> std::string {
		bool restart = needs_restart(name);
		if (restart && session.grabbing)
			session.stop();
		if (!set_feature(session.cap, scap_camera, name.c_str(), value)) {
			std::string err = last_err();
			if (restart && !session.grabbing)
				session.start(bufs);
			return "ERR " + err;
		}
		session.refresh();
		if (restart && !session.grabbing && !session.start(bufs))
			return std::string("ERR ") + last_err();
		return "OK";
	};
	if (line.compare(0, 4, "SET ") == 0) {
		std::string rest = line.substr(4);
		auto sp = rest.find(' ');
		std::string name = sp == std::string::npos ? rest : rest.substr(0, sp);
		std::string value = sp == std::string::npos ? "" : rest.substr(sp + 1);
		if (name.empty())
			return "ERR пустое имя";
		if ((value == "On" || value == "Off") && !needs_restart(name)) {
			if (!set_feature(session.cap, scap_camera, name.c_str(), value.c_str())) {
				const char* alt = value == "On" ? "1" : "0";
				if (!set_feature(session.cap, scap_camera, name.c_str(), alt))
					return std::string("ERR ") + last_err();
			}
			session.refresh();
			return "OK";
		}
		return restart_and_set(name, value.c_str());
	}
	if (line.compare(0, 4, "ROI ") == 0) {
		long long w = 0;
		long long h = 0;
		long long x = 0;
		long long y = 0;
		if (std::sscanf(line.c_str() + 4, "%lld %lld %lld %lld", &w, &h, &x, &y) != 4)
			return "ERR нужно: ROI ширина высота x y";
		if (session.grabbing)
			session.stop();
		char buf[64];
		auto seti = [&](const char* name, int64_t v) {
			std::snprintf(buf, sizeof(buf), "%lld", (long long)v);
			return set_feature(session.cap, scap_camera, name, buf);
		};
		if (!seti("OffsetX", 0) || !seti("OffsetY", 0) || !seti("Width", w) || !seti("Height", h)
			|| !seti("OffsetX", x) || !seti("OffsetY", y)) {
			std::string err = last_err();
			session.refresh();
			session.start(bufs);
			return "ERR " + err;
		}
		session.refresh();
		if (!session.start(bufs))
			return std::string("ERR ") + last_err();
		return "OK";
	}
	return "ERR неизвестная команда";
}

void finish_job(Job* job, std::string reply)
{
	{
		std::lock_guard<std::mutex> lock(g_mu);
		job->reply = std::move(reply);
		job->ready = true;
	}
	g_cv.notify_all();
}

void wake_socket()
{
	int poke = ::socket(AF_UNIX, SOCK_STREAM, 0);
	if (poke < 0)
		return;
	sockaddr_un addr{};
	addr.sun_family = AF_UNIX;
	std::snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", kSockPath);
	::connect(poke, reinterpret_cast<sockaddr*>(&addr), sizeof(addr));
	::close(poke);
}

Job* take_job()
{
	std::lock_guard<std::mutex> lock(g_mu);
	if (!g_job || g_job->ready)
		return nullptr;
	return g_job;
}

void socket_thread()
{
	int fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
	if (fd < 0) {
		std::perror("socket");
		g_stop.store(true);
		return;
	}
	::unlink(kSockPath);
	sockaddr_un addr{};
	addr.sun_family = AF_UNIX;
	std::snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", kSockPath);
	if (::bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
		std::perror("bind");
		::close(fd);
		g_stop.store(true);
		return;
	}
	if (::listen(fd, 1) != 0) {
		std::perror("listen");
		::close(fd);
		g_stop.store(true);
		return;
	}
	std::fprintf(stderr, "gui socket %s\n", kSockPath);
	while (!g_stop.load()) {
		int client = ::accept(fd, nullptr, nullptr);
		if (client < 0) {
			if (errno == EINTR)
				continue;
			break;
		}
		std::string buf;
		char tmp[512];
		while (!g_stop.load()) {
			ssize_t n = ::recv(client, tmp, sizeof(tmp), 0);
			if (n <= 0)
				break;
			buf.append(tmp, tmp + n);
			for (;;) {
				auto nl = buf.find('\n');
				if (nl == std::string::npos)
					break;
				std::string line = buf.substr(0, nl);
				buf.erase(0, nl + 1);
				if (!line.empty() && line.back() == '\r')
					line.pop_back();
				Job job;
				job.line = line;
				{
					std::unique_lock<std::mutex> lock(g_mu);
					g_cv.wait(lock, [] { return g_job == nullptr || g_stop.load(); });
					if (g_stop.load())
						break;
					g_job = &job;
					g_cv.notify_all();
					g_cv.wait(lock, [&] { return job.ready || g_stop.load(); });
					g_job = nullptr;
				}
				g_cv.notify_all();
				std::string reply = job.reply.empty() ? "ERR нет ответа" : job.reply;
				reply.push_back('\n');
				if (::send(client, reply.data(), reply.size(), MSG_NOSIGNAL) < 0)
					break;
				if (line == "QUIT")
					g_stop.store(true);
			}
		}
		::close(client);
		if (g_stop.load())
			break;
	}
	::close(fd);
	::unlink(kSockPath);
}

bool setup_shm()
{
	::shm_unlink(kShmName);
	g_shm_fd = ::shm_open(kShmName, O_CREAT | O_RDWR, 0644);
	if (g_shm_fd < 0) {
		std::perror("shm_open");
		return false;
	}
	if (::ftruncate(g_shm_fd, static_cast<off_t>(kShmSize)) != 0) {
		std::perror("ftruncate");
		return false;
	}
	void* mem = ::mmap(nullptr, kShmSize, PROT_READ | PROT_WRITE, MAP_SHARED, g_shm_fd, 0);
	if (mem == MAP_FAILED) {
		std::perror("mmap");
		return false;
	}
	std::memset(mem, 0, kShmSize);
	g_head = static_cast<Head*>(mem);
	g_head->magic = kMagic;
	g_pix = static_cast<uint8_t*>(mem) + kPixOff;
	return true;
}

void usage()
{
	std::fputs(
		"graberview [--card N] [--camera N] [--link NAME] [--link-wait SEC]\n"
		"Живое превью и сокет команд для gui/live.py\n",
		stderr);
}

} // namespace

int main(int argc, char** argv)
{
	Opt opt;
	opt.bufs = 12;
	for (int i = 1; i < argc; ++i) {
		std::string arg = argv[i];
		auto need = [&](const char* flag) -> const char* {
			if (i + 1 >= argc) {
				std::fprintf(stderr, "нет значения у %s\n", flag);
				return nullptr;
			}
			return argv[++i];
		};
		if (arg == "-h" || arg == "--help") {
			usage();
			return 0;
		} else if (arg == "--card") {
			const char* v = need("--card");
			if (!v)
				return 1;
			opt.card = static_cast<uint32_t>(std::strtoul(v, nullptr, 10));
		} else if (arg == "--camera") {
			const char* v = need("--camera");
			if (!v)
				return 1;
			opt.camera = static_cast<uint32_t>(std::strtoul(v, nullptr, 10));
		} else if (arg == "--link") {
			const char* v = need("--link");
			if (!v)
				return 1;
			opt.link = v;
		} else if (arg == "--link-wait") {
			const char* v = need("--link-wait");
			if (!v)
				return 1;
			opt.link_wait = std::strtod(v, nullptr);
		} else if (arg == "--gentl") {
			const char* v = need("--gentl");
			if (!v)
				return 1;
			opt.gentl = v;
		} else {
			std::fprintf(stderr, "неизвестный параметр %s\n", arg.c_str());
			return 1;
		}
	}

	if (!setup_shm())
		return 1;

	std::thread io(socket_thread);

	Session session;
	if (!session.open(opt, OpenMode::Camera)) {
		g_stop.store(true);
		wake_socket();
		io.join();
		return 2;
	}
	if (!session.start(opt.bufs)) {
		g_stop.store(true);
		session.close();
		wake_socket();
		io.join();
		return 3;
	}

	uint64_t frames = 0;
	uint64_t previews = 0;
	auto fps_t0 = std::chrono::steady_clock::now();
	uint64_t fps_n0 = 0;
	uint64_t prev_n0 = 0;
	float grab_fps = 0;
	float preview_fps = 0;
	Job* snap_job = nullptr;
	std::string snap_path;

	while (!g_stop.load()) {
		if (!snap_job) {
			if (Job* job = take_job()) {
				if (job->line.compare(0, 5, "SNAP ") == 0) {
					snap_path = job->line.substr(5);
					snap_job = job;
				} else {
					finish_job(job, exec_line(session, opt.bufs, job->line));
				}
			}
		}
		if (g_stop.load())
			break;
		if (!session.grabbing) {
			std::this_thread::sleep_for(std::chrono::milliseconds(20));
			continue;
		}
		BUFFER_HANDLE buf = nullptr;
		void* pointer = nullptr;
		size_t bytes = 0;
		if (!scap_get_buf(session.cap, 100, &buf, &pointer, &bytes) || !pointer || bytes == 0)
			continue;
		cxp_buf_info_t info{};
		scap_get_buf_info(session.cap, buf, &info, true);
		int64_t w = info.width > 0 ? info.width : session.width;
		int64_t h = info.height > 0 ? info.height : session.height;
		++frames;
		auto now = std::chrono::steady_clock::now();
		double dt = std::chrono::duration<double>(now - fps_t0).count();
		if (dt >= 0.25) {
			grab_fps = static_cast<float>((frames - fps_n0) / dt);
			preview_fps = static_cast<float>((previews - prev_n0) / dt);
			fps_n0 = frames;
			prev_n0 = previews;
			fps_t0 = now;
		}
		if (snap_job) {
			int bpp = bytes_per_pixel(bytes, w, h);
			bool ok = save_frame(snap_path, pointer, bytes, w, h, bpp);
			if (ok)
				std::fprintf(stderr, "кадр %s (%zu байт)\n", snap_path.c_str(), bytes);
			finish_job(snap_job, ok ? ("OK " + std::to_string(bytes)) : "ERR запись");
			snap_job = nullptr;
		}
		publish(static_cast<const uint8_t*>(pointer), bytes, w, h, session, grab_fps, preview_fps);
		++previews;
		scap_back_buffer(session.cap, buf);
	}

	if (snap_job)
		finish_job(snap_job, "ERR остановлено");
	session.close();
	g_stop.store(true);
	g_cv.notify_all();
	wake_socket();
	io.join();
	if (g_head)
		::munmap(g_head, kShmSize);
	if (g_shm_fd >= 0)
		::close(g_shm_fd);
	::shm_unlink(kShmName);
	return 0;
}
