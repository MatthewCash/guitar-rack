use std::{fs::OpenOptions, os::fd::AsRawFd, path::PathBuf, process::Stdio, time::Duration};

use anyhow::{Context, Result, anyhow};
use guitar_rack::protocol::{Command, Message};
use tokio::{
    io::{AsyncBufReadExt, AsyncWriteExt, BufReader, Lines},
    net::{UnixListener, UnixStream},
    process::{Child, ChildStdin, ChildStdout, Command as ProcessCommand},
    signal::unix::{SignalKind, signal},
    sync::{broadcast, mpsc, watch},
    time::{Instant, Sleep, sleep, timeout},
};

const LOCK_EXCLUSIVE: i32 = 2;
const LOCK_NONBLOCKING: i32 = 4;
const READY_LINE: &str = "guitar-rack-dsp-ready";

unsafe extern "C" {
    fn flock(fd: i32, operation: i32) -> i32;
}

struct DspProcess {
    child: Child,
    stdin: ChildStdin,
    detail: String,
}

impl DspProcess {
    async fn start(
        messages: mpsc::UnboundedSender<Message>,
        bank: &str,
        preset: &str,
    ) -> Result<Self> {
        let executable = required_env("GUITAR_RACK_DSP")?;
        let instance =
            std::env::var("GUITAR_RACK_INSTANCE").unwrap_or_else(|_| "guitar-rack-dsp".into());
        let preset_dir = std::env::var_os("GUITAR_RACK_PRESET_DIR")
            .map(PathBuf::from)
            .unwrap_or_else(|| home_dir().join(".config/guitarix/banks"));

        let mut child = ProcessCommand::new(&executable)
            .arg("--bank")
            .arg(bank)
            .arg("--preset")
            .arg(preset)
            .arg("--preset-dir")
            .arg(&preset_dir)
            .arg("--instance")
            .arg(&instance)
            .stdin(Stdio::piped())
            .stdout(Stdio::piped())
            .stderr(Stdio::piped())
            .kill_on_drop(true)
            .spawn()
            .with_context(|| format!("failed to start {executable}"))?;

        let stdin = child.stdin.take().context("DSP child has no stdin")?;
        let stdout = child.stdout.take().context("DSP child has no stdout")?;
        let stderr = child.stderr.take().context("DSP child has no stderr")?;
        tokio::spawn(async move {
            let mut lines = BufReader::new(stderr).lines();
            while let Ok(Some(line)) = lines.next_line().await {
                log::info!("dsp: {line}");
            }
        });

        let mut stdout = BufReader::new(stdout).lines();
        let ready = timeout(Duration::from_secs(20), wait_for_ready(&mut stdout)).await;
        match ready {
            Ok(Ok(())) => {}
            Ok(Err(error)) => {
                stop_child(&mut child).await;
                return Err(error);
            }
            Err(_) => {
                stop_child(&mut child).await;
                return Err(anyhow!("DSP did not become ready within 20 seconds"));
            }
        }
        tokio::spawn(async move {
            while let Ok(Some(line)) = stdout.next_line().await {
                match serde_json::from_str(&line) {
                    Ok(message) => {
                        let _ = messages.send(message);
                    }
                    Err(error) => log::warn!("invalid DSP message ({error}): {line}"),
                }
            }
        });

        log::info!(
            "DSP ready: bank={bank:?}, preset={preset:?}, preset_dir={}",
            preset_dir.display()
        );
        Ok(Self {
            child,
            stdin,
            detail: format!("Guitarix DSP: {bank}/{preset}"),
        })
    }

    async fn send(&mut self, command: &Command) -> Result<()> {
        log::debug!("sending DSP command: {command:?}");
        self.stdin
            .write_all(serde_json::to_string(command)?.as_bytes())
            .await?;
        self.stdin.write_all(b"\n").await?;
        self.stdin.flush().await?;
        Ok(())
    }

    async fn wait(&mut self) -> Result<std::process::ExitStatus> {
        self.child
            .wait()
            .await
            .context("failed to wait for DSP child")
    }

    async fn stop(&mut self) {
        stop_child(&mut self.child).await;
    }
}

async fn wait_for_ready(lines: &mut Lines<BufReader<ChildStdout>>) -> Result<()> {
    while let Some(line) = lines.next_line().await? {
        if line == READY_LINE {
            return Ok(());
        }
        log::info!("dsp: {line}");
    }
    Err(anyhow!("DSP exited before reporting readiness"))
}

async fn stop_child(child: &mut Child) {
    if let Some(pid) = child.id() {
        unsafe {
            libc::kill(pid as i32, libc::SIGTERM);
        }
    }
    if timeout(Duration::from_secs(3), child.wait()).await.is_err() {
        let _ = child.kill().await;
        let _ = child.wait().await;
    }
}

fn required_env(name: &str) -> Result<String> {
    std::env::var(name).with_context(|| format!("{name} is not configured"))
}

fn home_dir() -> PathBuf {
    std::env::var_os("HOME")
        .map(PathBuf::from)
        .unwrap_or_else(|| PathBuf::from("/tmp"))
}

fn socket_path() -> PathBuf {
    std::env::var_os("GUITAR_RACK_RUNTIME_DIR")
        .or_else(|| std::env::var_os("XDG_RUNTIME_DIR"))
        .map(PathBuf::from)
        .unwrap_or_else(|| PathBuf::from("/tmp"))
        .join("guitar-rack/control.sock")
}

async fn serve_client(
    stream: UnixStream,
    commands: mpsc::UnboundedSender<Command>,
    mut snapshot: watch::Receiver<Option<Message>>,
    mut status: watch::Receiver<Message>,
    mut events: broadcast::Receiver<Message>,
) -> Result<()> {
    let (reader, mut writer) = stream.into_split();
    let mut lines = BufReader::new(reader).lines();
    let initial_snapshot = { snapshot.borrow_and_update().clone() };
    if let Some(initial) = initial_snapshot {
        write_message(&mut writer, &initial).await?;
    }
    let initial_status = { status.borrow_and_update().clone() };
    write_message(&mut writer, &initial_status).await?;
    loop {
        tokio::select! {
            line = lines.next_line() => match line? {
                Some(line) => match serde_json::from_str(&line) {
                    Ok(command) => { let _ = commands.send(command); }
                    Err(error) => {
                        let message = Message::Error { message: error.to_string() };
                        write_message(&mut writer, &message).await?;
                    }
                },
                None => return Ok(()),
            },
            changed = snapshot.changed() => {
                changed?;
                let message = { snapshot.borrow_and_update().clone() };
                if let Some(message) = message {
                    write_message(&mut writer, &message).await?;
                }
            }
            changed = status.changed() => {
                changed?;
                let message = { status.borrow_and_update().clone() };
                write_message(&mut writer, &message).await?;
            }
            event = events.recv() => match event {
                Ok(message) => write_message(&mut writer, &message).await?,
                Err(broadcast::error::RecvError::Lagged(_)) => {},
                Err(broadcast::error::RecvError::Closed) => return Ok(()),
            }
        }
    }
}

async fn write_message(
    writer: &mut tokio::net::unix::OwnedWriteHalf,
    message: &Message,
) -> Result<()> {
    writer
        .write_all(serde_json::to_string(message)?.as_bytes())
        .await?;
    writer.write_all(b"\n").await?;
    Ok(())
}

fn status(status: &str, detail: impl Into<String>) -> Message {
    Message::Status {
        status: status.into(),
        detail: detail.into(),
    }
}

fn reset_retry(retry: &mut std::pin::Pin<&mut Sleep>, delay: Duration) {
    retry.as_mut().reset(Instant::now() + delay);
}

#[tokio::main(flavor = "current_thread")]
async fn main() -> Result<()> {
    env_logger::Builder::from_env(env_logger::Env::default().default_filter_or("info")).init();
    let path = socket_path();
    tokio::fs::create_dir_all(path.parent().context("control socket has no parent")?).await?;
    let instance_lock = OpenOptions::new()
        .create(true)
        .write(true)
        .truncate(false)
        .open(path.with_extension("lock"))?;
    anyhow::ensure!(
        unsafe { flock(instance_lock.as_raw_fd(), LOCK_EXCLUSIVE | LOCK_NONBLOCKING) } == 0,
        "another Guitar Rack engine is already running"
    );
    if tokio::fs::try_exists(&path).await? {
        tokio::fs::remove_file(&path).await?;
    }

    let listener = UnixListener::bind(&path)?;
    let (command_sender, mut command_receiver) = mpsc::unbounded_channel();
    let (dsp_message_sender, mut dsp_message_receiver) = mpsc::unbounded_channel();
    let (snapshot_sender, snapshot_receiver) = watch::channel(None);
    let (status_sender, status_receiver) =
        watch::channel(status("recovering", "Starting Guitarix DSP"));
    let (event_sender, _) = broadcast::channel(64);
    let mut terminate = signal(SignalKind::terminate())?;
    let mut child: Option<DspProcess> = None;
    let mut selected_bank = std::env::var("GUITAR_RACK_BANK").unwrap_or_else(|_| "my stuff".into());
    let mut selected_preset = std::env::var("GUITAR_RACK_PRESET").unwrap_or_else(|_| "main".into());
    let retry = sleep(Duration::ZERO);
    tokio::pin!(retry);

    loop {
        tokio::select! {
            accepted = listener.accept() => {
                let (stream, _) = accepted?;
                tokio::spawn(serve_client(
                    stream,
                    command_sender.clone(),
                    snapshot_receiver.clone(),
                    status_receiver.clone(),
                    event_sender.subscribe(),
                ));
            }
            _ = &mut retry, if child.is_none() => {
                match DspProcess::start(
                    dsp_message_sender.clone(),
                    &selected_bank,
                    &selected_preset,
                ).await {
                    Ok(process) => {
                        let detail = process.detail.clone();
                        child = Some(process);
                        status_sender.send_replace(status("online", detail));
                    }
                    Err(error) => {
                        log::warn!("DSP recovery failed: {error:#}");
                        status_sender.send_replace(status("recovering", format!("DSP restart: {error}")));
                        reset_retry(&mut retry, Duration::from_secs(1));
                    }
                }
            }
            result = async { child.as_mut().expect("guarded by select condition").wait().await }, if child.is_some() => {
                let detail = match result {
                    Ok(exit) => format!("DSP exited with {exit}; restarting"),
                    Err(error) => format!("DSP wait failed: {error}; restarting"),
                };
                log::warn!("{detail}");
                child = None;
                status_sender.send_replace(status("recovering", detail));
                let _ = event_sender.send(Message::Tuner { frequency: 0.0 });
                reset_retry(&mut retry, Duration::from_secs(1));
            }
            Some(command) = command_receiver.recv() => {
                let supported = matches!(
                    &command,
                    Command::GetState
                        | Command::SetParameter { .. }
                        | Command::SetBypass { .. }
                        | Command::SetMasterVolume { .. }
                        | Command::SetPreset { .. }
                );
                if supported {
                    if let Some(process) = child.as_mut() {
                        if let Err(error) = process.send(&command).await {
                            let _ = event_sender.send(Message::Error { message: error.to_string() });
                        } else if let Command::SetPreset { bank, preset } = command {
                            process.detail = format!("Guitarix DSP: {bank}/{preset}");
                            selected_bank = bank;
                            selected_preset = preset;
                        }
                    } else {
                        let _ = event_sender.send(Message::Error { message: "The Guitarix DSP is recovering".into() });
                    }
                } else {
                    let _ = event_sender.send(Message::Error {
                        message: "The loaded Guitarix preset owns rack structure; add, remove, move, save, and native UI are unavailable".into(),
                    });
                }
            }
            Some(message) = dsp_message_receiver.recv() => {
                match message {
                    message @ Message::Snapshot { .. } => {
                        snapshot_sender.send_replace(Some(message));
                    }
                    message @ Message::Status { .. } => {
                        status_sender.send_replace(message);
                    }
                    message => {
                        let _ = event_sender.send(message);
                    }
                }
            }
            _ = tokio::signal::ctrl_c() => break,
            _ = terminate.recv() => break,
        }
    }

    if let Some(mut process) = child {
        process.stop().await;
    }
    let _ = tokio::fs::remove_file(path).await;
    Ok(())
}
