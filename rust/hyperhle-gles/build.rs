/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 */
//! Builds the PVRTC software decompressor (Imagination Technologies code,
//! vendored under `third_party/PVRTDecompress`).
//!
//! The decompressor is only needed for the real targets. Skipping it on the
//! host keeps `cargo check` usable without a C++ toolchain; `cargo check`
//! does not link, so the missing symbol is not a problem there.

use std::path::PathBuf;

fn main() {
    let manifest = PathBuf::from(std::env::var("CARGO_MANIFEST_DIR").unwrap());
    let wrapper = manifest.join("native/pvrt_decompress_wrapper/lib.cpp");
    let vendor = manifest.join("third_party/PVRTDecompress");

    println!("cargo:rerun-if-changed={}", wrapper.display());
    println!(
        "cargo:rerun-if-changed={}",
        vendor.join("PVRTDecompress.cpp").display()
    );
    println!(
        "cargo:rerun-if-changed={}",
        vendor.join("PVRTDecompress.h").display()
    );

    let target_os = std::env::var("CARGO_CFG_TARGET_OS").unwrap_or_default();
    if target_os == "android" || std::env::var_os("HYPERHLE_FORCE_PVRTC_BUILD").is_some() {
        let mut build = cc::Build::new();
        build
            .cpp(true)
            .file(&wrapper)
            .include(&vendor)
            .warnings(false);
        if target_os == "android" {
            build.cpp_link_stdlib(Some("c++_static"));
            println!("cargo:rustc-link-arg=-static-libstdc++");
        }
        build.compile("hyperhle_pvrtc");
    }
}
