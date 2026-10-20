/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 */
//! Logging shim.
//!
//! HyperHLE's GLES core logs through `log!`, `log_dbg!` and `log_once!`.
//! The standalone library routes them to logcat instead of touchHLE's log
//! file/terminal output.

use std::os::raw::c_char;

/// Module paths for which `log_dbg!` is enabled. Empty by default; a host can
/// flip this later if per-module debug logging becomes useful.
pub static ENABLED_MODULES: &[&str] = &[];

/// Writes one already-formatted line to the host log.
pub fn write_log(msg: &str) {
    #[cfg(target_os = "android")]
    unsafe {
        extern "C" {
            fn __android_log_write(prio: i32, tag: *const c_char, text: *const c_char) -> i32;
        }
        const ANDROID_LOG_INFO: i32 = 4;
        if let Ok(text) = std::ffi::CString::new(msg) {
            static TAG: &[u8] = b"S5LBox-GLES\0";
            __android_log_write(
                ANDROID_LOG_INFO,
                TAG.as_ptr() as *const c_char,
                text.as_ptr(),
            );
        }
    }
    #[cfg(not(target_os = "android"))]
    {
        eprintln!("{msg}");
    }
}

/// Prints a message (with implicit newline).
#[macro_export]
macro_rules! echo {
    ($($arg:tt)+) => {
        $crate::log::write_log(&format!($($arg)+))
    };
}

/// Prints a log message unconditionally, prefixed with the module path.
#[macro_export]
macro_rules! log {
    ($($arg:tt)+) => {
        $crate::echo!("{}: {}", module_path!(), format_args!($($arg)+))
    };
}

/// Same as [log], but never panics.
#[macro_export]
macro_rules! log_no_panic {
    ($($arg:tt)+) => {
        $crate::log!($($arg)+)
    };
}

/// Like [log], but only when debugging is enabled for the calling module.
#[macro_export]
macro_rules! log_dbg {
    ($($arg:tt)+) => {
        if $crate::log::ENABLED_MODULES.contains(&module_path!()) {
            $crate::log!($($arg)+);
        }
    };
}

/// Like [log], but logs only once. Must be given a literal message.
#[macro_export]
macro_rules! log_once {
    ($msg:literal) => {{
        static LOG_ONCE: std::sync::Once = std::sync::Once::new();
        LOG_ONCE.call_once(|| {
            $crate::log!("{} [this log will only be shown once]", $msg);
        });
    }};
}
