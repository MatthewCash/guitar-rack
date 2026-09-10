use std::{collections::BTreeMap, path::Path};

use anyhow::{Context, Result};
use serde::{Deserialize, Serialize};

pub const STATE_VERSION: u32 = 11;

#[derive(Clone, Debug, Default, Deserialize, Serialize)]
pub struct RackState {
    pub version: u32,
    pub preset: String,
    #[serde(default)]
    pub master_volume_db: f32,
    pub plugins: Vec<PluginState>,
}

#[derive(Clone, Debug, Deserialize, Serialize)]
pub struct PluginState {
    pub id: String,
    pub uri: String,
    pub name: String,
    pub bypassed: bool,
    #[serde(default)]
    pub fixed: bool,
    pub parameters: Vec<Parameter>,
    #[serde(default)]
    pub values: BTreeMap<u32, f32>,
}

#[derive(Clone, Debug, Deserialize, Serialize)]
pub struct Parameter {
    pub index: u32,
    pub name: String,
    pub symbol: String,
    pub value: f32,
    pub default: f32,
    pub minimum: f32,
    pub maximum: f32,
    pub step: f32,
    pub hints: u32,
}

#[derive(Clone, Debug, Deserialize, Serialize)]
pub struct CatalogPlugin {
    pub uri: String,
    pub name: String,
}

impl RackState {
    pub fn empty() -> Self {
        Self {
            version: STATE_VERSION,
            preset: "Default".into(),
            master_volume_db: 0.0,
            plugins: Vec::new(),
        }
    }

    pub async fn load(path: &Path) -> Result<Self> {
        match tokio::fs::read(path).await {
            Ok(data) => serde_json::from_slice(&data)
                .with_context(|| format!("failed to parse {}", path.display())),
            Err(error) if error.kind() == std::io::ErrorKind::NotFound => Ok(Self::empty()),
            Err(error) => Err(error).with_context(|| format!("failed to read {}", path.display())),
        }
    }

    pub async fn save(&self, path: &Path) -> Result<()> {
        let parent = path.parent().context("rack state path has no parent")?;
        tokio::fs::create_dir_all(parent).await?;
        let temporary = path.with_extension("json.tmp");
        tokio::fs::write(&temporary, serde_json::to_vec_pretty(self)?).await?;
        tokio::fs::rename(&temporary, path).await?;
        Ok(())
    }
}
