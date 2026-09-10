use std::{cmp::Ordering, collections::BTreeMap, path::Path};

use anyhow::{Context, Result};
use serde_json::{Map, Value};

#[derive(Debug)]
pub struct ImportedPlugin {
    pub uri: &'static str,
    pub name: &'static str,
    pub source_prefix: &'static str,
    pub values: BTreeMap<String, f32>,
    strings: BTreeMap<String, String>,
    position: f64,
    pub fixed: bool,
}

const MAPPINGS: &[(&str, &str, &str)] = &[
    (
        "amp.clip",
        "https://faustlv2.bitbucket.io/softclip",
        "Guitarix Soft Clip",
    ),
    (
        "JCM800Pre",
        "http://guitarix.sourceforge.net/plugins/gx_jcm800pre_#_jcm800pre_",
        "GxJCM800Pre",
    ),
    (
        "bossds1",
        "http://guitarix.sourceforge.net/plugins/gx_bossds1_#_bossds1_",
        "GxBoss DS1",
    ),
    (
        "cab_st",
        "http://guitarix.sourceforge.net/plugins/gx_cabinet#CABINET",
        "GxCabinet",
    ),
    (
        "eq",
        "https://faustlv2.bitbucket.io/peak_eq",
        "Guitarix Peak EQ",
    ),
    (
        "graphiceq",
        "http://guitarix.sourceforge.net/plugins/gx_graphiceq_#_graphiceq_",
        "GxGraphicEQ",
    ),
    (
        "gx_distortion",
        "https://faustlv2.bitbucket.io/gx_distortion",
        "Gx Multi Band Distortion",
    ),
    (
        "jconv",
        "http://tomszilagyi.github.io/plugins/lv2/ir",
        "EVH 5150 III",
    ),
    (
        "mxrdis",
        "http://guitarix.sourceforge.net/plugins/gx_mxrdist_#_mxrdist_",
        "GxMXRDistortion",
    ),
    (
        "stereoverb",
        "http://guitarix.sourceforge.net/plugins/gx_reverb_stereo#_reverb_stereo",
        "GxReverb",
    ),
    (
        "antyfizz",
        "https://faustlv2.bitbucket.io/fizz_remover",
        "Guitarix Anti-Fizz",
    ),
];

const FIXED_STAGES: &[(&str, &str, &str)] = &[
    (
        "dcblocker",
        "https://faustlv2.bitbucket.io/dcblocker",
        "Guitarix DC Blocker",
    ),
    (
        "gxamp",
        "https://faustlv2.bitbucket.io/gxamp_stereo",
        "Guitarix 12AX7 Preamp",
    ),
    (
        "bassbooster",
        "https://faustlv2.bitbucket.io/bassbooster",
        "Guitarix Bassbooster",
    ),
];

const UNSUPPORTED_STAGES: &[&str] = &[];

impl ImportedPlugin {
    pub fn parameter_value(&self, symbol: &str) -> Option<f32> {
        let generated = matches!(
            self.source_prefix,
            "amp.clip"
                | "eq"
                | "gx_distortion"
                | "antyfizz"
                | "bassbooster"
                | "dcblocker"
                | "gxamp"
        );
        let symbol = if generated {
            symbol
                .rsplit_once('_')
                .filter(|(_, suffix)| suffix.chars().all(|character| character.is_ascii_digit()))
                .map_or(symbol, |(base, _)| base)
        } else {
            symbol
        };
        let symbol = normalize(symbol);
        match (self.source_prefix, symbol.as_str()) {
            ("amp.clip", "ampfuzz") => self.values.get("ampfuzz").copied(),
            ("bassbooster", "bassboosterlevel") => self.values.get("level").copied(),
            ("gxamp", "gxdistortiondrive") => self.values.get("drive").copied(),
            ("gxamp", "gxdistortionwetdry") => self.values.get("wetdry").copied(),
            ("gxamp", "amp2stage1pregain") => self.values.get("pregain").copied(),
            ("gxamp", "amp2stage2gain1") => self.values.get("gain1").copied(),
            ("gxamp", "amphighgain") => self.values.get("highgain").copied(),
            ("cab_st", "clevel") => self.values.get("level").copied(),
            ("cab_st", "cbass") => self.values.get("bass").copied(),
            ("cab_st", "ctreble") => self.values.get("treble").copied(),
            ("cab_st", "cmodel") => self
                .strings
                .get("select")
                .and_then(|model| cabinet_model(model)),
            ("stereoverb", "drywet") => self.values.get("wetdry").copied(),
            ("stereoverb", "lfo") => self.values.get("lfofreq").copied(),
            ("stereoverb", "mode") => {
                self.strings
                    .get("invert")
                    .and_then(|mode| match mode.as_str() {
                        "linear" => Some(0.0),
                        "pingpong" => Some(1.0),
                        _ => None,
                    })
            }
            ("jconv", "portagcsw") => Some(0.0),
            ("jconv", "portdrysw") => Some(0.0),
            ("jconv", "portdrygain") => Some(0.0),
            ("jconv", "portwetsw") => Some(1.0),
            ("jconv", "portwetgain") => self.values.get("gain").copied(),
            _ => self.values.get(&symbol).copied(),
        }
    }
}

fn cabinet_model(model: &str) -> Option<f32> {
    [
        "4x12",
        "2x12",
        "1x12",
        "4x10",
        "2x10",
        "HighGain",
        "Twin",
        "Bassman",
        "Marshall",
        "AC-30",
        "Princeton",
        "A2",
        "1x15",
        "Mesa Boogie",
        "Briliant",
        "Vitalize",
        "Charisma",
    ]
    .iter()
    .position(|candidate| *candidate == model)
    .map(|index| index as f32)
}

pub async fn import(path: &Path, preset: &str) -> Result<(Vec<ImportedPlugin>, f32)> {
    let data = tokio::fs::read(path)
        .await
        .with_context(|| format!("failed to read Guitarix bank {}", path.display()))?;
    let document: Value = serde_json::from_slice(&data)?;
    let entries = document
        .as_array()
        .context("Guitarix bank is not an array")?;
    let preset_index = entries
        .iter()
        .position(|entry| entry.as_str() == Some(preset))
        .with_context(|| format!("Guitarix preset {preset:?} was not found"))?;
    let engine = entries
        .get(preset_index + 1)
        .and_then(Value::as_object)
        .and_then(|preset| preset.get("engine"))
        .and_then(Value::as_object)
        .context("Guitarix preset has no engine state")?;

    let unsupported = UNSUPPORTED_STAGES
        .iter()
        .filter(|prefix| {
            engine
                .get(&format!("{prefix}.on_off"))
                .and_then(Value::as_f64)
                .is_some_and(|enabled| enabled >= 0.5)
        })
        .copied()
        .collect::<Vec<_>>();
    if !unsupported.is_empty() {
        log::warn!(
            "Guitarix stages have no exact LV2 equivalent and were skipped: {}",
            unsupported.join(", ")
        );
    }

    let mut plugins = MAPPINGS
        .iter()
        .filter_map(|(prefix, uri, name)| import_plugin(engine, prefix, uri, name))
        .collect::<Vec<_>>();
    for (prefix, uri, name) in FIXED_STAGES {
        if prefix == &"bassbooster"
            && engine
                .get("amp.bass_boost.on_off")
                .and_then(Value::as_f64)
                .is_some_and(|enabled| enabled < 0.5)
        {
            continue;
        }
        let (values, strings) = if *prefix == "gxamp" {
            (collect_amp_parameters(engine), BTreeMap::new())
        } else {
            collect_parameters(engine, prefix)
        };
        plugins.push(ImportedPlugin {
            uri,
            name,
            source_prefix: prefix,
            values,
            strings,
            position: f64::MAX,
            fixed: true,
        });
    }
    plugins.sort_by(|left, right| {
        chain_rank(left.source_prefix)
            .cmp(&chain_rank(right.source_prefix))
            .then_with(|| {
                left.position
                    .partial_cmp(&right.position)
                    .unwrap_or(Ordering::Equal)
            })
    });
    let master_volume_db = engine
        .get("amp.out_master")
        .and_then(Value::as_f64)
        .unwrap_or(0.0) as f32;
    Ok((plugins, master_volume_db))
}

fn chain_rank(source_prefix: &str) -> i8 {
    match source_prefix {
        "dcblocker" => -1,
        "gxamp" => 0,
        "amp.clip" => 1,
        "cab_st" | "jconv" | "stereoverb" => 3,
        "antyfizz" => 4,
        _ => 2,
    }
}

fn collect_amp_parameters(engine: &Map<String, Value>) -> BTreeMap<String, f32> {
    let mut values = BTreeMap::new();
    for (key, symbol) in [
        ("gxdistortion.drive", "drive"),
        ("gxdistortion.wet_dry", "wetdry"),
        ("amp2.stage1.Pregain", "pregain"),
        ("amp2.stage2.gain1", "gain1"),
        ("amp.highgain", "highgain"),
    ] {
        if let Some(value) = engine.get(key).and_then(Value::as_f64) {
            values.insert(symbol.into(), value as f32);
        }
    }
    values
}

fn collect_parameters(
    engine: &Map<String, Value>,
    source_prefix: &str,
) -> (BTreeMap<String, f32>, BTreeMap<String, String>) {
    let marker = format!("{source_prefix}.");
    let values: BTreeMap<String, f32> = engine
        .iter()
        .filter_map(|(key, value)| {
            key.strip_prefix(&marker).and_then(|symbol| {
                value
                    .as_f64()
                    .map(|value| (normalize(symbol), value as f32))
            })
        })
        .collect();
    let strings = engine
        .iter()
        .filter_map(|(key, value)| {
            key.strip_prefix(&marker).and_then(|symbol| {
                value
                    .as_str()
                    .map(|value| (normalize(symbol), value.into()))
            })
        })
        .collect();
    (values, strings)
}

fn import_plugin(
    engine: &Map<String, Value>,
    source_prefix: &'static str,
    uri: &'static str,
    name: &'static str,
) -> Option<ImportedPlugin> {
    if engine.get(&format!("{source_prefix}.on_off"))?.as_f64()? < 0.5 {
        return None;
    }

    let (mut values, strings) = collect_parameters(engine, source_prefix);
    if source_prefix == "amp.clip"
        && let Some(value) = engine.get("amp.fuzz").and_then(Value::as_f64)
    {
        values.insert(normalize("amp.fuzz"), value as f32);
    }
    Some(ImportedPlugin {
        uri,
        name,
        source_prefix,
        values,
        strings,
        position: engine
            .get(&format!("{source_prefix}.position"))
            .and_then(Value::as_f64)
            .unwrap_or(f64::MAX),
        fixed: false,
    })
}

pub fn normalize(value: &str) -> String {
    value
        .chars()
        .filter(|character| character.is_ascii_alphanumeric())
        .flat_map(char::to_lowercase)
        .collect()
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn normalizes_parameter_names() {
        assert_eq!(normalize("LFO freq"), "lfofreq");
        assert_eq!(normalize("dry/wet"), "drywet");
    }

    #[tokio::test]
    async fn imports_enabled_plugins_in_position_order() {
        let path =
            std::env::temp_dir().join(format!("guitar-rack-import-{}.gx", std::process::id()));
        std::fs::write(
            &path,
            r#"["gx_head_file_version", [1, 2, "0.47.0"], "main", {"engine": {
                "mxrdis.on_off": 1, "mxrdis.position": 2, "mxrdis.drive": 0.7,
                "bossds1.on_off": 1, "bossds1.position": 1, "bossds1.Level": 2.0,
                "stereoverb.on_off": 0, "stereoverb.position": 3
            }}]"#,
        )
        .unwrap();
        let (plugins, _) = import(&path, "main").await.unwrap();
        std::fs::remove_file(path).unwrap();
        assert_eq!(plugins.len(), 5);
        assert_eq!(plugins[0].source_prefix, "dcblocker");
        assert!(plugins[0].fixed);
        assert_eq!(plugins[1].source_prefix, "gxamp");
        assert!(plugins[1].fixed);
        assert_eq!(plugins[2].source_prefix, "bossds1");
        assert_eq!(plugins[3].source_prefix, "mxrdis");
        assert_eq!(plugins[3].values["drive"], 0.7);
        assert_eq!(plugins[4].source_prefix, "bassbooster");
        assert!(plugins[4].fixed);
    }

    #[tokio::test]
    async fn places_the_mono_rack_before_the_stereo_rack() {
        let path = std::env::temp_dir().join(format!(
            "guitar-rack-chain-import-{}.gx",
            std::process::id()
        ));
        std::fs::write(
            &path,
            serde_json::to_vec(&serde_json::json!([
                "main",
                { "engine": {
                    "amp.clip.on_off": 1, "amp.fuzz": 1.01,
                    "cab_st.on_off": 1, "cab_st.position": 0,
                    "jconv.on_off": 1, "jconv.position": 1,
                    "graphiceq.on_off": 1, "graphiceq.position": 2,
                    "eq.on_off": 1, "eq.position": 3, "eq.level1": -3.7,
                    "gx_distortion.on_off": 1, "gx_distortion.position": 4,
                    "stereoverb.on_off": 1, "stereoverb.position": 2,
                    "antyfizz.on_off": 1
                }}
            ]))
            .unwrap(),
        )
        .unwrap();

        let (plugins, _) = import(&path, "main").await.unwrap();
        std::fs::remove_file(path).unwrap();

        assert_eq!(
            plugins
                .iter()
                .map(|plugin| plugin.source_prefix)
                .collect::<Vec<_>>(),
            [
                "dcblocker",
                "gxamp",
                "amp.clip",
                "graphiceq",
                "eq",
                "gx_distortion",
                "bassbooster",
                "cab_st",
                "jconv",
                "stereoverb",
                "antyfizz"
            ]
        );
        assert_eq!(plugins[2].parameter_value("amp_fuzz_0"), Some(1.01));
        assert_eq!(plugins[4].parameter_value("level1_4"), Some(-3.7));
    }

    #[tokio::test]
    async fn translates_cabinet_and_reverb_parameters() {
        let path =
            std::env::temp_dir().join(format!("guitar-rack-values-{}.gx", std::process::id()));
        std::fs::write(
            &path,
            r#"["main", {"engine": {
                "cab_st.on_off": 1, "cab_st.Level": 3, "cab_st.bass": -4,
                "cab_st.treble": -1, "cab_st.select": "Vitalize",
                "stereoverb.on_off": 1, "stereoverb.LFO freq": 0.9,
                "stereoverb.wet_dry": 18, "stereoverb.invert": "linear"
            }}]"#,
        )
        .unwrap();
        let (plugins, _) = import(&path, "main").await.unwrap();
        std::fs::remove_file(path).unwrap();

        let cabinet = &plugins[3];
        assert_eq!(cabinet.parameter_value("CLevel"), Some(3.0));
        assert_eq!(cabinet.parameter_value("CBass"), Some(-4.0));
        assert_eq!(cabinet.parameter_value("CTreble"), Some(-1.0));
        assert_eq!(cabinet.parameter_value("c_model"), Some(15.0));

        let reverb = &plugins[4];
        assert_eq!(reverb.parameter_value("lfo"), Some(0.9));
        assert_eq!(reverb.parameter_value("dry_wet"), Some(18.0));
        assert_eq!(reverb.parameter_value("mode"), Some(0.0));
    }

    #[tokio::test]
    async fn translates_generated_distortion_symbols() {
        let path =
            std::env::temp_dir().join(format!("guitar-rack-distortion-{}.gx", std::process::id()));
        std::fs::write(
            &path,
            r#"["main", {"engine": {
                "gx_distortion.on_off": 1,
                "gx_distortion.drive": 1,
                "gx_distortion.middle_h_gain": 10,
                "gx_distortion.resonator.on_off": 0
            }}]"#,
        )
        .unwrap();
        let (plugins, _) = import(&path, "main").await.unwrap();
        std::fs::remove_file(path).unwrap();

        assert_eq!(plugins[2].parameter_value("drive_0"), Some(1.0));
        assert_eq!(plugins[2].parameter_value("middle_h_gain_8"), Some(10.0));
        assert_eq!(plugins[2].parameter_value("resonator_on_off_11"), Some(0.0));
    }

    #[tokio::test]
    async fn translates_guitarix_convolver_mix() {
        let path =
            std::env::temp_dir().join(format!("guitar-rack-jconv-{}.gx", std::process::id()));
        std::fs::write(
            &path,
            r#"["main", {"engine": {
                "jconv.on_off": 1, "jconv.position": 1,
                "jconv.gain": 0.1, "jconv.wet_dry": 100
            }}]"#,
        )
        .unwrap();
        let (plugins, _) = import(&path, "main").await.unwrap();
        std::fs::remove_file(path).unwrap();

        assert_eq!(plugins[3].parameter_value("port_agc_sw"), Some(0.0));
        assert_eq!(plugins[3].parameter_value("port_dry_sw"), Some(0.0));
        assert_eq!(plugins[3].parameter_value("port_wet_sw"), Some(1.0));
        assert_eq!(plugins[3].parameter_value("port_wet_gain"), Some(0.1));
    }

    #[tokio::test]
    async fn imports_fixed_stages_and_bass_boost_level() {
        let path =
            std::env::temp_dir().join(format!("guitar-rack-fixed-{}.gx", std::process::id()));
        std::fs::write(
            &path,
            r#"["main", {"engine": {
                "amp.clip.on_off": 1, "amp.fuzz": 1.01,
                "bassbooster.Level": 3.5
            }}]"#,
        )
        .unwrap();
        let (plugins, _) = import(&path, "main").await.unwrap();
        std::fs::remove_file(path).unwrap();

        assert_eq!(plugins[0].source_prefix, "dcblocker");
        assert!(plugins[0].fixed);
        assert_eq!(plugins[0].parameter_value("_0"), None);
        assert_eq!(plugins[1].source_prefix, "gxamp");
        assert_eq!(plugins[2].source_prefix, "amp.clip");
        assert_eq!(plugins[3].source_prefix, "bassbooster");
        assert!(plugins[3].fixed);
        assert_eq!(
            plugins[3].parameter_value("_bassbooster_Level_0"),
            Some(3.5)
        );
    }

    #[tokio::test]
    async fn imports_tube_preamp_parameters() {
        let path = std::env::temp_dir().join(format!("guitar-rack-amp-{}.gx", std::process::id()));
        std::fs::write(
            &path,
            r#"["main", {"engine": {
                "gxdistortion.drive": 0.515, "gxdistortion.wet_dry": 97,
                "amp2.stage1.Pregain": -11.9, "amp2.stage2.gain1": 0.3,
                "amp.highgain": 0, "amp.bass_boost.on_off": 0,
                "amp.out_master": -43.2, "bassbooster.Level": 3.5
            }}]"#,
        )
        .unwrap();
        let (plugins, master_volume_db) = import(&path, "main").await.unwrap();
        std::fs::remove_file(path).unwrap();

        assert_eq!(master_volume_db, -43.2);

        assert_eq!(plugins[1].source_prefix, "gxamp");
        assert!(plugins[1].fixed);
        assert_eq!(
            plugins[1].parameter_value("_gxdistortion_drive_0"),
            Some(0.515)
        );
        assert_eq!(
            plugins[1].parameter_value("_gxdistortion_wet_dry_1"),
            Some(97.0)
        );
        assert_eq!(
            plugins[1].parameter_value("_amp2_stage1_Pregain_3"),
            Some(-11.9)
        );
        assert_eq!(
            plugins[1].parameter_value("_amp2_stage2_gain1_5"),
            Some(0.3)
        );
        assert_eq!(plugins[1].parameter_value("_amp_highgain_2"), Some(0.0));
        assert!(
            !plugins
                .iter()
                .any(|plugin| plugin.source_prefix == "bassbooster")
        );
    }
}
