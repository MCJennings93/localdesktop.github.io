# Sourced by the desktop sessions (startplasmamobile-localdesktop, startxfce4-localdesktop) for GPU
# rendering: Vulkan on Turnip, Mesa's Adreno driver built with the KGSL backend in /opt/turnip-kgsl,
# and OpenGL on Zink over it. The app shows Qualcomm's GPU (/dev/kgsl-3d0) as the DRM render node
# /dev/dri/renderD0 and offers linux-dmabuf naming it, so compositors nested on the app (KWin,
# labwc) and their clients render on the GPU and hand over GPU buffers. Mesa can't name a driver
# for that node (KGSL isn't a DRM driver), hence MESA_LOADER_DRIVER_OVERRIDE=zink. The preloaded
# shims (sources in /usr/local/src) fill what KGSL lacks: kgsl-drm-shim emulates PRIME handles,
# kgsl-syncobj gives explicit sync for the buffers clients and compositors exchange.
#
# Sets LOCALDESKTOP_GPU_PRELOAD (for the session's LD_PRELOAD) and leaves everything as it was when
# the GPU isn't usable. LOCALDESKTOP_GPU=0 opts out.
#
# Managed by the Local Desktop app: it is rewritten on every app start, so local edits are lost.
LOCALDESKTOP_GPU_PRELOAD=
LOCALDESKTOP_TURNIP_ICD=/opt/turnip-kgsl/share/vulkan/icd.d/freedreno_icd.aarch64.json
if [ "${LOCALDESKTOP_GPU:-1}" != 0 ] && [ -c /dev/dri/renderD0 ] && [ -r "$LOCALDESKTOP_TURNIP_ICD" ] \
    && [ -r /usr/local/lib/kgsl-drm-shim.so ]; then
    export VK_ICD_FILENAMES="$LOCALDESKTOP_TURNIP_ICD" MESA_LOADER_DRIVER_OVERRIDE=zink
    for shim in kgsl-drm-shim kgsl-syncobj; do
        [ -r "/usr/local/lib/$shim.so" ] && LOCALDESKTOP_GPU_PRELOAD="$LOCALDESKTOP_GPU_PRELOAD /usr/local/lib/$shim.so"
    done
    # Chromium's pool for software-decoded video (MappableSharedImageVideoFramePool) sometimes
    # hands the compositor frames whose chroma is still zero, which show as green flashes, mostly
    # while an Android overlay (shade, toast, notification) is up; without the pool, frames go
    # straight to the compositor and none are green (2026-10-27). Brave's launcher passes
    # BRAVE_FLAGS on (its flags.conf holds --no-sandbox as one argument); /usr/local/bin/chromium
    # adds the same flag. Unset BRAVE_FLAGS to reproduce the bug.
    export BRAVE_FLAGS=--disable-gpu-memory-buffer-video-frames
fi
