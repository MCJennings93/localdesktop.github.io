use super::bind::bind_socket;
use smithay::{
    backend::{
        allocator::{dmabuf::Dmabuf, Format, Fourcc, Modifier},
        renderer::utils::on_commit_buffer_handler,
    },
    delegate_compositor, delegate_data_device, delegate_dmabuf, delegate_fractional_scale, delegate_output,
    delegate_pointer_constraints, delegate_presentation, delegate_seat, delegate_shm, delegate_single_pixel_buffer,
    delegate_viewporter, delegate_xdg_shell,
    input::{self, keyboard::KeyboardHandle, touch::TouchHandle, Seat, SeatHandler, SeatState},
    output::Output,
    reexports::{
        wayland_protocols::xdg::shell::server::xdg_toplevel,
        wayland_server::{protocol::wl_seat, Display},
    },
    utils::{Clock, Logical, Monotonic, Point, Serial, Size},
    wayland::{
        buffer::BufferHandler,
        dmabuf::{DmabufFeedbackBuilder, DmabufGlobal, DmabufHandler, DmabufState, ImportNotifier},
        compositor::{
            with_states, with_surface_tree_downward, CompositorClientState, CompositorHandler,
            CompositorState, SurfaceAttributes, TraversalAction,
        },
        fractional_scale::{
            with_fractional_scale, FractionalScaleHandler, FractionalScaleManagerState,
        },
        output::OutputHandler,
        pointer_constraints::{PointerConstraintsHandler, PointerConstraintsState},
        presentation::{
            PresentationFeedbackCachedState, PresentationFeedbackCallback, PresentationState,
        },
        selection::{
            data_device::{
                ClientDndGrabHandler, DataDeviceHandler, DataDeviceState, ServerDndGrabHandler,
            },
            SelectionHandler,
        },
        shell::xdg::{
            PopupSurface, PositionerState, ToplevelSurface, XdgShellHandler, XdgShellState,
        },
        shm::{ShmHandler, ShmState},
        single_pixel_buffer::SinglePixelBufferState,
        viewporter::ViewporterState,
    },
};
use smithay::{
    input::pointer::PointerHandle,
    reexports::wayland_server::{
        backend::{ClientData, ClientId, DisconnectReason, GlobalId},
        protocol::{wl_buffer, wl_surface::WlSurface},
        Client, ListeningSocket,
    },
};
use std::{error::Error, os::unix::io::OwnedFd, time::Instant};

pub struct Compositor {
    pub state: State,
    pub display: Display<State>,
    pub listener: ListeningSocket,
    pub clients: Vec<Client>,
    pub start_time: Instant,
    pub seat: Seat<State>,
    pub keyboard: KeyboardHandle<State>,
    pub touch: TouchHandle<State>,
    pub pointer: PointerHandle<State>,
    pub output: Option<Output>,
    pub output_global: Option<GlobalId>,
    /// Clock and frame counter reported to clients through wp_presentation feedback.
    pub clock: Clock<Monotonic>,
    pub frame_seq: u64,
}

pub struct State {
    pub compositor_state: CompositorState,
    pub xdg_shell_state: XdgShellState,
    pub shm_state: ShmState,
    pub data_device_state: DataDeviceState,
    pub seat_state: SeatState<Self>,
    /// Android window size in pixels.
    pub size: Size<i32, Logical>,
    /// Android density scale of the display the window is on (e.g. 2.625 at 420 dpi). Only
    /// surfaces that opt into wp_fractional_scale use it; see [`surface_scale`].
    pub scale: f64,
    pub fractional_scale_manager_state: FractionalScaleManagerState,
    // Nested KWin (Plasma) refuses to start without these four globals.
    pub viewporter_state: ViewporterState,
    pub single_pixel_buffer_state: SinglePixelBufferState,
    pub pointer_constraints_state: PointerConstraintsState,
    pub presentation_state: PresentationState,
    /// linux-dmabuf, offered when the device has a KGSL GPU (see [`create_dmabuf_global`]).
    pub dmabuf_state: DmabufState,
    pub dmabuf_global: Option<DmabufGlobal>,
    /// A client attached a new buffer since the last render.
    pub render_pending: bool,
    /// A client committed since frame callbacks were last released.
    pub frame_pending: bool,
}

/// Offer zwp_linux_dmabuf_v1 so nested KWin can composite with OpenGL (Zink on Turnip/KGSL)
/// instead of QPainter. KWin opens the main device named here, `/dev/kgsl-3d0`, which the
/// guest sees as `/dev/dri/renderD0` (see `kgsl_render_node` in the proot setup). Android's
/// EGL can't import dmabufs, so only linear single-plane buffers are offered: the renderer
/// textures them in place through GL_EXT_memory_object_fd, or else maps and uploads them (smithay
/// patch, `GlesRenderer::import_dmabuf_shared` and `import_dmabuf_by_copy`).
fn create_dmabuf_global(
    dmabuf_state: &mut DmabufState,
    dh: &smithay::reexports::wayland_server::DisplayHandle,
) -> Option<DmabufGlobal> {
    use std::os::unix::fs::MetadataExt;
    let device = std::fs::metadata("/dev/kgsl-3d0").ok()?.rdev();
    let formats = [Fourcc::Argb8888, Fourcc::Xrgb8888, Fourcc::Abgr8888, Fourcc::Xbgr8888]
        .into_iter()
        .map(|code| Format { code, modifier: Modifier::Linear });
    let feedback = DmabufFeedbackBuilder::new(device, formats)
        .build()
        .map_err(|error| log::error!("linux-dmabuf feedback: {error}"))
        .ok()?;
    Some(dmabuf_state.create_global_with_default_feedback::<State>(dh, &feedback))
}

impl DmabufHandler for State {
    fn dmabuf_state(&mut self) -> &mut DmabufState {
        &mut self.dmabuf_state
    }

    fn dmabuf_imported(&mut self, _global: &DmabufGlobal, dmabuf: Dmabuf, notifier: ImportNotifier) {
        use smithay::backend::allocator::Buffer;
        let linear = matches!(dmabuf.format().modifier, Modifier::Linear | Modifier::Invalid);
        if dmabuf.num_planes() == 1 && linear {
            let _ = notifier.successful::<State>();
        } else {
            notifier.failed();
        }
    }
}

impl BufferHandler for State {
    fn buffer_destroyed(&mut self, _buffer: &wl_buffer::WlBuffer) {}
}

impl XdgShellHandler for State {
    fn xdg_shell_state(&mut self) -> &mut XdgShellState {
        &mut self.xdg_shell_state
    }

    fn new_toplevel(&mut self, surface: ToplevelSurface) {
        self.configure_toplevel(&surface);
    }

    fn new_popup(&mut self, _surface: PopupSurface, _positioner: PositionerState) {
        // Handle popup creation here
    }

    fn grab(&mut self, _surface: PopupSurface, _seat: wl_seat::WlSeat, _serial: Serial) {
        // Handle popup grab here
    }

    fn reposition_request(
        &mut self,
        _surface: PopupSurface,
        _positioner: PositionerState,
        _token: u32,
    ) {
        // Handle popup reposition here
    }
}

impl SelectionHandler for State {
    type SelectionUserData = ();
}

impl DataDeviceHandler for State {
    fn data_device_state(&self) -> &DataDeviceState {
        &self.data_device_state
    }
}

impl ClientDndGrabHandler for State {}
impl ServerDndGrabHandler for State {
    fn send(&mut self, _mime_type: String, _fd: OwnedFd, _seat: Seat<Self>) {}
}

impl CompositorHandler for State {
    fn compositor_state(&mut self) -> &mut CompositorState {
        &mut self.compositor_state
    }

    fn client_compositor_state<'a>(&self, client: &'a Client) -> &'a CompositorClientState {
        &client.get_data::<ClientState>().unwrap().compositor_state
    }

    fn commit(&mut self, surface: &WlSurface) {
        // Frame-callback-only commits (no attach) change nothing on screen.
        let attached = with_states(surface, |states| {
            states
                .cached_state
                .get::<SurfaceAttributes>()
                .current()
                .buffer
                .is_some()
        });
        if super::frame_stats::enabled() {
            let new_buffer = with_states(surface, |states| {
                matches!(
                    states.cached_state.get::<SurfaceAttributes>().current().buffer,
                    Some(smithay::wayland::compositor::BufferAssignment::NewBuffer(_))
                )
            });
            if new_buffer {
                super::frame_stats::buffer_committed();
            }
        }
        on_commit_buffer_handler::<Self>(surface);
        self.render_pending |= attached;
        self.frame_pending = true;
    }
}

impl ShmHandler for State {
    fn shm_state(&self) -> &ShmState {
        &self.shm_state
    }
}

impl SeatHandler for State {
    type KeyboardFocus = WlSurface;
    type PointerFocus = WlSurface;
    type TouchFocus = WlSurface;

    fn seat_state(&mut self) -> &mut SeatState<Self> {
        &mut self.seat_state
    }

    fn focus_changed(&mut self, _seat: &Seat<Self>, _focused: Option<&WlSurface>) {}
    fn cursor_image(&mut self, _seat: &Seat<Self>, _image: input::pointer::CursorImageStatus) {}
}

pub fn send_frames_surface_tree(surface: &WlSurface, time: u32) {
    with_surface_tree_downward(
        surface,
        (),
        |_, _, &()| TraversalAction::DoChildren(()),
        |_surf, states, &()| {
            // the surface may not have any user_data if it is a subsurface and has not
            // yet been commited
            for callback in states
                .cached_state
                .get::<SurfaceAttributes>()
                .current()
                .frame_callbacks
                .drain(..)
            {
                callback.done(time);
            }
        },
        |_, _, &()| true,
    );
}

/// Takes the wp_presentation feedback requested for the frame just drawn from a surface tree.
pub fn take_presentation_feedback_surface_tree(
    surface: &WlSurface,
    feedback: &mut Vec<PresentationFeedbackCallback>,
) {
    with_surface_tree_downward(
        surface,
        (),
        |_, _, &()| TraversalAction::DoChildren(()),
        |_surf, states, &()| {
            feedback.extend(
                states
                    .cached_state
                    .get::<PresentationFeedbackCachedState>()
                    .current()
                    .callbacks
                    .drain(..),
            );
        },
        |_, _, &()| true,
    );
}

#[derive(Default)]
pub struct ClientState {
    compositor_state: CompositorClientState,
}

impl ClientData for ClientState {
    fn initialized(&self, _client_id: ClientId) {}

    fn disconnected(&self, _client_id: ClientId, _reason: DisconnectReason) {}
}

impl OutputHandler for State {}

/// Scale of a surface whose client opted into wp_fractional_scale (nested KWin): it sizes itself in
/// logical units and renders its buffers at this scale. `None` for clients that size themselves in
/// window pixels (labwc, Xfce), which keep the 1:1 mapping.
pub fn surface_scale(surface: &WlSurface) -> Option<f64> {
    with_states(surface, |states| {
        with_fractional_scale(states, |fractional_scale| fractional_scale.preferred_scale())
    })
}

impl State {
    /// The window size in the surface's own coordinate space.
    pub fn surface_size(&self, surface: &WlSurface) -> Size<i32, Logical> {
        match surface_scale(surface) {
            Some(scale) => (
                (self.size.w as f64 / scale).round() as i32,
                (self.size.h as f64 / scale).round() as i32,
            )
                .into(),
            None => self.size,
        }
    }

    fn configure_toplevel(&self, surface: &ToplevelSurface) {
        let size = self.surface_size(surface.wl_surface());
        surface.with_pending_state(|state| {
            state.size.replace(size);
            state.states.set(xdg_toplevel::State::Activated);
        });
        surface.send_configure();
    }

    /// Applies a new window size and density scale (startup, rotation, fold/unfold, moving to
    /// another display). Scale-aware surfaces get the new preferred scale before the configure:
    /// nested KWin only applies a pending scale with the next configure.
    pub fn set_host_geometry(&mut self, size: Size<i32, Logical>, scale: f64) {
        self.size = size;
        self.scale = scale;
        for surface in self.xdg_shell_state.toplevel_surfaces() {
            let wl_surface = surface.wl_surface();
            if surface_scale(wl_surface).is_some() {
                with_states(wl_surface, |states| {
                    with_fractional_scale(states, |fractional_scale| {
                        fractional_scale.set_preferred_scale(scale)
                    })
                });
            }
            self.configure_toplevel(surface);
        }
    }
}

// Nested KWin 6.7 takes its output scale only from this protocol (it ignores --scale and
// kscreen changes), so this is what makes Plasma follow the Android density.
impl FractionalScaleHandler for State {
    fn new_fractional_scale(&mut self, surface: WlSurface) {
        let scale = self.scale;
        with_states(&surface, |states| {
            with_fractional_scale(states, |fractional_scale| {
                fractional_scale.set_preferred_scale(scale)
            })
        });
        // The toplevel was configured in window pixels before its client asked for a scale.
        if let Some(toplevel) = self
            .xdg_shell_state
            .toplevel_surfaces()
            .iter()
            .find(|toplevel| toplevel.wl_surface() == &surface)
        {
            self.configure_toplevel(toplevel);
        }
    }
}

// Advertised so nested compositors can start; locking/confining the Android pointer isn't supported,
// so constraints are accepted but never activated.
impl PointerConstraintsHandler for State {
    fn new_constraint(&mut self, _surface: &WlSurface, _pointer: &PointerHandle<Self>) {}

    fn cursor_position_hint(
        &mut self,
        _surface: &WlSurface,
        _pointer: &PointerHandle<Self>,
        _location: Point<f64, Logical>,
    ) {
    }
}

// Macros used to delegate protocol handling to types in the app state.
delegate_xdg_shell!(State);
delegate_compositor!(State);
delegate_shm!(State);
delegate_seat!(State);
delegate_data_device!(State);
delegate_output!(State);
delegate_viewporter!(State);
delegate_single_pixel_buffer!(State);
delegate_pointer_constraints!(State);
delegate_presentation!(State);
delegate_fractional_scale!(State);
delegate_dmabuf!(State);

impl Compositor {
    pub fn build() -> Result<Compositor, Box<dyn Error>> {
        let display = Display::new()?;
        let dh = display.handle();

        let mut seat_state = SeatState::new();
        let mut seat = seat_state.new_wl_seat(&dh, "Local Desktop");

        let listener = bind_socket()?;
        let clients = Vec::new();

        let start_time = Instant::now();
        let clock = Clock::<Monotonic>::new();

        // Key repeat rate and delay are in milliseconds: https://wayland-book.com/seat/keyboard.html
        let keyboard = seat
            .add_keyboard(Default::default(), 1000, 200)
            .expect("Failed to add keyboard");
        let touch = seat.add_touch();
        let pointer = seat.add_pointer();

        let mut dmabuf_state = DmabufState::new();
        let dmabuf_global = create_dmabuf_global(&mut dmabuf_state, &dh);
        log::info!("linux-dmabuf global: {}", if dmabuf_global.is_some() { "offered" } else { "not offered" });

        let state = State {
            compositor_state: CompositorState::new::<State>(&dh),
            xdg_shell_state: XdgShellState::new::<State>(&dh),
            shm_state: ShmState::new::<State>(&dh, vec![]),
            data_device_state: DataDeviceState::new::<State>(&dh),
            seat_state,
            size: (1920, 1080).into(),
            scale: 1.0,
            fractional_scale_manager_state: FractionalScaleManagerState::new::<State>(&dh),
            viewporter_state: ViewporterState::new::<State>(&dh),
            single_pixel_buffer_state: SinglePixelBufferState::new::<State>(&dh),
            pointer_constraints_state: PointerConstraintsState::new::<State>(&dh),
            presentation_state: PresentationState::new::<State>(&dh, clock.id() as u32),
            dmabuf_state,
            dmabuf_global,
            render_pending: false,
            frame_pending: false,
        };

        Ok(Compositor {
            state,
            listener,
            clients,
            start_time,
            display,
            seat,
            keyboard,
            touch,
            pointer,
            output: None,
            output_global: None,
            clock,
            frame_seq: 0,
        })
    }
}
