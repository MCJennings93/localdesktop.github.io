//! Which controller drives which ring. The guest has one pad ring per slot
//! (`/run/localdesktop-pad/fakeinput-rings/ringN`, made by `localdesktop-pad setup`): slot 0 is the
//! on-screen gamepad's Steam Deck controller, which controllers without a slot of their own also
//! drive; slots 1 to 3 each carry one physical controller, which libfakeinput presents to Steam as
//! itself (as the pad type written into its ring) on `/dev/hidraw33` to `35`, so each is a player
//! of its own with its own glyphs and layouts. Today a Nintendo controller gets a slot, as a Switch
//! Pro Controller.
//!
//! Steam sees slot 0's Steam Deck controller only while it is in use: while the on-screen gamepad is
//! shown (`set_on_screen`) or a controller without a slot of its own is connected. Otherwise slot 0's
//! pad type is "none" and libfakeinput takes the Deck away, so Steam lists only real controllers.
//!
//! `ControllerMotion.java` reports controllers connecting and going away (`nativeControllerAdded`,
//! `nativeControllerRemoved`) and asks twice a second for the slots to be written again
//! (`nativeControllersRefresh`), which puts them back into the new rings of a new session.

use crate::core::config::arch_fs_root;
use jni::objects::JClass;
use jni::sys::jint;
use jni::JNIEnv;
use std::fs::File;
use std::os::unix::fs::FileExt;
use std::path::PathBuf;
use std::sync::Mutex;

pub const SLOTS: usize = 4;
const RING_MAGIC: u32 = 0x4649_4252;
const SNAPSHOT_SEQ: u64 = 32;
const SNAPSHOT_BUTTONS: u64 = 40;
/// libfakeinput's `DeckImu::pad_type`, in the IMU block after the 64-byte header and 4096 events.
pub const IMU: u64 = 64 + 4096 * 24;
const PAD_TYPE: u64 = IMU + 44;
const PAD_TYPE_SET: u32 = 0x8000_0000;
const PAD_TYPE_DECK: u32 = 0;
const PAD_TYPE_SWITCH_PRO: u32 = 5;
/// libfakeinput's PAD_TYPE_NONE: no controller on the slot.
const PAD_TYPE_NONE: u32 = 0xff;
const NINTENDO: i32 = 0x057e;

struct Slot {
    device: i32,
    pad_type: u32,
}

struct Slots {
    slots: [Option<Slot>; SLOTS],
    rings: [Option<File>; SLOTS],
    /// Connected controllers that drive slot 0 (none of their own).
    on_deck: Vec<i32>,
    /// Whether the on-screen gamepad is shown.
    on_screen: bool,
}

static SLOT_TABLE: Mutex<Slots> = Mutex::new(Slots {
    slots: [None, None, None, None],
    rings: [None, None, None, None],
    on_deck: Vec::new(),
    on_screen: false,
});

pub fn ring_path(slot: usize) -> PathBuf {
    PathBuf::from(arch_fs_root()).join(format!("run/localdesktop-pad/fakeinput-rings/ring{slot}"))
}

fn read_u32(file: &File, offset: u64) -> Option<u32> {
    let mut word = [0u8; 4];
    file.read_exact_at(&mut word, offset).ok()?;
    Some(u32::from_le_bytes(word))
}

impl Slots {
    /// A slot's ring, opened again whenever `localdesktop-pad setup` has made a new one.
    fn ring(&mut self, slot: usize) -> Option<&File> {
        let valid = |file: &File| read_u32(file, 0) == Some(RING_MAGIC);
        if !self.rings[slot].as_ref().is_some_and(valid) {
            self.rings[slot] = std::fs::OpenOptions::new().read(true).write(true).open(ring_path(slot)).ok();
        }
        self.rings[slot].as_ref().filter(|file| valid(file))
    }

    /// Writes the slot's pad type (or none) into its ring, if it isn't there already.
    fn show(&mut self, slot: usize) {
        let want = if slot == 0 {
            let in_use = self.on_screen || !self.on_deck.is_empty();
            PAD_TYPE_SET | if in_use { PAD_TYPE_DECK } else { PAD_TYPE_NONE }
        } else {
            self.slots[slot].as_ref().map_or(0, |it| PAD_TYPE_SET | it.pad_type)
        };
        let Some(ring) = self.ring(slot) else { return };
        if read_u32(ring, PAD_TYPE) != Some(want) {
            if want == 0 || want == PAD_TYPE_SET | PAD_TYPE_NONE {
                write_snapshot(ring, 0, &[0; 8]);
            }
            let _ = ring.write_all_at(&want.to_le_bytes(), PAD_TYPE);
        }
    }
}

/// The pad type a controller is presented as on a slot of its own, or None to drive slot 0.
fn own_pad_type(vendor: i32) -> Option<u32> {
    (vendor == NINTENDO).then_some(PAD_TYPE_SWITCH_PRO)
}

/// The slot a controller drives (0 when it has none of its own).
pub fn slot_of(device: i32) -> usize {
    let table = SLOT_TABLE.lock().unwrap();
    (1..SLOTS)
        .find(|&slot| table.slots[slot].as_ref().is_some_and(|it| it.device == device))
        .unwrap_or(0)
}

/// Runs `write` on a slot's ring, if it exists.
pub fn with_ring<T>(slot: usize, write: impl FnOnce(&File) -> T) -> Option<T> {
    let mut table = SLOT_TABLE.lock().unwrap();
    table.ring(slot).map(write)
}

/// Writes a seqlocked snapshot (12 button bits, 8 axes) into a ring's header, as `localdesktop-pad`
/// lays it out.
pub fn write_snapshot(ring: &File, buttons: u32, axes: &[i16; 8]) {
    let mut word = [0u8; 8];
    if ring.read_exact_at(&mut word, SNAPSHOT_SEQ).is_err() {
        return;
    }
    let seq = u64::from_le_bytes(word) & !1;
    let mut state = Vec::with_capacity(20);
    state.extend_from_slice(&buttons.to_le_bytes());
    for axis in axes {
        state.extend_from_slice(&axis.to_le_bytes());
    }
    let _ = ring.write_all_at(&(seq + 1).to_le_bytes(), SNAPSHOT_SEQ);
    let _ = ring.write_all_at(&state, SNAPSHOT_BUTTONS);
    let _ = ring.write_all_at(&(seq + 2).to_le_bytes(), SNAPSHOT_SEQ);
}

fn added(device: i32, vendor: i32) {
    let mut table = SLOT_TABLE.lock().unwrap();
    if table.slots.iter().flatten().any(|it| it.device == device) || table.on_deck.contains(&device) {
        return;
    }
    let free = (1..SLOTS).find(|&slot| table.slots[slot].is_none());
    let (Some(pad_type), Some(slot)) = (own_pad_type(vendor), free) else {
        if own_pad_type(vendor).is_some() {
            log::warn!("controllers: no free slot for controller {device}; it drives the Steam Deck controller");
        }
        table.on_deck.push(device);
        table.show(0);
        log::info!("controllers: controller {device} (vendor {vendor:04x}) drives the Steam Deck controller");
        return;
    };
    table.slots[slot] = Some(Slot { device, pad_type });
    if let Some(ring) = table.ring(slot) {
        write_snapshot(ring, 0, &[0; 8]);
    }
    table.show(slot);
    log::info!("controllers: controller {device} (vendor {vendor:04x}) is slot {slot}");
}

fn removed(device: i32) {
    let mut table = SLOT_TABLE.lock().unwrap();
    if let Some(at) = table.on_deck.iter().position(|&it| it == device) {
        table.on_deck.remove(at);
        table.show(0);
        log::info!("controllers: controller {device} no longer drives the Steam Deck controller");
    }
    for slot in 1..SLOTS {
        if table.slots[slot].as_ref().is_some_and(|it| it.device == device) {
            table.slots[slot] = None;
            table.show(slot);
            log::info!("controllers: controller {device} left slot {slot}");
        }
    }
}

#[no_mangle]
pub extern "system" fn Java_io_github_localdesktop_ControllerMotion_nativeControllerAdded(
    _env: JNIEnv,
    _class: JClass,
    device: jint,
    vendor: jint,
) {
    added(device, vendor);
}

#[no_mangle]
pub extern "system" fn Java_io_github_localdesktop_ControllerMotion_nativeControllerRemoved(
    _env: JNIEnv,
    _class: JClass,
    device: jint,
) {
    removed(device);
}

#[no_mangle]
pub extern "system" fn Java_io_github_localdesktop_ControllerMotion_nativeControllersRefresh(
    _env: JNIEnv,
    _class: JClass,
) {
    let mut table = SLOT_TABLE.lock().unwrap();
    for slot in 0..SLOTS {
        table.show(slot);
    }
}

/// Whether the on-screen gamepad is shown: Steam then sees the Steam Deck controller it drives.
pub fn set_on_screen(shown: bool) {
    let mut table = SLOT_TABLE.lock().unwrap();
    if table.on_screen != shown {
        table.on_screen = shown;
        table.show(0);
        log::info!("controllers: on-screen gamepad {}", if shown { "shown" } else { "hidden" });
    }
}
