use std::thread;

use super::build::{PolarBearApp, PolarBearBackend};
use crate::android::{
    accessibility::{self, AppUserEvent},
    backend::{
        pipewire_standalone_aaudio,
        wayland::{
            bind, centralize, centralize_injected_keyboard, dispatch_clients, flush_clients,
            handle, write_guest_output_state, CentralizedEvent, State,
        },
        webview::ErrorVariant,
    },
    proot::launch::launch,
    utils::{
        ndk::{self, run_in_jvm},
        webview::show_webview_popup,
    },
};
use crate::core::config;
use smithay::output::{Mode, Output, PhysicalProperties, Scale, Subpixel};
use smithay::utils::Transform;
use winit::application::ApplicationHandler;
use winit::event::WindowEvent;
use winit::event_loop::{ActiveEventLoop, ControlFlow};
use winit::window::WindowId;

fn configure_output(backend: &mut crate::android::backend::wayland::WaylandBackend) {
    let Some(winit) = backend.graphic_renderer.as_ref() else {
        return;
    };

    let window_size = winit.window_size();
    let size = (window_size.w, window_size.h);
    // Not `winit.scale_factor()`: that reads `AConfiguration`, which still reports the 160 dpi
    // default on the first launch and only becomes accurate after a configuration change.
    let guest_scale_factor = ndk::scale_factor(&backend.android_app);
    backend.guest_scale_factor = guest_scale_factor;

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

    let refresh = ndk::refresh_rate_mhz(&backend.android_app).unwrap_or(60000);
    log::info!("Output mode {}x{} @ {} mHz", size.0, size.1, refresh);
    output.change_current_state(
        Some(Mode {
            size: size.into(),
            refresh,
        }),
        Some(Transform::Normal),
        Some(Scale::Integer(1)),
        Some((0, 0).into()),
    );

    let guest_scale = guest_scale_factor.round().max(1.0) as i32;
    write_guest_output_state(window_size.w, window_size.h, guest_scale);

    backend
        .compositor
        .state
        .set_host_geometry(size.into(), guest_scale_factor);
}

impl ApplicationHandler<AppUserEvent> for PolarBearApp {
    fn resumed(&mut self, event_loop: &ActiveEventLoop) {
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
                let android_app = self.frontend.android_app.clone();
                thread::spawn(move || {
                    run_in_jvm(
                        move |env, app| {
                            show_webview_popup(env, app, &url);
                        },
                        android_app,
                    );
                });
            }
            PolarBearBackend::Wayland(ref mut backend) => {
                if backend.graphic_renderer.is_none() {
                    match bind(event_loop) {
                        Ok(winit) => backend.graphic_renderer = Some(winit),
                        Err(error) => {
                            log::error!("Failed to initialize Wayland renderer on resume: {error}");
                            accessibility::set_runtime_active(false);
                            event_loop.set_control_flow(ControlFlow::Wait);
                            return;
                        }
                    }
                } else {
                    log::info!("Ignoring redundant resume while renderer is already active");
                }

                configure_output(backend);
                accessibility::set_runtime_active(true);

                if let Some(winit) = backend.graphic_renderer.as_ref() {
                    winit.window().request_redraw();
                }
                handle(CentralizedEvent::Redraw, backend, event_loop);
                launch();
                // Start the standalone-client PipeWire/AAudio backend.
                pipewire_standalone_aaudio::spawn_after_ready(self.frontend.android_app.clone());
            }
        }
    }

    fn user_event(&mut self, event_loop: &ActiveEventLoop, event: AppUserEvent) {
        let PolarBearBackend::Wayland(backend) = &mut self.backend else {
            accessibility::drain_pending_events();
            return;
        };

        if let AppUserEvent::WaylandReady = event {
            dispatch_clients(&mut backend.compositor);
            if backend.compositor.state.needs_redraw {
                if let Some(winit) = backend.graphic_renderer.as_ref() {
                    winit.window().request_redraw();
                }
            }
            return;
        }

        for event in accessibility::drain_pending_events() {
            let event = centralize_injected_keyboard(
                event.scancode,
                event.state,
                event.event_time_ms,
                backend,
            );
            handle(event, backend, event_loop);
        }
        flush_clients(&mut backend.compositor);
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

            // The app only redraws when something changed (see `wake`), so a new window size or
            // display needs a frame of its own.
            let window_changed = matches!(
                event,
                WindowEvent::Resized(_) | WindowEvent::ScaleFactorChanged { .. } | WindowEvent::Occluded(false)
            );

            // Map raw events to our own events
            let event = centralize(event, backend);

            // Handle the centralized events
            handle(event, backend, event_loop);

            // Input goes out to clients now rather than at the next redraw; a finger held down
            // keeps redraws going for long-press detection.
            flush_clients(&mut backend.compositor);
            if window_changed || !backend.touch_points.is_empty() || backend.compositor.state.needs_redraw {
                backend.compositor.state.needs_redraw = true;
                if let Some(winit) = backend.graphic_renderer.as_ref() {
                    winit.window().request_redraw();
                }
            }
        }
    }

    fn suspended(&mut self, event_loop: &ActiveEventLoop) {
        accessibility::set_runtime_active(false);
        event_loop.set_control_flow(ControlFlow::Wait);

        if let PolarBearBackend::Wayland(backend) = &mut self.backend {
            backend.graphic_renderer = None;
            backend.key_counter = 0;
            backend.reset_touch_state();
            backend.pointer_pressed = false;
            // Kill the standalone-client PipeWire/AAudio backend if it was started.
            pipewire_standalone_aaudio::shutdown();
        }
    }
}
