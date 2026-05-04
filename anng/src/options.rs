//! Internal helpers for setting NNG options on sockets, contexts, dialers, and listeners.
//!
//! Each helper centralizes the bounds check on the value, the unsafe FFI call, and the
//! `unreachable!` arms for errnos that NNG documents but that callers in this crate are
//! statically responsible for never triggering. Callers in the protocol modules pass the
//! `nng_sys::NNG_OPT_*` byte slice and a human-readable label.
//!
//! ## Future maintenance
//!
//! Today only `_set_ms` exists, so we have one pair (`set_socket_ms`, `set_ctx_ms`) that share
//! ~90% of their bodies. When a second value type lands (`_set_int`, `_set_bool`, `_set_size`,
//! `_set_string`), do **not** keep copy-pasting: that's the moment to extract a private trait
//! over the handle (e.g. `trait OptionTarget { fn set_ms(...); fn set_int(...); ... }`) with
//! impls for `nng_socket` and `nng_ctx`, and rewrite the helpers as a single generic over the
//! trait. Doing it now (with one value type) would be premature; doing it after the third would
//! be painful.

use core::{
    ffi::{CStr, c_int},
    time::Duration,
};
use nng_sys::ErrorCode;

/// Sets a `nng_duration` (ms) option on a socket, panicking on any NNG-reported error.
///
/// Every `nng_socket_set_ms` failure is a programming error at this layer (closed handle, unknown
/// option, read-only option, invalid duration, or any errno NNG hasn't documented). The caller is
/// statically responsible for pairing a valid socket with a valid option supported by the
/// socket's protocol; the `label` argument is a human-readable name used in the panic messages
/// (e.g. `"resend time"`).
///
/// Note: NNG defines sentinel duration values (`NNG_DURATION_INFINITE`, `NNG_DURATION_DEFAULT`)
/// as negative numbers. They cannot be expressed via [`Duration`] and are deliberately not
/// supported by this helper — protocol-level helpers should expose them through their own API
/// surface if needed.
pub(crate) fn set_socket_ms(
    socket: nng_sys::nng_socket,
    option: &'static [u8],
    duration: Duration,
    label: &'static str,
) {
    let ms = duration_to_nng_ms(duration, label);
    let option_cstr = CStr::from_bytes_with_nul(option)
        .unwrap_or_else(|e| panic!("option name is not a valid C string: {e}"));
    // SAFETY: caller guarantees the socket is valid and supports the named option;
    //         `option_cstr` is built from a `&'static [u8]`, so its `.as_ptr()` is valid for the FFI call.
    let raw_errno = unsafe { nng_sys::nng_socket_set_ms(socket, option_cstr.as_ptr(), ms) };
    let errno = u32::try_from(raw_errno).expect("errno is never negative");
    check_set_ms_errno(errno, label, "nng_socket_set_ms");
}

/// Sets a `nng_duration` (ms) option on a context. See [`set_socket_ms`] for invariants.
pub(crate) fn set_ctx_ms(
    ctx: nng_sys::nng_ctx,
    option: &'static [u8],
    duration: Duration,
    label: &'static str,
) {
    let ms = duration_to_nng_ms(duration, label);
    let option_cstr = CStr::from_bytes_with_nul(option)
        .unwrap_or_else(|e| panic!("option name is not a valid C string: {e}"));
    // SAFETY: caller guarantees the context is valid and supports the named option;
    //         `option_str` is built from a `&'static [u8]`, so its `.as_ptr()` is valid for the FFI call.
    let raw_errno = unsafe { nng_sys::nng_ctx_set_ms(ctx, option_cstr.as_ptr(), ms) };
    let errno = u32::try_from(raw_errno).expect("errno is never negative");
    check_set_ms_errno(errno, label, "nng_ctx_set_ms");
}

fn duration_to_nng_ms(duration: Duration, label: &'static str) -> c_int {
    let ms = duration.as_millis();
    if ms > i32::MAX as u128 {
        panic!("{label} is too large: {duration:?}");
    }
    ms as c_int
}

fn check_set_ms_errno(errno: u32, label: &'static str, fn_name: &str) {
    match errno {
        0 => {}
        e if e == ErrorCode::ECLOSED as u32 => {
            unreachable!("{fn_name}({label}): NNG returned ECLOSED ({errno}) unexpectedly");
        }
        e if e == ErrorCode::EINVAL as u32 => {
            unreachable!(
                "{fn_name}({label}): NNG rejected duration as invalid via EINVAL ({errno})"
            );
        }
        e if e == ErrorCode::ENOTSUP as u32 => {
            unreachable!(
                "{fn_name}({label}): option not supported on this protocol/scope — NNG returned ENOTSUP ({errno})"
            );
        }
        e if e == ErrorCode::EREADONLY as u32 => {
            unreachable!(
                "{fn_name}({label}): option is read-only — NNG returned EREADONLY ({errno})"
            );
        }
        _ => {
            unreachable!("{fn_name}({label}) returned undocumented errno {errno}");
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn duration_to_nng_ms_accepts_zero() {
        assert_eq!(duration_to_nng_ms(Duration::ZERO, "test"), 0);
    }

    #[test]
    fn duration_to_nng_ms_accepts_max_i32() {
        assert_eq!(
            duration_to_nng_ms(Duration::from_millis(i32::MAX as u64), "test"),
            i32::MAX,
        );
    }

    #[test]
    #[should_panic(expected = "test is too large")]
    fn duration_to_nng_ms_rejects_overflow() {
        // Duration::MAX is the worst case a caller can hand us; it must be caught.
        duration_to_nng_ms(Duration::MAX, "test");
    }
}
