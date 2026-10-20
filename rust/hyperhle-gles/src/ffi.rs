/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0.
 * If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 */
//! Minimal C ABI for embedding HyperHLE's GLES core.
//!
//! Step 1 only needs enough surface to prove the extracted core builds and
//! can bring up a real EGL context: create/destroy, make-current, swap, and a
//! handful of ES 1.1 calls. The guest-call translation layer will grow this
//! ABI later.

use std::ffi::c_void;
use std::os::raw::c_char;
use std::ptr;

use crate::gles::gles1_native::GLES1NativeContext;
use crate::gles::gles2_native::GLES2NativeContext;
use crate::gles::gles3_native::GLES3NativeContext;
use crate::gles::{GLESContext, GLES};
use crate::log;
use crate::window::Window;

/// Opaque handle owned by the embedding application.
pub struct GlesHandle {
    // Field order matters: the context must be destroyed before the window
    // terminates the EGL display.
    context: Box<dyn GLESContext>,
    window: Window,
}

impl Drop for GlesHandle {
    fn drop(&mut self) {
        // Unbind before the context and surface go away.
        unsafe {
            self.window.make_current_none();
        }
    }
}

unsafe fn create_handle(
    api: i32,
    native_window: *mut c_void,
    width: u32,
    height: u32,
) -> Result<GlesHandle, String> {
    let mut window = Window::new(native_window, width, height)?;

    let mut context: Box<dyn GLESContext> = match api {
        1 => Box::new(GLES1NativeContext::new(&mut window)?),
        2 => Box::new(GLES2NativeContext::new(&mut window)?),
        3 => Box::new(GLES3NativeContext::new(&mut window)?),
        other => return Err(format!("unsupported GLES API version {other}")),
    };

    // Bring the context up once so driver detection (extension strings, PVRTC
    // support, shader setup) happens at creation time rather than on the
    // first guest call.
    {
        let gl = context.make_current(&mut window);
        let description = unsafe { gl.driver_description() };
        log!("hyperhle-gles: created {} ({})", api_name(api), description);
    }

    Ok(GlesHandle { context, window })
}

fn api_name(api: i32) -> &'static str {
    match api {
        1 => "native GLES 1.1",
        2 => "native GLES 2.0",
        3 => "native GLES 3.0",
        _ => "unknown GLES",
    }
}

unsafe fn with_gles<R>(handle: *mut GlesHandle, f: impl FnOnce(&mut dyn GLES) -> R) -> Option<R> {
    let handle = handle.as_mut()?;
    let GlesHandle { context, window } = handle;
    let mut gl = context.make_current(window);
    Some(f(gl.as_mut()))
}

/// Creates a context for `api` (1, 2 or 3). `native_window` is an
/// `ANativeWindow*`; pass null for an offscreen pbuffer of `width` x `height`.
///
/// Returns null on failure (details go to logcat).
#[no_mangle]
pub unsafe extern "C" fn s5l_gles_create(
    api: i32,
    native_window: *mut c_void,
    width: u32,
    height: u32,
) -> *mut GlesHandle {
    match create_handle(api, native_window, width, height) {
        Ok(handle) => Box::into_raw(Box::new(handle)),
        Err(error) => {
            log!("hyperhle-gles: context creation failed: {error}");
            ptr::null_mut()
        }
    }
}

/// Destroys a handle returned by [s5l_gles_create]. Null is accepted.
#[no_mangle]
pub unsafe extern "C" fn s5l_gles_destroy(handle: *mut GlesHandle) {
    if !handle.is_null() {
        drop(Box::from_raw(handle));
    }
}

/// Makes the context current. Returns 1 on success, 0 for a null handle.
#[no_mangle]
pub unsafe extern "C" fn s5l_gles_make_current(handle: *mut GlesHandle) -> i32 {
    with_gles(handle, |_| ()).map(|_| 1).unwrap_or(0)
}

/// Presents the window surface. Returns 1 on success.
#[no_mangle]
pub unsafe extern "C" fn s5l_gles_swap_buffers(handle: *mut GlesHandle) -> i32 {
    match handle.as_ref() {
        Some(handle) => handle.window.swap_buffers() as i32,
        None => 0,
    }
}

/// `glClearColor`.
#[no_mangle]
pub unsafe extern "C" fn s5l_gles_clear_color(
    handle: *mut GlesHandle,
    red: f32,
    green: f32,
    blue: f32,
    alpha: f32,
) {
    with_gles(handle, |gl| unsafe {
        gl.ClearColor(red, green, blue, alpha)
    });
}

/// `glClear`.
#[no_mangle]
pub unsafe extern "C" fn s5l_gles_clear(handle: *mut GlesHandle, mask: u32) {
    with_gles(handle, |gl| unsafe { gl.Clear(mask) });
}

/// `glViewport`.
#[no_mangle]
pub unsafe extern "C" fn s5l_gles_viewport(
    handle: *mut GlesHandle,
    x: i32,
    y: i32,
    width: i32,
    height: i32,
) {
    with_gles(handle, |gl| unsafe {
        gl.Viewport(x, y, width, height)
    });
}

/// `glDrawArrays`.
#[no_mangle]
pub unsafe extern "C" fn s5l_gles_draw_arrays(
    handle: *mut GlesHandle,
    mode: u32,
    first: i32,
    count: i32,
) {
    with_gles(handle, |gl| unsafe {
        gl.DrawArrays(mode, first, count)
    });
}

/// `glFlush`.
#[no_mangle]
pub unsafe extern "C" fn s5l_gles_flush(handle: *mut GlesHandle) {
    with_gles(handle, |gl| unsafe { gl.Flush() });
}

/// `glFinish`.
#[no_mangle]
pub unsafe extern "C" fn s5l_gles_finish(handle: *mut GlesHandle) {
    with_gles(handle, |gl| unsafe { gl.Finish() });
}

/// `glGetError`.
#[no_mangle]
pub unsafe extern "C" fn s5l_gles_get_error(handle: *mut GlesHandle) -> u32 {
    with_gles(handle, |gl| unsafe { gl.GetError() }).unwrap_or(0)
}

/// `glGetString`. The returned pointer is owned by the GL driver and remains
/// valid while the context is alive.
#[no_mangle]
pub unsafe extern "C" fn s5l_gles_get_string(
    handle: *mut GlesHandle,
    name: u32,
) -> *const c_char {
    with_gles(handle, |gl| unsafe { gl.GetString(name) as *const c_char })
        .unwrap_or(ptr::null())
}

/// Sets the directory used for decoded-texture caches. Pass a NUL-terminated
/// UTF-8 path. Returns 1 on success.
#[no_mangle]
pub unsafe extern "C" fn s5l_gles_set_data_dir(path: *const c_char) -> i32 {
    if path.is_null() {
        return 0;
    }
    match std::ffi::CStr::from_ptr(path).to_str() {
        Ok(text) => {
            crate::paths::set_user_data_base_path(std::path::PathBuf::from(text));
            1
        }
        Err(_) => 0,
    }
}
