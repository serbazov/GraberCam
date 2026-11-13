#!/usr/bin/env python3
"""Окно live-view: превью с граббера и настройки камеры."""

import json
import mmap
import os
import socket
import struct
import subprocess
import sys
import threading
from pathlib import Path

import gi

gi.require_version("Gtk", "4.0")
gi.require_version("Gdk", "4.0")
from gi.repository import GLib, Gdk, Gtk

ROOT = Path(__file__).resolve().parent.parent
BIN = ROOT / "graberview"
SHM_PATH = "/dev/shm/grabercam-preview"
SOCK_PATH = "/tmp/grabercam-view.sock"
PIX_OFF = 512

FEATURES = (
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
)

CSS = """
.preview { background: #2c313a; }
.side {
  background: #f4f6f8;
  color: #14181f;
  padding: 14px;
}
.side label,
.side dropdown label,
.side button label {
  color: #14181f;
}
label.title {
  font-size: 18px;
  font-weight: 700;
  color: #10141a;
}
label.section {
  font-weight: 600;
  color: #10141a;
  margin-top: 4px;
}
label.muted { color: #2a3140; }
label.hud {
  background: #14181f;
  color: #f7f8fa;
  padding: 8px 12px;
  border-radius: 8px;
}
.side entry,
.side spinbutton,
.side spinbutton entry,
.side dropdown > button {
  background-color: #ffffff;
  color: #14181f;
  caret-color: #14181f;
  border: 1px solid #8d96a3;
  border-radius: 6px;
}
.side entry placeholder {
  color: #4e5866;
  opacity: 1;
}
.side entry selection {
  background-color: #1d4f91;
  color: #ffffff;
}
.side button {
  background: #e6ebf1;
  color: #14181f;
  border: 1px solid #8d96a3;
  border-radius: 6px;
  padding: 6px 10px;
}
.side button:hover { background: #d7dee7; }
.side separator { background: #c5ccd6; min-height: 1px; }
"""


def is_on(value):
	return str(value).strip().lower() in ("1", "on", "true", "yes")


class Live(Gtk.Application):
	def __init__(self):
		super().__init__(application_id="local.grabercam.live")
		self.proc = None
		self.sock = None
		self.mm = None
		self.block = False
		self.win = None
		self.pending = {}
		self.last_seq = 0
		self.shown = 0
		self.shown_t = 0
		self.disp_fps = 0.0
		self._tex_bytes = None
		self.extra = []
		self.ready = False

	def do_activate(self):
		if self.win is not None:
			self.win.present()
			return
		self.win = Gtk.ApplicationWindow(application=self, title="GraberCam")
		self.win.set_default_size(1440, 900)
		self.win.connect("close-request", self.on_close)

		provider = Gtk.CssProvider()
		provider.load_from_string(CSS)
		Gtk.StyleContext.add_provider_for_display(
			Gdk.Display.get_default(),
			provider,
			Gtk.STYLE_PROVIDER_PRIORITY_APPLICATION,
		)

		root = Gtk.Box(orientation=Gtk.Orientation.HORIZONTAL)
		self.picture = Gtk.Picture()
		self.picture.set_hexpand(True)
		self.picture.set_vexpand(True)
		self.picture.set_content_fit(Gtk.ContentFit.CONTAIN)
		self.picture.set_can_shrink(True)
		self.hud = Gtk.Label(label="подключение…")
		self.hud.add_css_class("hud")
		self.hud.set_halign(Gtk.Align.START)
		self.hud.set_valign(Gtk.Align.END)
		self.hud.set_margin_start(12)
		self.hud.set_margin_bottom(12)
		overlay = Gtk.Overlay()
		overlay.add_css_class("preview")
		overlay.set_child(self.picture)
		overlay.add_overlay(self.hud)
		overlay.set_hexpand(True)

		side = Gtk.Box(orientation=Gtk.Orientation.VERTICAL, spacing=8)
		side.add_css_class("side")
		self.title = Gtk.Label(label="Камера", xalign=0)
		self.title.add_css_class("title")
		self.meta = Gtk.Label(label="", xalign=0, wrap=True)
		self.meta.add_css_class("muted")
		self.status = Gtk.Label(label="", xalign=0, wrap=True)
		self.status.add_css_class("muted")
		side.append(self.title)
		side.append(self.meta)
		side.append(self.status)
		side.append(Gtk.Separator())

		self.format = self.combo("Формат", ["Mono8", "Mono10", "Mono12"], "PixelFormat")
		self.exposure = self.spin("Выдержка, мкс", 27, 5_000_000, 100, "ExposureTime", integer=True)
		self.gain = self.spin("GainRaw", 0, 64, 0.1, "GainRaw")
		self.analog = self.spin("AnalogGain", 1, 8, 0.1, "AnalogGain")
		self.trigger = self.combo("Триггер", ["Off", "On"], "TriggerMode")
		self.rate_on = self.switch("Ограничить частоту", "AcquisitionFrameRateEnable")
		self.rate = self.spin("Частота, Гц", 0.1, 120, 0.1, "AcquisitionFrameRate")
		self.test = self.combo("Тест-картинка", ["Off", "TestImage1", "TestImage2"], "TestImageSelector")
		self.rev_x = self.switch("Отразить по X", "ReverseX")
		self.rev_y = self.switch("Отразить по Y", "ReverseY")

		for widget in (
			self.format[0], self.exposure[0], self.gain[0], self.analog[0],
			self.trigger[0], self.rate_on[0], self.rate[0], self.test[0],
			self.rev_x[0], self.rev_y[0],
		):
			side.append(widget)

		side.append(Gtk.Separator())
		side.append(self.section("Область кадра"))
		self.roi_w = Gtk.SpinButton.new_with_range(16, 20000, 8)
		self.roi_h = Gtk.SpinButton.new_with_range(16, 20000, 8)
		self.roi_x = Gtk.SpinButton.new_with_range(0, 20000, 8)
		self.roi_y = Gtk.SpinButton.new_with_range(0, 20000, 8)
		grid = Gtk.Grid(column_spacing=8, row_spacing=4)
		for i, (label, spin) in enumerate((
			("Ширина", self.roi_w), ("Высота", self.roi_h),
			("Сдвиг X", self.roi_x), ("Сдвиг Y", self.roi_y),
		)):
			grid.attach(Gtk.Label(label=label, xalign=0), 0, i, 1, 1)
			grid.attach(spin, 1, i, 1, 1)
		side.append(grid)
		roi_btn = Gtk.Button(label="Применить область")
		roi_btn.connect("clicked", self.apply_roi)
		side.append(roi_btn)

		side.append(Gtk.Separator())
		side.append(self.section("Параметр GenICam"))
		feat_model = Gtk.StringList()
		for name in FEATURES:
			feat_model.append(name)
		self.feat_name = Gtk.DropDown(model=feat_model)
		self.feat_name.set_expression(Gtk.PropertyExpression.new(Gtk.StringObject, None, "string"))
		self.feat_name.set_enable_search(True)
		self.feat_name.connect("notify::selected", self.on_feature_selected)
		self.feat_value = Gtk.Entry()
		self.feat_value.set_placeholder_text("значение для записи")
		row = Gtk.Box(spacing=6)
		read_btn = Gtk.Button(label="Читать")
		write_btn = Gtk.Button(label="Записать")
		read_btn.connect("clicked", lambda *_: self.read_custom())
		write_btn.connect("clicked", lambda *_: self.write_custom())
		row.append(read_btn)
		row.append(write_btn)
		side.append(self.feat_name)
		side.append(self.feat_value)
		side.append(row)

		side.append(Gtk.Separator())
		self.snap_path = Gtk.Entry()
		self.snap_path.set_text(str(ROOT / "snap.pgm"))
		snap_btn = Gtk.Button(label="Сохранить кадр")
		snap_btn.connect("clicked", self.save_frame)
		side.append(self.snap_path)
		side.append(snap_btn)
		reload_btn = Gtk.Button(label="Перечитать настройки")
		reload_btn.connect("clicked", lambda *_: self.pull_settings())
		side.append(reload_btn)

		scroll = Gtk.ScrolledWindow()
		scroll.set_child(side)
		scroll.set_policy(Gtk.PolicyType.NEVER, Gtk.PolicyType.AUTOMATIC)
		scroll.set_size_request(340, -1)
		root.append(overlay)
		root.append(scroll)
		self.win.set_child(root)
		self.win.present()

		if not BIN.is_file():
			self.status.set_text("нет graberview, соберите: make")
			return
		args = [str(BIN), *self.extra]
		self.proc = subprocess.Popen(
			args,
			cwd=ROOT,
			stderr=subprocess.PIPE,
			text=True,
		)
		threading.Thread(target=self.read_stderr, daemon=True).start()
		GLib.timeout_add(100, self.try_connect)
		GLib.timeout_add(16, self.tick)

	def section(self, text):
		label = Gtk.Label(label=text, xalign=0)
		label.add_css_class("section")
		return label

	def labeled(self, text, widget):
		box = Gtk.Box(orientation=Gtk.Orientation.VERTICAL, spacing=2)
		label = Gtk.Label(label=text, xalign=0)
		box.append(label)
		box.append(widget)
		return box

	def combo(self, title, values, feature):
		model = Gtk.StringList()
		for value in values:
			model.append(value)
		drop = Gtk.DropDown(model=model)
		drop.connect("notify::selected", lambda widget, _pspec, name=feature: self.on_combo(widget, name))
		return self.labeled(title, drop), drop

	def spin(self, title, lo, hi, step, feature, integer=False):
		spin = Gtk.SpinButton.new_with_range(lo, hi, step)
		spin.set_digits(0 if integer else 2)
		spin.connect("value-changed", lambda s, f=feature, i=integer: self.on_spin(s, f, i))
		return self.labeled(title, spin), spin

	def switch(self, title, feature):
		row = Gtk.Box(spacing=8)
		row.append(Gtk.Label(label=title, xalign=0, hexpand=True))
		sw = Gtk.Switch()
		sw.set_valign(Gtk.Align.CENTER)
		sw.connect("state-set", lambda s, state, f=feature: self.on_switch(f, state))
		row.append(sw)
		return row, sw

	def on_combo(self, drop, feature):
		if self.block:
			return
		item = drop.get_selected_item()
		if item is not None:
			self.send_set(feature, item.get_string())

	def on_spin(self, spin, feature, integer):
		if self.block:
			return
		value = int(spin.get_value()) if integer else spin.get_value()
		text = str(value) if integer else f"{value:.6g}"
		if feature in self.pending:
			GLib.source_remove(self.pending[feature])
		self.pending[feature] = GLib.timeout_add(250, self.send_set, feature, text)

	def on_switch(self, feature, state):
		if self.block:
			return False
		self.send_set(feature, "On" if state else "Off")
		return False

	def send_set(self, feature, text):
		self.pending.pop(feature, None)
		reply = self.cmd(f"SET {feature} {text}")
		if reply is None:
			return False
		if reply.startswith("ERR"):
			self.status.set_text(reply)
		else:
			self.status.set_text(f"{feature} = {text}")
			if feature in ("PixelFormat", "Width", "Height", "OffsetX", "OffsetY"):
				self.pull_settings()
		return False

	def apply_roi(self, *_):
		w = int(self.roi_w.get_value())
		h = int(self.roi_h.get_value())
		x = int(self.roi_x.get_value())
		y = int(self.roi_y.get_value())
		reply = self.cmd(f"ROI {w} {h} {x} {y}")
		self.status.set_text(reply or "нет связи")
		self.pull_settings()

	def selected_feature(self):
		item = self.feat_name.get_selected_item()
		if item is None:
			return ""
		return item.get_string()

	def on_feature_selected(self, *_):
		if self.block or self.sock is None:
			return
		self.read_custom()

	def read_custom(self):
		name = self.selected_feature()
		if not name:
			return
		reply = self.cmd(f"GET {name}")
		if reply and reply.startswith("OK "):
			self.feat_value.set_text(reply[3:])
			self.status.set_text(f"{name} = {reply[3:]}")
		else:
			self.status.set_text(reply or "нет связи")

	def write_custom(self):
		name = self.selected_feature()
		if not name:
			return
		reply = self.cmd(f"SET {name} {self.feat_value.get_text()}")
		self.status.set_text(reply or "нет связи")

	def save_frame(self, *_):
		path = self.snap_path.get_text().strip()
		if not path:
			return
		reply = self.cmd(f"SNAP {path}")
		self.status.set_text("сохранено " + path if reply and reply.startswith("OK") else (reply or "нет связи"))

	def cmd(self, line):
		if self.sock is None:
			return None
		try:
			self.sock.sendall((line + "\n").encode())
			data = b""
			while b"\n" not in data:
				chunk = self.sock.recv(4096)
				if not chunk:
					self.sock = None
					return None
				data += chunk
			return data.split(b"\n", 1)[0].decode(errors="replace")
		except OSError as exc:
			self.status.set_text(str(exc))
			return None

	def try_connect(self):
		if self.proc and self.proc.poll() is not None:
			self.status.set_text("захват завершился, смотрите сообщение выше")
			return False
		if not os.path.exists(SOCK_PATH):
			return True
		try:
			sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
			sock.connect(SOCK_PATH)
			sock.settimeout(None)
			self.sock = sock
		except OSError:
			return True
		try:
			fd = os.open(SHM_PATH, os.O_RDONLY)
			self.mm = mmap.mmap(fd, 0, access=mmap.ACCESS_READ)
			os.close(fd)
		except OSError as exc:
			self.status.set_text(str(exc))
			return False
		self.pull_settings()
		self.ready = True
		self.read_custom()
		self.status.set_text("поток идёт")
		return False

	def pull_settings(self):
		reply = self.cmd("INFO")
		if not reply or not reply.startswith("OK "):
			self.status.set_text(reply or "нет INFO")
			return
		try:
			info = json.loads(reply[3:])
		except json.JSONDecodeError:
			self.status.set_text(reply)
			return
		self.block = True
		self.title.set_text(info.get("model") or "Камера")
		self.meta.set_text(
			f"sn {info.get('serial', '')}\n"
			f"{info.get('w')}×{info.get('h')}  {info.get('format', '')}"
		)
		self.set_combo(self.format[1], info.get("format", ""))
		self.set_spin(self.exposure[1], info.get("exposure"))
		self.set_spin(self.gain[1], info.get("gain"))
		self.set_spin(self.analog[1], info.get("analog"))
		self.set_combo(self.trigger[1], info.get("trigger", "Off"))
		self.set_spin(self.roi_w, info.get("w"))
		self.set_spin(self.roi_h, info.get("h"))
		self.block = False
		for feature, widget, kind in (
			("AcquisitionFrameRateEnable", self.rate_on[1], "bool"),
			("AcquisitionFrameRate", self.rate[1], "spin"),
			("TestImageSelector", self.test[1], "combo"),
			("ReverseX", self.rev_x[1], "bool"),
			("ReverseY", self.rev_y[1], "bool"),
			("OffsetX", self.roi_x, "spin"),
			("OffsetY", self.roi_y, "spin"),
		):
			got = self.cmd(f"GET {feature}")
			if not got or not got.startswith("OK "):
				continue
			value = got[3:]
			self.block = True
			if kind == "bool":
				widget.set_active(is_on(value))
			elif kind == "combo":
				self.set_combo(widget, value)
			else:
				self.set_spin(widget, value)
			self.block = False

	def set_combo(self, drop, value):
		if not value:
			return
		model = drop.get_model()
		for i in range(model.get_n_items()):
			if model.get_string(i) == value:
				drop.set_selected(i)
				return
		model.append(value)
		drop.set_selected(model.get_n_items() - 1)

	def set_spin(self, spin, value):
		try:
			spin.set_value(float(value))
		except (TypeError, ValueError):
			pass

	def tick(self):
		if self.mm is None:
			return True
		seq1 = struct.unpack_from("<I", self.mm, 4)[0]
		if seq1 == 0 or seq1 & 1 or seq1 == self.last_seq:
			self.update_hud_only()
			return True
		width, height, cam_w, cam_h = struct.unpack_from("<IIII", self.mm, 8)
		grab_fps, preview_fps = struct.unpack_from("<ff", self.mm, 24)
		nbytes = int(width) * int(height) * 3
		if width == 0 or height == 0 or PIX_OFF + nbytes > len(self.mm):
			return True
		raw = self.mm[PIX_OFF:PIX_OFF + nbytes]
		seq2 = struct.unpack_from("<I", self.mm, 4)[0]
		if seq1 != seq2 or seq2 & 1:
			return True
		self.last_seq = seq2
		now = GLib.get_monotonic_time()
		if self.shown_t:
			dt = (now - self.shown_t) / 1e6
			if dt > 0:
				self.disp_fps = 0.8 * self.disp_fps + 0.2 * (1.0 / dt)
		self.shown_t = now
		self.shown += 1
		self._tex_bytes = GLib.Bytes.new(raw)
		texture = Gdk.MemoryTexture.new(
			int(width), int(height), Gdk.MemoryFormat.R8G8B8, self._tex_bytes, int(width) * 3
		)
		self.picture.set_paintable(texture)
		self.hud.set_text(
			f"камера {grab_fps:.1f} к/с   превью {preview_fps:.1f} к/с   экран {self.disp_fps:.0f} к/с\n"
			f"{cam_w}×{cam_h} → показ {width}×{height}"
		)
		return True

	def update_hud_only(self):
		if self.mm is None:
			return
		seq = struct.unpack_from("<I", self.mm, 4)[0]
		if seq == 0 or seq & 1:
			return
		grab_fps, preview_fps = struct.unpack_from("<ff", self.mm, 24)
		cam_w, cam_h = struct.unpack_from("<II", self.mm, 16)
		width, height = struct.unpack_from("<II", self.mm, 8)
		self.hud.set_text(
			f"камера {grab_fps:.1f} к/с   превью {preview_fps:.1f} к/с   экран {self.disp_fps:.0f} к/с\n"
			f"{cam_w}×{cam_h} → показ {width}×{height}"
		)

	def read_stderr(self):
		if not self.proc or not self.proc.stderr:
			return
		for line in self.proc.stderr:
			text = line.strip()
			if not text:
				continue
			if self.ready and "не " not in text and "ERR" not in text and "error" not in text.lower():
				continue
			GLib.idle_add(self.status.set_text, text)

	def on_close(self, *_):
		self.shutdown()
		return False

	def shutdown(self):
		if self.sock is not None:
			try:
				self.sock.sendall(b"QUIT\n")
			except OSError:
				pass
			self.sock = None
		if self.proc is not None:
			try:
				self.proc.wait(timeout=2)
			except subprocess.TimeoutExpired:
				self.proc.kill()
			self.proc = None


def main():
	extra = sys.argv[1:]
	sys.argv = sys.argv[:1]
	app = Live()
	app.extra = extra
	raise SystemExit(app.run(None))


if __name__ == "__main__":
	main()
