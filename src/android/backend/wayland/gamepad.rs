//! The on-screen gamepad: Steam Deck controls drawn over Game Mode, whose touches drive the
//! emulated Steam Deck controller (`/usr/local/bin/localdesktop-pad` in the guest, served to
//! Steam by libfakeinput) instead of reaching the session.
//!
//! It is shown only while Game Mode runs: `localdesktop-gamemode` writes gamescope's pid to
//! `/run/localdesktop-pad/gamemode`. A toggle at the top of the screen hides and shows the
//! controls (touches then go to the game), and the choice is kept in
//! `/root/.config/localdesktop/gamepad-hidden`. Controls are circles laid out from the window
//! size, drawn once per size with tiny-skia and Android's Roboto, and uploaded as textures.
//!
//! Off unless `/root/.config/localdesktop/gamepad-enabled` exists (while it is unfinished: it was
//! drawn on some frames only and its controls then took touches invisibly).

use crate::core::config::ARCH_FS_ROOT;
use smithay::backend::allocator::Fourcc;
use smithay::backend::renderer::element::texture::{TextureBuffer, TextureRenderElement};
use smithay::backend::renderer::element::Kind;
use smithay::backend::renderer::gles::{GlesRenderer, GlesTexture};
use smithay::utils::Transform;
use std::collections::HashMap;
use std::fs::File;
use std::os::unix::fs::FileExt;
use std::path::PathBuf;
use std::time::{Duration, Instant};
use winit::event::{Touch, TouchPhase};

/// The ring's header fields (see `localdesktop-pad`): a seqlocked snapshot of 12 button bits
/// and 8 axes, little-endian.
const SNAPSHOT_SEQ: u64 = 32;
const SNAPSHOT_BUTTONS: u64 = 40;
const RING_MAGIC: u32 = 0x4642_4952;

// Snapshot button bits (libfakeinput's kSnapshotButtons, Quick Access at 11).
const A: u32 = 0;
const B: u32 = 1;
const X: u32 = 2;
const Y: u32 = 3;
const L1: u32 = 4;
const R1: u32 = 5;
const VIEW: u32 = 6;
const MENU: u32 = 7;
const L3: u32 = 8;
const R3: u32 = 9;
const STEAM: u32 = 10;
const QAM: u32 = 11;

// Snapshot axes: sticks (evdev signs: y grows downward), right then left trigger, d-pad.
const LX: usize = 0;
const LY: usize = 1;
const RX: usize = 2;
const RY: usize = 3;
const RT: usize = 4;
const LT: usize = 5;
const HX: usize = 6;
const HY: usize = 7;

#[derive(Clone, Copy)]
enum Action {
    Button(u32),
    Trigger(usize),
    /// A d-pad direction: (x, y) of the hat.
    Hat(i16, i16),
    /// A stick: its x and y axes and the click button (a double tap).
    Stick(usize, usize, u32),
    Toggle,
}

struct Control {
    action: Action,
    label: &'static str,
    /// Centre and radius in window pixels.
    x: f64,
    y: f64,
    r: f64,
}

struct Textures {
    /// Per control: released and pressed.
    faces: Vec<(TextureBuffer<GlesTexture>, TextureBuffer<GlesTexture>)>,
    /// A stick's knob.
    knob: TextureBuffer<GlesTexture>,
}

struct Held {
    control: usize,
    /// Where a stick's finger went down, so the knob moves from there rather than jumping.
    origin: (f64, f64),
}

pub struct Gamepad {
    size: (i32, i32),
    controls: Vec<Control>,
    textures: Option<Textures>,
    held: HashMap<u64, Held>,
    buttons: u32,
    axes: [i16; 8],
    /// Stick offsets for drawing the knobs, in [-1, 1].
    sticks: HashMap<usize, (f64, f64)>,
    hidden: bool,
    game_mode: bool,
    checked: Option<Instant>,
    ring: Option<File>,
    font: Option<ab_glyph::FontVec>,
    last_stick_tap: HashMap<usize, Instant>,
}

fn guest(path: &str) -> PathBuf {
    PathBuf::from(ARCH_FS_ROOT).join(path.trim_start_matches('/'))
}

impl Gamepad {
    pub fn new() -> Self {
        Gamepad {
            size: (0, 0),
            controls: Vec::new(),
            textures: None,
            held: HashMap::new(),
            buttons: 0,
            axes: [0; 8],
            sticks: HashMap::new(),
            hidden: guest("/root/.config/localdesktop/gamepad-hidden").exists(),
            game_mode: false,
            checked: None,
            ring: None,
            font: None,
            last_stick_tap: HashMap::new(),
        }
    }

    /// Whether Game Mode runs: the pid in the flag file is alive. Checked at most twice a second.
    fn refresh_game_mode(&mut self) {
        if self.checked.is_some_and(|at| at.elapsed() < Duration::from_millis(500)) {
            return;
        }
        self.checked = Some(Instant::now());
        if !guest("/root/.config/localdesktop/gamepad-enabled").exists() {
            if self.game_mode {
                self.game_mode = false;
                self.release_all();
            }
            return;
        }
        let flag = std::fs::read_to_string(guest("/run/localdesktop-pad/gamemode"));
        let pid = flag.as_ref().ok().and_then(|pid| pid.trim().parse::<u32>().ok());
        let running = pid.is_some_and(|pid| std::path::Path::new(&format!("/proc/{pid}")).exists());
        if running != self.game_mode {
            log::info!(
                "gamepad: Game Mode {} (flag {:?}, pid {pid:?})",
                if running { "started" } else { "ended" },
                flag.as_ref().map(|text| text.trim().to_string()).map_err(|error| error.to_string())
            );
            self.game_mode = running;
            if !running {
                self.release_all();
            }
        }
    }

    fn lay_out(&mut self, w: i32, h: i32) {
        self.size = (w, h);
        self.textures = None;
        let (w, h) = (w as f64, h as f64);
        let u = w.min(h);
        let c = |action, label, fx: f64, fy: f64, r: f64| Control { action, label, x: fx * w, y: fy * h, r: r * u };
        let face = 0.052;
        let (ax, ay, gap) = (0.86, 0.42, 0.095 * u / h);
        let gapx = 0.095 * u / w;
        let (dx, dy) = (0.14, 0.42);
        self.controls = vec![
            c(Action::Toggle, "PAD", 0.5, 0.035, 0.032),
            c(Action::Trigger(LT), "L2", 0.06, 0.10, 0.045),
            c(Action::Button(L1), "L1", 0.06, 0.21, 0.045),
            c(Action::Trigger(RT), "R2", 0.94, 0.10, 0.045),
            c(Action::Button(R1), "R1", 0.94, 0.21, 0.045),
            c(Action::Button(Y), "Y", ax, ay - gap, face),
            c(Action::Button(A), "A", ax, ay + gap, face),
            c(Action::Button(X), "X", ax - gapx, ay, face),
            c(Action::Button(B), "B", ax + gapx, ay, face),
            c(Action::Hat(0, -1), "up", dx, dy - gap, face),
            c(Action::Hat(0, 1), "down", dx, dy + gap, face),
            c(Action::Hat(-1, 0), "left", dx - gapx, dy, face),
            c(Action::Hat(1, 0), "right", dx + gapx, dy, face),
            c(Action::Stick(LX, LY, L3), "", 0.20, 0.76, 0.12),
            c(Action::Stick(RX, RY, R3), "", 0.80, 0.76, 0.12),
            c(Action::Button(VIEW), "VIEW", 0.39, 0.86, 0.036),
            c(Action::Button(STEAM), "STEAM", 0.465, 0.86, 0.036),
            c(Action::Button(QAM), "•••", 0.535, 0.86, 0.036),
            c(Action::Button(MENU), "MENU", 0.61, 0.86, 0.036),
        ];
    }

    fn visible(&self) -> bool {
        self.game_mode && !self.hidden
    }

    /// Takes a touch that lands on, or began on, a control. Returns whether it was taken (the
    /// session must not see it).
    pub fn claim(&mut self, touch: &Touch, window: (i32, i32)) -> bool {
        self.refresh_game_mode();
        if !self.game_mode {
            return false;
        }
        if window != self.size {
            self.lay_out(window.0, window.1);
        }
        let (x, y) = (touch.location.x, touch.location.y);
        match touch.phase {
            TouchPhase::Started => {
                let Some(index) = self.hit(x, y) else {
                    return false;
                };
                self.held.insert(touch.id, Held { control: index, origin: (x, y) });
                self.press(index, x, y, (x, y));
            }
            TouchPhase::Moved => {
                let Some(held) = self.held.get(&touch.id) else {
                    return false;
                };
                let (index, origin) = (held.control, held.origin);
                self.press(index, x, y, origin);
            }
            TouchPhase::Ended | TouchPhase::Cancelled => {
                let Some(held) = self.held.remove(&touch.id) else {
                    return false;
                };
                self.release(held.control);
            }
        }
        self.publish();
        true
    }

    fn hit(&self, x: f64, y: f64) -> Option<usize> {
        self.controls.iter().position(|control| {
            let visible = self.visible() || matches!(control.action, Action::Toggle);
            visible && (x - control.x).hypot(y - control.y) <= control.r * 1.2
        })
    }

    fn press(&mut self, index: usize, x: f64, y: f64, origin: (f64, f64)) {
        let control = &self.controls[index];
        match control.action {
            Action::Button(bit) => self.buttons |= 1 << bit,
            Action::Trigger(axis) => self.axes[axis] = 255,
            Action::Hat(hx, hy) => {
                self.axes[HX] = hx;
                self.axes[HY] = hy;
            }
            Action::Stick(ax, ay, click) => {
                if (x, y) == origin {
                    let now = Instant::now();
                    if self.last_stick_tap.insert(index, now).is_some_and(|last| now - last < Duration::from_millis(300)) {
                        self.buttons |= 1 << click;
                    }
                }
                let reach = control.r * 0.8;
                let (mut sx, mut sy) = ((x - origin.0) / reach, (y - origin.1) / reach);
                let length = sx.hypot(sy);
                if length > 1.0 {
                    sx /= length;
                    sy /= length;
                }
                self.axes[ax] = (sx * 32767.0) as i16;
                self.axes[ay] = (sy * 32767.0) as i16;
                self.sticks.insert(index, (sx, sy));
            }
            Action::Toggle => {}
        }
    }

    fn release(&mut self, index: usize) {
        match self.controls[index].action {
            Action::Button(bit) => self.buttons &= !(1 << bit),
            Action::Trigger(axis) => self.axes[axis] = 0,
            Action::Hat(..) => {
                self.axes[HX] = 0;
                self.axes[HY] = 0;
            }
            Action::Stick(ax, ay, click) => {
                self.axes[ax] = 0;
                self.axes[ay] = 0;
                self.buttons &= !(1 << click);
                self.sticks.remove(&index);
            }
            Action::Toggle => {
                self.hidden = !self.hidden;
                let flag = guest("/root/.config/localdesktop/gamepad-hidden");
                let saved = if self.hidden {
                    std::fs::create_dir_all(flag.parent().unwrap()).and_then(|_| std::fs::write(&flag, ""))
                } else {
                    std::fs::remove_file(&flag).or(Ok(()))
                };
                if let Err(error) = saved {
                    log::warn!("gamepad: could not save the toggle: {error}");
                }
                if self.hidden {
                    self.release_all();
                }
            }
        }
    }

    fn release_all(&mut self) {
        let toggles: Vec<u64> = self
            .held
            .iter()
            .filter(|(_, held)| matches!(self.controls[held.control].action, Action::Toggle))
            .map(|(id, _)| *id)
            .collect();
        self.held.retain(|id, _| toggles.contains(id));
        self.buttons = 0;
        self.axes = [0; 8];
        self.sticks.clear();
        self.publish();
    }

    /// Writes the state into the ring's snapshot, under its seqlock (odd while it changes).
    fn publish(&mut self) {
        if self.ring.is_none() {
            let path = guest("/run/localdesktop-pad/fakeinput-rings/ring0");
            match std::fs::OpenOptions::new().read(true).write(true).open(&path) {
                Ok(file) => self.ring = Some(file),
                Err(error) => {
                    log::warn!("gamepad: {} not available: {error}", path.display());
                    return;
                }
            }
        }
        let ring = self.ring.as_ref().unwrap();
        let mut word = [0u8; 8];
        if ring.read_at(&mut word[..4], 0).is_err() || u32::from_le_bytes(word[..4].try_into().unwrap()) != RING_MAGIC {
            self.ring = None;
            return;
        }
        let _ = ring.read_at(&mut word, SNAPSHOT_SEQ);
        let seq = u64::from_le_bytes(word);
        let mut state = Vec::with_capacity(20);
        state.extend_from_slice(&self.buttons.to_le_bytes());
        for axis in self.axes {
            state.extend_from_slice(&axis.to_le_bytes());
        }
        let _ = ring.write_at(&(seq + 1).to_le_bytes(), SNAPSHOT_SEQ);
        let _ = ring.write_at(&state, SNAPSHOT_BUTTONS);
        let _ = ring.write_at(&(seq + 2).to_le_bytes(), SNAPSHOT_SEQ);
    }

    fn pressed(&self, index: usize) -> bool {
        match self.controls[index].action {
            Action::Button(bit) => self.buttons & (1 << bit) != 0,
            Action::Trigger(axis) => self.axes[axis] != 0,
            Action::Hat(hx, hy) => self.axes[HX] == hx && self.axes[HY] == hy,
            Action::Stick(..) => self.sticks.contains_key(&index),
            Action::Toggle => !self.hidden,
        }
    }

    /// The controls to draw over the session this frame (topmost first).
    pub fn elements(&mut self, renderer: &mut GlesRenderer, window: (i32, i32)) -> Vec<TextureRenderElement<GlesTexture>> {
        self.refresh_game_mode();
        if !self.game_mode {
            return Vec::new();
        }
        if window != self.size {
            self.lay_out(window.0, window.1);
        }
        if self.textures.is_none() {
            match self.draw_textures(renderer) {
                Ok(textures) => self.textures = Some(textures),
                Err(error) => {
                    log::warn!("gamepad: could not draw the controls: {error}");
                    return Vec::new();
                }
            }
        }
        let textures = self.textures.as_ref().unwrap();
        let mut elements = Vec::new();
        for (index, control) in self.controls.iter().enumerate() {
            if !self.visible() && !matches!(control.action, Action::Toggle) {
                continue;
            }
            if let Action::Stick(..) = control.action {
                let (sx, sy) = self.sticks.get(&index).copied().unwrap_or((0.0, 0.0));
                let knob = control.r * 0.45;
                let at = (control.x + sx * control.r * 0.55 - knob, control.y + sy * control.r * 0.55 - knob);
                elements.push(TextureRenderElement::from_texture_buffer(at, &textures.knob, None, None, None, Kind::Unspecified));
            }
            let (released, pressed) = &textures.faces[index];
            let face = if self.pressed(index) { pressed } else { released };
            let at = (control.x - control.r, control.y - control.r);
            elements.push(TextureRenderElement::from_texture_buffer(at, face, None, None, None, Kind::Unspecified));
        }
        elements
    }

    fn draw_textures(&mut self, renderer: &mut GlesRenderer) -> Result<Textures, String> {
        if self.font.is_none() {
            let data = std::fs::read("/system/fonts/Roboto-Regular.ttf").map_err(|error| error.to_string())?;
            self.font = Some(ab_glyph::FontVec::try_from_vec(data).map_err(|error| error.to_string())?);
        }
        let font = self.font.as_ref().unwrap();
        let mut faces = Vec::new();
        for control in &self.controls {
            let label = if matches!(control.action, Action::Stick(..)) { "" } else { control.label };
            let released = upload(renderer, &circle(control.r, 0.16, 0.55, label, font))?;
            let pressed = upload(renderer, &circle(control.r, 0.50, 0.90, label, font))?;
            faces.push((released, pressed));
        }
        let knob_r = self.controls.iter().find(|c| matches!(c.action, Action::Stick(..))).map_or(40.0, |c| c.r * 0.45);
        let knob = upload(renderer, &circle(knob_r, 0.40, 0.85, "", font))?;
        Ok(Textures { faces, knob })
    }
}

/// A circle of radius `r`: white fill at `fill` opacity, a ring at `edge` opacity, and a label.
fn circle(r: f64, fill: f32, edge: f32, label: &str, font: &ab_glyph::FontVec) -> tiny_skia::Pixmap {
    let side = (r * 2.0).ceil().max(2.0) as u32;
    let mut pixmap = tiny_skia::Pixmap::new(side, side).unwrap();
    let centre = side as f32 / 2.0;
    let stroke = (r as f32 * 0.07).max(2.0);
    if let Some(path) = tiny_skia::PathBuilder::from_circle(centre, centre, centre - stroke) {
        let mut paint = tiny_skia::Paint::default();
        paint.anti_alias = true;
        paint.set_color(tiny_skia::Color::from_rgba(1.0, 1.0, 1.0, fill).unwrap());
        pixmap.fill_path(&path, &paint, tiny_skia::FillRule::Winding, tiny_skia::Transform::identity(), None);
        paint.set_color(tiny_skia::Color::from_rgba(1.0, 1.0, 1.0, edge).unwrap());
        let line = tiny_skia::Stroke { width: stroke, ..Default::default() };
        pixmap.stroke_path(&path, &paint, &line, tiny_skia::Transform::identity(), None);
    }
    match label {
        "" => {}
        "up" | "down" | "left" | "right" => draw_arrow(&mut pixmap, label, r as f32),
        _ => draw_label(&mut pixmap, label, r as f32, font),
    }
    pixmap
}

/// A d-pad arrow pointing `direction`, centred in the pixmap.
fn draw_arrow(pixmap: &mut tiny_skia::Pixmap, direction: &str, r: f32) {
    let c = pixmap.width() as f32 / 2.0;
    let (tip, half) = (r * 0.42, r * 0.36);
    // Points of an upward triangle, rotated to the direction.
    let points = [(0.0, -tip), (-half, tip * 0.6), (half, tip * 0.6)];
    let turn = |(x, y): (f32, f32)| match direction {
        "down" => (-x, -y),
        "left" => (y, -x),
        "right" => (-y, x),
        _ => (x, y),
    };
    let mut path = tiny_skia::PathBuilder::new();
    for (i, point) in points.into_iter().map(turn).enumerate() {
        if i == 0 {
            path.move_to(c + point.0, c + point.1);
        } else {
            path.line_to(c + point.0, c + point.1);
        }
    }
    path.close();
    if let Some(path) = path.finish() {
        let mut paint = tiny_skia::Paint::default();
        paint.anti_alias = true;
        paint.set_color(tiny_skia::Color::from_rgba(1.0, 1.0, 1.0, 0.95).unwrap());
        pixmap.fill_path(&path, &paint, tiny_skia::FillRule::Winding, tiny_skia::Transform::identity(), None);
    }
}

/// Centres `label` in the pixmap, sized to fit inside a circle of radius `r`.
fn draw_label(pixmap: &mut tiny_skia::Pixmap, label: &str, r: f32, font: &ab_glyph::FontVec) {
    use ab_glyph::{Font, ScaleFont};
    let mut px = r * if label.chars().count() > 2 { 0.55 } else { 0.9 };
    let width = |px: f32| {
        let scaled = font.as_scaled(px);
        label.chars().map(|c| scaled.h_advance(font.glyph_id(c))).sum::<f32>()
    };
    while width(px) > r * 1.6 && px > 4.0 {
        px *= 0.9;
    }
    let scaled = font.as_scaled(px);
    let side = pixmap.width() as f32;
    let mut pen = (side - width(px)) / 2.0;
    let baseline = side / 2.0 + (scaled.ascent() + scaled.descent()) / 2.0;
    let stride = pixmap.width() as usize;
    let data = pixmap.data_mut();
    for c in label.chars() {
        let id = font.glyph_id(c);
        let glyph = id.with_scale_and_position(px, ab_glyph::point(pen, baseline));
        pen += scaled.h_advance(id);
        let Some(outline) = font.outline_glyph(glyph) else { continue };
        let bounds = outline.px_bounds();
        outline.draw(|gx, gy, coverage| {
            let (x, y) = (bounds.min.x as i32 + gx as i32, bounds.min.y as i32 + gy as i32);
            if x < 0 || y < 0 || x as usize >= stride || y as f32 >= side {
                return;
            }
            let at = (y as usize * stride + x as usize) * 4;
            // Premultiplied white over what is there.
            let a = (coverage.min(1.0) * 255.0) as u32;
            for channel in 0..4 {
                let old = data[at + channel] as u32;
                data[at + channel] = (a + old * (255 - a) / 255) as u8;
            }
        });
    }
}

fn upload(renderer: &mut GlesRenderer, pixmap: &tiny_skia::Pixmap) -> Result<TextureBuffer<GlesTexture>, String> {
    // tiny-skia's premultiplied RGBA is Abgr8888 in DRM's little-endian naming.
    TextureBuffer::from_memory(
        renderer,
        pixmap.data(),
        Fourcc::Abgr8888,
        (pixmap.width() as i32, pixmap.height() as i32),
        false,
        1,
        Transform::Normal,
        None,
    )
    .map_err(|error| format!("{error:?}"))
}
