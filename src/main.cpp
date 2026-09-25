#include "session.hpp"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cerrno>
#include <cstdlib>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <dirent.h>
#include <fcntl.h>
#include <fstream>
#include <limits>
#include <numeric>
#include <string>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <unistd.h>
#include <vector>

extern "C" {
char* readline(const char* prompt);
void add_history(const char* line);
void using_history(void);
int read_history(const char* filename);
int write_history(const char* filename);
int history_truncate_file(const char* filename, int nlines);
typedef char* (*rl_compentry_func_t)(const char*, int);
char** rl_completion_matches(const char* text, rl_compentry_func_t entry_func);
typedef char** (*rl_completion_func_t)(const char*, int, int);
extern rl_completion_func_t rl_attempted_completion_function;
extern int rl_attempted_completion_over;
extern char* rl_line_buffer;
extern FILE* rl_outstream;
}

// Захват идёт через scap_get_buf / scap_back_buffer в одном потоке:
// указатель кадра жив, пока буфер не возвращён карте, лишней копии нет.
// discard — потолок камеры и грабера, copy — memcpy, disk — запись этого
// буфера, sink — тот же размер блока на текущий диск без камеры,
// ssd — то же на смонтированный NVMe.

static std::atomic<bool> g_stop{false};

static void on_signal(int)
{
	g_stop.store(true);
}

struct Sample {
	uint64_t id = 0;
	uint64_t dev_ts = 0;
	std::chrono::steady_clock::time_point host;
};

struct Report {
	std::string mode;
	std::string error;
	std::string path;
	std::string format;
	std::string exposure;
	std::string gain_raw;
	std::string analog_gain;
	std::string trigger;
	std::string acquisition;
	std::string model;
	int64_t width = 0;
	int64_t height = 0;
	uint64_t payload = 0;
	uint64_t warmup = 0;
	uint64_t frames = 0;
	uint64_t dropped = 0;
	uint64_t disorder = 0;
	uint64_t timeouts = 0;
	uint64_t bytes = 0;
	double time_to_first_ms = 0;
	double span_s = 0;
	double delivered_fps = 0;
	double source_fps = 0;
	double grab_MBps = 0;
	double source_MBps = 0;
	double copy_MBps = 0;
	double write_MBps = 0;
	double save_MBps = 0;
	double fsync_ms = 0;
	double copy_s = 0;
	double write_s = 0;
	double iv_min = 0;
	double iv_mean = 0;
	double iv_p50 = 0;
	double iv_p99 = 0;
	double iv_max = 0;
	bool have_host = false;
	bool have_dev = false;
	double dev_min = 0;
	double dev_mean = 0;
	double dev_p50 = 0;
	double dev_p99 = 0;
	double dev_max = 0;
	size_t queued_min = std::numeric_limits<size_t>::max();
	size_t queued_last = 0;
	bool queued_seen = false;
	uint64_t tick_hz = 0;
	std::string storage;
};

struct GrabRequest {
	std::string mode;
	std::string path;
	double seconds = 0;
	uint64_t frames = 0;
	uint32_t warmup = 0;
	uint32_t bufs = 8;
	uint64_t timeout_ms = 1000;
	bool write_meta = false;
	bool sync_io = false;
};

static void usage()
{
	std::fputs(
		"grabercam <команда> [параметры]\n"
		"\n"
		"Команды:\n"
		"  info                     карта, камера, линк, размер кадра\n"
		"  get <feature>            чтение параметра; имена ниже\n"
		"  set <feature> [значение] запись; без значения это команда\n"
		"  snap                     один кадр, .raw и .pgm\n"
		"  record                   непрерывная запись в один raw\n"
		"  bench                    замер пропускной способности\n"
		"  shell                    одна сессия, команды со stdin до quit\n"
		"\n"
		"Режимы bench (--mode):\n"
		"  discard   захват, буфер сразу возвращается карте\n"
		"  copy      плюс memcpy кадра в память\n"
		"  disk      запись буфера SDK, write ждёт диск\n"
		"  sink      то же без камеры, блок --bytes, на cwd/диск проекта\n"
		"  ssd       как sink, но на смонтированный NVMe (--ssd-dir)\n"
		"  all       discard, copy, disk, sink, затем ssd (по умолчанию)\n"
		"\n"
		"Параметры:\n"
		"  --card N            карта, по умолчанию 0\n"
		"  --camera N          камера на карте, по умолчанию 0\n"
		"  --link NAME         CXP12_X4\n"
		"  --firmware NAME     AAAA\n"
		"  --link-wait SEC     пауза после PoCXP, по умолчанию 2\n"
		"  --format NAME       формат пикселя; без флага камера не переписывается\n"
		"  --exposure US       выдержка в мкс; без флага камера не переписывается\n"
		"  --gain X            GainRaw и AnalogGain; без флага камера не переписывается\n"
		"  --buf N             глубина DMA, по умолчанию 8\n"
		"  --timeout-ms N      ожидание кадра, по умолчанию 1000\n"
		"  --level L           camera | interface | device | stream\n"
		"  -o, --output PATH   файл назначения\n"
		"  -n, --frames N      сколько кадров сохранить или замерить\n"
		"  -t, --seconds SEC   длительность; у bench по умолчанию 5\n"
		"  --warmup N          кадры прогрева bench, в скорость не входят, 8\n"
		"  --skip N            сколько кадров пропустить в snap/record, 2\n"
		"  --bytes N           размер блока sink/ssd; в режиме all берётся размер кадра камеры\n"
		"  --ssd-dir PATH      каталог на NVMe для режима ssd; иначе ищется сам\n"
		"  --json PATH         отчёт bench, по умолчанию bench.json\n"
		"  --no-json\n"
		"  --gentl PATH        каталог .cti, по умолчанию /opt/magewell/usr/lib\n"
		"\n"
		"МБ/с считаются как 10^6 байт/с. record требует -n или -t.\n"
		"record пишет в кэш и делает fsync в конце. bench disk, sink и ssd\n"
		"ждут диск на каждом блоке, поэтому их МБ/с — это уже пластина.\n"
		"\n"
		"shell поднимает линк один раз и читает команды со stdin:\n"
		"info, get, set, snap, record, bench, help, quit.\n"
		"В TTY: Tab — подсказки, ↑/↓ — история команд (~/.grabercam_history).\n"
		"Флаги запуска shell — значения по умолчанию для этих команд.\n"
		"--card, --camera, --link, --firmware, --link-wait и --gentl\n"
		"внутри сессии не меняются. FirmwareMode, CxpLinkConfiguration,\n"
		"CxpPoCxpAuto и DeviceSelector из shell тоже не пишутся: они\n"
		"заново поднимают линк.\n"
		"Ctrl+C прерывает текущую команду и возвращает к приглашению.\n"
		"Ctrl+C на приглашении, quit и конец stdin закрывают сессию.\n"
		"\n"
		"Параметры камеры для get/set (уровень camera):\n"
		"  Width                        ширина кадра, пиксели\n"
		"  Height                       высота кадра, пиксели\n"
		"  OffsetX                      сдвиг области по горизонтали, пиксели\n"
		"  OffsetY                      сдвиг области по вертикали, пиксели\n"
		"  PixelFormat                  Mono8, Mono10, Mono12; BayerGR/RG/GB/BG 8, 10 или 12, если сенсор цветной\n"
		"  PixelSize                    полный размер пикселя в битах: Bpp8, Bpp10, Bpp12, Bpp16\n"
		"  PixelDynamicRangeMin         минимальное число после оцифровки\n"
		"  PixelDynamicRangeMax         максимальное число после оцифровки\n"
		"  SensorWidth                  полная ширина сенсора\n"
		"  SensorHeight                 полная высота сенсора\n"
		"  WidthMax                     наибольшая доступная ширина\n"
		"  HeightMax                    наибольшая доступная высота\n"
		"  PayloadSize                  байт в одном кадре на шине\n"
		"  TestImageSelector            картинка с камеры: Off, TestImage1, TestImage2\n"
		"  AcquisitionMode              Continuous, SingleFrame, MultiFrame\n"
		"  AcquisitionStart             команда: начать съёмку\n"
		"  AcquisitionStop              команда: остановить съёмку\n"
		"  AcquisitionFrameCount        сколько кадров снять в MultiFrame\n"
		"  AcquisitionFrameRate         заданная частота, Гц\n"
		"  AcquisitionFrameRateEnable   включить свою частоту вместо максимально возможной\n"
		"  ResultingFrameRateAbs        какая частота выходит при текущих настройках, Гц, чтение\n"
		"  AcquisitionStatusSelector    какой внутренний сигнал читать\n"
		"  AcquisitionStatus            состояние выбранного сигнала\n"
		"  TriggerSelector              что настраивать: AcquisitionStart или FrameStart\n"
		"  TriggerMode                  Off или On\n"
		"  TriggerSource                Software, Line0…Line5, CXPin\n"
		"  TriggerActivation            RisingEdge или FallingEdge\n"
		"  TriggerDelay                 задержка после импульса, мкс\n"
		"  TriggerSoftware              команда: выдать программный импульс\n"
		"  ExposureMode                 Timed (по ExposureTime) или TriggerWidth (пока держится импульс)\n"
		"  ExposureTime                 выдержка, мкс\n"
		"  ShutterMode                  режим затвора\n"
		"  GainSelector                 какой канал усиления меняют (All)\n"
		"  GainRaw                      усиление абсолютным значением\n"
		"  AnalogGain                   аналоговое усиление сенсора\n"
		"  ReferenceGain                внутреннее усиление для калибровки поля\n"
		"  Gamma                        гамма яркости\n"
		"  BlackLevelSelector           какой чёрный уровень менять\n"
		"  BlackLevel                   уровень чёрного\n"
		"  BalanceWhiteAuto             автобаланс белого\n"
		"  BalanceRatioSelector         какой цветовой канал баланса\n"
		"  BalanceRatio                 доля выбранного канала\n"
		"  Binning                      объединение ячеек сенсора\n"
		"  BinningMode                  как именно объединять ячейки\n"
		"  ReverseX                     отразить кадр по горизонтали\n"
		"  ReverseY                     отразить кадр по вертикали\n"
		"  ReverseMode                  как применяются ReverseX и ReverseY\n"
		"  MultipleROIEnable            несколько областей интереса\n"
		"  MultipleROICount             сколько областей\n"
		"  ROISelector                  какую область настраивать\n"
		"  ROIOffsetY                   вертикальный сдвиг выбранной области\n"
		"  ROIHeight                    высота выбранной области\n"
		"  AverageFilter                сглаживание: Off, On\n"
		"  MedianFilter                 медианный фильтр шума: Off, On\n"
		"  BackgroundSubtractionEnable  вычитание фона: Off, On\n"
		"  FrameAveragingEnable         усреднение нескольких кадров: Off, On\n"
		"  FrameAveraging               сколько кадров усреднять\n"
		"  PixelShiftEnable             режим сдвига пикселя: Off, On\n"
		"  PixelShiftDistance           шаг: FourShot_1_2_Pixel, FourShot_1_Pixel, NineShot_1_3_Pixel, UserTable\n"
		"  PixelShiftPositionReset      команда: вернуть сенсор в позицию 0\n"
		"  PixelShiftMetaData           писать данные сдвига в поток: Off, On\n"
		"  PixelShiftUserTableCount     длина пользовательской таблицы позиций\n"
		"  PixelShiftUserTableIndex     номер строки таблицы\n"
		"  PixelShiftUserTableValue     значение выбранной строки\n"
		"  PixelShiftUserTableShots     сколько кадров снять в этой позиции\n"
		"  PixelShiftTriggerMode        Auto или SoftwareTrigger\n"
		"  PixelShiftSoftwareTrigger    команда: шаг сдвига\n"
		"  LineSelector                 какой вход или выход разъёма настраивать\n"
		"  LineMode                     линия как вход или как выход\n"
		"  LineSource                   какой внутренний сигнал вывести на линию\n"
		"  LineFormat                   электрический тип линии\n"
		"  LineInverter                 инвертировать сигнал линии\n"
		"  LineStatus                   текущий уровень выбранной линии\n"
		"  LineStatusAll                уровни всех линий одним числом\n"
		"  LineDebouncerTimeAbs         подавление дребезга, мкс\n"
		"  UserOutputSelector           какой бит пользовательского выхода менять\n"
		"  UserOutputValue              значение этого бита\n"
		"  UserOutputValueAll           все биты пользовательского выхода\n"
		"  UserSetSelector              набор: Default, UserSet1, UserSet2\n"
		"  UserSetLoad                  команда: загрузить выбранный набор\n"
		"  UserSetSave                  команда: записать текущие настройки в набор\n"
		"  UserSetDefault               какой набор загружать при включении\n"
		"  DeviceScanType               тип развёртки сенсора\n"
		"  DeviceTemperatureSelector    где мерить температуру\n"
		"  DeviceTemperature            температура, °C\n"
		"  DeviceHumiditySelector       где мерить влажность\n"
		"  DeviceHumidity               влажность\n"
		"  DeviceColdDewPoint           точка росы при работе TEC\n"
		"  TECSetpoint                  уставка охладителя, °C\n"
		"  TECPwm                       скважность охладителя\n"
		"  StatusVoltageSelector        какой узел по напряжению\n"
		"  StatusVoltage                измеренное напряжение\n"
		"  DeviceReset                  команда: сброс камеры к состоянию включения\n"
		"  FanSelector                  какой вентилятор\n"
		"  FanMode                      режим вентилятора\n"
		"  FanSpeed                     скорость вентилятора\n"
		"  FanRpm                       обороты вентилятора\n"
		"  LedMode                      режим светодиода\n"
		"  NumberOfLink                 сколько линий CXP сейчас активно\n"
		"  LinkSpeed                    скорость линии\n"
		"  DefaultNumberOfLink          число линий по умолчанию\n"
		"  DefaultLinkSpeed             скорость линии по умолчанию\n"
		"  StartupNumberOfLink          число линий при старте\n"
		"  StartupLinkSpeed             скорость линий при старте\n"
		"  DeviceTapGeometry            геометрия тапов сенсора\n"
		"  FFCMode                      коррекция плоского поля\n"
		"  FFCSet                       какой набор калибровки поля\n"
		"  FFCStatus                    состояние калибровки\n"
		"  FFCRefreshStatus             команда: обновить состояние калибровки\n"
		"  FFCReloadParameter           команда: пересчитать калибровку под текущую область\n"
		"  FFCRGBEnable                 коррекция поля по каналам цвета\n"
		"  FFCRGBChannelSelector        какой цветовой канал калибровки\n"
		"  FFCRGBParameter              коэффициент выбранного канала\n"
		"  ShadingSelector              какой тип затенения править\n"
		"  LSCTableSelector             какая таблица коррекции затенения\n"
		"  LSCEnable                    включить коррекцию затенения\n"
		"  ActivateShading              команда: снять калибровку затенения\n"
		"  ReceivedTriggerCount         сколько импульсов пришло\n"
		"  AcceptedTriggerCount         сколько импульсов принято\n"
		"  RejectedTriggerCount         сколько импульсов отвергнуто\n"
		"  FrameOutCount                сколько кадров вышло\n"
		"  ResetCounter                 команда: обнулить счётчики\n"
		"  SequencerMode                включить секвенсор наборов\n"
		"  SequencerSetCount            сколько наборов в серии\n"
		"  SequencerSetSelector         какой набор серии редактировать\n"
		"  SequencerSetActive           какой набор сейчас активен\n"
		"  SequencerReset               команда: вернуться к набору 0\n"
		"  SequencerFeatureSelector     какой параметр входит в серию\n"
		"  SequencerFeatureEnable       включить выбранный параметр во всех наборах\n"
		"  SequencerExposureTime        выдержка набора, мкс\n"
		"  SequencerGainRaw             усиление набора\n"
		"  SequencerBalanceRatioSelector какой цветовой баланс набора\n"
		"  SequencerBalanceRatio        коэффициент баланса набора\n"
		"  SequencerFFCSet              набор калибровки поля для серии\n"
		"  SequencerLSCTableSet         таблица затенения для серии\n"
		"  ConnectionReset              сброс соединения\n"
		"  MasterHostConnectionID       идентификатор соединения хоста\n"
		"  StreamPacketSizeMax          наибольший размер пакета потока\n"
		"  Image1StreamID               номер основного потока\n"
		"  Image2StreamID               номер второго потока\n"
		"  XmlManifestSelector          какой XML-манифест камеры\n"
		"  TestMode                     тестовый режим линии\n"
		"  TestErrorCountSelector       какой счётчик ошибок читать\n"
		"  TestErrorCount               число ошибок соединения\n"
		"  TestPacketCountTx            отправлено тестовых пакетов\n"
		"  TestPacketCountRx            принято тестовых пакетов\n"
		"  ElectricalComplianceTest     электрический тест соответствия\n"
		"\n"
		"Параметры грабера для get/set --level interface:\n"
		"  FirmwareMode                 режим прошивки карты, для этой камеры AAAA\n"
		"  DeviceSelector               какая камера на карте\n"
		"  CxpLinkConfiguration         линк, для этой камеры CXP12_X4 или Auto\n"
		"  CxpPoCxpAuto                 команда: подать питание по коаксиалу\n",
		stderr);
}

static bool take(int& i, int argc, char** argv, const char* flag, std::string& out)
{
	if (i + 1 >= argc) {
		std::fprintf(stderr, "нет значения у %s\n", flag);
		return false;
	}
	out = argv[++i];
	return true;
}

static bool parse_u32(const std::string& text, uint32_t& out)
{
	char* end = nullptr;
	errno = 0;
	unsigned long value = std::strtoul(text.c_str(), &end, 10);
	if (!end || end == text.c_str() || *end || errno)
		return false;
	out = static_cast<uint32_t>(value);
	return true;
}

static bool parse_u64(const std::string& text, uint64_t& out)
{
	char* end = nullptr;
	errno = 0;
	unsigned long long value = std::strtoull(text.c_str(), &end, 10);
	if (!end || end == text.c_str() || *end || errno)
		return false;
	out = static_cast<uint64_t>(value);
	return true;
}

static bool parse_f64(const std::string& text, double& out)
{
	char* end = nullptr;
	errno = 0;
	double value = std::strtod(text.c_str(), &end);
	if (!end || end == text.c_str() || *end || errno)
		return false;
	out = value;
	return true;
}

static bool parse_args(int argc, char** argv, std::string& cmd, Opt& opt)
{
	if (argc < 2) {
		usage();
		return false;
	}
	cmd = argv[1];
	if (cmd == "-h" || cmd == "--help" || cmd == "help") {
		usage();
		return false;
	}
	for (int i = 2; i < argc; ++i) {
		std::string arg = argv[i];
		std::string value;
		if (arg == "--help" || arg == "-h") {
			usage();
			return false;
		} else if (arg == "--card") {
			if (!take(i, argc, argv, arg.c_str(), value) || !parse_u32(value, opt.card))
				return false;
		} else if (arg == "--camera") {
			if (!take(i, argc, argv, arg.c_str(), value) || !parse_u32(value, opt.camera))
				return false;
		} else if (arg == "--link") {
			if (!take(i, argc, argv, arg.c_str(), opt.link))
				return false;
		} else if (arg == "--firmware") {
			if (!take(i, argc, argv, arg.c_str(), opt.firmware))
				return false;
		} else if (arg == "--link-wait") {
			if (!take(i, argc, argv, arg.c_str(), value) || !parse_f64(value, opt.link_wait))
				return false;
		} else if (arg == "--format") {
			if (!take(i, argc, argv, arg.c_str(), opt.format))
				return false;
			opt.format_set = true;
		} else if (arg == "--exposure") {
			if (!take(i, argc, argv, arg.c_str(), value) || !parse_f64(value, opt.exposure_us)) {
				std::fprintf(stderr, "--exposure: нужно число микросекунд\n");
				return false;
			}
			opt.exposure_set = true;
		} else if (arg == "--gain") {
			if (!take(i, argc, argv, arg.c_str(), value) || !parse_f64(value, opt.gain))
				return false;
			opt.gain_set = true;
		} else if (arg == "--buf") {
			if (!take(i, argc, argv, arg.c_str(), value) || !parse_u32(value, opt.bufs))
				return false;
		} else if (arg == "--timeout-ms") {
			if (!take(i, argc, argv, arg.c_str(), value) || !parse_u64(value, opt.timeout_ms))
				return false;
		} else if (arg == "--level") {
			if (!take(i, argc, argv, arg.c_str(), opt.level))
				return false;
		} else if (arg == "--output" || arg == "-o") {
			if (!take(i, argc, argv, arg.c_str(), opt.output))
				return false;
		} else if (arg == "--frames" || arg == "-n") {
			if (!take(i, argc, argv, arg.c_str(), value) || !parse_u64(value, opt.frames))
				return false;
		} else if (arg == "--seconds" || arg == "-t") {
			if (!take(i, argc, argv, arg.c_str(), value) || !parse_f64(value, opt.seconds))
				return false;
		} else if (arg == "--mode") {
			if (!take(i, argc, argv, arg.c_str(), opt.mode))
				return false;
		} else if (arg == "--warmup") {
			if (!take(i, argc, argv, arg.c_str(), value) || !parse_u32(value, opt.warmup))
				return false;
		} else if (arg == "--skip") {
			if (!take(i, argc, argv, arg.c_str(), value) || !parse_u32(value, opt.skip))
				return false;
		} else if (arg == "--bytes") {
			if (!take(i, argc, argv, arg.c_str(), value) || !parse_u64(value, opt.bytes))
				return false;
			opt.bytes_set = true;
		} else if (arg == "--json") {
			if (!take(i, argc, argv, arg.c_str(), opt.json))
				return false;
			opt.json_set = true;
		} else if (arg == "--no-json") {
			opt.no_json = true;
		} else if (arg == "--gentl") {
			if (!take(i, argc, argv, arg.c_str(), opt.gentl))
				return false;
		} else if (arg == "--ssd-dir") {
			if (!take(i, argc, argv, arg.c_str(), opt.ssd_dir))
				return false;
		} else if (!arg.empty() && arg[0] == '-') {
			std::fprintf(stderr, "неизвестный параметр %s\n", arg.c_str());
			return false;
		} else {
			opt.pos.push_back(arg);
		}
	}
	return true;
}

static double percentile(std::vector<double> values, double p)
{
	if (values.empty())
		return 0;
	std::sort(values.begin(), values.end());
	double idx = (p / 100.0) * static_cast<double>(values.size() - 1);
	size_t i = static_cast<size_t>(idx);
	double frac = idx - static_cast<double>(i);
	if (i + 1 >= values.size())
		return values.back();
	return values[i] * (1.0 - frac) + values[i + 1] * frac;
}

static double mean_of(const std::vector<double>& values)
{
	if (values.empty())
		return 0;
	double sum = std::accumulate(values.begin(), values.end(), 0.0);
	return sum / static_cast<double>(values.size());
}

static void fill_interval(const std::vector<double>& values, double& vmin, double& vmean, double& vp50, double& vp99, double& vmax)
{
	if (values.empty())
		return;
	vmin = *std::min_element(values.begin(), values.end());
	vmax = *std::max_element(values.begin(), values.end());
	vmean = mean_of(values);
	vp50 = percentile(values, 50);
	vp99 = percentile(values, 99);
}

static void finish_stats(Report& rep, const std::vector<Sample>& samples)
{
	rep.frames = samples.size();
	if (samples.size() < 2)
		return;
	rep.span_s = std::chrono::duration<double>(samples.back().host - samples.front().host).count();
	if (rep.span_s <= 0)
		return;
	double intervals = static_cast<double>(samples.size() - 1);
	rep.delivered_fps = intervals / rep.span_s;
	rep.source_fps = (intervals + static_cast<double>(rep.dropped)) / rep.span_s;
	rep.grab_MBps = rep.delivered_fps * static_cast<double>(rep.payload) / 1e6;
	rep.source_MBps = rep.source_fps * static_cast<double>(rep.payload) / 1e6;

	std::vector<double> host_us;
	std::vector<double> dev_us;
	host_us.reserve(samples.size());
	for (size_t i = 1; i < samples.size(); ++i) {
		host_us.push_back(std::chrono::duration<double, std::micro>(samples[i].host - samples[i - 1].host).count());
		if (rep.tick_hz > 0 && samples[i].dev_ts >= samples[i - 1].dev_ts) {
			double ticks = static_cast<double>(samples[i].dev_ts - samples[i - 1].dev_ts);
			dev_us.push_back(ticks / static_cast<double>(rep.tick_hz) * 1e6);
		}
	}
	fill_interval(host_us, rep.iv_min, rep.iv_mean, rep.iv_p50, rep.iv_p99, rep.iv_max);
	rep.have_host = !host_us.empty();
	if (!dev_us.empty() && mean_of(dev_us) > 0.001) {
		fill_interval(dev_us, rep.dev_min, rep.dev_mean, rep.dev_p50, rep.dev_p99, rep.dev_max);
		rep.have_dev = true;
	}
	if (rep.copy_s > 0)
		rep.copy_MBps = static_cast<double>(rep.bytes) / rep.copy_s / 1e6;
	if (rep.write_s > 0)
		rep.write_MBps = static_cast<double>(rep.bytes) / rep.write_s / 1e6;
}

static void print_report(const Report& rep)
{
	std::printf("\n--- %s ---\n", rep.mode.c_str());
	if (!rep.model.empty()) {
		std::printf("камера: %s %s, %lldx%lld, кадр %llu байт\n",
			rep.model.c_str(), rep.format.c_str(),
			(long long)rep.width, (long long)rep.height,
			(unsigned long long)rep.payload);
		if (!rep.exposure.empty() || !rep.gain_raw.empty() || !rep.trigger.empty())
			std::printf("съёмка: выдержка %s мкс, GainRaw %s, AnalogGain %s, триггер %s, режим %s\n",
				rep.exposure.empty() ? "-" : rep.exposure.c_str(),
				rep.gain_raw.empty() ? "-" : rep.gain_raw.c_str(),
				rep.analog_gain.empty() ? "-" : rep.analog_gain.c_str(),
				rep.trigger.empty() ? "-" : rep.trigger.c_str(),
				rep.acquisition.empty() ? "-" : rep.acquisition.c_str());
	}
	else if (rep.payload)
		std::printf("блок: %llu байт\n", (unsigned long long)rep.payload);
	if (!rep.storage.empty())
		std::printf("диск: %s\n", rep.storage.c_str());
	if (!rep.error.empty())
		std::printf("ошибка: %s\n", rep.error.c_str());
	std::printf("кадры: %llu", (unsigned long long)rep.frames);
	if (rep.warmup)
		std::printf(" (прогрев %llu не входит в скорость)", (unsigned long long)rep.warmup);
	std::printf("\n");
	std::printf("пропуски frame_id: %llu\n", (unsigned long long)rep.dropped);
	if (rep.disorder)
		std::printf("скачки frame_id назад: %llu\n", (unsigned long long)rep.disorder);
	std::printf("таймауты: %llu\n", (unsigned long long)rep.timeouts);
	if (rep.time_to_first_ms > 0)
		std::printf("до первого кадра: %.1f мс\n", rep.time_to_first_ms);
	if (rep.span_s > 0)
		std::printf("окно: %.3f с\n", rep.span_s);
	if (rep.frames >= 2) {
		std::printf("по frame_id: %.2f к/с, %.1f МБ/с\n", rep.source_fps, rep.source_MBps);
		std::printf("принято: %.2f к/с, %.1f МБ/с\n", rep.delivered_fps, rep.grab_MBps);
	}
	if (rep.copy_s > 0)
		std::printf("memcpy: %.1f МБ/с\n", rep.copy_MBps);
	if (rep.write_s > 0)
		std::printf("write(): %.1f МБ/с\n", rep.write_MBps);
	if (rep.write_s > 0 || rep.fsync_ms > 0)
		std::printf("write+fsync: %.1f МБ/с (fsync %.0f мс)\n", rep.save_MBps, rep.fsync_ms);
	if (rep.have_host) {
		std::printf("интервал хоста, мкс: min %.0f  среднее %.0f  p50 %.0f  p99 %.0f  max %.0f\n",
			rep.iv_min, rep.iv_mean, rep.iv_p50, rep.iv_p99, rep.iv_max);
	}
	if (rep.have_dev) {
		std::printf("интервал камеры, мкс: min %.0f  среднее %.0f  p50 %.0f  p99 %.0f  max %.0f  (такт %llu Гц)\n",
			rep.dev_min, rep.dev_mean, rep.dev_p50, rep.dev_p99, rep.dev_max,
			(unsigned long long)rep.tick_hz);
	}
	if (rep.queued_seen) {
		std::printf("свободных буферов DMA: min %zu, последний %zu\n",
			rep.queued_min, rep.queued_last);
	}
	if (!rep.path.empty())
		std::printf("файл: %s (%llu байт)\n", rep.path.c_str(), (unsigned long long)rep.bytes);
	if (rep.dropped > 0)
		std::printf("в номерах кадров есть дыры: часть кадров не дошла\n");
	else if (rep.have_dev && rep.have_host && rep.iv_mean > rep.dev_mean * 1.5)
		std::printf("frame_id сплошные, хост медленнее периода камеры: съёмка ждала буфер\n");
	else if (rep.frames >= 2)
		std::printf("пропусков по frame_id нет\n");
}

static void json_escape(FILE* file, const std::string& text)
{
	std::fputc('"', file);
	for (unsigned char c : text) {
		if (c == '"' || c == '\\') {
			std::fputc('\\', file);
			std::fputc(c, file);
		} else if (c == '\n') {
			std::fputs("\\n", file);
		} else {
			std::fputc(c, file);
		}
	}
	std::fputc('"', file);
}

static void save_json(const std::string& path, const std::vector<Report>& reports)
{
	FILE* file = std::fopen(path.c_str(), "w");
	if (!file) {
		std::fprintf(stderr, "не записать %s: %s\n", path.c_str(), std::strerror(errno));
		return;
	}
	std::fputs("[\n", file);
	for (size_t i = 0; i < reports.size(); ++i) {
		const Report& rep = reports[i];
		std::fputs("  {\n", file);
		std::fputs("    \"mode\": ", file);
		json_escape(file, rep.mode);
		std::fputs(",\n    \"error\": ", file);
		json_escape(file, rep.error);
		std::fputs(",\n    \"model\": ", file);
		json_escape(file, rep.model);
		std::fputs(",\n    \"format\": ", file);
		json_escape(file, rep.format);
		std::fputs(",\n    \"exposure\": ", file);
		json_escape(file, rep.exposure);
		std::fputs(",\n    \"gain_raw\": ", file);
		json_escape(file, rep.gain_raw);
		std::fputs(",\n    \"analog_gain\": ", file);
		json_escape(file, rep.analog_gain);
		std::fputs(",\n    \"trigger\": ", file);
		json_escape(file, rep.trigger);
		std::fputs(",\n    \"acquisition\": ", file);
		json_escape(file, rep.acquisition);
		std::fprintf(file, ",\n    \"width\": %lld,\n    \"height\": %lld,\n",
			(long long)rep.width, (long long)rep.height);
		std::fprintf(file, "    \"payload_bytes\": %llu,\n", (unsigned long long)rep.payload);
		std::fprintf(file, "    \"warmup\": %llu,\n", (unsigned long long)rep.warmup);
		std::fprintf(file, "    \"frames\": %llu,\n", (unsigned long long)rep.frames);
		std::fprintf(file, "    \"dropped\": %llu,\n", (unsigned long long)rep.dropped);
		std::fprintf(file, "    \"disorder\": %llu,\n", (unsigned long long)rep.disorder);
		std::fprintf(file, "    \"timeouts\": %llu,\n", (unsigned long long)rep.timeouts);
		std::fprintf(file, "    \"bytes\": %llu,\n", (unsigned long long)rep.bytes);
		std::fprintf(file, "    \"time_to_first_ms\": %.3f,\n", rep.time_to_first_ms);
		std::fprintf(file, "    \"span_s\": %.6f,\n", rep.span_s);
		std::fprintf(file, "    \"source_fps\": %.4f,\n", rep.source_fps);
		std::fprintf(file, "    \"delivered_fps\": %.4f,\n", rep.delivered_fps);
		std::fprintf(file, "    \"source_MBps\": %.3f,\n", rep.source_MBps);
		std::fprintf(file, "    \"grab_MBps\": %.3f,\n", rep.grab_MBps);
		std::fprintf(file, "    \"copy_MBps\": %.3f,\n", rep.copy_MBps);
		std::fprintf(file, "    \"write_MBps\": %.3f,\n", rep.write_MBps);
		std::fprintf(file, "    \"save_MBps\": %.3f,\n", rep.save_MBps);
		std::fprintf(file, "    \"fsync_ms\": %.3f,\n", rep.fsync_ms);
		std::fprintf(file,
			"    \"host_interval_us\": {\"min\": %.1f, \"mean\": %.1f, \"p50\": %.1f, \"p99\": %.1f, \"max\": %.1f},\n",
			rep.iv_min, rep.iv_mean, rep.iv_p50, rep.iv_p99, rep.iv_max);
		if (rep.have_dev) {
			std::fprintf(file,
				"    \"device_interval_us\": {\"min\": %.1f, \"mean\": %.1f, \"p50\": %.1f, \"p99\": %.1f, \"max\": %.1f},\n",
				rep.dev_min, rep.dev_mean, rep.dev_p50, rep.dev_p99, rep.dev_max);
		}
		std::fprintf(file, "    \"tick_hz\": %llu,\n", (unsigned long long)rep.tick_hz);
		if (rep.queued_seen) {
			std::fprintf(file, "    \"queued_min\": %zu,\n    \"queued_last\": %zu,\n",
				rep.queued_min, rep.queued_last);
		}
		std::fputs("    \"storage\": ", file);
		json_escape(file, rep.storage);
		std::fputs(",\n    \"output\": ", file);
		json_escape(file, rep.path);
		std::fputs("\n  }", file);
		if (i + 1 != reports.size())
			std::fputc(',', file);
		std::fputc('\n', file);
	}
	std::fputs("]\n", file);
	std::fclose(file);
	std::fprintf(stderr, "json: %s\n", path.c_str());
}

struct HeldBuf {
	Session* session = nullptr;
	BUFFER_HANDLE handle = nullptr;
	void* base = nullptr;
	size_t size = 0;
	cxp_buf_info_t info{};
	bool live = false;

	explicit HeldBuf(Session& owner) : session(&owner) {}
	HeldBuf(const HeldBuf&) = delete;
	HeldBuf& operator=(const HeldBuf&) = delete;
	~HeldBuf() { release(); }

	bool pull(uint64_t timeout_ms)
	{
		BUFFER_HANDLE buf = nullptr;
		void* pointer = nullptr;
		size_t bytes = 0;
		if (!scap_get_buf(session->cap, timeout_ms, &buf, &pointer, &bytes) || !pointer || bytes == 0)
			return false;
		handle = buf;
		base = pointer;
		size = bytes;
		live = true;
		std::memset(&info, 0, sizeof(info));
		scap_get_buf_info(session->cap, handle, &info, true);
		return true;
	}

	void release()
	{
		if (live && session && session->cap) {
			scap_back_buffer(session->cap, handle);
			live = false;
		}
	}
};

static bool write_all(int fd, const void* data, size_t size)
{
	const char* pointer = static_cast<const char*>(data);
	size_t done = 0;
	while (done < size) {
		ssize_t wrote = ::write(fd, pointer + done, size - done);
		if (wrote < 0) {
			if (errno == EINTR)
				continue;
			return false;
		}
		if (wrote == 0)
			return false;
		done += static_cast<size_t>(wrote);
	}
	return true;
}

static std::string trim_copy(std::string text)
{
	while (!text.empty() && (text.back() == '\n' || text.back() == '\r' || text.back() == ' '))
		text.pop_back();
	size_t begin = 0;
	while (begin < text.size() && text[begin] == ' ')
		++begin;
	return text.substr(begin);
}

static std::string read_sysfs(const std::string& path)
{
	std::ifstream in(path);
	if (!in)
		return {};
	std::string line;
	std::getline(in, line);
	return trim_copy(line);
}

static bool parse_maj_min(const std::string& text, unsigned& maj, unsigned& min)
{
	unsigned a = 0;
	unsigned b = 0;
	if (std::sscanf(text.c_str(), "%u:%u", &a, &b) != 2)
		return false;
	maj = a;
	min = b;
	return true;
}

static std::string parent_block(std::string name)
{
	if (name.compare(0, 4, "nvme") == 0) {
		auto p = name.find('p');
		if (p != std::string::npos && p > 0 && std::isdigit(static_cast<unsigned char>(name[p - 1])))
			return name.substr(0, p);
		return name;
	}
	while (!name.empty() && std::isdigit(static_cast<unsigned char>(name.back())))
		name.pop_back();
	return name;
}

struct BlockDisk {
	std::string name;
	std::string model;
	std::string transport;
	bool rotational = true;
};

static BlockDisk describe_block(const std::string& name)
{
	BlockDisk disk;
	disk.name = name;
	std::string root = parent_block(name);
	disk.model = read_sysfs("/sys/block/" + root + "/device/model");
	if (disk.model.empty())
		disk.model = read_sysfs("/sys/block/" + root + "/device/name");
	std::string rota = read_sysfs("/sys/block/" + root + "/queue/rotational");
	disk.rotational = (rota != "0");
	if (root.compare(0, 4, "nvme") == 0)
		disk.transport = "nvme";
	else if (!read_sysfs("/sys/block/" + root + "/device/vendor").empty())
		disk.transport = disk.rotational ? "sata/hdd" : "sata/ssd";
	else
		disk.transport = disk.rotational ? "hdd" : "ssd";
	return disk;
}

static std::string format_storage(const BlockDisk& disk)
{
	std::string text = disk.name;
	if (!disk.transport.empty())
		text += " " + disk.transport;
	if (!disk.model.empty())
		text += " " + disk.model;
	if (!disk.rotational)
		text += " (SSD)";
	else if (disk.transport.find("hdd") != std::string::npos || disk.rotational)
		text += " (HDD)";
	return text;
}

static bool block_by_dev(dev_t dev, BlockDisk& out)
{
	char link[256];
	std::snprintf(link, sizeof(link), "/sys/dev/block/%u:%u", major(dev), minor(dev));
	char target[512];
	ssize_t n = ::readlink(link, target, sizeof(target) - 1);
	if (n < 0)
		return false;
	target[n] = 0;
	std::string path(target);
	auto slash = path.find_last_of('/');
	std::string name = slash == std::string::npos ? path : path.substr(slash + 1);
	if (name.empty())
		return false;
	out = describe_block(name);
	return true;
}

static std::string storage_for_path(const std::string& path)
{
	struct stat st {};
	std::string probe = path;
	if (::stat(probe.c_str(), &st) != 0) {
		auto slash = probe.find_last_of('/');
		if (slash == std::string::npos)
			probe = ".";
		else if (slash == 0)
			probe = "/";
		else
			probe = probe.substr(0, slash);
		if (::stat(probe.c_str(), &st) != 0)
			return {};
	}
	BlockDisk disk;
	if (!block_by_dev(st.st_dev, disk))
		return {};
	return format_storage(disk);
}

struct NvmeTarget {
	std::string dir;
	std::string storage;
	std::string error;
};

static bool dir_writable(const std::string& dir)
{
	if (::access(dir.c_str(), W_OK) != 0)
		return false;
	std::string probe = dir;
	if (!probe.empty() && probe.back() != '/')
		probe += '/';
	probe += ".grabercam_ssd_probe";
	int fd = ::open(probe.c_str(), O_CREAT | O_TRUNC | O_WRONLY | O_CLOEXEC, 0644);
	if (fd < 0)
		return false;
	::close(fd);
	::unlink(probe.c_str());
	return true;
}

static void collect_nvme_names(std::vector<std::string>& names)
{
	DIR* dir = ::opendir("/sys/block");
	if (!dir)
		return;
	while (dirent* ent = ::readdir(dir)) {
		if (std::strncmp(ent->d_name, "nvme", 4) == 0)
			names.push_back(ent->d_name);
	}
	::closedir(dir);
	std::sort(names.begin(), names.end());
}

static void collect_dev_nodes(const std::string& disk, std::vector<dev_t>& nodes)
{
	auto add = [&](const std::string& path) {
		unsigned maj = 0;
		unsigned min = 0;
		if (!parse_maj_min(read_sysfs(path), maj, min))
			return;
		nodes.push_back(makedev(maj, min));
	};
	add("/sys/block/" + disk + "/dev");
	DIR* dir = ::opendir(("/sys/block/" + disk).c_str());
	if (!dir)
		return;
	while (dirent* ent = ::readdir(dir)) {
		if (std::strncmp(ent->d_name, disk.c_str(), disk.size()) == 0)
			add("/sys/block/" + disk + "/" + ent->d_name + "/dev");
	}
	::closedir(dir);
}

static NvmeTarget resolve_ssd_target(const Opt& opt)
{
	NvmeTarget target;
	if (!opt.ssd_dir.empty()) {
		if (!dir_writable(opt.ssd_dir)) {
			target.error = "каталог --ssd-dir недоступен для записи: " + opt.ssd_dir;
			return target;
		}
		target.dir = opt.ssd_dir;
		target.storage = storage_for_path(opt.ssd_dir);
		if (target.storage.find("nvme") == std::string::npos && target.storage.find("SSD") == std::string::npos)
			target.storage += target.storage.empty() ? "задан --ssd-dir" : " (путь --ssd-dir)";
		return target;
	}

	std::vector<std::string> nvmes;
	collect_nvme_names(nvmes);
	if (nvmes.empty()) {
		target.error = "NVMe не найден в /sys/block";
		return target;
	}

	std::vector<dev_t> wanted;
	std::vector<std::string> labels;
	for (const std::string& name : nvmes) {
		collect_dev_nodes(name, wanted);
		labels.push_back(format_storage(describe_block(name)));
	}

	std::ifstream mounts("/proc/self/mountinfo");
	std::string line;
	std::string best_dir;
	std::string best_storage;
	size_t best_len = 0;
	while (std::getline(mounts, line)) {
		unsigned id = 0;
		unsigned parent = 0;
		unsigned maj = 0;
		unsigned min = 0;
		char root[512];
		char mnt[512];
		if (std::sscanf(line.c_str(), "%u %u %u:%u %511s %511s", &id, &parent, &maj, &min, root, mnt) < 6)
			continue;
		dev_t node = makedev(maj, min);
		bool match = false;
		for (dev_t want : wanted) {
			if (want == node) {
				match = true;
				break;
			}
		}
		if (!match)
			continue;
		std::string dir = mnt;
		if (!dir_writable(dir))
			continue;
		if (dir.size() > best_len) {
			best_len = dir.size();
			best_dir = dir;
			BlockDisk disk;
			if (block_by_dev(node, disk))
				best_storage = format_storage(disk);
		}
	}

	if (!best_dir.empty()) {
		target.dir = best_dir;
		target.storage = best_storage;
		return target;
	}

	std::string list;
	for (size_t i = 0; i < labels.size(); ++i) {
		if (i)
			list += ", ";
		list += labels[i];
	}
	target.error = "NVMe есть (" + list + "), но не смонтирован — задайте --ssd-dir или смонтируйте раздел";
	return target;
}

static std::string meta_path_for(const std::string& raw)
{
	std::string::size_type slash = raw.find_last_of('/');
	std::string::size_type dot = raw.find_last_of('.');
	if (dot != std::string::npos && (slash == std::string::npos || dot > slash))
		return raw.substr(0, dot) + ".txt";
	return raw + ".txt";
}

static bool write_meta(const Report& rep)
{
	std::string path = meta_path_for(rep.path);
	FILE* file = std::fopen(path.c_str(), "w");
	if (!file)
		return false;
	std::fprintf(file,
		"frames=%llu\nwidth=%lld\nheight=%lld\nformat=%s\npayload=%llu\nfps=%.4f\n"
		"exposure=%s\ngain_raw=%s\nanalog_gain=%s\ntrigger=%s\nacquisition=%s\n",
		(unsigned long long)rep.frames,
		(long long)rep.width,
		(long long)rep.height,
		rep.format.c_str(),
		(unsigned long long)rep.payload,
		rep.delivered_fps,
		rep.exposure.c_str(),
		rep.gain_raw.c_str(),
		rep.analog_gain.c_str(),
		rep.trigger.c_str(),
		rep.acquisition.c_str());
	std::fclose(file);
	std::fprintf(stderr, "мета: %s\n", path.c_str());
	return true;
}

static int run_grab(Session& session, const GrabRequest& req, Report& rep)
{
	rep.mode = (req.mode == "disk" && !req.sync_io) ? "record" : req.mode;
	rep.path = req.path;
	rep.model = session.model;
	rep.format = session.format;
	rep.exposure = session.exposure;
	rep.gain_raw = session.gain_raw;
	rep.analog_gain = session.analog_gain;
	rep.trigger = session.trigger;
	rep.acquisition = session.acquisition;
	rep.width = session.width;
	rep.height = session.height;
	rep.tick_hz = session.tick_hz;
	rep.warmup = req.warmup;

	int fd = -1;
	if (req.mode == "disk") {
		int flags = O_CREAT | O_TRUNC | O_WRONLY | O_CLOEXEC;
		if (req.sync_io)
			flags |= O_DSYNC;
		fd = ::open(req.path.c_str(), flags, 0644);
		if (fd < 0) {
			rep.error = std::string("не открыть ") + req.path + ": " + std::strerror(errno);
			return 5;
		}
		posix_fadvise(fd, 0, 0, POSIX_FADV_SEQUENTIAL);
		rep.storage = storage_for_path(req.path);
	}

	if (!session.start(req.bufs)) {
		rep.error = std::string("scap_start: ") + last_err();
		if (fd >= 0)
			::close(fd);
		return 3;
	}

	std::vector<unsigned char> scratch;
	std::vector<Sample> samples;
	uint32_t warmup_left = req.warmup;
	bool saw = false;
	int rc = 0;
	auto armed = std::chrono::steady_clock::now();
	auto deadline = armed + std::chrono::duration<double>(req.seconds > 0 ? req.seconds : 3600.0);
	bool use_deadline = req.seconds > 0;
	auto progress = armed;

	while (!g_stop.load()) {
		auto now = std::chrono::steady_clock::now();
		if (use_deadline && now >= deadline)
			break;
		if (req.frames > 0 && samples.size() >= req.frames)
			break;

		uint64_t wait_ms = req.timeout_ms;
		if (use_deadline) {
			auto left = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now).count();
			if (left <= 0)
				break;
			if (static_cast<uint64_t>(left) < wait_ms)
				wait_ms = static_cast<uint64_t>(left);
			if (wait_ms == 0)
				wait_ms = 1;
		}

		HeldBuf held(session);
		if (!held.pull(wait_ms)) {
			if (g_stop.load() || (use_deadline && std::chrono::steady_clock::now() >= deadline))
				break;
			++rep.timeouts;
			continue;
		}
		now = std::chrono::steady_clock::now();
		if (!saw) {
			saw = true;
			rep.time_to_first_ms = std::chrono::duration<double, std::milli>(now - armed).count();
		}
		if (rep.payload == 0)
			rep.payload = held.size;
		if (held.info.width > 0)
			rep.width = held.info.width;
		if (held.info.height > 0)
			rep.height = held.info.height;

		if (warmup_left > 0) {
			--warmup_left;
			continue;
		}

		if (!samples.empty()) {
			uint64_t prev = samples.back().id;
			if (held.info.frame_id > prev)
				rep.dropped += held.info.frame_id - prev - 1;
			else
				++rep.disorder;
		}
		Sample sample;
		sample.id = held.info.frame_id;
		sample.dev_ts = held.info.timestamp;
		sample.host = now;
		samples.push_back(sample);

		{
			cxp_ds_info_t stream;
			std::memset(&stream, 0, sizeof(stream));
			if (scap_get_data_stream_info(session.cap, &stream)) {
				rep.queued_seen = true;
				rep.queued_last = stream.num_queued;
				if (stream.num_queued < rep.queued_min)
					rep.queued_min = stream.num_queued;
			}
		}

		if (req.mode == "copy") {
			if (scratch.size() != held.size)
				scratch.resize(held.size);
			auto copy_begin = std::chrono::steady_clock::now();
			std::memcpy(scratch.data(), held.base, held.size);
			asm volatile("" :: "r"(scratch.data()) : "memory");
			rep.copy_s += std::chrono::duration<double>(std::chrono::steady_clock::now() - copy_begin).count();
			rep.bytes += held.size;
		} else if (req.mode == "disk") {
			auto write_begin = std::chrono::steady_clock::now();
			bool ok = write_all(fd, held.base, held.size);
			rep.write_s += std::chrono::duration<double>(std::chrono::steady_clock::now() - write_begin).count();
			if (!ok) {
				rep.error = std::string("запись: ") + std::strerror(errno);
				rc = 5;
				break;
			}
			rep.bytes += held.size;
		}

		if (now - progress > std::chrono::milliseconds(500)) {
			std::fprintf(stderr, "\r%-8s кадры %llu  пропуски %llu   ",
				req.mode.c_str(),
				(unsigned long long)samples.size(),
				(unsigned long long)rep.dropped);
			progress = now;
		}
	}
	std::fprintf(stderr, "\n");
	session.stop();

	if (fd >= 0) {
		auto sync_begin = std::chrono::steady_clock::now();
		if (::fsync(fd) != 0 && rep.error.empty()) {
			rep.error = std::string("fsync: ") + std::strerror(errno);
			rc = 5;
		}
		rep.fsync_ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - sync_begin).count();
		::close(fd);
		if (rep.bytes == 0)
			::unlink(req.path.c_str());
	}

	finish_stats(rep, samples);
	double save_s = rep.write_s + rep.fsync_ms / 1e3;
	if (save_s > 0 && rep.bytes > 0)
		rep.save_MBps = static_cast<double>(rep.bytes) / save_s / 1e6;
	if (req.write_meta && rep.bytes > 0)
		write_meta(rep);
	if (rc == 0 && samples.empty()) {
		rep.error = g_stop.load() ? "остановлено, кадров после прогрева нет" : "кадров после прогрева нет";
		return 4;
	}
	return rc;
}

static int run_sink(const Opt& opt, const std::string& path, const std::string& mode_name, Report& rep)
{
	rep.mode = mode_name;
	rep.path = path;
	rep.payload = opt.bytes;
	rep.storage = storage_for_path(path);
	if (opt.bytes == 0) {
		rep.error = "размер блока 0";
		return 2;
	}
	std::vector<unsigned char> block(static_cast<size_t>(opt.bytes), 0xA5);
	int fd = ::open(path.c_str(), O_CREAT | O_TRUNC | O_WRONLY | O_CLOEXEC | O_DSYNC, 0644);
	if (fd < 0) {
		rep.error = std::string("не открыть ") + path + ": " + std::strerror(errno);
		return 5;
	}
	posix_fadvise(fd, 0, 0, POSIX_FADV_SEQUENTIAL);

	double seconds = opt.seconds > 0 ? opt.seconds : 5;
	auto begin = std::chrono::steady_clock::now();
	auto deadline = begin + std::chrono::duration<double>(seconds);
	std::vector<Sample> samples;
	int rc = 0;
	while (!g_stop.load()) {
		auto now = std::chrono::steady_clock::now();
		if (now >= deadline)
			break;
		if (opt.frames > 0 && samples.size() >= opt.frames)
			break;
		auto write_begin = std::chrono::steady_clock::now();
		bool ok = write_all(fd, block.data(), block.size());
		auto write_end = std::chrono::steady_clock::now();
		rep.write_s += std::chrono::duration<double>(write_end - write_begin).count();
		if (!ok) {
			rep.error = std::string("запись: ") + std::strerror(errno);
			rc = 5;
			break;
		}
		rep.bytes += block.size();
		Sample sample;
		sample.id = samples.size();
		sample.host = write_end;
		samples.push_back(sample);
		if (write_end - begin > std::chrono::milliseconds(500) && (samples.size() % 4) == 0) {
			std::fprintf(stderr, "\r%-8s блоки %llu   ", mode_name.c_str(),
				(unsigned long long)samples.size());
		}
	}
	std::fprintf(stderr, "\n");
	auto sync_begin = std::chrono::steady_clock::now();
	if (::fsync(fd) != 0 && rep.error.empty()) {
		rep.error = std::string("fsync: ") + std::strerror(errno);
		rc = 5;
	}
	rep.fsync_ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - sync_begin).count();
	::close(fd);
	if (rep.bytes == 0)
		::unlink(path.c_str());

	rep.frames = samples.size();
	if (samples.size() >= 2) {
		rep.span_s = std::chrono::duration<double>(samples.back().host - samples.front().host).count();
		if (rep.span_s > 0) {
			rep.delivered_fps = static_cast<double>(samples.size() - 1) / rep.span_s;
			rep.source_fps = rep.delivered_fps;
			rep.grab_MBps = rep.delivered_fps * static_cast<double>(rep.payload) / 1e6;
			rep.source_MBps = rep.grab_MBps;
		}
		std::vector<double> host_us;
		for (size_t i = 1; i < samples.size(); ++i) {
			host_us.push_back(std::chrono::duration<double, std::micro>(
				samples[i].host - samples[i - 1].host).count());
		}
		fill_interval(host_us, rep.iv_min, rep.iv_mean, rep.iv_p50, rep.iv_p99, rep.iv_max);
		rep.have_host = true;
	}
	if (rep.write_s > 0)
		rep.write_MBps = static_cast<double>(rep.bytes) / rep.write_s / 1e6;
	double save_s = rep.write_s + rep.fsync_ms / 1e3;
	if (save_s > 0 && rep.bytes > 0)
		rep.save_MBps = static_cast<double>(rep.bytes) / save_s / 1e6;
	if (rc == 0 && samples.empty()) {
		rep.error = "ни одного блока";
		return 4;
	}
	return rc;
}

static std::string tagged(const std::string& path, const std::string& tag)
{
	std::string::size_type slash = path.find_last_of('/');
	std::string::size_type dot = path.find_last_of('.');
	if (dot != std::string::npos && (slash == std::string::npos || dot > slash))
		return path.substr(0, dot) + "_" + tag + path.substr(dot);
	return path + "_" + tag;
}

static int do_info(Session& session, const Opt& opt)
{
	std::printf("картa: %u %s\n", opt.card, session.card_name.c_str());
	std::printf("камера: %s\nмодель: %s\nсерийный: %s\n",
		session.vendor.c_str(), session.model.c_str(), session.serial.c_str());
	std::printf("кадр: %lld x %lld %s\n",
		(long long)session.width, (long long)session.height, session.format.c_str());
	if (session.tick_hz)
		std::printf("такт времени: %llu Гц\n", (unsigned long long)session.tick_hz);

	const char* iface[] = {"FirmwareMode", "CxpLinkConfiguration"};
	for (const char* name : iface) {
		std::string value;
		if (get_feature(session.card, scap_interface, name, value))
			std::printf("interface %s = %s\n", name, value.c_str());
		else
			std::printf("interface %s = <нет>\n", name);
	}
	const char* camera[] = {
		"PixelFormat", "ExposureTime", "GainRaw", "AnalogGain",
		"AcquisitionFrameRate", "ResultingFrameRate", "Width", "Height",
		"TriggerMode", "AcquisitionMode"
	};
	for (const char* name : camera) {
		std::string value;
		if (get_feature(session.cap, scap_camera, name, value))
			std::printf("camera %s = %s\n", name, value.c_str());
	}
	cxp_ds_info_t stream;
	std::memset(&stream, 0, sizeof(stream));
	if (scap_get_data_stream_info(session.cap, &stream) && stream.payload_size > 0) {
		std::printf("payload: %zu байт, буферов минимум %zu\n",
			stream.payload_size, stream.announce_min);
	}
	return 0;
}

static int cmd_info(const Opt& opt)
{
	Session session;
	if (!session.open(opt, OpenMode::Camera))
		return 2;
	return do_info(session, opt);
}

static int do_get(Session& session, const Opt& opt)
{
	if (opt.pos.size() != 1) {
		std::fprintf(stderr, "нужно: get <feature>\n");
		return 1;
	}
	scap_level_t level;
	if (!parse_level(opt.level, level)) {
		std::fprintf(stderr, "неизвестный --level %s\n", opt.level.c_str());
		return 1;
	}
	if (level == scap_interface && !session.card) {
		std::fprintf(stderr, "карта не открыта\n");
		return 2;
	}
	if (level != scap_interface && !session.cap) {
		std::fprintf(stderr, "камера не открыта\n");
		return 2;
	}
	void* handle = feature_handle(session, level);
	std::string value;
	if (!get_feature(handle, level, opt.pos[0].c_str(), value)) {
		std::fprintf(stderr, "%s: %s\n", opt.pos[0].c_str(), last_err());
		return 2;
	}
	std::printf("%s = %s\n", opt.pos[0].c_str(), value.c_str());
	return 0;
}

static int cmd_get(const Opt& opt)
{
	if (opt.pos.size() != 1) {
		std::fprintf(stderr, "нужно: get <feature>\n");
		return 1;
	}
	scap_level_t level;
	if (!parse_level(opt.level, level)) {
		std::fprintf(stderr, "неизвестный --level %s\n", opt.level.c_str());
		return 1;
	}
	Session session;
	OpenMode mode = level == scap_interface ? OpenMode::CardOnly : OpenMode::Camera;
	if (!session.open(opt, mode))
		return 2;
	return do_get(session, opt);
}

static bool link_feature(const std::string& name)
{
	return name == "FirmwareMode" || name == "CxpLinkConfiguration"
		|| name == "CxpPoCxpAuto" || name == "DeviceSelector";
}

static int do_set(Session& session, const Opt& opt, bool persistent)
{
	if (opt.pos.empty() || opt.pos.size() > 2) {
		std::fprintf(stderr, "нужно: set <feature> [значение]\n");
		return 1;
	}
	scap_level_t level;
	if (!parse_level(opt.level, level)) {
		std::fprintf(stderr, "неизвестный --level %s\n", opt.level.c_str());
		return 1;
	}
	if (persistent && link_feature(opt.pos[0])) {
		std::fprintf(stderr, "%s заново поднимает линк и в shell не пишется\n", opt.pos[0].c_str());
		return 2;
	}
	if (level == scap_interface && !session.card) {
		std::fprintf(stderr, "карта не открыта\n");
		return 2;
	}
	if (level != scap_interface && !session.cap) {
		std::fprintf(stderr, "камера не открыта\n");
		return 2;
	}
	void* handle = feature_handle(session, level);
	const char* value = opt.pos.size() == 2 ? opt.pos[1].c_str() : nullptr;
	if (!set_feature(handle, level, opt.pos[0].c_str(), value)) {
		std::fprintf(stderr, "%s: %s\n", opt.pos[0].c_str(), last_err());
		return 2;
	}
	std::printf("%s: ok\n", opt.pos[0].c_str());
	if (session.cap)
		session.refresh();
	return 0;
}

static int cmd_set(const Opt& opt)
{
	if (opt.pos.empty() || opt.pos.size() > 2) {
		std::fprintf(stderr, "нужно: set <feature> [значение]\n");
		return 1;
	}
	scap_level_t level;
	if (!parse_level(opt.level, level)) {
		std::fprintf(stderr, "неизвестный --level %s\n", opt.level.c_str());
		return 1;
	}
	Session session;
	OpenMode mode = level == scap_interface ? OpenMode::CardOnly : OpenMode::Camera;
	if (!session.open(opt, mode))
		return 2;
	return do_set(session, opt, false);
}

static bool save_snap(const std::string& stem, const void* base, size_t size, int64_t width, int64_t height)
{
	std::string raw_path = stem + ".raw";
	int fd = ::open(raw_path.c_str(), O_CREAT | O_TRUNC | O_WRONLY | O_CLOEXEC, 0644);
	if (fd < 0 || !write_all(fd, base, size)) {
		std::fprintf(stderr, "%s: %s\n", raw_path.c_str(), std::strerror(errno));
		if (fd >= 0)
			::close(fd);
		return false;
	}
	::fsync(fd);
	::close(fd);
	std::printf("записан %s (%zu байт)\n", raw_path.c_str(), size);

	if (width <= 0 || height <= 0 || size < static_cast<size_t>(width * height))
		return true;
	std::string pgm_path = stem + ".pgm";
	FILE* file = std::fopen(pgm_path.c_str(), "wb");
	if (!file) {
		std::fprintf(stderr, "%s: %s\n", pgm_path.c_str(), std::strerror(errno));
		return false;
	}
	std::fprintf(file, "P5\n%lld %lld\n255\n", (long long)width, (long long)height);
	size_t pixels = static_cast<size_t>(width * height);
	bool ok = std::fwrite(base, 1, pixels, file) == pixels;
	std::fclose(file);
	if (!ok) {
		std::fprintf(stderr, "%s: запись не полная\n", pgm_path.c_str());
		return false;
	}
	std::printf("записан %s\n", pgm_path.c_str());
	return true;
}

static int do_snap(Session& session, const Opt& opt)
{
	if (!session.cap) {
		std::fprintf(stderr, "камера не открыта\n");
		return 2;
	}
	std::string stem = opt.output.empty() ? "frame" : opt.output;
	std::string::size_type slash = stem.find_last_of('/');
	std::string::size_type dot = stem.find_last_of('.');
	if (dot != std::string::npos && (slash == std::string::npos || dot > slash)) {
		std::string ext = stem.substr(dot);
		if (ext == ".raw" || ext == ".pgm" || ext == ".png")
			stem.resize(dot);
	}
	if (!session.start(opt.bufs))
		return 3;
	int rc = 4;
	uint32_t skip = opt.skip;
	for (int attempt = 0; attempt < 200 && !g_stop.load(); ++attempt) {
		HeldBuf held(session);
		if (!held.pull(opt.timeout_ms))
			continue;
		if (skip > 0) {
			--skip;
			continue;
		}
		rc = save_snap(stem, held.base, held.size, held.info.width, held.info.height) ? 0 : 5;
		break;
	}
	session.stop();
	if (rc == 4)
		std::fprintf(stderr, "кадр не пришёл\n");
	return rc;
}

static int cmd_snap(const Opt& opt)
{
	Session session;
	if (!session.open(opt, OpenMode::CameraConfigured))
		return 2;
	return do_snap(session, opt);
}

static int do_record(Session& session, const Opt& opt)
{
	if (!session.cap) {
		std::fprintf(stderr, "камера не открыта\n");
		return 2;
	}
	if (opt.frames == 0 && opt.seconds <= 0) {
		std::fprintf(stderr, "record нужен -n или -t\n");
		return 1;
	}
	GrabRequest req;
	req.mode = "disk";
	req.path = opt.output.empty() ? "clip.raw" : opt.output;
	req.seconds = opt.seconds;
	req.frames = opt.frames;
	req.warmup = opt.skip;
	req.bufs = opt.bufs;
	req.timeout_ms = opt.timeout_ms;
	req.write_meta = true;
	Report rep;
	int rc = run_grab(session, req, rep);
	print_report(rep);
	return rc;
}

static int cmd_record(const Opt& opt)
{
	if (opt.frames == 0 && opt.seconds <= 0) {
		std::fprintf(stderr, "record нужен -n или -t\n");
		return 1;
	}
	Session session;
	if (!session.open(opt, OpenMode::CameraConfigured))
		return 2;
	return do_record(session, opt);
}

static int do_bench(Session* session, const Opt& opt, bool keep)
{
	std::string mode = opt.mode.empty() ? "all" : opt.mode;
	if (mode != "all" && mode != "discard" && mode != "copy" && mode != "disk"
		&& mode != "sink" && mode != "ssd") {
		std::fprintf(stderr, "неизвестный --mode %s\n", mode.c_str());
		return 1;
	}
	Opt timed = opt;
	if (timed.seconds <= 0 && timed.frames == 0)
		timed.seconds = 5;

	std::string disk_path = timed.output.empty() ? "bench_disk.raw" : timed.output;
	std::string sink_path = mode == "sink"
		? (timed.output.empty() ? "bench_sink.raw" : timed.output)
		: tagged(disk_path, "sink");

	std::vector<Report> reports;
	int rc = 0;
	bool camera = mode != "sink" && mode != "ssd";
	if (camera && (!session || !session->cap)) {
		std::fprintf(stderr, "камера не открыта\n");
		return 2;
	}

	auto run_mode = [&](const std::string& name, const std::string& path, bool meta) {
		if (g_stop.load())
			return;
		GrabRequest req;
		req.mode = name;
		req.path = path;
		req.seconds = timed.seconds;
		req.frames = timed.frames;
		req.warmup = timed.warmup;
		req.bufs = timed.bufs;
		req.timeout_ms = timed.timeout_ms;
		req.write_meta = meta;
		req.sync_io = name == "disk";
		Report rep;
		int one = run_grab(*session, req, rep);
		reports.push_back(rep);
		print_report(rep);
		if (one != 0 && rc == 0)
			rc = one;
	};

	auto fill_bytes_from_camera = [&]() {
		if (timed.bytes_set)
			return;
		for (const Report& prev : reports) {
			if (prev.payload > 0) {
				timed.bytes = prev.payload;
				break;
			}
		}
	};

	if (mode == "discard" || mode == "all")
		run_mode("discard", "", false);
	if (mode == "copy" || mode == "all")
		run_mode("copy", "", false);
	if (mode == "disk" || mode == "all")
		run_mode("disk", disk_path, true);
	if (!keep && session && camera && (mode == "all" || mode == "sink" || mode == "ssd"))
		session->close();
	if (mode == "sink" || mode == "all") {
		fill_bytes_from_camera();
		Report rep;
		int one = run_sink(timed, sink_path, "sink", rep);
		reports.push_back(rep);
		print_report(rep);
		if (one != 0 && rc == 0)
			rc = one;
	}
	if (mode == "ssd" || mode == "all") {
		fill_bytes_from_camera();
		NvmeTarget nvme = resolve_ssd_target(timed);
		Report rep;
		rep.mode = "ssd";
		if (!nvme.error.empty()) {
			rep.error = nvme.error;
			rep.storage = nvme.storage;
			reports.push_back(rep);
			print_report(rep);
			if (rc == 0)
				rc = 5;
		} else {
			std::string ssd_path;
			if (mode == "ssd" && !timed.output.empty())
				ssd_path = timed.output;
			else {
				ssd_path = nvme.dir;
				if (!ssd_path.empty() && ssd_path.back() != '/')
					ssd_path += '/';
				ssd_path += "bench_ssd.raw";
			}
			int one = run_sink(timed, ssd_path, "ssd", rep);
			if (rep.storage.empty())
				rep.storage = nvme.storage;
			reports.push_back(rep);
			print_report(rep);
			if (one != 0 && rc == 0)
				rc = one;
		}
	}

	bool write_json = !timed.no_json;
	if (write_json) {
		std::string json_path = timed.json_set ? timed.json : "bench.json";
		save_json(json_path, reports);
	}
	std::printf("\nМБ/с = 10^6 байт/с. discard — камера, copy — память, disk — запись с камеры,\n"
		"sink — текущий диск без камеры, ssd — NVMe без камеры.\n");
	return rc;
}

static int cmd_bench(const Opt& opt)
{
	std::string mode = opt.mode.empty() ? "all" : opt.mode;
	if (mode != "all" && mode != "discard" && mode != "copy" && mode != "disk"
		&& mode != "sink" && mode != "ssd") {
		std::fprintf(stderr, "неизвестный --mode %s\n", mode.c_str());
		return 1;
	}
	if (mode == "sink" || mode == "ssd")
		return do_bench(nullptr, opt, false);
	Session session;
	if (!session.open(opt, OpenMode::CameraConfigured))
		return 2;
	return do_bench(&session, opt, false);
}

static void shell_usage()
{
	std::fputs(
		"Команды shell (подробнее: help <команда>):\n"
		"  info                 карта, камера, линк, размер кадра\n"
		"  get <feature>        прочитать параметр GenICam\n"
		"  set <feature> [val]  записать параметр или выполнить команду\n"
		"  snap                 один кадр → .raw и .pgm\n"
		"  record               непрерывная запись в один .raw\n"
		"  bench                замер пропускной способности\n"
		"  help [команда]       эта справка или описание команды\n"
		"  quit / exit          закрыть сессию\n"
		"\n"
		"Tab — подсказки, ↑/↓ — история. Ctrl+C прерывает команду.\n"
		"Флаги линка (--card, --link, …) в shell не меняются.\n",
		stderr);
}

static bool shell_help_topic(const std::string& topic)
{
	std::string name = topic;
	if (!name.empty() && name[0] == '-')
		name.erase(0, name[1] == '-' ? 2 : 1);
	if (name == "info") {
		std::fputs(
			"info — состояние карты и камеры\n"
			"\n"
			"Показывает имя карты, вендор/модель/серийник камеры, линк,\n"
			"PixelFormat, размер кадра, выдержку, усиление, триггер.\n"
			"Параметров нет.\n"
			"\n"
			"Пример:\n"
			"  info\n",
			stderr);
		return true;
	}
	if (name == "get") {
		std::fputs(
			"get <feature> [--level camera|interface|device|stream]\n"
			"\n"
			"Читает параметр GenICam. По умолчанию уровень camera.\n"
			"Имена параметров — в `grabercam -h` (длинный список) или Tab после get.\n"
			"\n"
			"Примеры:\n"
			"  get DeviceTemperature\n"
			"  get ExposureTime\n"
			"  get CxpLinkConfiguration --level interface\n"
			"  get PixelFormat\n",
			stderr);
		return true;
	}
	if (name == "set") {
		std::fputs(
			"set <feature> [значение] [--level camera|interface|device|stream]\n"
			"\n"
			"Пишет параметр или выполняет команду GenICam (если значения нет).\n"
			"По умолчанию уровень camera. В shell нельзя писать FirmwareMode,\n"
			"CxpLinkConfiguration, CxpPoCxpAuto, DeviceSelector — они поднимают линк.\n"
			"\n"
			"Примеры:\n"
			"  set ExposureTime 5000\n"
			"  set PixelFormat Mono10\n"
			"  set TriggerMode Off\n"
			"  set TriggerSoftware\n",
			stderr);
		return true;
	}
	if (name == "snap") {
		std::fputs(
			"snap [-o путь] [--skip N] [--buf N] [--timeout-ms N]\n"
			"\n"
			"Снимает один кадр после прогрева. Пишет .raw и рядом .pgm (для Mono8/10/12).\n"
			"По умолчанию: frame.raw / frame.pgm, --skip 2.\n"
			"--format/--exposure/--gain в shell задавайте через set, не флагами snap.\n"
			"\n"
			"Примеры:\n"
			"  snap\n"
			"  snap -o /mnt/nvme/shot.raw\n"
			"  snap --skip 5\n",
			stderr);
		return true;
	}
	if (name == "record") {
		std::fputs(
			"record -n N | -t SEC [-o путь] [--skip N] [--buf N] [--timeout-ms N]\n"
			"\n"
			"Пишет непрерывный поток кадров в один .raw (нужны -n или -t).\n"
			"Пишет через кэш ОС, fsync в конце. Для скорости без дропов нужен\n"
			"быстрый диск или буфер в RAM (сейчас запись синхронно по кадрам\n"
			"для disk-режима bench; record ждёт write).\n"
			"Рядом пишется .txt с метаданными.\n"
			"\n"
			"Примеры:\n"
			"  record -t 5 -o /mnt/nvme/clip.raw\n"
			"  record -n 100\n",
			stderr);
		return true;
	}
	if (name == "bench") {
		std::fputs(
			"bench [--mode MODE] [-t SEC] [-n N] [--warmup N] [--bytes N]\n"
			"      [--ssd-dir PATH] [-o PATH] [--json PATH] [--no-json]\n"
			"\n"
			"Замер пропускной способности. По умолчанию --mode all, -t 5.\n"
			"\n"
			"Режимы (--mode):\n"
			"  discard  только захват, буфер сразу возвращается карте\n"
			"  copy     захват + memcpy в RAM\n"
			"  disk     захват + запись на диск текущего каталога (O_DSYNC)\n"
			"  sink     запись блоков без камеры на cwd\n"
			"  ssd      как sink на смонтированный NVMe\n"
			"  all      discard → copy → disk → sink → ssd\n"
			"\n"
			"МБ/с = 10^6 байт/с. Отчёт в bench.json, если не --no-json.\n"
			"\n"
			"Примеры:\n"
			"  bench\n"
			"  bench --mode discard -t 3\n"
			"  bench --mode ssd --ssd-dir /mnt/nvme\n",
			stderr);
		return true;
	}
	if (name == "help") {
		std::fputs(
			"help [команда]\n"
			"\n"
			"Без аргумента — список команд. С аргументом — справка по команде.\n"
			"\n"
			"Примеры:\n"
			"  help\n"
			"  help snap\n"
			"  help get\n",
			stderr);
		return true;
	}
	if (name == "quit" || name == "exit") {
		std::fputs(
			"quit / exit — закрыть shell-сессию\n"
			"\n"
			"Останавливает захват, закрывает карту и GenTL. Параметров нет.\n"
			"Ctrl+C на пустом приглашении тоже завершает сессию.\n",
			stderr);
		return true;
	}
	return false;
}

static const char* k_shell_cmds[] = {
	"info", "get", "set", "snap", "record", "bench", "help", "quit", "exit",
	nullptr
};

static const char* k_shell_features[] = {
	"Width", "Height", "OffsetX", "OffsetY", "PixelFormat", "PixelSize",
	"PixelDynamicRangeMin", "PixelDynamicRangeMax", "SensorWidth", "SensorHeight",
	"WidthMax", "HeightMax", "PayloadSize", "TestImageSelector",
	"AcquisitionMode", "AcquisitionStart", "AcquisitionStop", "AcquisitionFrameCount",
	"AcquisitionFrameRate", "AcquisitionFrameRateEnable", "ResultingFrameRateAbs",
	"AcquisitionStatusSelector", "AcquisitionStatus",
	"TriggerSelector", "TriggerMode", "TriggerSource", "TriggerActivation",
	"TriggerDelay", "TriggerSoftware",
	"ExposureMode", "ExposureTime", "ShutterMode",
	"GainSelector", "GainRaw", "AnalogGain", "ReferenceGain", "Gamma",
	"BlackLevelSelector", "BlackLevel",
	"BalanceWhiteAuto", "BalanceRatioSelector", "BalanceRatio",
	"Binning", "BinningMode", "ReverseX", "ReverseY", "ReverseMode",
	"MultipleROIEnable", "MultipleROICount", "ROISelector", "ROIOffsetY", "ROIHeight",
	"AverageFilter", "MedianFilter", "BackgroundSubtractionEnable",
	"FrameAveragingEnable", "FrameAveraging",
	"PixelShiftEnable", "PixelShiftDistance", "PixelShiftPositionReset",
	"PixelShiftMetaData", "PixelShiftUserTableCount", "PixelShiftUserTableIndex",
	"PixelShiftUserTableValue", "PixelShiftUserTableShots",
	"PixelShiftTriggerMode", "PixelShiftSoftwareTrigger",
	"LineSelector", "LineMode", "LineSource", "LineFormat", "LineInverter",
	"LineStatus", "LineStatusAll", "LineDebouncerTimeAbs",
	"UserOutputSelector", "UserOutputValue", "UserOutputValueAll",
	"UserSetSelector", "UserSetLoad", "UserSetSave", "UserSetDefault",
	"DeviceScanType", "DeviceTemperatureSelector", "DeviceTemperature",
	"DeviceHumiditySelector", "DeviceHumidity", "DeviceColdDewPoint",
	"TECSetpoint", "TECPwm", "StatusVoltageSelector", "StatusVoltage", "DeviceReset",
	"FanSelector", "FanMode", "FanSpeed", "FanRpm", "LedMode",
	"NumberOfLink", "LinkSpeed", "DefaultNumberOfLink", "DefaultLinkSpeed",
	"StartupNumberOfLink", "StartupLinkSpeed", "DeviceTapGeometry",
	"FFCMode", "FFCSet", "FFCStatus", "FFCRefreshStatus", "FFCReloadParameter",
	"FFCRGBEnable", "FFCRGBChannelSelector", "FFCRGBParameter",
	"ShadingSelector", "LSCTableSelector", "LSCEnable", "ActivateShading",
	"ReceivedTriggerCount", "AcceptedTriggerCount", "RejectedTriggerCount",
	"FrameOutCount", "ResetCounter",
	"SequencerMode", "SequencerSetCount", "SequencerSetSelector", "SequencerSetActive",
	"SequencerReset", "SequencerFeatureSelector", "SequencerFeatureEnable",
	"SequencerExposureTime", "SequencerGainRaw",
	"SequencerBalanceRatioSelector", "SequencerBalanceRatio",
	"SequencerFFCSet", "SequencerLSCTableSet",
	"ConnectionReset", "MasterHostConnectionID", "StreamPacketSizeMax",
	"Image1StreamID", "Image2StreamID", "XmlManifestSelector",
	"TestMode", "TestErrorCountSelector", "TestErrorCount",
	"TestPacketCountTx", "TestPacketCountRx", "ElectricalComplianceTest",
	"FirmwareMode", "DeviceSelector", "CxpLinkConfiguration", "CxpPoCxpAuto",
	nullptr
};

static const char* k_shell_flags[] = {
	"--level", "--mode", "--output", "-o", "--frames", "-n", "--seconds", "-t",
	"--warmup", "--skip", "--bytes", "--buf", "--timeout-ms", "--ssd-dir",
	"--json", "--no-json", "--format", "--exposure", "--gain",
	nullptr
};

static const char* k_shell_levels[] = {
	"camera", "interface", "device", "stream", nullptr
};

static const char* k_shell_modes[] = {
	"discard", "copy", "disk", "sink", "ssd", "all", nullptr
};

static const char* k_shell_formats[] = {
	"Mono8", "Mono10", "Mono12", nullptr
};

static char* shell_dup(const char* text)
{
	size_t n = std::strlen(text) + 1;
	char* out = static_cast<char*>(std::malloc(n));
	if (out)
		std::memcpy(out, text, n);
	return out;
}

static char* shell_match_gen(const char* text, int state, const char* const* list)
{
	static int index = 0;
	static size_t len = 0;
	if (state == 0) {
		index = 0;
		len = std::strlen(text);
	}
	while (list[index]) {
		const char* item = list[index++];
		if (std::strncmp(item, text, len) == 0)
			return shell_dup(item);
	}
	return nullptr;
}

static char* shell_cmd_gen(const char* text, int state)
{
	return shell_match_gen(text, state, k_shell_cmds);
}

static char* shell_feature_gen(const char* text, int state)
{
	return shell_match_gen(text, state, k_shell_features);
}

static char* shell_flag_gen(const char* text, int state)
{
	return shell_match_gen(text, state, k_shell_flags);
}

static char* shell_level_gen(const char* text, int state)
{
	return shell_match_gen(text, state, k_shell_levels);
}

static char* shell_mode_gen(const char* text, int state)
{
	return shell_match_gen(text, state, k_shell_modes);
}

static char* shell_format_gen(const char* text, int state)
{
	return shell_match_gen(text, state, k_shell_formats);
}

static std::string shell_token_before(int point)
{
	if (!rl_line_buffer || point <= 0)
		return {};
	int end = point;
	while (end > 0 && !std::isspace(static_cast<unsigned char>(rl_line_buffer[end - 1])))
		--end;
	int begin = end;
	while (begin > 0 && std::isspace(static_cast<unsigned char>(rl_line_buffer[begin - 1])))
		--begin;
	int start = begin;
	while (start > 0 && !std::isspace(static_cast<unsigned char>(rl_line_buffer[start - 1])))
		--start;
	if (start >= begin)
		return {};
	return std::string(rl_line_buffer + start, rl_line_buffer + begin);
}

static std::string shell_first_word()
{
	if (!rl_line_buffer)
		return {};
	const char* p = rl_line_buffer;
	while (*p && std::isspace(static_cast<unsigned char>(*p)))
		++p;
	const char* start = p;
	while (*p && !std::isspace(static_cast<unsigned char>(*p)))
		++p;
	return std::string(start, p);
}

static char** shell_completion(const char* text, int start, int /*end*/)
{
	rl_attempted_completion_over = 1;
	if (start == 0)
		return rl_completion_matches(text, shell_cmd_gen);

	std::string prev = shell_token_before(start);
	std::string cmd = shell_first_word();
	if (prev == "--level")
		return rl_completion_matches(text, shell_level_gen);
	if (prev == "--mode")
		return rl_completion_matches(text, shell_mode_gen);
	if (prev == "--format" || prev == "PixelFormat")
		return rl_completion_matches(text, shell_format_gen);
	if (text[0] == '-')
		return rl_completion_matches(text, shell_flag_gen);
	if (cmd == "get" || cmd == "set") {
		if (prev == "get" || prev == "set" || prev.empty())
			return rl_completion_matches(text, shell_feature_gen);
	}
	if (cmd == "help")
		return rl_completion_matches(text, shell_cmd_gen);
	return nullptr;
}

static std::string shell_history_path()
{
	const char* home = std::getenv("HOME");
	if (!home || !*home)
		return {};
	return std::string(home) + "/.grabercam_history";
}

static bool tokenize(const std::string& line, std::vector<std::string>& words)
{
	words.clear();
	std::string current;
	bool quote = false;
	for (char c : line) {
		if (quote) {
			if (c == '"')
				quote = false;
			else
				current.push_back(c);
			continue;
		}
		if (c == '"') {
			quote = true;
			continue;
		}
		if (c == ' ' || c == '\t' || c == '\r') {
			if (!current.empty()) {
				words.push_back(current);
				current.clear();
			}
			continue;
		}
		current.push_back(c);
	}
	if (quote) {
		std::fprintf(stderr, "незакрытая кавычка\n");
		return false;
	}
	if (!current.empty())
		words.push_back(current);
	return true;
}

static bool parse_words(const std::vector<std::string>& words, std::string& cmd, Opt& opt)
{
	std::vector<std::string> storage;
	storage.reserve(words.size() + 1);
	storage.emplace_back("grabercam");
	storage.insert(storage.end(), words.begin(), words.end());
	std::vector<char*> argv;
	argv.reserve(storage.size());
	for (std::string& word : storage)
		argv.push_back(word.data());
	return parse_args(static_cast<int>(argv.size()), argv.data(), cmd, opt);
}

static bool link_flag(const std::string& arg)
{
	return arg == "--card" || arg == "--camera" || arg == "--link"
		|| arg == "--firmware" || arg == "--link-wait" || arg == "--gentl";
}

static bool configure_flag(const std::string& arg)
{
	return arg == "--format" || arg == "--exposure" || arg == "--gain";
}

static bool read_line_plain(std::string& line)
{
	line.clear();
	char buf[4096];
	while (!g_stop.load()) {
		errno = 0;
		if (!std::fgets(buf, sizeof(buf), stdin)) {
			if (errno == EINTR) {
				std::clearerr(stdin);
				continue;
			}
			return false;
		}
		line.append(buf);
		if (!line.empty() && line.back() == '\n') {
			line.pop_back();
			if (!line.empty() && line.back() == '\r')
				line.pop_back();
			return true;
		}
	}
	return false;
}

static bool read_line_readline(std::string& line)
{
	line.clear();
	errno = 0;
	char* raw = readline("grabercam> ");
	if (!raw)
		return false;
	line = raw;
	if (!line.empty())
		add_history(raw);
	std::free(raw);
	return true;
}

static int run_shell_command(Session& session, const Opt& base, const std::vector<std::string>& words)
{
	for (const std::string& word : words) {
		if (link_flag(word)) {
			std::fprintf(stderr, "%s в shell не меняется, он задаётся при запуске\n", word.c_str());
			return 2;
		}
	}
	Opt opt = base;
	opt.pos.clear();
	std::string cmd;
	bool asked_help = false;
	for (const std::string& word : words) {
		if (word == "-h" || word == "--help")
			asked_help = true;
	}
	if (!parse_words(words, cmd, opt)) {
		if (!asked_help)
			std::fprintf(stderr, "команда не разобрана\n");
		return 1;
	}
	bool configure = false;
	for (const std::string& word : words) {
		if (configure_flag(word))
			configure = true;
	}
	if (configure && cmd != "snap" && cmd != "record" && cmd != "bench") {
		std::fprintf(stderr, "--format, --exposure и --gain здесь задаются через set\n");
		return 1;
	}
	session.stop();
	if (configure && !session.configure(opt))
		return 2;
	if (cmd == "info")
		return do_info(session, opt);
	if (cmd == "get")
		return do_get(session, opt);
	if (cmd == "set")
		return do_set(session, opt, true);
	if (cmd == "snap")
		return do_snap(session, opt);
	if (cmd == "record")
		return do_record(session, opt);
	if (cmd == "bench")
		return do_bench(&session, opt, true);
	if (cmd == "shell") {
		std::fprintf(stderr, "сессия уже открыта\n");
		return 1;
	}
	std::fprintf(stderr, "неизвестная команда %s\n", cmd.c_str());
	shell_usage();
	return 1;
}

static int cmd_shell(const Opt& opt)
{
	Session session;
	OpenMode mode = (opt.format_set || opt.exposure_set || opt.gain_set)
		? OpenMode::CameraConfigured : OpenMode::Camera;
	if (!session.open(opt, mode))
		return 2;
	bool tty = ::isatty(fileno(stdin)) == 1;
	std::string history = shell_history_path();
	if (tty) {
		using_history();
		rl_outstream = stderr;
		rl_attempted_completion_function = shell_completion;
		if (!history.empty())
			read_history(history.c_str());
		std::fprintf(stderr, "сессия открыта. help — список, quit — выход; Tab и ↑/↓ доступны\n");
	} else {
		std::fprintf(stderr, "сессия открыта. help — список, quit — выход\n");
	}
	int rc = 0;
	while (!g_stop.load()) {
		std::string line;
		bool ok = false;
		if (tty)
			ok = read_line_readline(line);
		else
			ok = read_line_plain(line);
		if (!ok)
			break;
		std::vector<std::string> words;
		if (!tokenize(line, words) || words.empty())
			continue;
		if (words[0] == "quit" || words[0] == "exit")
			break;
		if (words[0] == "help") {
			if (words.size() == 1) {
				shell_usage();
			} else if (!shell_help_topic(words[1])) {
				std::fprintf(stderr, "нет справки для %s\n", words[1].c_str());
				shell_usage();
			}
			continue;
		}
		run_shell_command(session, opt, words);
		if (g_stop.load()) {
			std::fprintf(stderr, "прервано\n");
			g_stop.store(false);
		}
	}
	if (tty && !history.empty()) {
		write_history(history.c_str());
		history_truncate_file(history.c_str(), 500);
	}
	if (g_stop.load())
		rc = 130;
	session.stop();
	std::fprintf(stderr, "сессия закрыта\n");
	session.close();
	return rc;
}

int main(int argc, char** argv)
{
	std::signal(SIGINT, on_signal);
	std::signal(SIGTERM, on_signal);

	std::string cmd;
	Opt opt;
	if (!parse_args(argc, argv, cmd, opt))
		return cmd.empty() || cmd == "-h" || cmd == "--help" || cmd == "help" ? 0 : 1;

	if (cmd == "info")
		return cmd_info(opt);
	if (cmd == "get")
		return cmd_get(opt);
	if (cmd == "set")
		return cmd_set(opt);
	if (cmd == "snap")
		return cmd_snap(opt);
	if (cmd == "record")
		return cmd_record(opt);
	if (cmd == "bench")
		return cmd_bench(opt);
	if (cmd == "shell")
		return cmd_shell(opt);

	std::fprintf(stderr, "неизвестная команда %s\n", cmd.c_str());
	usage();
	return 1;
}
