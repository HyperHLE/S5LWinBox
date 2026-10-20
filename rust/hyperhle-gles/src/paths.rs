/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 */
//! Host storage for decoded-texture caches.
//!
//! HyperHLE uses `user_data_base_path()` for its PVRTC disk cache. The
//! standalone library defaults to the process temporary directory; the
//! embedding app can override it from JNI/EGL setup code.

use std::borrow::Cow;
use std::path::{Path, PathBuf};
use std::sync::OnceLock;

static BASE: OnceLock<PathBuf> = OnceLock::new();

/// Sets the directory used for caches. Only the first call has an effect.
pub fn set_user_data_base_path(path: PathBuf) {
    let _ = BASE.set(path);
}

/// Directory used for caches.
pub fn user_data_base_path() -> Cow<'static, Path> {
    static FALLBACK: OnceLock<PathBuf> = OnceLock::new();
    let path = BASE
        .get()
        .unwrap_or_else(|| FALLBACK.get_or_init(std::env::temp_dir));
    Cow::Borrowed(path.as_path())
}
