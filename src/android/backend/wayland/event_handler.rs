use crate::android::{
    accessibility,
    backend::wayland::{
        compositor::{
            send_frames_surface_tree, surface_scale, take_presentation_feedback_surface_tree,
            ClientState, State,
        },
        write_guest_output_state, CentralizedEvent, TouchMode, WaylandBackend,
    },
};
use smithay::backend::input::ButtonState;
use smithay::backend::renderer::element::surface::{
    render_elements_from_surface_tree, WaylandSurfaceRenderElement,
};
use smithay::backend::renderer::element::utils::RescaleRenderElement;
use smithay::backend::renderer::element::Kind;
use smithay::backend::renderer::gles::GlesRenderer;
use smithay::backend::renderer::utils::draw_render_elements;
use smithay::backend::renderer::{Color32F, Frame, Renderer};
use smithay::input::keyboard::FilterResult;
use smithay::input::{pointer, touch};
use smithay::reexports::wayland_server::protocol::wl_pointer::ButtonState as WlButtonState;
use smithay::reexports::wayland_protocols::wp::presentation_time::server::wp_presentation_feedback;
use smithay::utils::{Point, Rectangle, Transform, SERIAL_COUNTER};
use smithay::wayland::presentation::Refresh;
use smithay::wayland::shell::xdg::ToplevelSurface;
use smithay::{
    backend::input::{
        AbsolutePositionEvent, Axis, AxisSource, Event, InputEvent, KeyboardKeyEvent, PointerAxisEvent,
        PointerButtonEvent,
    },
    output::{Mode, Scale},
};
use std::sync::Arc;
use std::time::Duration;
use winit::event::TouchPhase;
use winit::event_loop::{ActiveEventLoop, ControlFlow};

/// Linux input event code for the left mouse button (`BTN_LEFT`).
const BTN_LEFT: u32 = 0x110;
/// Linux input event code for the right mouse button (`BTN_RIGHT`).
const BTN_RIGHT: u32 = 0x111;

/**
 * As we currently use Xwayland, there is only 1 surface
 */
fn get_surface(state: &State) -> Option<ToplevelSurface> {
    state
        .xdg_shell_state
        .toplevel_surfaces()
        .iter()
        .next()
        .cloned()
}

/// The pointer focus for window position `(x, y)`, with the position converted into that surface's
/// coordinate space (scale-aware surfaces work in logical units).
fn pointer_target(
    state: &State,
    x: f64,
    y: f64,
) -> Option<(
    smithay::reexports::wayland_server::protocol::wl_surface::WlSurface,
    Point<f64, smithay::utils::Logical>,
)> {
    get_surface(state).map(|surface| {
        let scale = surface_scale(surface.wl_surface()).unwrap_or(1.0);
        (surface.wl_surface().clone(), (x / scale, y / scale).into())
    })
}

fn emit_pointer_motion(
    compositor: &mut crate::android::backend::wayland::Compositor,
    x: f64,
    y: f64,
    time: u32,
) {
    let pointer = compositor.pointer.clone();
    let state = &mut compositor.state;
    if let Some((surface, location)) = pointer_target(state, x, y) {
        let serial = SERIAL_COUNTER.next_serial();
        pointer.motion(
            state,
            Some((surface, (0f64, 0f64).into())),
            &pointer::MotionEvent {
                location,
                serial,
                time,
            },
        );
        pointer.frame(state);
    }
}

/// Press a button. Also moves keyboard focus to the surface under the pointer.
fn emit_pointer_press(
    compositor: &mut crate::android::backend::wayland::Compositor,
    button: u32,
    time: u32,
) {
    let pointer = compositor.pointer.clone();
    let state = &mut compositor.state;
    if let Some(surface) = get_surface(state) {
        compositor.keyboard.set_focus(
            state,
            Some(surface.wl_surface().clone()),
            SERIAL_COUNTER.next_serial().into(),
        );
    }

    let serial = SERIAL_COUNTER.next_serial();
    pointer.button(
        state,
        &pointer::ButtonEvent {
            button,
            state: ButtonState::Pressed,
            serial,
            time,
        },
    );
    pointer.frame(state);
}

/// Release a button.
fn emit_pointer_release(
    compositor: &mut crate::android::backend::wayland::Compositor,
    button: u32,
    time: u32,
) {
    let pointer = compositor.pointer.clone();
    let state = &mut compositor.state;
    let serial = SERIAL_COUNTER.next_serial();
    pointer.button(
        state,
        &pointer::ButtonEvent {
            button,
            state: ButtonState::Released,
            serial,
            time,
        },
    );
    pointer.frame(state);
}

/// End a finger scroll, so clients finish it (e.g. start a kinetic fling) instead of waiting.
fn emit_axis_stop(compositor: &mut crate::android::backend::wayland::Compositor, time: u32) {
    let pointer = compositor.pointer.clone();
    let state = &mut compositor.state;
    let frame = pointer::AxisFrame::new(time)
        .source(AxisSource::Finger)
        .stop(Axis::Horizontal)
        .stop(Axis::Vertical);
    pointer.axis(state, frame);
    pointer.frame(state);
}

/// Forward a touch to the session's surface as `wl_touch`, leaving gestures to the client.
fn emit_native_touch(
    compositor: &mut crate::android::backend::wayland::Compositor,
    phase: TouchPhase,
    id: u64,
    x: f64,
    y: f64,
    time: u32,
) {
    let touch = compositor.touch.clone();
    let state = &mut compositor.state;
    let slot = Some(id as u32).into();
    match phase {
        TouchPhase::Started => {
            let Some((surface, location)) = pointer_target(state, x, y) else {
                return;
            };
            // Typing goes where the finger last landed, as with a mouse click.
            compositor
                .keyboard
                .set_focus(state, Some(surface.clone()), SERIAL_COUNTER.next_serial());
            touch.down(
                state,
                Some((surface, (0f64, 0f64).into())),
                &touch::DownEvent {
                    slot,
                    location,
                    serial: SERIAL_COUNTER.next_serial(),
                    time,
                },
            );
        }
        TouchPhase::Moved => {
            let Some((surface, location)) = pointer_target(state, x, y) else {
                return;
            };
            touch.motion(
                state,
                Some((surface, (0f64, 0f64).into())),
                &touch::MotionEvent {
                    slot,
                    location,
                    time,
                },
            );
        }
        TouchPhase::Ended => touch.up(
            state,
            &touch::UpEvent {
                slot,
                serial: SERIAL_COUNTER.next_serial(),
                time,
            },
        ),
        TouchPhase::Cancelled => {
            touch.cancel(state);
            return;
        }
    }
    touch.frame(state);
}

/// A full tap: move to the location, then a press immediately followed by a release.
fn emit_pointer_click(
    compositor: &mut crate::android::backend::wayland::Compositor,
    button: u32,
    x: f64,
    y: f64,
    time: u32,
) {
    emit_pointer_motion(compositor, x, y, time);
    emit_pointer_press(compositor, button, time);
    emit_pointer_release(compositor, button, time);
}

/// Arm the long press once the finger has stayed put for `ViewConfiguration`'s timeout.
///
/// No button is sent here: moving afterwards starts a drag with the left button held, lifting
/// instead fires a right click. Called from the redraw loop, which already ticks every frame.
fn poll_long_press(backend: &mut WaylandBackend) {
    if backend.touch_mode != TouchMode::Undecided || backend.touch_points.len() != 1 {
        return;
    }
    let (Some(down_time), Some(down_position)) =
        (backend.touch_down_time, backend.touch_down_position)
    else {
        return;
    };
    let now = backend.clock.now().as_millis() as u64;
    if now.saturating_sub(down_time) < backend.long_press_timeout_ms {
        return;
    }

    backend.touch_mode = TouchMode::LongPress;
    // Anchor the pointer where the finger landed, so a drag selects from there.
    emit_pointer_motion(
        &mut backend.compositor,
        down_position.x,
        down_position.y,
        now as u32,
    );
}

pub fn handle(event: CentralizedEvent, backend: &mut WaylandBackend, event_loop: &ActiveEventLoop) {
    match event {
        CentralizedEvent::CloseRequested => {
            event_loop.exit();
        }
        CentralizedEvent::Redraw => {
            poll_long_press(backend);

            if let Err(error) = redraw(backend) {
                log::error!("Redraw failed; dropping renderer until next resume: {error}");
                backend.graphic_renderer = None;
                accessibility::set_runtime_active(false);
                event_loop.set_control_flow(ControlFlow::Wait);
                return;
            }

            // Redraw again only while there is something to show: a client committed during
            // this frame (an animation), or a finger is down (long presses are detected from
            // here). Otherwise the app sleeps until a client wakes it (see `wake`), instead of
            // redrawing unchanged content at every display refresh.
            if backend.compositor.state.needs_redraw || !backend.touch_points.is_empty() {
                if let Some(winit) = backend.graphic_renderer.as_ref() {
                    winit.window().request_redraw();
                }
            }
        }
        CentralizedEvent::NativeTouch {
            phase,
            id,
            location,
            time,
        } => emit_native_touch(
            &mut backend.compositor,
            phase,
            id,
            location.x,
            location.y,
            time as u32,
        ),
        CentralizedEvent::Input(event) => match event {
            InputEvent::Keyboard { event } => {
                let compositor = &mut backend.compositor;
                let state = &mut compositor.state;
                let serial = SERIAL_COUNTER.next_serial();
                let time = compositor.start_time.elapsed().as_millis() as u32;
                compositor.keyboard.input::<(), _>(
                    state,
                    event.key_code(),
                    event.state(),
                    serial,
                    time,
                    |_, _, _| {
                        //
                        FilterResult::Forward
                    },
                );
            }
            InputEvent::TouchDown { event } => {
                // Just move the cursor. Which button (if any) this gesture sends is only known
                // once the finger moves, lifts, or sits still long enough to be a long press.
                emit_pointer_motion(
                    &mut backend.compositor,
                    event.x(),
                    event.y(),
                    event.time_msec(),
                );
            }
            InputEvent::TouchMotion { event } => {
                let time = event.time_msec();

                // The centralizer only emits motion in Drag mode, and flips into it on the
                // first move after a long press — that transition is where the grab starts.
                if !backend.pointer_pressed {
                    emit_pointer_press(&mut backend.compositor, BTN_LEFT, time);
                    backend.pointer_pressed = true;
                }

                emit_pointer_motion(&mut backend.compositor, event.x(), event.y(), time);
            }
            InputEvent::TouchUp { event } => {
                let time = event.time_msec();

                if backend.pointer_pressed {
                    // End of a drag.
                    emit_pointer_motion(&mut backend.compositor, event.x, event.y, time);
                    emit_pointer_release(&mut backend.compositor, BTN_LEFT, time);
                    backend.pointer_pressed = false;
                } else {
                    match event.mode {
                        // A tap: left click where the finger lifted.
                        TouchMode::Undecided => emit_pointer_click(
                            &mut backend.compositor,
                            BTN_LEFT,
                            event.x,
                            event.y,
                            time,
                        ),
                        // Held still, then lifted without moving: a context menu, as on Android.
                        TouchMode::LongPress => emit_pointer_click(
                            &mut backend.compositor,
                            BTN_RIGHT,
                            event.x,
                            event.y,
                            time,
                        ),
                        // A scroll consumed the gesture; nothing to click, just end the scroll.
                        TouchMode::Scroll => emit_axis_stop(&mut backend.compositor, time),
                        TouchMode::Drag => {}
                    }
                }
            }
            InputEvent::TouchCancel { event } => {
                if event.mode == TouchMode::Scroll {
                    emit_axis_stop(&mut backend.compositor, event.time() as u32);
                }
                if backend.pointer_pressed {
                    emit_pointer_release(&mut backend.compositor, BTN_LEFT, event.time() as u32);
                    backend.pointer_pressed = false;
                }
            }
            InputEvent::PointerMotionAbsolute { event, .. } => {
                let compositor = &mut backend.compositor;
                let pointer = compositor.pointer.clone();
                let serial = SERIAL_COUNTER.next_serial();

                if let Some((surface, location)) =
                    pointer_target(&compositor.state, event.x(), event.y())
                {
                    pointer.motion(
                        &mut compositor.state,
                        Some((surface, (0f64, 0f64).into())),
                        &pointer::MotionEvent {
                            location,
                            serial,
                            time: event.time_msec(),
                        },
                    );
                }
                pointer.frame(&mut compositor.state);
            }
            InputEvent::PointerButton { event, .. } => {
                let serial = SERIAL_COUNTER.next_serial();
                let button = event.button_code();

                let state = WlButtonState::from(event.state());

                let compositor = &mut backend.compositor;
                let pointer = compositor.pointer.clone();

                if let Some(surface) = get_surface(&compositor.state) {
                    compositor.keyboard.set_focus(
                        &mut compositor.state,
                        Some(surface.wl_surface().clone()),
                        0.into(),
                    );
                }
                pointer.button(
                    &mut compositor.state,
                    &pointer::ButtonEvent {
                        button,
                        state: state.try_into().unwrap(),
                        serial,
                        time: event.time_msec(),
                    },
                );
                pointer.frame(&mut compositor.state);
            }
            InputEvent::PointerAxis { event } => {
                // A second finger can turn an in-progress drag into a scroll; drop the button
                // the drag was holding rather than scrolling with it down.
                if backend.pointer_pressed {
                    emit_pointer_release(&mut backend.compositor, BTN_LEFT, event.time_msec());
                    backend.pointer_pressed = false;
                }
                // Scroll distances are in window pixels; scale-aware surfaces expect logical units,
                // or content would outrun the finger.
                let scale = get_surface(&backend.compositor.state)
                    .and_then(|surface| surface_scale(surface.wl_surface()))
                    .unwrap_or(1.0);
                let horizontal_amount = event
                    .amount(Axis::Horizontal)
                    .map(|amount| amount / scale)
                    .unwrap_or_else(|| event.amount_v120(Axis::Horizontal).unwrap_or(0.0) / 120.);
                let vertical_amount = event
                    .amount(Axis::Vertical)
                    .map(|amount| amount / scale)
                    .unwrap_or_else(|| event.amount_v120(Axis::Vertical).unwrap_or(0.0) / 120.);
                let horizontal_amount_discrete = event.amount_v120(Axis::Horizontal);
                let vertical_amount_discrete = event.amount_v120(Axis::Vertical);

                {
                    let mut frame =
                        pointer::AxisFrame::new(event.time_msec()).source(event.source());
                    if horizontal_amount != 0.0 {
                        frame = frame.relative_direction(
                            Axis::Horizontal,
                            event.relative_direction(Axis::Horizontal),
                        );
                        frame = frame.value(Axis::Horizontal, horizontal_amount);
                        if let Some(discrete) = horizontal_amount_discrete {
                            frame = frame.v120(Axis::Horizontal, discrete as i32);
                        }
                    }
                    if vertical_amount != 0.0 {
                        frame = frame.relative_direction(
                            Axis::Vertical,
                            event.relative_direction(Axis::Vertical),
                        );
                        frame = frame.value(Axis::Vertical, vertical_amount);
                        if let Some(discrete) = vertical_amount_discrete {
                            frame = frame.v120(Axis::Vertical, discrete as i32);
                        }
                    }
                    if event.amount(Axis::Horizontal) == Some(0.0) {
                        frame = frame.stop(Axis::Horizontal);
                    }
                    if event.amount(Axis::Vertical) == Some(0.0) {
                        frame = frame.stop(Axis::Vertical);
                    }
                    let compositor = &mut backend.compositor;
                    let pointer = compositor.pointer.clone();
                    pointer.axis(&mut compositor.state, frame);
                    pointer.frame(&mut compositor.state);
                }
            }
            _ => {}
        },
        CentralizedEvent::Resized {
            size,
            guest_scale_factor,
        } => {
            if let Some(output) = &backend.compositor.output {
                output.change_current_state(
                    Some(Mode {
                        size: size.into(),
                        refresh: 60000,
                    }),
                    Some(Transform::Normal),
                    Some(Scale::Integer(1)),
                    Some((0, 0).into()),
                );
            }

            let guest_scale = guest_scale_factor.round().max(1.0) as i32;
            write_guest_output_state(size.w, size.h, guest_scale);

            backend
                .compositor
                .state
                .set_host_geometry((size.w, size.h).into(), guest_scale_factor);
        }
        _ => (),
    }
}

fn redraw(backend: &mut WaylandBackend) -> Result<(), String> {
    if backend.graphic_renderer.is_none() {
        return Ok(());
    }

    // Take the clients' latest commits first, so this frame shows them.
    dispatch_clients(&mut backend.compositor);
    backend.compositor.state.needs_redraw = false;

    // Frame callbacks go out as soon as the commits are taken, before drawing: clients (KWin)
    // render their next frame while this one is uploaded and drawn, instead of after it, which
    // made the two serial (KWin's render + our upload + swap per frame, ~50 fps).
    {
        let compositor = &mut backend.compositor;
        let time = compositor.start_time.elapsed().as_millis() as u32;
        for surface in compositor.state.xdg_shell_state.toplevel_surfaces() {
            send_frames_surface_tree(surface.wl_surface(), time);
        }
        flush_clients(compositor);
    }
    let Some(winit) = backend.graphic_renderer.as_mut() else {
        return Ok(());
    };

    let started = std::time::Instant::now();
    let size = winit.window_size();
    let damage = Rectangle::from_size(size);
    let mut presentation_feedback = Vec::new();
    {
        let (renderer, mut framebuffer) = winit
            .bind()
            .map_err(|error| format!("Failed to bind EGL surface: {error}"))?;

        let compositor = &mut backend.compositor;

        let elements = compositor
            .state
            .xdg_shell_state
            .toplevel_surfaces()
            .iter()
            .flat_map(|surface| {
                // The scale passed here only positions subsurfaces; element sizes come from the
                // draw scale, so scale-aware surfaces are scaled up by wrapping each element.
                let scale = surface_scale(surface.wl_surface()).unwrap_or(1.0);
                render_elements_from_surface_tree::<_, WaylandSurfaceRenderElement<GlesRenderer>>(
                    renderer,
                    surface.wl_surface(),
                    (0, 0),
                    1.0,
                    1.0,
                    Kind::Unspecified,
                )
                .into_iter()
                .map(move |element| RescaleRenderElement::from_element(element, (0, 0).into(), scale))
            })
            .collect::<Vec<_>>();
        let pad = backend.gamepad.elements(renderer, (size.w, size.h));

        let mut frame = renderer
            .render(&mut framebuffer, size, Transform::Flipped180)
            .map_err(|error| format!("Failed to render frame: {error:?}"))?;
        frame
            .clear(Color32F::new(0.1, 0.0, 0.0, 1.0), &[damage])
            .map_err(|error| format!("Failed to clear frame: {error:?}"))?;
        draw_render_elements(&mut frame, 1.0, &elements, &[damage])
            .map_err(|error| format!("Failed to draw render elements: {error:?}"))?;
        if !pad.is_empty() {
            draw_render_elements::<GlesRenderer, _, _>(&mut frame, 1.0, &pad, &[damage])
                .map_err(|error| format!("Failed to draw the gamepad: {error:?}"))?;
        }
        // We rely on the nested compositor to do the sync for us.
        let _ = frame
            .finish()
            .map_err(|error| format!("Failed to finish frame: {error:?}"))?;

    }

    let submitting = std::time::Instant::now();
    winit
        .submit(Some(&[damage]))
        .map_err(|error| format!("Failed to submit frame: {error}"))?;
    super::frame_stats::redrawn(submitting - started, submitting.elapsed());

    let compositor = &mut backend.compositor;
    for surface in compositor.state.xdg_shell_state.toplevel_surfaces() {
        take_presentation_feedback_surface_tree(surface.wl_surface(), &mut presentation_feedback);
    }

    // Report the frame as presented once it has been swapped; nested compositors (KWin) pace
    // their own rendering from this feedback.
    compositor.frame_seq += 1;
    match &compositor.output {
        Some(output) => {
            let now = Duration::from(compositor.clock.now());
            let refresh = output
                .current_mode()
                .filter(|mode| mode.refresh > 0)
                .map(|mode| Refresh::fixed(Duration::from_secs_f64(1000.0 / mode.refresh as f64)))
                .unwrap_or(Refresh::Unknown);
            for feedback in presentation_feedback {
                feedback.presented(
                    output,
                    now,
                    refresh,
                    compositor.frame_seq,
                    wp_presentation_feedback::Kind::Vsync,
                );
            }
        }
        None => presentation_feedback
            .into_iter()
            .for_each(|feedback| feedback.discarded()),
    }
    flush_clients(compositor);

    Ok(())
}

/// Accept new clients and handle what the connected ones sent, then let the wake thread watch
/// for more.
pub fn dispatch_clients(compositor: &mut crate::android::backend::wayland::Compositor) {
    match compositor.listener.accept() {
        Ok(Some(stream)) => match compositor
            .display
            .handle()
            .insert_client(stream, Arc::new(ClientState::default()))
        {
            Ok(client) => compositor.clients.push(client),
            Err(error) => log::error!("Failed to insert Wayland client: {error}"),
        },
        Ok(None) => {}
        Err(error) => log::error!("Failed to accept Wayland client: {error}"),
    }
    if let Err(error) = compositor.display.dispatch_clients(&mut compositor.state) {
        log::error!("Failed to dispatch clients: {error}");
    }
    flush_clients(compositor);
    let waker = compositor.waker.get_or_insert_with(|| {
        use std::os::fd::AsRawFd;
        super::wake::Waker::spawn([
            compositor.listener.as_raw_fd(),
            compositor.display.backend().poll_fd().as_raw_fd(),
        ])
    });
    waker.rearm();
}

pub fn flush_clients(compositor: &mut crate::android::backend::wayland::Compositor) {
    if let Err(error) = compositor.display.flush_clients() {
        log::error!("Failed to flush clients: {error}");
    }
}
