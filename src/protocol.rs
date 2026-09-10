use serde::{Deserialize, Serialize};

use crate::state::{CatalogPlugin, RackState};

#[derive(Debug, Deserialize, Serialize)]
#[serde(tag = "type", rename_all = "snake_case")]
pub enum Command {
    GetState,
    Add { uri: String, name: String },
    Remove { id: String },
    Move { id: String, offset: i32 },
    SetParameter { id: String, index: u32, value: f32 },
    SetBypass { id: String, bypassed: bool },
    SetMasterVolume { value: f32 },
    SetPreset { bank: String, preset: String },
    ShowUi { id: String },
    Save,
    ImportGuitarix,
}

#[derive(Clone, Debug, Deserialize, Serialize)]
#[serde(tag = "type", rename_all = "snake_case")]
pub enum Message {
    Snapshot {
        state: RackState,
        catalog: Vec<CatalogPlugin>,
    },
    Status {
        status: String,
        detail: String,
    },
    Tuner {
        frequency: f32,
    },
    Error {
        message: String,
    },
}
