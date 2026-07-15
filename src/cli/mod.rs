pub mod handlers;

use clap::{Parser, Subcommand};

#[derive(Parser)]
#[command(
    name = "nomount",
    version = env!("CARGO_PKG_VERSION"),
    about = "NoMount metamodule + CLI for the kernel VFS driver (/dev/nomount)"
)]
pub struct Cli {
    #[command(subcommand)]
    pub command: Commands,
}

#[derive(Subcommand)]
pub enum Commands {
    /// Metamodule mount pass: scan /data/adb/modules, inject rules, enable engine
    Mount,
    /// VFS driver operations (add/del/clear rules, enable/disable engine)
    Vfs {
        #[command(subcommand)]
        action: VfsAction,
    },
    /// UID exclusion management
    Uid {
        #[command(subcommand)]
        action: UidAction,
    },
    /// Print version
    Version,
}

#[derive(Subcommand)]
pub enum VfsAction {
    /// Add a redirection rule (virtual_path -> real_path)
    Add { virtual_path: String, real_path: String },
    /// Delete a rule by virtual path
    Del { virtual_path: String },
    /// Clear all rules
    Clear,
    /// Enable the VFS engine
    Enable,
    /// Disable the VFS engine
    Disable,
    /// Flush the dcache
    Refresh,
    /// List active rules
    List,
    /// Query engine enabled state + rule count
    QueryStatus,
}

#[derive(Subcommand)]
pub enum UidAction {
    /// Exclude a UID from redirection
    Block { uid: u32 },
    /// Re-include a UID in redirection
    Unblock { uid: u32 },
}
