use crate::android::utils::application_context::get_application_context;
use crate::core::config;
use std::ffi::CString;
use std::fs;
use std::io::{BufRead, BufReader, Read};
use std::os::unix::fs::PermissionsExt;
use std::path::Path;
use std::process::{Command, Output, Stdio};
use std::sync::{Arc, OnceLock};
use winit::platform::android::activity::AndroidApp;

pub type Log = Arc<dyn Fn(String) + Send + Sync>;

const SUPPORT_CHECK_BINARY: &str = "ld-linux-aarch64.so.1";

/// Guest path (under `ARCH_FS_ROOT`) of the masked `/proc/cpuinfo`, see `masked_cpuinfo`.
const MASKED_CPUINFO: &str = "proc/.cpuinfo";

/// Write a `/proc/cpuinfo` that names every core a Cortex-A78, if the real one would make
/// LLVM generate instructions this device can't run. Returns whether it should be bound.
///
/// Some SoCs (e.g. Snapdragon 8 Gen 2) have Armv9 cores whose LLVM CPU model implies SVE2,
/// while the kernel keeps SVE disabled. llvmpipe JITs with `-mcpu` from `getHostCPUName()`
/// alone, so every GL client (Qt Quick, KWin) dies with SIGILL on its first shader. A78 is
/// the newest core without SVE, and every Armv9 core is a superset of it.
fn masked_cpuinfo() -> bool {
    static MASKED: OnceLock<bool> = OnceLock::new();
    *MASKED.get_or_init(|| {
        let Ok(cpuinfo) = fs::read_to_string("/proc/cpuinfo") else {
            return false;
        };
        let has_sve = cpuinfo
            .lines()
            .filter(|line| line.starts_with("Features"))
            .any(|line| line.split_whitespace().any(|feature| feature == "sve"));
        if has_sve || !cpuinfo.lines().any(|line| line.starts_with("CPU part")) {
            return false;
        }
        let masked: String = cpuinfo
            .lines()
            .map(|line| {
                if line.starts_with("CPU implementer") {
                    "CPU implementer\t: 0x41".to_string()
                } else if line.starts_with("CPU part") {
                    "CPU part\t: 0xd41".to_string()
                } else {
                    line.to_string()
                }
            })
            .map(|line| line + "\n")
            .collect();
        fs::write(Path::new(config::ARCH_FS_ROOT).join(MASKED_CPUINFO), masked).is_ok()
    })
}

/// Guest path (under `ARCH_FS_ROOT`) of the empty PCI bus bound over `/sys/bus/pci`.
const EMPTY_PCI_BUS: &str = "sys/.pci";

/// Create an empty PCI bus (just an empty `devices/`) to stand in for `/sys/bus/pci`.
/// Returns whether it can be bound.
///
/// Android's SELinux makes the real `/sys/bus/pci/devices` unreadable and `/proc/bus/pci` has
/// no `devices`, so libpci finds no working access method and its default error handler calls
/// `exit(1)`. Chromium (Brave) probes the GPU through libpci at GPU init, so its GPU process
/// died silently on every start and it fell back to software rendering. An empty bus lets libpci
/// report zero devices instead, which is accurate for a phone.
/// Guest directory (under `ARCH_FS_ROOT`) holding the fake DRM sysfs entries, see `kgsl_render_node`.
const KGSL_DRM: &str = "sys/.drm";

/// The Adreno GPU node, shown to the guest as a DRM render node.
const KGSL_DEVICE: &str = "/dev/kgsl-3d0";

/// Prepare `/dev/kgsl-3d0` as `/dev/dri/renderD<minor>` with the sysfs entries libdrm and
/// KWin's GpuManager look up, so Mesa (Zink on Turnip/KGSL) can create a GBM device on it and
/// KWin can composite with OpenGL. KGSL has no GEM, so `/usr/local/lib/kgsl-drm-shim.so` in
/// the guest emulates PRIME handles. After DroidDeck's `LinuxRuntime.java` (GPL-3.0).
/// Returns the node name (`renderD0`), if the binds should be added.
fn kgsl_render_node() -> Option<&'static String> {
    use std::os::unix::fs::{symlink, MetadataExt};
    static NODE: OnceLock<Option<String>> = OnceLock::new();
    NODE.get_or_init(|| {
        let rdev = fs::metadata(KGSL_DEVICE).ok()?.rdev();
        let major = ((rdev >> 8) & 0xfff) | ((rdev >> 32) & !0xfff);
        let minor = (rdev & 0xff) | ((rdev >> 12) & !0xff);
        let node = format!("renderD{minor}");
        let numbers = format!("{major}:{minor}");
        let base = Path::new(config::ARCH_FS_ROOT).join(KGSL_DRM);
        let _ = fs::remove_dir_all(&base);
        let device = base.join("sys").join(&numbers).join("device");
        fs::create_dir_all(device.join("drm").join(&node)).ok()?;
        fs::create_dir_all(base.join("dri")).ok()?;
        fs::create_dir_all(base.join("class")).ok()?;
        fs::write(base.join("dri").join(&node), "").ok()?;
        fs::write(device.join("drm").join(&node).join("dev"), format!("{numbers}\n")).ok()?;
        fs::write(device.join("uevent"), "DRIVER=kgsl-3d0\nMODALIAS=platform:kgsl-3d0\n").ok()?;
        symlink("/sys/bus/platform", device.join("subsystem")).ok()?;
        symlink("/sys/bus/platform/drivers/msm_drm", device.join("driver")).ok()?;
        symlink(format!("/sys/dev/char/{numbers}"), base.join("class").join(&node)).ok()?;
        Some(node)
    })
    .as_ref()
}

fn empty_pci_bus() -> bool {
    static READY: OnceLock<bool> = OnceLock::new();
    *READY.get_or_init(|| {
        fs::create_dir_all(Path::new(config::ARCH_FS_ROOT).join(EMPTY_PCI_BUS).join("devices"))
            .is_ok()
    })
}

/// Runs a shell command inside the Arch Linux PRoot environment.
///
/// - `command`: The shell command to execute (passed to `sh -c`).
/// - `user`: The user to run as. Defaults to `"root"` when `None`.
/// - `log`: Optional stdout line callback. When set, stdout is streamed line-by-line
///   to the callback. When `None`, stdout/stderr are captured.
pub struct ArchProcess {
    pub command: String,
    pub user: Option<String>,
    pub log: Option<Log>,
}

impl ArchProcess {
    fn ensure_support_probe_rootfs(android_app: &AndroidApp) -> Option<()> {
        let context = get_application_context();
        let probe_exec = context.data_dir.join(SUPPORT_CHECK_BINARY);

        let asset_name = CString::new(SUPPORT_CHECK_BINARY).ok()?;
        let mut asset = android_app.asset_manager().open(&asset_name)?;

        let mut bytes = Vec::with_capacity(asset.length());
        asset.read_to_end(&mut bytes).ok()?;
        fs::write(&probe_exec, bytes).ok()?;
        fs::set_permissions(&probe_exec, fs::Permissions::from_mode(0o755)).ok()?;

        Some(())
    }

    fn try_proot_probe(rootfs: &Path, guest_program: &str, args: &[&str]) -> bool {
        let context = get_application_context();
        let proot_loader = context.native_library_dir.join("libproot_loader.so");

        let mut process = Command::new(context.native_library_dir.join("libproot.so"));
        process
            .env("PROOT_LOADER", &proot_loader)
            .env("PROOT_TMP_DIR", &context.data_dir);

        process
            .arg("-r")
            .arg(rootfs)
            .arg("-w")
            .arg("/")
            .arg(guest_program)
            .args(args)
            .output()
            .map(|o| {
                log::info!(
                    "try_proot_probe rootfs={}, program={}, status={:?}, stdout: {}, stderr: {}",
                    rootfs.display(),
                    guest_program,
                    o.status.code(),
                    String::from_utf8_lossy(&o.stdout),
                    String::from_utf8_lossy(&o.stderr)
                );
                o.status.success()
            })
            .unwrap_or_else(|e| {
                log::info!(
                    "try_proot_probe rootfs={}, program={} error: {}",
                    rootfs.display(),
                    guest_program,
                    e
                );
                false
            })
    }

    pub fn is_supported(android_app: &AndroidApp) -> bool {
        let context = get_application_context();
        let supported = if Self::ensure_support_probe_rootfs(android_app).is_some() {
            Self::try_proot_probe(
                &context.data_dir,
                &format!("/{}", SUPPORT_CHECK_BINARY),
                &["--help"],
            )
        } else {
            log::info!("Support probe asset missing or could not be extracted");
            false
        };

        if !supported {
            log::error!("⚡️ Device Unsupported");
        }
        supported
    }

    pub fn run(self) -> Output {
        let context = get_application_context();
        let user = self.user.as_deref().unwrap_or("root");

        let mut process = Command::new(context.native_library_dir.join("libproot.so"));
        process
            .env(
                "PROOT_LOADER",
                context.native_library_dir.join("libproot_loader.so"),
            )
            .env("PROOT_TMP_DIR", context.data_dir);

        process
            .arg("-r")
            .arg(config::ARCH_FS_ROOT)
            .arg("-L")
            .arg("--link2symlink")
            .arg("--sysvipc")
            .arg("--kill-on-exit")
            .arg("--root-id")
            .arg("--bind=/dev")
            .arg("--bind=/proc")
            .arg("--bind=/sys")
            .arg(format!("--bind={}/tmp:/dev/shm", config::ARCH_FS_ROOT))
            // /dev/pts and /dev/ptmx are already covered by --bind=/dev above.
            // The explicit sub-binds added in commit 61d9079 were redundant and caused
            // proot to double-translate PTY ioctls (TIOCGPTN/TIOCSPTLCK/TIOCSWINSZ),
            // breaking terminal initialisation and keyboard arrow keys inside QTerminal.
            // We only need /dev/tty explicitly for processes that open it by path.
            .arg("--bind=/dev/tty:/dev/tty");

        if context.permission_all_files_access {
            process
                .arg("--bind=/sdcard:/android")
                .arg("--bind=/sdcard:/root/Android");
        }

        process
            .arg("--bind=/dev/urandom:/dev/random")
            .arg("--bind=/proc/self/fd:/dev/fd")
            .arg("--bind=/proc/self/fd/0:/dev/stdin")
            .arg("--bind=/proc/self/fd/1:/dev/stdout")
            .arg("--bind=/proc/self/fd/2:/dev/stderr")
            .arg(format!("--bind={}/proc/.loadavg:/proc/loadavg", config::ARCH_FS_ROOT))
            .arg(format!("--bind={}/proc/.stat:/proc/stat", config::ARCH_FS_ROOT))
            .arg(format!("--bind={}/proc/.uptime:/proc/uptime", config::ARCH_FS_ROOT))
            .arg(format!("--bind={}/proc/.version:/proc/version", config::ARCH_FS_ROOT))
            .arg(format!("--bind={}/proc/.vmstat:/proc/vmstat", config::ARCH_FS_ROOT))
            .arg(format!("--bind={}/proc/.sysctl_entry_cap_last_cap:/proc/sys/kernel/cap_last_cap", config::ARCH_FS_ROOT))
            .arg(format!("--bind={}/proc/.sysctl_inotify_max_user_watches:/proc/sys/fs/inotify/max_user_watches", config::ARCH_FS_ROOT))
            .arg(format!("--bind={}/sys/.empty:/sys/fs/selinux", config::ARCH_FS_ROOT));

        if empty_pci_bus() {
            process.arg(format!(
                "--bind={}/{}:/sys/bus/pci",
                config::ARCH_FS_ROOT,
                EMPTY_PCI_BUS
            ));
        }

        if masked_cpuinfo() {
            process.arg(format!(
                "--bind={}/{}:/proc/cpuinfo",
                config::ARCH_FS_ROOT,
                MASKED_CPUINFO
            ));
        }

        if let Some(node) = kgsl_render_node() {
            let base = format!("{}/{}", config::ARCH_FS_ROOT, KGSL_DRM);
            process
                .arg(format!("--bind={base}/sys:/sys/dev/char"))
                .arg(format!("--bind={base}/dri:/dev/dri"))
                .arg(format!("--bind={KGSL_DEVICE}:/dev/dri/{node}"))
                .arg(format!("--bind={base}/class:/sys/class/drm"));
        }

        // env vars
        process.arg("/usr/bin/env").arg("-i");
        if user == "root" {
            process.arg("HOME=/root");
        } else {
            process.arg(format!("HOME=/home/{}", user));
        }
        process
            .arg("LANG=C.UTF-8")
            .arg("TERM=xterm-256color")
            .arg("PATH=/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin:/usr/local/games:/usr/games:/system/bin:/system/xbin")
            .arg("TMPDIR=/tmp")
            .arg(format!("USER={}", user))
            .arg(format!("LOGNAME={}", user));

        // user shell
        if user == "root" {
            process.arg("sh");
        } else {
            process
                .arg("runuser")
                .arg("--pty")
                .arg("-u")
                .arg(user)
                .arg("--")
                .arg("sh");
        }

        process.arg("-c").arg(&self.command);

        if let Some(log) = self.log {
            let mut child = process
                .stdout(Stdio::piped())
                .stderr(Stdio::inherit())
                .spawn()
                .expect("Failed to run command");

            let reader = BufReader::new(child.stdout.take().unwrap());
            for line in reader.lines() {
                let line = line.unwrap();
                log(line);
            }

            child
                .wait_with_output()
                .expect("Failed to wait for command")
        } else {
            process.output().expect("Failed to run command")
        }
    }
}
