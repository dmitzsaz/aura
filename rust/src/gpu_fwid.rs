// SPDX-License-Identifier: MIT

use sha2::{Digest, Sha256};

/// Hash a private copy of loaded GPU TEXT after the caller normalizes it.
///
/// # Safety
/// `data` must hold `len` readable bytes and `out` must hold 32 writable bytes.
#[no_mangle]
pub unsafe extern "C" fn rust_gpu_text_sha256(data: *const u8, len: usize, out: *mut u8) {
    // SAFETY: The caller owns both buffers for this call.
    let digest = Sha256::digest(unsafe { core::slice::from_raw_parts(data, len) });
    unsafe { core::ptr::copy_nonoverlapping(digest.as_ptr(), out, digest.len()) };
}
