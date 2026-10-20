/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0.
 * If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 */
//! Facade over the GLES implementations extracted from HyperHLE.
//!
//! The backend modules are unmodified upstream files and refer to their
//! siblings through `super::`. Re-exporting the raw bindings and the traits
//! here reproduces the layout they expect.

pub use touchHLE_gl_bindings::gl21compat as gl21compat_raw;
pub use touchHLE_gl_bindings::gl33core as gl33core_raw;
pub use touchHLE_gl_bindings::gles11 as gles11_raw;
pub use touchHLE_gl_bindings::gles2 as gles2_raw;
pub use touchHLE_gl_bindings::gles30 as gles30_raw;

pub mod gles_generic;
pub mod gles1_native;
pub mod gles2_native;
pub mod gles3_native;
pub mod util;

pub use gles_generic::{GLESContext, GLES};
