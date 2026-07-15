use std::path::Path;

use anyhow::{Context, Result};

use super::{UidAction, VfsAction};

pub fn handle_vfs(action: VfsAction) -> Result<()> {
    let driver = crate::vfs::VfsDriver::open()
        .context("cannot open /dev/nomount -- is the kernel module loaded?")?;

    match action {
        VfsAction::Add { virtual_path, real_path } => {
            let vp = Path::new(&virtual_path);
            let rp = Path::new(&real_path);
            let is_dir = rp.is_dir();
            driver.add_rule(vp, rp, is_dir)?;
            println!("ok");
        }
        VfsAction::Del { virtual_path } => {
            let vp = Path::new(&virtual_path);
            driver.del_rule(vp, vp)?;
            println!("ok");
        }
        VfsAction::Clear => {
            driver.clear_all()?;
            println!("ok");
        }
        VfsAction::Enable => {
            driver.enable()?;
            println!("ok");
        }
        VfsAction::Disable => {
            driver.disable()?;
            println!("ok");
        }
        VfsAction::Refresh => {
            driver.refresh()?;
            println!("ok");
        }
        VfsAction::List => {
            let list = driver.get_list()?;
            if list.is_empty() {
                println!("no rules");
            } else {
                print!("{list}");
            }
        }
        VfsAction::QueryStatus => {
            let version = driver.get_version()?;
            match driver.get_status()? {
                Some(status) => println!(
                    "driver: v{version}  engine: {}  rules: {}",
                    if status.enabled { "active" } else { "inactive" },
                    status.rule_count
                ),
                None => println!(
                    "driver: v{version}  engine: unknown (GET_STATUS not supported by kernel)"
                ),
            }
        }
    }
    Ok(())
}

pub fn handle_uid(action: UidAction) -> Result<()> {
    let driver = crate::vfs::VfsDriver::open()
        .context("cannot open /dev/nomount -- is the kernel module loaded?")?;

    match action {
        UidAction::Block { uid } => {
            driver.add_uid(uid)?;
            println!("ok");
        }
        UidAction::Unblock { uid } => {
            driver.del_uid(uid)?;
            println!("ok");
        }
    }
    Ok(())
}
