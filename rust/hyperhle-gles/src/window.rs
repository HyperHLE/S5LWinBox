/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0.
 * If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 */
//! Minimal EGL shim replacing touchHLE's SDL2 window for the GLES core.
//!
//! HyperHLE's native GLES backends only need four things from `Window`:
//! `create_gl_context`, `make_gl_context_current`, `gl_get_proc_address`, and
//! `GLContext::is_current`. This module provides them on top of EGL so the
//! core can run inside an Android app with a plain `Surface`.

use std::ffi::{c_void, CString};
use std::os::raw::c_char;
use std::ptr;

pub type EGLint = i32;
pub type EGLBoolean = u32;
pub type EGLenum = u32;
pub type EGLDisplay = *mut c_void;
pub type EGLConfig = *mut c_void;
pub type EGLContext = *mut c_void;
pub type EGLSurface = *mut c_void;
pub type EGLNativeDisplayType = *mut c_void;
pub type EGLNativeWindowType = *mut c_void;

const EGL_FALSE: EGLBoolean = 0;
const EGL_TRUE: EGLBoolean = 1;
pub const EGL_NO_CONTEXT: EGLContext = ptr::null_mut();
pub const EGL_NO_SURFACE: EGLSurface = ptr::null_mut();
pub const EGL_NO_DISPLAY: EGLDisplay = ptr::null_mut();
const EGL_DEFAULT_DISPLAY: EGLNativeDisplayType = ptr::null_mut();

const EGL_OPENGL_ES_API: EGLenum = 0x30A0;
const EGL_SURFACE_TYPE: EGLint = 0x3033;
const EGL_PBUFFER_BIT: EGLint = 0x0001;
const EGL_WINDOW_BIT: EGLint = 0x0004;
const EGL_RENDERABLE_TYPE: EGLint = 0x3040;
const EGL_OPENGL_ES_BIT: EGLint = 0x0001;
const EGL_OPENGL_ES2_BIT: EGLint = 0x0004;
const EGL_OPENGL_ES3_BIT_KHR: EGLint = 0x0040;
const EGL_NONE: EGLint = 0x3038;
const EGL_RED_SIZE: EGLint = 0x3024;
const EGL_GREEN_SIZE: EGLint = 0x3023;
const EGL_BLUE_SIZE: EGLint = 0x3022;
const EGL_ALPHA_SIZE: EGLint = 0x3021;
const EGL_DEPTH_SIZE: EGLint = 0x3025;
const EGL_STENCIL_SIZE: EGLint = 0x3026;
const EGL_CONTEXT_CLIENT_VERSION: EGLint = 0x3098;
const EGL_WIDTH: EGLint = 0x3057;
const EGL_HEIGHT: EGLint = 0x3056;

#[cfg(target_os = "android")]
#[link(name = "EGL")]
extern "C" {
    fn eglGetDisplay(display_id: EGLNativeDisplayType) -> EGLDisplay;
    fn eglInitialize(dpy: EGLDisplay, major: *mut EGLint, minor: *mut EGLint) -> EGLBoolean;
    fn eglBindAPI(api: EGLenum) -> EGLBoolean;
    fn eglChooseConfig(
        dpy: EGLDisplay,
        attrib_list: *const EGLint,
        configs: *mut EGLConfig,
        config_size: EGLint,
        num_config: *mut EGLint,
    ) -> EGLBoolean;
    fn eglCreateContext(
        dpy: EGLDisplay,
        config: EGLConfig,
        share_context: EGLContext,
        attrib_list: *const EGLint,
    ) -> EGLContext;
    fn eglCreateWindowSurface(
        dpy: EGLDisplay,
        config: EGLConfig,
        win: EGLNativeWindowType,
        attrib_list: *const EGLint,
    ) -> EGLSurface;
    fn eglCreatePbufferSurface(
        dpy: EGLDisplay,
        config: EGLConfig,
        attrib_list: *const EGLint,
    ) -> EGLSurface;
    fn eglDestroySurface(dpy: EGLDisplay, surface: EGLSurface) -> EGLBoolean;
    fn eglDestroyContext(dpy: EGLDisplay, ctx: EGLContext) -> EGLBoolean;
    fn eglMakeCurrent(
        dpy: EGLDisplay,
        draw: EGLSurface,
        read: EGLSurface,
        ctx: EGLContext,
    ) -> EGLBoolean;
    fn eglGetCurrentContext() -> EGLContext;
    fn eglSwapBuffers(dpy: EGLDisplay, surface: EGLSurface) -> EGLBoolean;
    fn eglGetProcAddress(procname: *const c_char) -> *mut c_void;
    fn eglGetError() -> EGLint;
    fn eglTerminate(dpy: EGLDisplay) -> EGLBoolean;
}

#[cfg(not(target_os = "android"))]
mod dyn_egl {
    use super::*;
    use std::sync::OnceLock;

    type FnGetDisplay = unsafe extern "C" fn(EGLNativeDisplayType) -> EGLDisplay;
    type FnInitialize = unsafe extern "C" fn(EGLDisplay, *mut EGLint, *mut EGLint) -> EGLBoolean;
    type FnBindAPI = unsafe extern "C" fn(EGLenum) -> EGLBoolean;
    type FnChooseConfig = unsafe extern "C" fn(EGLDisplay, *const EGLint, *mut EGLConfig, EGLint, *mut EGLint) -> EGLBoolean;
    type FnCreateContext = unsafe extern "C" fn(EGLDisplay, EGLConfig, EGLContext, *const EGLint) -> EGLContext;
    type FnCreateWindowSurface = unsafe extern "C" fn(EGLDisplay, EGLConfig, EGLNativeWindowType, *const EGLint) -> EGLSurface;
    type FnCreatePbufferSurface = unsafe extern "C" fn(EGLDisplay, EGLConfig, *const EGLint) -> EGLSurface;
    type FnDestroySurface = unsafe extern "C" fn(EGLDisplay, EGLSurface) -> EGLBoolean;
    type FnDestroyContext = unsafe extern "C" fn(EGLDisplay, EGLContext) -> EGLBoolean;
    type FnMakeCurrent = unsafe extern "C" fn(EGLDisplay, EGLSurface, EGLSurface, EGLContext) -> EGLBoolean;
    type FnGetCurrentContext = unsafe extern "C" fn() -> EGLContext;
    type FnSwapBuffers = unsafe extern "C" fn(EGLDisplay, EGLSurface) -> EGLBoolean;
    type FnGetProcAddress = unsafe extern "C" fn(*const c_char) -> *mut c_void;
    type FnGetError = unsafe extern "C" fn() -> EGLint;
    type FnTerminate = unsafe extern "C" fn(EGLDisplay) -> EGLBoolean;

    pub struct EglLib {
        pub get_display: FnGetDisplay,
        pub initialize: FnInitialize,
        pub bind_api: FnBindAPI,
        pub choose_config: FnChooseConfig,
        pub create_context: FnCreateContext,
        pub create_window_surface: FnCreateWindowSurface,
        pub create_pbuffer_surface: FnCreatePbufferSurface,
        pub destroy_surface: FnDestroySurface,
        pub destroy_context: FnDestroyContext,
        pub make_current: FnMakeCurrent,
        pub get_current_context: FnGetCurrentContext,
        pub swap_buffers: FnSwapBuffers,
        pub get_proc_address: FnGetProcAddress,
        pub get_error: FnGetError,
        pub terminate: FnTerminate,
    }

    #[cfg(target_os = "windows")]
    extern "system" {
        fn LoadLibraryA(lpLibFileName: *const c_char) -> *mut c_void;
        fn GetProcAddress(hModule: *mut c_void, lpProcName: *const c_char) -> *mut c_void;
    }

    pub fn get_egl() -> Option<&'static EglLib> {
        static EGL: OnceLock<Option<EglLib>> = OnceLock::new();
        EGL.get_or_init(|| {
            #[cfg(target_os = "windows")]
            unsafe {
                let mod_name = b"libEGL.dll\0";
                let mut h = LoadLibraryA(mod_name.as_ptr() as _);
                if h.is_null() {
                    let fallback = b"EGL.dll\0";
                    h = LoadLibraryA(fallback.as_ptr() as _);
                }
                if h.is_null() {
                    return None;
                }
                macro_rules! sym {
                    ($name:literal, $t:ty) => {{
                        let p = GetProcAddress(h, concat!($name, "\0").as_ptr() as _);
                        if p.is_null() { return None; }
                        std::mem::transmute::<*mut c_void, $t>(p)
                    }};
                }
                Some(EglLib {
                    get_display: sym!("eglGetDisplay", FnGetDisplay),
                    initialize: sym!("eglInitialize", FnInitialize),
                    bind_api: sym!("eglBindAPI", FnBindAPI),
                    choose_config: sym!("eglChooseConfig", FnChooseConfig),
                    create_context: sym!("eglCreateContext", FnCreateContext),
                    create_window_surface: sym!("eglCreateWindowSurface", FnCreateWindowSurface),
                    create_pbuffer_surface: sym!("eglCreatePbufferSurface", FnCreatePbufferSurface),
                    destroy_surface: sym!("eglDestroySurface", FnDestroySurface),
                    destroy_context: sym!("eglDestroyContext", FnDestroyContext),
                    make_current: sym!("eglMakeCurrent", FnMakeCurrent),
                    get_current_context: sym!("eglGetCurrentContext", FnGetCurrentContext),
                    swap_buffers: sym!("eglSwapBuffers", FnSwapBuffers),
                    get_proc_address: sym!("eglGetProcAddress", FnGetProcAddress),
                    get_error: sym!("eglGetError", FnGetError),
                    terminate: sym!("eglTerminate", FnTerminate),
                })
            }
            #[cfg(not(target_os = "windows"))]
            None
        }).as_ref()
    }

    pub unsafe fn eglGetDisplay(display_id: EGLNativeDisplayType) -> EGLDisplay {
        get_egl().map_or(EGL_NO_DISPLAY, |e| (e.get_display)(display_id))
    }
    pub unsafe fn eglInitialize(dpy: EGLDisplay, major: *mut EGLint, minor: *mut EGLint) -> EGLBoolean {
        get_egl().map_or(EGL_FALSE, |e| (e.initialize)(dpy, major, minor))
    }
    pub unsafe fn eglBindAPI(api: EGLenum) -> EGLBoolean {
        get_egl().map_or(EGL_FALSE, |e| (e.bind_api)(api))
    }
    pub unsafe fn eglChooseConfig(dpy: EGLDisplay, attrib_list: *const EGLint, configs: *mut EGLConfig, config_size: EGLint, num_config: *mut EGLint) -> EGLBoolean {
        get_egl().map_or(EGL_FALSE, |e| (e.choose_config)(dpy, attrib_list, configs, config_size, num_config))
    }
    pub unsafe fn eglCreateContext(dpy: EGLDisplay, config: EGLConfig, share_context: EGLContext, attrib_list: *const EGLint) -> EGLContext {
        get_egl().map_or(EGL_NO_CONTEXT, |e| (e.create_context)(dpy, config, share_context, attrib_list))
    }
    pub unsafe fn eglCreateWindowSurface(dpy: EGLDisplay, config: EGLConfig, win: EGLNativeWindowType, attrib_list: *const EGLint) -> EGLSurface {
        get_egl().map_or(EGL_NO_SURFACE, |e| (e.create_window_surface)(dpy, config, win, attrib_list))
    }
    pub unsafe fn eglCreatePbufferSurface(dpy: EGLDisplay, config: EGLConfig, attrib_list: *const EGLint) -> EGLSurface {
        get_egl().map_or(EGL_NO_SURFACE, |e| (e.create_pbuffer_surface)(dpy, config, attrib_list))
    }
    pub unsafe fn eglDestroySurface(dpy: EGLDisplay, surface: EGLSurface) -> EGLBoolean {
        get_egl().map_or(EGL_FALSE, |e| (e.destroy_surface)(dpy, surface))
    }
    pub unsafe fn eglDestroyContext(dpy: EGLDisplay, ctx: EGLContext) -> EGLBoolean {
        get_egl().map_or(EGL_FALSE, |e| (e.destroy_context)(dpy, ctx))
    }
    pub unsafe fn eglMakeCurrent(dpy: EGLDisplay, draw: EGLSurface, read: EGLSurface, ctx: EGLContext) -> EGLBoolean {
        get_egl().map_or(EGL_FALSE, |e| (e.make_current)(dpy, draw, read, ctx))
    }
    pub unsafe fn eglGetCurrentContext() -> EGLContext {
        get_egl().map_or(EGL_NO_CONTEXT, |e| (e.get_current_context)())
    }
    pub unsafe fn eglSwapBuffers(dpy: EGLDisplay, surface: EGLSurface) -> EGLBoolean {
        get_egl().map_or(EGL_FALSE, |e| (e.swap_buffers)(dpy, surface))
    }
    pub unsafe fn eglGetProcAddress(procname: *const c_char) -> *mut c_void {
        get_egl().map_or(ptr::null_mut(), |e| (e.get_proc_address)(procname))
    }
    pub unsafe fn eglGetError() -> EGLint {
        get_egl().map_or(0, |e| (e.get_error)())
    }
    pub unsafe fn eglTerminate(dpy: EGLDisplay) -> EGLBoolean {
        get_egl().map_or(EGL_FALSE, |e| (e.terminate)(dpy))
    }
}

#[cfg(not(target_os = "android"))]
use dyn_egl::*;

/// Same shape as touchHLE's `GLVersion`; only the GLES variants can be
/// created through EGL on Android.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum GLVersion {
    GLES11,
    GLES20,
    GLES30,
    GL21Compat,
    GL33Core,
}

/// An EGL context plus the display needed to destroy it.
pub struct GLContext {
    display: EGLDisplay,
    context: EGLContext,
}

impl GLContext {
    pub fn is_current(&self) -> bool {
        unsafe { eglGetCurrentContext() == self.context }
    }
}

impl Drop for GLContext {
    fn drop(&mut self) {
        unsafe {
            eglDestroyContext(self.display, self.context);
        }
    }
}

/// EGL display/config/surface pair standing in for touchHLE's SDL2 `Window`.
pub struct Window {
    display: EGLDisplay,
    config: EGLConfig,
    surface: EGLSurface,
    width: u32,
    height: u32,
    /// Set when EGL was initialised by this window and must be terminated.
    owns_display: bool,
}

impl Window {
    /// Creates an EGL window surface (`native_window` is an `ANativeWindow*`)
    /// or, when `native_window` is null, an offscreen pbuffer.
    pub unsafe fn new(
        native_window: *mut c_void,
        width: u32,
        height: u32,
    ) -> Result<Self, String> {
        let display = eglGetDisplay(EGL_DEFAULT_DISPLAY);
        if display == EGL_NO_DISPLAY {
            return Err(format!("eglGetDisplay failed (0x{:x})", eglGetError()));
        }
        let mut major: EGLint = 0;
        let mut minor: EGLint = 0;
        if eglInitialize(display, &mut major, &mut minor) == EGL_FALSE {
            return Err(format!("eglInitialize failed (0x{:x})", eglGetError()));
        }
        if eglBindAPI(EGL_OPENGL_ES_API) == EGL_FALSE {
            eglTerminate(display);
            return Err(format!(
                "eglBindAPI(EGL_OPENGL_ES_API) failed (0x{:x})",
                eglGetError()
            ));
        }

        let surface_bit = if native_window.is_null() {
            EGL_PBUFFER_BIT
        } else {
            EGL_WINDOW_BIT
        };

        // Prefer an ES3-capable config, then ES2, then ES1. A config that
        // advertises ES3 can still back ES1/ES2 contexts when the driver
        // offers legacy compatibility contexts.
        let mut config = ptr::null_mut();
        for renderable in [
            EGL_OPENGL_ES3_BIT_KHR,
            EGL_OPENGL_ES2_BIT,
            EGL_OPENGL_ES_BIT,
        ] {
            if let Some(found) = choose_config(display, surface_bit, renderable) {
                config = found;
                break;
            }
        }
        if config.is_null() {
            eglTerminate(display);
            return Err("eglChooseConfig found no usable config".into());
        }

        let surface = if native_window.is_null() {
            let attribs = [
                EGL_WIDTH,
                width.max(1) as EGLint,
                EGL_HEIGHT,
                height.max(1) as EGLint,
                EGL_NONE,
            ];
            eglCreatePbufferSurface(display, config, attribs.as_ptr())
        } else {
            let attribs = [EGL_NONE];
            eglCreateWindowSurface(
                display,
                config,
                native_window as EGLNativeWindowType,
                attribs.as_ptr(),
            )
        };
        if surface == EGL_NO_SURFACE {
            eglTerminate(display);
            return Err(format!(
                "eglCreate{}Surface failed (0x{:x})",
                if native_window.is_null() {
                    "Pbuffer"
                } else {
                    "Window"
                },
                eglGetError()
            ));
        }

        Ok(Self {
            display,
            config,
            surface,
            width: width.max(1),
            height: height.max(1),
            owns_display: true,
        })
    }

    pub fn size(&self) -> (u32, u32) {
        (self.width, self.height)
    }

    pub fn create_gl_context(&self, version: GLVersion) -> Result<GLContext, String> {
        let client_version = match version {
            GLVersion::GLES11 => 1,
            GLVersion::GLES20 => 2,
            GLVersion::GLES30 => 3,
            GLVersion::GL21Compat | GLVersion::GL33Core => {
                return Err(format!("{version:?} is not available through EGL"));
            }
        };
        let attribs = [EGL_CONTEXT_CLIENT_VERSION, client_version, EGL_NONE];
        let context = unsafe {
            eglCreateContext(self.display, self.config, EGL_NO_CONTEXT, attribs.as_ptr())
        };
        if context == EGL_NO_CONTEXT {
            return Err(format!(
                "eglCreateContext(ES{client_version}) failed (0x{:x})",
                unsafe { eglGetError() }
            ));
        }
        Ok(GLContext {
            display: self.display,
            context,
        })
    }

    pub unsafe fn make_gl_context_current(&self, gl_ctx: &GLContext) {
        eglMakeCurrent(self.display, self.surface, self.surface, gl_ctx.context);
    }

    pub unsafe fn make_current_none(&self) {
        eglMakeCurrent(self.display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    }

    pub fn gl_get_proc_address(&self, procname: &str) -> *const c_void {
        let Ok(name) = CString::new(procname) else {
            return ptr::null();
        };
        unsafe { eglGetProcAddress(name.as_ptr()) as *const c_void }
    }

    pub fn swap_buffers(&self) -> bool {
        unsafe { eglSwapBuffers(self.display, self.surface) == EGL_TRUE }
    }
}

impl Drop for Window {
    fn drop(&mut self) {
        unsafe {
            if self.surface != EGL_NO_SURFACE {
                eglDestroySurface(self.display, self.surface);
            }
            if self.owns_display && self.display != EGL_NO_DISPLAY {
                eglTerminate(self.display);
            }
        }
    }
}

unsafe fn choose_config(
    display: EGLDisplay,
    surface_bit: EGLint,
    renderable_bit: EGLint,
) -> Option<EGLConfig> {
    for with_buffers in [true, false] {
        let mut attribs = vec![
            EGL_SURFACE_TYPE,
            surface_bit,
            EGL_RENDERABLE_TYPE,
            renderable_bit,
            EGL_RED_SIZE,
            8,
            EGL_GREEN_SIZE,
            8,
            EGL_BLUE_SIZE,
            8,
        ];
        if with_buffers {
            attribs.extend_from_slice(&[
                EGL_ALPHA_SIZE,
                8,
                EGL_DEPTH_SIZE,
                24,
                EGL_STENCIL_SIZE,
                8,
            ]);
        }
        attribs.push(EGL_NONE);

        let mut config: EGLConfig = ptr::null_mut();
        let mut count: EGLint = 0;
        if eglChooseConfig(display, attribs.as_ptr(), &mut config, 1, &mut count) != EGL_FALSE
            && count > 0
            && !config.is_null()
        {
            return Some(config);
        }
    }
    None
}
