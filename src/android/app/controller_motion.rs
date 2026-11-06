//! A game controller's motion sensors (gyro and accelerometer) as the emulated Steam Deck
//! controller's IMU. `ControllerMotion.java` reads them through Android's controller sensor API
//! (`InputDevice.getSensorManager()`, Android 12+) while Steam wants motion, and hands each reading
//! here; this writes the IMU block of the ring of the controller's slot (see `controller_slots`),
//! which libfakeinput puts into the reports of the controller that slot presents.
//!
//! The IMU block follows the ring's 64-byte header and 4096 events (see `localdesktop-pad` and
//! libfakeinput's `DeckImu`, little-endian): magic, the IMU mode Steam last set (top bit: set),
//! a sequence counter that is odd while the block changes, then accel[3] and gyro[3] as i16.
//! Only those fields are written here; the Deck's trackpad and extra-button fields stay as they are.

use crate::android::app::controller_slots;
use jni::objects::JClass;
use jni::sys::{jboolean, jfloat, jint, JNI_FALSE, JNI_TRUE};
use jni::JNIEnv;
use std::fs::File;
use std::os::unix::fs::FileExt;
use std::sync::Mutex;

const IMU: u64 = controller_slots::IMU;
const IMU_MAGIC: u32 = 0x3155_4D49;
const IMU_MODE: u64 = IMU + 4;
const IMU_SEQ: u64 = IMU + 8;
const IMU_ACCEL: u64 = IMU + 16;
const IMU_MODE_SET: u32 = 0x8000_0000;

/// What `ControllerMotion.java` passes as a reading's kind.
const ACCELEROMETER: jint = 0;
const GYROSCOPE: jint = 1;

/// The Deck's IMU units, as DroidDeck's PadMotion uses them: ±2 g over ±32768 for the
/// accelerometer and ±2000 °/s for the gyro.
const ACCEL_COUNTS_PER_M_S2: f32 = 16384.0 / 9.80665;
const GYRO_COUNTS_PER_RAD_S: f32 = (32768.0 / 2000.0) * (180.0 / std::f32::consts::PI);

/// The last reading of the controller being read.
struct Motion {
    accel: [i16; 3],
    gyro: [i16; 3],
}

static MOTION: Mutex<Motion> = Mutex::new(Motion { accel: [0; 3], gyro: [0; 3] });

fn read_u32(file: &File, offset: u64) -> Option<u32> {
    let mut word = [0u8; 4];
    file.read_exact_at(&mut word, offset).ok()?;
    Some(u32::from_le_bytes(word))
}

/// Writes accel and gyro under the IMU block's sequence counter, as DroidDeck's FakeInputWriter does.
fn publish(ring: &File, accel: [i16; 3], gyro: [i16; 3]) {
    let mut word = [0u8; 8];
    if ring.read_exact_at(&mut word, IMU_SEQ).is_err() {
        return;
    }
    let seq = u64::from_le_bytes(word) & !1;
    let mut values = Vec::with_capacity(12);
    for value in accel.iter().chain(gyro.iter()) {
        values.extend_from_slice(&value.to_le_bytes());
    }
    let _ = ring.write_all_at(&(seq + 1).to_le_bytes(), IMU_SEQ);
    let _ = ring.write_all_at(&values, IMU_ACCEL);
    let _ = ring.write_all_at(&IMU_MAGIC.to_le_bytes(), IMU);
    let _ = ring.write_all_at(&(seq + 2).to_le_bytes(), IMU_SEQ);
}

fn counts(value: f32) -> i16 {
    value.round().clamp(-32768.0, 32767.0) as i16
}

/// A reading in a controller's sensor axes to the Deck's. Android passes a controller's sensors on
/// in the convention the Linux drivers (hid-sony, hid-playstation, hid-nintendo) and SDL share:
/// x to the right, y up, z toward the player. The Deck's report has x, -z and y in those places
/// (SDL's Steam Deck driver undoes this), the mapping DroidDeck uses for a phone held like a Deck.
/// A controller is not turned with the screen, so no display rotation applies.
fn to_deck(x: f32, y: f32, z: f32, scale: f32) -> [i16; 3] {
    [counts(x * scale), counts(-z * scale), counts(y * scale)]
}

/// Whether Steam wants motion from a controller: the mode Steam set on the controller its slot
/// presents (top bit set means Steam has set it), with an unset mode counting as wanted, as
/// DroidDeck does.
fn wanted(device: i32) -> bool {
    let slot = controller_slots::slot_of(device);
    match controller_slots::with_ring(slot, |ring| read_u32(ring, IMU_MODE)).flatten() {
        Some(mode) if mode & IMU_MODE_SET != 0 => mode & 0xffff != 0,
        Some(_) => true,
        None => false,
    }
}

#[no_mangle]
pub extern "system" fn Java_io_github_localdesktop_ControllerMotion_nativeMotionWanted(
    _env: JNIEnv,
    _class: JClass,
    device: jint,
) -> jboolean {
    if wanted(device) {
        JNI_TRUE
    } else {
        JNI_FALSE
    }
}

#[no_mangle]
pub extern "system" fn Java_io_github_localdesktop_ControllerMotion_nativeMotion(
    _env: JNIEnv,
    _class: JClass,
    device: jint,
    kind: jint,
    x: jfloat,
    y: jfloat,
    z: jfloat,
) {
    let mut motion = MOTION.lock().unwrap();
    match kind {
        ACCELEROMETER => motion.accel = to_deck(x, y, z, ACCEL_COUNTS_PER_M_S2),
        GYROSCOPE => motion.gyro = to_deck(x, y, z, GYRO_COUNTS_PER_RAD_S),
        _ => return,
    }
    let (accel, gyro) = (motion.accel, motion.gyro);
    controller_slots::with_ring(controller_slots::slot_of(device), |ring| publish(ring, accel, gyro));
}

/// The controller stopped reporting (Steam wants no motion, or it went away): hold the gyro still.
#[no_mangle]
pub extern "system" fn Java_io_github_localdesktop_ControllerMotion_nativeMotionStopped(
    _env: JNIEnv,
    _class: JClass,
    device: jint,
) {
    let mut motion = MOTION.lock().unwrap();
    motion.gyro = [0; 3];
    let (accel, gyro) = (motion.accel, motion.gyro);
    controller_slots::with_ring(controller_slots::slot_of(device), |ring| publish(ring, accel, gyro));
}
