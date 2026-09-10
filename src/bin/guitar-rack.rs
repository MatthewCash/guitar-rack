use anyhow::Result;

fn main() -> Result<()> {
    env_logger::init();
    let runtime = std::env::var("GUITAR_RACK_RUNTIME_DIR")
        .or_else(|_| std::env::var("XDG_RUNTIME_DIR"))
        .unwrap_or_else(|_| "/tmp".into());
    let socket = format!("{runtime}/guitar-rack/control.sock");
    let result = guitar_rack::ui_bridge::run(&socket);
    anyhow::ensure!(result == 0, "Guitar Rack exited with {result}");
    Ok(())
}
