use std::sync::Arc;

use super::build::{PolarBearApp, PolarBearBackend};
use super::session_service;
use crate::android::{
    accessibility::{self, AppUserEvent},
    backend::{
        pipewire_standalone_aaudio,
        wayland::{
            bind, centralize, centralize_injected_keyboard, dispatch_clients, handle, idle,
            on_external_pointer, on_vsync, paint_trackpad, set_keyboard_inset, start_frame_loop,
            window_hidden, window_shown, write_guest_output_state, CentralizedEvent, State, WaylandBackend,
        },
        webview::ErrorVariant,
    },
    external_display,
    proot::launch::launch,
    utils::{
        application_context::get_application_context,
        ndk::{self, run_in_jvm},
        webview::show_webview_popup,
    },
};
use crate::core::config;
use smithay::output::{Mode, Output, PhysicalProperties, Scale, Subpixel};
use smithay::utils::Transform;
use winit::application::ApplicationHandler;
use winit::event::{DeviceEvent, DeviceId, ElementState, WindowEvent};
use winit::event_loop::{ActiveEventLoop, ControlFlow};
use winit::platform::android::activity::AndroidApp;
use winit::platform::android::DeviceIdExtAndroid;
use winit::window::WindowId;

fn configure_output(backend: &mut WaylandBackend) {
    let Some(winit) = backend.graphic_renderer.as_ref() else {
        return;
    };

    let window_size = winit.output_size();
    let size = (window_size.w, window_size.h);
    // The external display's own density when the desktop is on one, else the phone's. Not
    // `winit.scale_factor()`: that reads `AConfiguration`, which still reports the 160 dpi default
    // on the first launch and only becomes accurate after a configuration change.
    let (density_scale, refresh_millihz) = match winit.external_surface() {
        Some(surface) => (
            (surface.density_dpi as f64 / 160.0).max(1.0),
            surface.refresh_millihz,
        ),
        None => (
            ndk::scale_factor(&backend.android_app),
            ndk::refresh_rate_millihz(&backend.android_app),
        ),
    };
    // Lowered only as far as a narrow window needs to fit desktop programs.
    let guest_scale_factor = config::desktop_scale(density_scale, size.0, size.1);
    backend.guest_scale_factor = guest_scale_factor;
    backend.refresh_millihz = refresh_millihz;
    backend.compositor.state.size = size.into();

    let output = backend
        .compositor
        .output
        .get_or_insert_with(|| {
            Output::new(
                "Local Desktop Wayland Compositor".into(),
                PhysicalProperties {
                    size: size.into(),
                    subpixel: Subpixel::HorizontalRgb,
                    make: "Local Desktop".into(),
                    model: config::VERSION.into(),
                },
            )
        })
        .clone();

    if backend.compositor.output_global.is_none() {
        let dh = backend.compositor.display.handle();
        backend.compositor.output_global = Some(output.create_global::<State>(&dh));
    }

    log::info!("Output mode {}x{} @ {} mHz", size.0, size.1, backend.refresh_millihz);
    output.change_current_state(
        Some(Mode {
            size: size.into(),
            refresh: backend.refresh_millihz,
        }),
        Some(Transform::Normal),
        Some(Scale::Integer(1)),
        Some((0, 0).into()),
    );

    let guest_scale = guest_scale_factor.round().max(1.0) as i32;
    let touch_keyboard = winit.external_surface().is_none()
        && !get_application_context().local_config.android_keyboard;
    write_guest_output_state(window_size.w, window_size.h, guest_scale, touch_keyboard);

    backend
        .compositor
        .state
        .set_host_geometry(size.into(), guest_scale_factor);
}

fn start_session(android_app: AndroidApp) {
    launch();
    // Start the standalone-client PipeWire/AAudio backend.
    pipewire_standalone_aaudio::spawn_after_ready(android_app);
}

/// Draw to the external monitor if one is connected, otherwise back to the phone's screen.
fn apply_external_display(backend: &mut WaylandBackend) {
    let Some(winit) = backend.graphic_renderer.as_mut() else {
        return;
    };
    let surface = external_display::surface();
    let unchanged = match (&surface, winit.external_surface()) {
        (Some(new), Some(current)) => Arc::ptr_eq(new, current),
        (None, None) => true,
        _ => false,
    };
    if unchanged {
        return;
    }
    let attached = match surface {
        Some(surface) => match winit.attach_external(surface) {
            Ok(()) => true,
            Err(error) => {
                log::error!("{error}");
                false
            }
        },
        None => false,
    };
    if !attached {
        winit.detach_external();
    }
    external_display::set_in_use(attached);

    backend.reset_touch_state();
    configure_output(backend);
    if attached {
        let size = backend.compositor.state.size;
        backend.cursor_position = (size.w as f64 / 2.0, size.h as f64 / 2.0).into();
        paint_trackpad(backend);
    }
}

impl ApplicationHandler<AppUserEvent> for PolarBearApp {
    fn resumed(&mut self, event_loop: &ActiveEventLoop) {
        run_in_jvm(
            accessibility::show_keyboard_shortcuts_prompt,
            self.frontend.android_app.clone(),
        );
        match self.backend {
            PolarBearBackend::WebView(ref mut backend) => {
                accessibility::set_runtime_active(false);
                let url = match backend.error {
                    ErrorVariant::None => {
                        let port = backend.socket_port;
                        format!("file:///android_asset/setup-progress.html?port={}", port)
                    }
                    ErrorVariant::Unsupported => {
                        format!("file:///android_asset/unsupported.html")
                    }
                };
                // Returns at once: the popup is built on Android's main thread.
                run_in_jvm(
                    move |env, app| show_webview_popup(env, app, &url),
                    self.frontend.android_app.clone(),
                );
            }
            PolarBearBackend::Wayland(ref mut backend) => {
                if backend.graphic_renderer.is_none() {
                    match bind(event_loop) {
                        Ok(winit) => {
                            backend.graphic_renderer = Some(winit);
                            window_shown(backend);
                        }
                        Err(error) => {
                            log::error!("Failed to initialize Wayland renderer on resume: {error}");
                            accessibility::set_runtime_active(false);
                            event_loop.set_control_flow(ControlFlow::Wait);
                            return;
                        }
                    }
                } else if let Some(winit) = backend.graphic_renderer.as_mut() {
                    // Back from the background, where only the external display was drawn to.
                    if let Err(error) = winit.resume_window() {
                        log::error!("Failed to draw to the window again on resume: {error}");
                    }
                }

                apply_external_display(backend);
                configure_output(backend);
                accessibility::set_runtime_active(true);
                run_in_jvm(external_display::start, self.frontend.android_app.clone());

                start_frame_loop(backend, &self.frontend.proxy);
                handle(CentralizedEvent::Redraw, backend, event_loop);
                run_in_jvm(session_service::start, self.frontend.android_app.clone());
                start_session(self.frontend.android_app.clone());
            }
        }
    }

    fn user_event(&mut self, event_loop: &ActiveEventLoop, event: AppUserEvent) {
        let PolarBearBackend::Wayland(backend) = &mut self.backend else {
            accessibility::drain_pending_events();
            return;
        };

        match event {
            AppUserEvent::AccessibilityInputReady => {
                for event in accessibility::drain_pending_events() {
                    let event = centralize_injected_keyboard(
                        event.scancode,
                        event.state,
                        event.event_time_ms,
                        backend,
                    );
                    handle(event, backend, event_loop);
                }
            }
            AppUserEvent::WaylandReady => dispatch_clients(backend),
            AppUserEvent::Vsync => on_vsync(backend),
            AppUserEvent::ExternalDisplayChanged => {
                apply_external_display(backend);
                handle(CentralizedEvent::Redraw, backend, event_loop);
            }
            AppUserEvent::ExternalPointer => {
                for event in external_display::drain_pointer_events() {
                    on_external_pointer(backend, event);
                }
            }
            AppUserEvent::KeyboardRequested => {
                if backend.window_visible() {
                    run_in_jvm(
                        external_display::show_keyboard,
                        self.frontend.android_app.clone(),
                    );
                }
            }
            AppUserEvent::KeyboardInset => {
                set_keyboard_inset(backend, external_display::keyboard_inset());
            }
            AppUserEvent::SteamTapEnded => backend.gamepad.end_steam_tap(),
            AppUserEvent::SessionEnded { early } => {
                backend.gamepad.session_ended();
                // Restart if it's in view, unless it just died at once and would keep doing so.
                // Otherwise the next resume does it.
                let visible = backend.graphic_renderer.is_some();
                let restarting = visible && !early;
                log::warn!("Desktop session ended, visible: {visible}, restarting: {restarting}");
                run_in_jvm(
                    move |env, app| session_service::session_ended(env, app, visible, restarting),
                    self.frontend.android_app.clone(),
                );
                if restarting {
                    start_session(self.frontend.android_app.clone());
                }
            }
        }
    }

    fn device_event(&mut self, _event_loop: &ActiveEventLoop, device: DeviceId, event: DeviceEvent) {
        let PolarBearBackend::Wayland(backend) = &mut self.backend else {
            return;
        };
        match event {
            DeviceEvent::Motion { axis, value } => backend.gamepad.controller_axis(device.android_id(), axis, value),
            DeviceEvent::Button { button, state } => {
                let pressed = state == ElementState::Pressed;
                let android_app = &self.frontend.android_app;
                let device = device.android_id();
                let vendor = || ndk::input_device_vendor(android_app, device);
                if let Some(delay) = backend.gamepad.controller_button(device, button, pressed, vendor) {
                    let proxy = self.frontend.proxy.clone();
                    std::thread::spawn(move || {
                        std::thread::sleep(delay);
                        let _ = proxy.send_event(AppUserEvent::SteamTapEnded);
                    });
                }
            }
            _ => {}
        }
    }

    fn about_to_wait(&mut self, event_loop: &ActiveEventLoop) {
        if let PolarBearBackend::Wayland(backend) = &mut self.backend {
            idle(backend, event_loop);
        }
    }

    fn window_event(&mut self, event_loop: &ActiveEventLoop, _id: WindowId, event: WindowEvent) {
        if let PolarBearBackend::Wayland(backend) = &mut self.backend {
            if backend.graphic_renderer.is_none() {
                if matches!(event, WindowEvent::CloseRequested) {
                    event_loop.exit();
                } else {
                    log::info!(
                        "Ignoring window event while renderer is suspended: {:?}",
                        event
                    );
                }
                return;
            }

            // Map raw events to our own events
            let event = centralize(event, backend);

            // Handle the centralized events
            handle(event, backend, event_loop);
        }
    }

    fn suspended(&mut self, event_loop: &ActiveEventLoop) {
        accessibility::set_runtime_active(false);
        event_loop.set_control_flow(ControlFlow::Wait);

        if let PolarBearBackend::Wayland(backend) = &mut self.backend {
            backend.key_counter = 0;
            backend.reset_touch_state();
            backend.pointer_pressed = false;

            if backend.is_trackpad() {
                // The monitor stays a desktop display, for a mouse and keyboard on it.
                if let Some(winit) = backend.graphic_renderer.as_mut() {
                    winit.suspend_window();
                }
                run_in_jvm(
                    external_display::background,
                    self.frontend.android_app.clone(),
                );
                return;
            }

            // The session keeps running in the background (see `window_hidden`).
            external_display::set_in_use(false);
            run_in_jvm(external_display::stop, self.frontend.android_app.clone());
            window_hidden(backend);
        }
    }

    fn exiting(&mut self, _event_loop: &ActiveEventLoop) {
        // Kill the standalone-client PipeWire/AAudio backend if it was started.
        pipewire_standalone_aaudio::shutdown();
    }
}
