use std::ffi::CString;
use std::path::Path;

use thiserror::Error;

// -- FFI structs matching kernel's nomount_ioctl_data --

/// ARM64 layout: two 8-byte pointers + 4-byte flags + 4-byte padding = 24 bytes
#[cfg(target_pointer_width = "64")]
#[repr(C)]
pub struct IoctlData {
    pub virtual_path: *const libc::c_char,
    pub real_path: *const libc::c_char,
    pub flags: u32,
    pub _pad: u32,
}

/// ARM32 layout: two 4-byte pointers + 4-byte flags = 12 bytes
#[cfg(target_pointer_width = "32")]
#[repr(C)]
pub struct IoctlData {
    pub virtual_path: *const libc::c_char,
    pub real_path: *const libc::c_char,
    pub flags: u32,
}

/// Flags for IoctlData.flags, matching kernel constants
pub const ZM_ACTIVE: u32 = 1;
pub const ZM_DIR: u32 = 128;

/// Owned version of IoctlData that holds CStrings for lifetime safety.
/// The raw IoctlData pointers borrow from these CStrings.
pub struct VfsRule {
    pub virtual_path: CString,
    pub real_path: CString,
    pub is_dir: bool,
}

impl VfsRule {
    pub fn new(virtual_path: &Path, real_path: &Path, is_dir: bool) -> Result<Self, IoctlError> {
        let vp = CString::new(
            virtual_path
                .to_str()
                .ok_or_else(|| IoctlError::InvalidPath(virtual_path.display().to_string()))?,
        )
        .map_err(|_| IoctlError::InvalidPath(virtual_path.display().to_string()))?;

        let rp = CString::new(
            real_path
                .to_str()
                .ok_or_else(|| IoctlError::InvalidPath(real_path.display().to_string()))?,
        )
        .map_err(|_| IoctlError::InvalidPath(real_path.display().to_string()))?;

        Ok(Self {
            virtual_path: vp,
            real_path: rp,
            is_dir,
        })
    }

    /// Build the raw FFI struct. The returned IoctlData borrows from self --
    /// caller must ensure self outlives any ioctl call using the result.
    pub fn as_ioctl_data(&self) -> IoctlData {
        let flags = ZM_ACTIVE | if self.is_dir { ZM_DIR } else { 0 };

        IoctlData {
            virtual_path: self.virtual_path.as_ptr(),
            real_path: self.real_path.as_ptr(),
            flags,
            #[cfg(target_pointer_width = "64")]
            _pad: 0,
        }
    }
}

// -- VFS engine status (for GET_STATUS ioctl, may not exist on older kernels) --

#[derive(Debug, Clone)]
pub struct VfsStatus {
    pub enabled: bool,
    pub rule_count: u32,
}

// -- Errors --

#[derive(Debug, Error)]
pub enum IoctlError {
    #[error("failed to open /dev/nomount: {0} (errno {1})")]
    OpenFailed(String, i32),

    #[error("ioctl {name} failed: {msg} (errno {errno})")]
    IoctlFailed {
        name: &'static str,
        msg: String,
        errno: i32,
    },

    #[error("invalid path: {0}")]
    InvalidPath(String),

}
