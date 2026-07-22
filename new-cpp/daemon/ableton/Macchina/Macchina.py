# Macchina.py — the Remote Script proper.
#
# Design: Live's embedded Python is a hostile place (no pip, silent crashes),
# so this stays deliberately tiny: one non-blocking UDP socket, a ~50-line
# OSC codec, Live listeners pushing state OUT, and a periodic tick pulling
# daemon events IN. All hardware knowledge lives in macchinad; this file only
# speaks the /event/* + /led/name + /display/* vocabulary.
#
# v1 scope — transport + mixer:
#   Play        -> toggle playback          (LED follows Live)
#   Rec         -> toggle session record    (LED follows)
#   Restart     -> jump to 1.1.1
#   Metro       -> toggle metronome         (LED follows)
#   Master knob -> master volume
#   Wheel       -> tempo +-1 BPM per detent (push = +-0.1 fine... push+turn)
#   VU meters   -> Live master output level
#   Left screen -> tempo / position / play state / volume HUD

import socket
import struct

from _Framework.ControlSurface import ControlSurface

DAEMON = ('127.0.0.1', 7000)

# ---- mini OSC codec ---------------------------------------------------------

def _pad(b):
    return b + b'\x00' * (4 - (len(b) % 4) if len(b) % 4 else 4)

def osc_encode(addr, *args):
    out = _pad(addr.encode())
    tags, data = ',', b''
    for a in args:
        if isinstance(a, float):
            tags += 'f'
            data += struct.pack('>f', a)
        elif isinstance(a, int):
            tags += 'i'
            data += struct.pack('>i', a)
        else:
            tags += 's'
            data += _pad(str(a).encode())
    return out + _pad(tags.encode()) + data

def _read_str(buf, pos):
    end = buf.index(b'\x00', pos)
    s = buf[pos:end].decode('utf-8', 'replace')
    return s, pos + ((end - pos) // 4 + 1) * 4

def osc_decode(buf):
    addr, pos = _read_str(buf, 0)
    tags, pos = _read_str(buf, pos)
    args = []
    for t in tags[1:]:
        if t == 'i':
            args.append(struct.unpack('>i', buf[pos:pos + 4])[0]); pos += 4
        elif t == 'f':
            args.append(struct.unpack('>f', buf[pos:pos + 4])[0]); pos += 4
        elif t == 's':
            s, pos = _read_str(buf, pos)
            args.append(s)
        else:
            return addr, args   # unknown tag: bail with what we have
    return addr, args

# ---- colors (RGB565) --------------------------------------------------------

WHITE, ORANGE, GREY, GREEN = 0xFFFF, 0xFC00, 0x8410, 0x07E0


class Macchina(ControlSurface):

    def __init__(self, c_instance):
        super(Macchina, self).__init__(c_instance)
        with self.component_guard():
            self._sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
            self._sock.bind(('127.0.0.1', 0))
            self._sock.setblocking(False)
            self._hud_dirty = True
            self._wheel_pushed = False
            self._page = 'home'   # daemon-owned; it tells us via /page
            self._track_offset = 0
            self._param_offset = 0
            self._plug_dev = None   # to reset the param window on device change
            self._resub_countdown = 0   # periodic re-subscribe (see _tick)

            self._send('/macchina/subscribe')

            song = self.song()
            song.add_is_playing_listener(self._on_transport_changed)
            song.add_session_record_listener(self._on_transport_changed)
            song.add_metronome_listener(self._on_transport_changed)
            song.add_tempo_listener(self._on_transport_changed)
            song.master_track.mixer_device.volume.add_value_listener(self._on_volume_changed)

            self._push_leds()
            self._draw_static_screen()
            self._send_hud_state()
            self.schedule_message(1, self._tick)
            self.log_message('Macchina: connected to macchinad')

    def disconnect(self):
        try:
            self._send('/all', 0)
            self._send('/display/clear', 0, 0)
            self._send('/display/flush', 0)
            self._send('/display/clear', 1, 0)
            self._send('/display/flush', 1)
            self._sock.close()
        except Exception:
            pass
        super(Macchina, self).disconnect()

    # ---- outgoing -----------------------------------------------------------

    def _send(self, addr, *args):
        try:
            self._sock.sendto(osc_encode(addr, *args), DAEMON)
        except Exception:
            pass   # daemon down: keep Live healthy, retry on next event

    def _on_transport_changed(self):
        self._push_leds()
        self._hud_dirty = True

    def _on_volume_changed(self):
        self._hud_dirty = True

    def _push_leds(self):
        song = self.song()
        self._send('/led/name', 'transport_play', 127 if song.is_playing else 8)
        self._send('/led/name', 'transport_record', 127 if song.session_record else 8)
        self._send('/led/name', 'transport_metro', 127 if song.metronome else 8)
        self._send('/led/name', 'transport_loop', 8)   # Restart: dim = "available"

    # ---- screens ------------------------------------------------------------

    def _draw_static_screen(self):
        s = self._send
        s('/display/clear', 1, 0)
        s('/display/text', 1, 8, 8, 2, ORANGE, 'MACCHINA <-> ABLETON LIVE')
        s('/display/rect', 1, 0, 30, 480, 2, ORANGE)
        s('/display/text', 1, 8, 48, 2, WHITE, 'PLAY    START/STOP')
        s('/display/text', 1, 8, 72, 2, WHITE, 'REC     SESSION RECORD')
        s('/display/text', 1, 8, 96, 2, WHITE, 'RESTART JUMP TO 1.1.1')
        s('/display/text', 1, 8, 120, 2, WHITE, 'METRO   METRONOME')
        s('/display/text', 1, 8, 144, 2, WHITE, 'WHEEL   TEMPO (PUSH: FINE)')
        s('/display/text', 1, 8, 168, 2, WHITE, 'MASTER  MASTER VOLUME')
        s('/display/flush', 1)

    def _send_hud_state(self):
        # The daemon owns the HUD pixels and animates locally (e.g. the
        # master-vol bar follows the encoder at 60 fps); we just send state
        # at Remote-Script speed (~10 Hz) and it reconciles.
        song = self.song()
        beats = song.get_current_beats_song_time()
        pos = '%d.%d.%d' % (beats.bars, beats.beats, beats.sub_division)
        self._send('/hud/state', float(song.tempo), 1 if song.is_playing else 0,
                   pos, float(song.master_track.mixer_device.volume.value))

    # ---- incoming (polled) --------------------------------------------------

    def _tick(self):
        try:
            while True:
                data = self._sock.recv(2048)
                if not data:
                    break
                self._handle(*osc_decode(data))
        except (BlockingIOError, socket.error):
            pass
        except Exception as e:
            self.log_message('Macchina tick error: %r' % e)

        # Meters + position move constantly; refresh them on the tick.
        song = self.song()
        self._send('/vu', float(song.master_track.output_meter_left),
                          float(song.master_track.output_meter_right))
        # Re-subscribe every ~3 s: harmless while connected (the daemon
        # ignores repeats from the same socket) and it means a restarted
        # daemon picks us back up without touching Live.
        self._resub_countdown -= 1
        if self._resub_countdown <= 0:
            self._resub_countdown = 30
            self._send('/macchina/subscribe')

        # Always send — the daemon change-detects, and this catches position
        # changes that fire no listener (Restart, clicking in the arrangement).
        self._send_hud_state()
        self._send_mix_state()
        if self._page == 'chan':
            self._send_chan_state()
        elif self._page == 'plug':
            self._send_plug_state()

        self.schedule_message(1, self._tick)   # ~100 ms cadence

    def _visible_tracks(self):
        # Live already folds groups (recursively) into is_visible — following
        # it means the mixer always mirrors what Live's own mixer shows.
        return [t for t in self.song().tracks if t.is_visible]

    def _track_depth(self, track):
        depth, g = 0, track.group_track
        while g is not None:
            depth, g = depth + 1, g.group_track
        return depth

    def _send_mix_state(self):
        # The 8 visible tracks in view. The daemon change-detects and
        # animates; we just stream at tick rate.
        tracks = self._visible_tracks()
        self._track_offset = min(self._track_offset, max(0, len(tracks) - 8))
        o = self._track_offset
        self._send('/mix/view', o, len(tracks))
        for i, track in enumerate(tracks[o:o + 8]):
            meter = 0.0
            try:
                if track.has_audio_output:
                    meter = max(track.output_meter_left, track.output_meter_right)
            except Exception:
                pass
            foldable = track.is_foldable
            self._send('/mix/track', i + 1, str(track.name)[:12],
                       float(track.mixer_device.volume.value), float(meter),
                       1 if track.mute else 0, 1 if track.solo else 0,
                       int(track.color),
                       1 if foldable else 0,
                       1 if (foldable and track.fold_state) else 0,
                       self._track_depth(track))

    def _scroll_tracks(self, direction):
        limit = max(0, len(self._visible_tracks()) - 8)
        new = max(0, min(limit, self._track_offset + direction))
        if new == self._track_offset:
            return
        self._track_offset = new
        # Arrow LEDs: lit only when there is more to scroll to.
        self._send('/led/name', 'left', 127 if new > 0 else 0)
        self._send('/led/name', 'right', 127 if new < limit else 0)
        self._show_highlight()
        self._send_mix_state()

    def _show_highlight(self):
        # Live's "red box" — shows which tracks the hardware has in view.
        # The highlight wants an absolute track index, our offset is visible-space.
        try:
            visible = self._visible_tracks()
            first = visible[min(self._track_offset, len(visible) - 1)]
            absolute = list(self.song().tracks).index(first)
            self._c_instance.set_session_highlight(absolute, 0, 8, 0, False)
        except Exception:
            pass

    # ---- channel / plug-in pages -------------------------------------------

    def _selected(self):
        track = self.song().view.selected_track
        device = track.view.selected_device
        if device is None and len(track.devices):
            device = track.devices[0]
        return track, device

    def _send_chan_state(self):
        track, selected = self._selected()
        self._send('/chan/state', str(track.name)[:20], int(track.color),
                   len(track.devices))
        for i, d in enumerate(track.devices[:8]):
            active = 1
            try:
                active = 1 if d.parameters[0].value else 0   # "Device On"
            except Exception:
                pass
            self._send('/chan/device', i + 1, str(d.name)[:16],
                       1 if d == selected else 0, active,
                       1 if d.class_name == 'Eq8' else 0)

    def _eq8_param(self, device, band, suffix):
        name = '%d %s A' % (band, suffix)
        for p in device.parameters:
            if p.name == name:
                return p
        return None

    def _norm(self, p):
        span = p.max - p.min
        return float((p.value - p.min) / span) if span else 0.0

    def _plug_limit(self, d):
        return max(0, len(d.parameters) - 1 - 8)   # [0] is Device On

    def _update_plug_arrows(self, d):
        if d is not None and d.class_name == 'Eq8':
            return   # on EQ8 the daemon owns the arrows (gain/freq/Q cycling)
        limit = 0 if d is None else self._plug_limit(d)
        self._send('/led/name', 'left', 127 if self._param_offset > 0 else 0)
        self._send('/led/name', 'right', 127 if self._param_offset < limit else 0)

    def _send_plug_state(self):
        track, d = self._selected()
        if d is not self._plug_dev:   # new device: param window back to start
            self._plug_dev = d
            self._param_offset = 0
            if self._page == 'plug':
                self._update_plug_arrows(d)
        if d is None:
            self._send('/plug/state', '(no device)', 0)
            return
        eq8 = d.class_name == 'Eq8'
        self._send('/plug/state', str(d.name)[:20], 1 if eq8 else 0)
        if eq8:
            for band in range(1, 9):
                on = self._eq8_param(d, band, 'Filter On')
                freq = self._eq8_param(d, band, 'Frequency')
                gain = self._eq8_param(d, band, 'Gain')
                q = self._eq8_param(d, band, 'Resonance')
                ftype = self._eq8_param(d, band, 'Filter Type')
                if on is None or freq is None or gain is None:
                    continue
                self._send('/plug/eq8', band, 1 if on.value else 0,
                           self._norm(freq), self._norm(gain),
                           self._norm(q) if q is not None else 0.3,
                           int(ftype.value) if ftype is not None else 3)
        else:
            o = self._param_offset
            self._send('/plug/view', o, len(d.parameters) - 1)
            for i, p in enumerate(d.parameters[1 + o:9 + o]):   # [0] is Device On
                try:
                    disp = str(p)
                except Exception:
                    disp = ''
                self._send('/plug/param', i + 1, str(p.name)[:12],
                           self._norm(p), disp[:9])

    def _nudge_param(self, p, delta):
        span = p.max - p.min
        p.value = max(p.min, min(p.max, p.value + delta * span))

    def _handle(self, addr, args):
        song = self.song()
        if addr == '/page' and args:
            self._page = args[0]
            if self._page == 'mix':
                limit = max(0, len(song.tracks) - 8)
                self._send('/led/name', 'left', 127 if self._track_offset > 0 else 0)
                self._send('/led/name', 'right', 127 if self._track_offset < limit else 0)
                self._show_highlight()
            elif self._page == 'plug':
                self._update_plug_arrows(self._selected()[1])
            else:
                self._send('/led/name', 'left', 0)
                self._send('/led/name', 'right', 0)
            return
        if addr == '/live/chan/device' and args:
            track, _ = self._selected()
            i = args[0]
            if 0 <= i < len(track.devices):
                self.song().view.select_device(track.devices[i])
            return
        if addr == '/live/plug/eq8_on' and args:
            _, d = self._selected()
            if d is not None and d.class_name == 'Eq8':
                p = self._eq8_param(d, args[0] + 1, 'Filter On')
                if p is not None:
                    p.value = 0 if p.value else 1
            return
        if addr == '/event/knob' and len(args) >= 2 and self._page == 'plug':
            _, d = self._selected()
            if d is None:
                return
            k, delta = args[0] - 1, args[1]
            if d.class_name == 'Eq8':
                p = self._eq8_param(d, k + 1, 'Gain')
                if p is not None:
                    self._nudge_param(p, delta)
            else:
                params = d.parameters[1 + self._param_offset:9 + self._param_offset]
                if 0 <= k < len(params):
                    self._nudge_param(params[k], delta)
            return
        if addr in ('/live/track/mute', '/live/track/solo', '/live/track/fold') and args:
            # Dumb executors: the daemon owns the gestures, we just toggle.
            tracks = self._visible_tracks()
            i = args[0]
            if 0 <= i < len(tracks):
                t = tracks[i]
                if addr.endswith('mute'):
                    t.mute = not t.mute
                elif addr.endswith('solo'):
                    t.solo = not t.solo
                elif t.is_foldable:
                    t.fold_state = not t.fold_state
            return
        if addr == '/live/plug/eq8_nudge' and len(args) >= 3:
            _, d = self._selected()
            if d is not None and d.class_name == 'Eq8':
                suffix = {'gain': 'Gain', 'freq': 'Frequency', 'q': 'Resonance'}.get(args[1])
                p = self._eq8_param(d, args[0], suffix) if suffix else None
                if p is not None:
                    self._nudge_param(p, args[2])
            return
        if addr == '/event/knob' and len(args) >= 2 and self._page == 'mix':
            k = self._track_offset + args[0] - 1
            tracks = self._visible_tracks()
            if self._track_offset <= k < min(self._track_offset + 8, len(tracks)):
                vol = tracks[k].mixer_device.volume
                vol.value = max(0.0, min(1.0, vol.value + args[1]))
            return
        if addr == '/event/button' and len(args) >= 2:
            name, down = args[0], args[1]
            if not down:
                if name == 'wheel':
                    self._wheel_pushed = False
                return
            if name == 'left' and self._page == 'mix':
                self._scroll_tracks(-1)
            elif name == 'right' and self._page == 'mix':
                self._scroll_tracks(+1)
            elif name in ('left', 'right') and self._page == 'plug':
                _, d = self._selected()
                if d is not None and d.class_name != 'Eq8':
                    limit = self._plug_limit(d)
                    self._param_offset = max(0, min(limit,
                        self._param_offset + (1 if name == 'right' else -1)))
                    self._update_plug_arrows(d)
            elif name == 'transport_play':
                song.is_playing = not song.is_playing
            elif name == 'transport_record':
                song.session_record = not song.session_record
            elif name == 'transport_loop':      # the RESTART button
                song.current_song_time = 0.0
            elif name == 'transport_metro':
                song.metronome = not song.metronome
            elif name == 'wheel':
                self._wheel_pushed = True
        elif addr == '/event/master' and args:
            vol = song.master_track.mixer_device.volume
            vol.value = max(0.0, min(1.0, vol.value + args[0]))
        elif addr == '/event/wheel' and args:
            step = args[0] * (0.1 if self._wheel_pushed else 1.0)
            song.tempo = max(20.0, min(999.0, song.tempo + step))
