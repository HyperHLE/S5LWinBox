/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0.
 * If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 */
//! `hyperhle-gles`: a standalone library exposing HyperHLE's OpenGL ES
//! implementation (native ES 1.1 / ES 2.0 / ES 3.0 backends) through a small
//! C ABI.
//!
//! The GLES sources under `src/gles/` are vendored from HyperHLE (a touchHLE
//! fork, MPL-2.0). Only two things were replaced for embedding: SDL2 window
//! management (`window.rs`, now EGL) and touchHLE's logging/paths/image
//! plumbing (`log.rs`, `paths.rs`, `image.rs`).

#![allow(clippy::all)]
#![allow(non_snake_case)]
#![allow(non_upper_case_globals)]
#![allow(dead_code)]
#![allow(unused_imports)]
#![allow(improper_ctypes)]

#[macro_use]
mod log;

pub mod ffi;
pub mod gles;
pub mod image;
pub mod matrix;
pub mod paths;
pub mod window;
