/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 */
//! Image decoding shim for the GLES core.
//!
//! Only PVRTC is needed by the GLES code; the decompressor itself lives in
//! `native/pvrt_decompress_wrapper` and is built by `build.rs`.

use std::ffi::c_void;

extern "C" {
    fn touchHLE_decompress_pvrtc(
        pvrtc_data: *const c_void,
        is_2bit: bool,
        width: u32,
        height: u32,
        rgba_data: *mut u8,
    ) -> u32;
}

/// Decodes Imagination Technologies' PVRTC texture compression format to
/// RGBA (8 bits per channel).
pub fn decode_pvrtc(pvrtc_data: &[u8], is_2bit: bool, width: u32, height: u32) -> Vec<u32> {
    // This formula is from the IMG_texture_compression_pvrtc extension spec.
    let expected_size = if is_2bit {
        (width.max(16) as usize * height.max(8) as usize * 2).div_ceil(8)
    } else {
        (width.max(8) as usize * height.max(8) as usize * 4).div_ceil(8)
    };
    assert!(pvrtc_data.len() == expected_size);

    let rgba8_word_count = width as usize * height as usize;
    let mut rgba8_data = Vec::with_capacity(rgba8_word_count);
    unsafe {
        let consumed_size = touchHLE_decompress_pvrtc(
            pvrtc_data.as_ptr() as *const c_void,
            is_2bit,
            width,
            height,
            rgba8_data.as_mut_ptr() as *mut u8,
        );
        assert_eq!(consumed_size as usize, expected_size);
        rgba8_data.set_len(rgba8_word_count);
    }
    rgba8_data
}
