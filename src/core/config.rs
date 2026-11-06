use serde::Deserialize;
use std::{
    fs::{self, OpenOptions},
    io::Write,
    path::Path,
    sync::OnceLock,
};

pub const VERSION: &str = env!("CARGO_PKG_VERSION");

static ARCH_FS_ROOT: OnceLock<String> = OnceLock::new();

/// Decide where the Arch Linux FS lives, from the app's files dir (`Context.getFilesDir()`).
///
/// `/data/data/<package>` only points at the owner's (user 0) storage, so in a secondary user
/// or Private Space the FS must live under the files dir Android gives that user instead.
/// When `/data/data/<package>/files` is this user's files dir, keep that historical path as is,
/// because PRoot bakes absolute host paths into the FS (e.g. link2symlink).
pub fn init_arch_fs_root(files_dir: &Path) {
    let root = files_dir
        .parent()
        .and_then(|app_dir| app_dir.file_name())
        .map(|package| Path::new("/data/data").join(package).join("files"))
        .filter(|legacy_files_dir| {
            match (
                fs::canonicalize(legacy_files_dir),
                fs::canonicalize(files_dir),
            ) {
                (Ok(legacy), Ok(current)) => legacy == current,
                _ => false,
            }
        })
        .unwrap_or_else(|| files_dir.to_path_buf())
        .join("arch");
    // Only the first call wins, the FS must not move while the app is running
    let _ = ARCH_FS_ROOT.set(root.to_string_lossy().into_owned());
}

/// Absolute host path of the Arch Linux FS, set once at startup by [`init_arch_fs_root`].
#[cfg(not(test))]
pub fn arch_fs_root() -> &'static str {
    ARCH_FS_ROOT
        .get()
        .expect("Arch FS root is not initialized. Please make sure `ApplicationContext::build(&android_app);` is called in `android_main`.")
}
#[cfg(test)]
pub fn arch_fs_root() -> &'static str {
    ARCH_FS_ROOT.get_or_init(|| "/data/local/tmp/arch".to_string())
}

pub const ARCH_FS_ARCHIVE: &str = "https://github.com/termux/proot-distro/releases/download/v4.29.0/archlinux-aarch64-pd-v4.29.0.tar.xz";

/// Project homepage, also the online documentation entry point.
pub const DOCS_HOME_URL: &str = "https://localdesktop.github.io/";

/// Download URL for the offline User Manual PDF matching the running version.
/// The release asset is dot-free/hyphenated (GitHub turns spaces into dots).
pub fn user_manual_url() -> String {
    format!(
        "https://github.com/localdesktop/localdesktop.github.io/releases/download/v{VERSION}/Local-Desktop-v{VERSION}-User-Manual.pdf"
    )
}

pub const WAYLAND_SOCKET_NAME: &str = "wayland-0";

pub const MAX_PANEL_LOG_ENTRIES: usize = 100;

pub const SENTRY_DSN: &str = "https://d8af27f864ade027ff81ecadea91b02e@o4509548388417536.ingest.de.sentry.io/4509548392480848";

/// PipeWire runtime path as seen from inside the proot guest.
pub const PIPEWIRE_GUEST_RUNTIME_DIR: &str = "/tmp";

/// PipeWire-Pulse socket as seen from inside the proot guest.
pub const PULSE_GUEST_SERVER: &str = "unix:/tmp/pulse/native";

/// Settings from "Additional settings in the app" in Android's App info (see `utils::settings`),
/// then the guest's own [`CONFIG_FILE`] on top (see [`apply_guest_config`]).
#[derive(Debug, Clone)]
pub struct LocalConfig {
    pub user: UserConfig,
    pub command: CommandConfig,
    /// The Android keyboard types into the desktop, instead of Squeekboard.
    pub android_keyboard: bool,
    pub input: InputConfig,
}

impl Default for LocalConfig {
    fn default() -> Self {
        Self {
            user: UserConfig::default(),
            command: CommandConfig::default(),
            android_keyboard: true,
            input: InputConfig::default(),
        }
    }
}

#[derive(Debug, Default, Clone)]
pub struct InputConfig {
    /// Send touches to the session as real `wl_touch` events instead of emulating a mouse.
    /// For touch-first sessions such as Plasma Mobile, which drag, flick and handle edge swipes
    /// themselves; mouse emulation suits desktops like Xfce.
    pub native_touch: bool,
}

#[derive(Debug, Clone)]
pub struct UserConfig {
    pub username: String,
}

impl Default for UserConfig {
    fn default() -> Self {
        Self {
            username: "root".to_string(),
        }
    }
}

#[derive(Debug, Clone)]
pub struct CommandConfig {
    pub check: String,
    pub install: String,
    pub launch: String,
}

fn default_check() -> String {
    "pacman -Q noto-fonts && pacman -Q xfce4-session && pacman -Q xfce4-panel && pacman -Q xfce4-settings && pacman -Q xfce4-terminal && pacman -Q thunar && pacman -Q xfdesktop && pacman -Q xfconf && pacman -Q labwc && pacman -Q wlr-randr && pacman -Q xorg-xwayland && pacman -Q xdg-desktop-portal && pacman -Q xdg-desktop-portal-gtk && pacman -Q squeekboard && pacman -Q firefox && pacman -Q evince && pacman -Q pipewire && pacman -Q pipewire-audio && pacman -Q pipewire-alsa"
        .to_string()
}

fn default_install() -> String {
    "stdbuf -oL pacman -Syu --needed --noconfirm --noprogressbar noto-fonts noto-fonts-cjk python-gobject xfce4 labwc wlr-randr xorg-xwayland xdg-desktop-portal xdg-desktop-portal-gtk squeekboard firefox evince pipewire pipewire-audio pipewire-alsa"
        .to_string()
}
/// Direct the desktop session to the compositor and the host PipeWire socket.
fn default_launch() -> String {
    format!("export PIPEWIRE_RUNTIME_DIR={PIPEWIRE_GUEST_RUNTIME_DIR} PULSE_SERVER={PULSE_GUEST_SERVER}; XDG_RUNTIME_DIR=/tmp WAYLAND_DISPLAY=wayland-0 XDG_SESSION_TYPE=wayland XDG_CURRENT_DESKTOP=XFCE /usr/local/bin/startxfce4-localdesktop 2>&1")
        .to_string()
}

impl Default for CommandConfig {
    fn default() -> Self {
        Self {
            check: default_check(),
            install: default_install(),
            launch: default_launch(),
        }
    }
}

/// Make sure the config keys are all lowercase, and config values are single-line. Use \n for multi-line config values if needed
/// If a key exists multiple time, the first entry is applied
/// If a `try_` config exsists multiple time, the last entry is applied
/// But in general, it is **invalid** to have duplicated config keys inside a TOML file
pub const CONFIG_FILE: &str = "/etc/localdesktop/localdesktop.toml";

/// The keys the guest file may set; any it leaves out keep the settings page's value.
#[derive(Debug, Default, Deserialize)]
struct GuestConfig {
    #[serde(default)]
    user: GuestUser,
    #[serde(default)]
    command: GuestCommand,
    #[serde(default)]
    input: GuestInput,
    android_keyboard: Option<bool>,
}

#[derive(Debug, Default, Deserialize)]
struct GuestUser {
    username: Option<String>,
}

#[derive(Debug, Default, Deserialize)]
struct GuestCommand {
    check: Option<String>,
    install: Option<String>,
    launch: Option<String>,
}

#[derive(Debug, Default, Deserialize)]
struct GuestInput {
    native_touch: Option<bool>,
}

/// This function does 2 major tasks:
/// - Read config from `CONFIG_FILE`, and override configs with their `try_*` versions, and return the configs line by line
/// - Write back to the config file, with `try_*` configs commented out
///
/// **Important**: As each call to this function will comment out the `try_*` config, it is **non-idempotent**.
fn process_config_file(full_config_path: String) -> Vec<String> {
    let mut write_back_lines: Vec<String> = vec![];
    let mut effective_config: Vec<String> = vec![];

    if let Ok(content) = fs::read_to_string(&full_config_path) {
        for line in content.lines() {
            let trimmed = line.trim();

            if let Some((key, value)) = trimmed.split_once('=') {
                let key = key.trim();
                let value = value.trim();

                if key.starts_with("try_") {
                    // Comment out the `try_*` configs
                    write_back_lines.push(format!("# {}", trimmed));

                    // Prefer the `try_*` configs
                    let actual_key = key.trim_start_matches("try_");
                    if let Some(line_index) = effective_config
                        .iter()
                        .position(|line| line.starts_with(&format!("{}=", actual_key)))
                    {
                        // Config exists, overriding
                        effective_config[line_index] = format!("{}={}", actual_key, value);
                    } else {
                        // Config does not exist, appending
                        effective_config.push(format!("{}={}", actual_key, value));
                        // Make sure there are no spaces around = so that the check existing key logic works
                    }
                } else {
                    // Keep the config as is
                    write_back_lines.push(trimmed.to_string());

                    if effective_config
                        .iter()
                        .any(|line| line.starts_with(&format!("{}=", key)))
                    {
                        // If already overridden by try_ version, skip inserting
                    } else {
                        // Config does not exist, appending
                        effective_config.push(format!("{}={}", key, value)); // Make sure there are no spaces around = so that the check existing key logic works
                    }
                }
            } else {
                // Keep the line as is
                write_back_lines.push(trimmed.to_string());
                effective_config.push(trimmed.to_string());
            }
        }

        // Rewrite config with try_* lines commented out
        let _ = OpenOptions::new()
            .write(true)
            .truncate(true)
            .open(&full_config_path)
            .and_then(|mut file| {
                for line in &write_back_lines {
                    writeln!(file, "{}", line)?;
                }
                Ok(())
            });
    }

    // Convert effective config back to lines
    effective_config
}

/// Applies the guest's own config file ([`CONFIG_FILE`], `full_config_path` on the host) over the
/// settings page's: a guest installer can choose the session there (the Plasma Mobile one does),
/// which the settings page, outside the guest, can't know about. A missing file changes nothing; a
/// malformed one is ignored, so the settings page's values hold.
pub fn apply_guest_config(config: &mut LocalConfig, full_config_path: String) {
    let lines = process_config_file(full_config_path);
    if lines.is_empty() {
        return;
    }
    let guest = match toml::from_str::<GuestConfig>(&lines.join("\n")) {
        Ok(guest) => guest,
        Err(error) => {
            log::warn!("Ignoring the guest's {CONFIG_FILE}: {error}");
            return;
        }
    };
    if let Some(username) = guest.user.username {
        config.user.username = username;
    }
    if let Some(check) = guest.command.check {
        config.command.check = check;
    }
    if let Some(install) = guest.command.install {
        config.command.install = install;
    }
    if let Some(launch) = guest.command.launch {
        config.command.launch = launch;
    }
    if let Some(native_touch) = guest.input.native_touch {
        config.input.native_touch = native_touch;
    }
    if let Some(android_keyboard) = guest.android_keyboard {
        config.android_keyboard = android_keyboard;
    }
}

/// The settings page's defaults with the guest file over them, for the tests.
#[cfg(test)]
fn parse_config(full_config_path: String) -> LocalConfig {
    let mut config = LocalConfig::default();
    apply_guest_config(&mut config, full_config_path);
    config
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::fs;
    use tempfile::tempdir;

    fn with_config_file(content: &str, f: impl Fn(String)) -> () {
        let dir = tempdir().unwrap();
        let base_dir = dir.path().to_str().unwrap();
        let path = format!("{}/etc/localdesktop", base_dir);
        fs::create_dir_all(&path).unwrap();
        let file_path = format!("{}/localdesktop.toml", path);
        fs::write(&file_path, content).unwrap();
        f(file_path)
    }

    #[test]
    fn should_handle_configs_without_try() {
        with_config_file(
            r#"
                [user]
                username = "alice"

                [command]
                check = "check-cmd"
                install = "install-cmd"
                launch = "launch-cmd"
            "#,
            |full_config_path| {
                let config = parse_config(full_config_path);
                assert_eq!(config.user.username, "alice");
                assert_eq!(config.command.check, "check-cmd");
                assert_eq!(config.command.install, "install-cmd");
                assert_eq!(config.command.launch, "launch-cmd");
            },
        );
    }

    #[test]
    fn should_handle_configs_with_try() {
        with_config_file(
            r#"
                [user]
                username = "root"
                try_username = "testuser"

                [command]
                check = "check-cmd"
                try_check = "try-check"
                install = "install-cmd"
                launch = "launch-cmd"
            "#,
            |full_config_path| {
                let config = parse_config(full_config_path);
                assert_eq!(config.user.username, "testuser");
                assert_eq!(config.command.check, "try-check");
                assert_eq!(config.command.install, "install-cmd")
            },
        );
    }

    #[test]
    fn should_comment_out_try_configs() {
        with_config_file(
            r#"
                username = "root"
                try_username = "commented"

                check = "normal"
                try_check = "try"
            "#,
            |full_config_path| {
                let _ = parse_config(full_config_path.clone()); // This triggers rewriting the config file
                let content = fs::read_to_string(full_config_path).unwrap();

                assert!(
                    content.contains("# try_username = \"commented\""),
                    "❌ `try_username` is not commented out after being applied"
                );
                assert!(
                    content.contains("# try_check = \"try\""),
                    "❌ `try_check` is not commented out after being  applied"
                );
            },
        );
    }
}

#[cfg(test)]
mod guest_override_tests {
    use super::*;

    #[test]
    fn keys_the_guest_leaves_out_keep_the_settings_value() {
        let dir = tempfile::tempdir().unwrap();
        let path = dir.path().join("localdesktop.toml");
        fs::write(&path, "[input]\nnative_touch = true\n").unwrap();
        let mut config = LocalConfig::default();
        config.command.launch = "from-the-settings-page".to_string();
        apply_guest_config(&mut config, path.to_string_lossy().into_owned());
        assert!(config.input.native_touch);
        assert_eq!(config.command.launch, "from-the-settings-page");
    }

    #[test]
    fn missing_or_malformed_files_change_nothing() {
        let dir = tempfile::tempdir().unwrap();
        let mut config = LocalConfig::default();
        apply_guest_config(&mut config, dir.path().join("none.toml").to_string_lossy().into_owned());
        assert_eq!(config.command.launch, LocalConfig::default().command.launch);
        let bad = dir.path().join("bad.toml");
        fs::write(&bad, "[command\nlaunch = ").unwrap();
        apply_guest_config(&mut config, bad.to_string_lossy().into_owned());
        assert_eq!(config.command.launch, LocalConfig::default().command.launch);
    }
}

/// The shortest side a desktop gets, in logical pixels. Desktop programs have minimum widths that
/// a phone's density can leave no room for: Chromium browsers stay at least 500 wide, whatever the
/// window. A little over that, so they fit with their window's edges in view.
pub const MIN_SHORT_SIDE: f64 = 516.0;

/// The desktop's scale for a window: the display's density scale, lowered only as far as the window
/// needs to give [`MIN_SHORT_SIDE`]. It goes by the short side so it stays the same when the
/// display rotates, and rounds down to the 1/120 steps of `wp_fractional_scale_v1`. Never below 1x.
pub fn desktop_scale(density_scale: f64, width: i32, height: i32) -> f64 {
    let short_side = width.min(height) as f64;
    if short_side <= 0.0 {
        return density_scale.max(1.0);
    }
    let fit = (short_side / MIN_SHORT_SIDE * 120.0).floor() / 120.0;
    density_scale.min(fit).max(1.0)
}

#[cfg(test)]
mod desktop_scale_tests {
    use super::desktop_scale;

    #[test]
    fn roomy_window_keeps_its_density_scale() {
        assert_eq!(desktop_scale(2.625, 1812, 2176), 2.625);
        assert_eq!(desktop_scale(2.625, 2176, 1812), 2.625);
    }

    #[test]
    fn narrow_window_fits_the_minimum_in_both_orientations() {
        let upright = desktop_scale(2.625, 904, 2316);
        assert_eq!(upright, 1.75);
        assert_eq!(upright, desktop_scale(2.625, 2316, 904));
        assert!(904.0 / upright >= 516.0);
    }

    #[test]
    fn never_below_one() {
        assert_eq!(desktop_scale(2.0, 400, 800), 1.0);
        assert_eq!(desktop_scale(0.5, 2000, 2000), 1.0);
    }

    #[test]
    fn unknown_window_keeps_its_density_scale() {
        assert_eq!(desktop_scale(2.625, 0, 0), 2.625);
    }
}
