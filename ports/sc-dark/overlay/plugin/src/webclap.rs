//! Browser-only glue; the compressor and FFT processing are shared with the desktop plugin.
use crate::{SpectralCompressor, curve::Curve, match_level::MatchResult};
use nih_plug::prelude::*;
use serde::Deserialize;
use serde_json::json;
use std::collections::HashMap;
use std::sync::{Arc, Mutex, atomic::{AtomicBool, Ordering}};
mod presets;

#[derive(Default)]
pub struct WebEditorState(AtomicBool, Mutex<Sent>);

/// What the page holds already: a poll answer carries only what differs from it.
#[derive(Default)]
struct Sent {
    params: HashMap<String, (f32, String)>,
    sections: HashMap<&'static str, serde_json::Value>,
}

impl WebEditorState {
    pub fn is_open(&self) -> bool {
        self.0.load(Ordering::Relaxed)
    }

    fn forget(&self) {
        if let Ok(mut sent) = self.1.lock() { *sent = Sent::default(); }
    }
}

#[derive(Deserialize)]
struct Message {
    op: String,
    id: Option<String>,
    value: Option<f32>,
    text: Option<String>,
    request: Option<String>,
    preset: Option<serde_json::Value>,
    full: Option<bool>,
    point: Option<usize>,
    frequency: Option<f32>,
    offset: Option<f32>,
}

impl SpectralCompressor {
    pub(crate) fn web_open(&mut self, open: bool) {
        self.params.editor_state.0.store(open, Ordering::Relaxed);
        self.params.editor_state.forget();
        if !open { self.delta_active.store(false, Ordering::Relaxed); }
    }

    pub(crate) fn web_message(&mut self, bytes: &[u8], context: Arc<dyn GuiContext>, pending_parameters: bool) -> Vec<u8> {
        let Ok(message) = serde_json::from_slice::<Message>(bytes) else { return Vec::new() };
        let map = self.params.param_map();
        if message.op == "point-set" {
            // GUI drag gestures begin/end through the same generic parameter pathway.
            // Validate the whole physical-value pair before queueing either parameter.
            let (Some(index), Some(frequency), Some(offset)) = (message.point, message.frequency, message.offset) else { return Vec::new() };
            if !(1..=self.params.threshold.curve_points.len()).contains(&index)
                || !frequency.is_finite() || !offset.is_finite() { return Vec::new(); }
            let point = &self.params.threshold.curve_points[index - 1];
            unsafe {
                context.raw_set_parameter_normalized(point.frequency.as_ptr(),
                    point.frequency.preview_normalized(frequency.clamp(30.0, 22000.0)));
                context.raw_set_parameter_normalized(point.offset_db.as_ptr(),
                    point.offset_db.preview_normalized(offset.clamp(-60.0, 60.0)));
            }
            return Vec::new();
        }
        if message.op == "factory-presets" {
            return serde_json::to_vec(&json!({"type": "factory-presets", "request": message.request,
                "presets": presets::factory_presets(&map).unwrap_or_default()})).unwrap_or_default();
        }
        if message.op == "capture-preset" {
            let preset = presets::Preset::capture(message.text.as_deref().unwrap_or("Custom"), &map);
            return serde_json::to_vec(&json!({"type": "preset-capture", "request": message.request,
                "busy": pending_parameters, "preset": preset})).unwrap_or_default();
        }
        if message.op == "apply-preset" {
            let parsed = message.preset.and_then(|value| serde_json::from_value::<presets::Preset>(value).ok());
            let error = parsed.as_ref().map_or(Some("Invalid preset file"), |preset| preset.validate(&map).err());
            if error.is_some() || pending_parameters {
                return serde_json::to_vec(&json!({"type": "preset-result", "request": message.request,
                    "ok": false, "busy": pending_parameters && error.is_none(), "message": error.unwrap_or("Waiting for parameter edits")})).unwrap_or_default();
            }
            let preset = parsed.unwrap();
            self.delta_active.store(false, Ordering::Relaxed);
            self.match_runtime.cancel(); self.match_meter = crate::match_level::MatchMeter::new();
            for (id, param, _) in &map {
                unsafe {
                    context.raw_begin_set_parameter(*param);
                    context.raw_set_parameter_normalized(*param, preset.parameters[id]);
                    context.raw_end_set_parameter(*param);
                }
            }
            return serde_json::to_vec(&json!({"type": "preset-result", "request": message.request,
                "ok": true, "busy": false, "message": "Preset queued"})).unwrap_or_default();
        }
        if matches!(message.op.as_str(), "begin" | "set" | "end" | "text" | "reset") {
            let Some((_, param, _)) = map.iter().find(|(id, _, _)| Some(id) == message.id.as_ref()) else { return Vec::new() };
            unsafe {
                match message.op.as_str() {
                    "begin" => context.raw_begin_set_parameter(*param),
                    "end" => context.raw_end_set_parameter(*param),
                    "set" => {
                        if let Some(value) = message.value.filter(|value| value.is_finite()) {
                            context.raw_set_parameter_normalized(*param, value.clamp(0.0, 1.0));
                        }
                    },
                    _ => {
                        let normalized = if message.op == "reset" {
                            Some(param.default_normalized_value())
                        } else {
                            message.text.as_ref().and_then(|text| param.string_to_normalized_value(text))
                        };
                        if let Some(value) = normalized.filter(|value| value.is_finite()) {
                            context.raw_begin_set_parameter(*param);
                            context.raw_set_parameter_normalized(*param, value.clamp(0.0, 1.0));
                            context.raw_end_set_parameter(*param);
                        }
                    },
                }
            }
            return Vec::new();
        }
        if message.op == "delta" {
            self.delta_active.fetch_xor(true, Ordering::Relaxed);
            return Vec::new();
        }
        if message.op == "match" {
            self.delta_active.store(false, Ordering::Relaxed);
            let bypass = self.params.global.bypass.as_ptr();
            unsafe {
                context.raw_begin_set_parameter(bypass);
                context.raw_set_parameter_normalized(bypass, 0.0);
                context.raw_end_set_parameter(bypass);
            }
            self.match_runtime.request();
            return Vec::new();
        }
        if message.op != "poll" { return Vec::new(); }
        if message.full == Some(true) { self.params.editor_state.forget(); }
        let Ok(mut sent) = self.params.editor_state.1.lock() else { return Vec::new() };
        let mut status = "";
        if let Some(result) = self.match_runtime.take_finished_result() {
            match result {
                MatchResult::Matched(gain) => {
                    let param = &self.params.global.output_gain;
                    unsafe {
                        context.raw_begin_set_parameter(param.as_ptr());
                        context.raw_set_parameter_normalized(param.as_ptr(), param.preview_normalized(gain));
                        context.raw_end_set_parameter(param.as_ptr());
                    }
                    status = "Level matched";
                },
                MatchResult::Failed => status = "Not enough signal to match",
            }
        }
        let params: Vec<_> = map.iter().filter_map(|(id, param, group)| unsafe {
            let value = param.unmodulated_normalized_value();
            let text = param.normalized_value_to_string(value, true);
            let known = sent.params.insert(id.clone(), (value, text.clone()));
            if let Some((known_value, known_text)) = known {
                if known_value == value && known_text == text { return None; }
                return Some(json!({ "id": id, "value": value, "text": text }));
            }
            let steps = param.step_count();
            let choices: Vec<_> = if steps.is_some_and(|steps| steps <= 16) {
                let steps = steps.unwrap();
                (0..=steps).map(|step| param.normalized_value_to_string(step as f32 / steps.max(1) as f32, true)).collect()
            } else { Vec::new() };
            Some(json!({ "id": id, "name": param.name(), "group": group, "value": value,
                "text": text, "steps": steps, "default": param.default_normalized_value(), "choices": choices }))
        }).collect();
        let mut output = self.analyzer_output_data.borrow_mut();
        let analyzer = output.read();
        let rate = self.sample_rate.load(Ordering::Relaxed);
        // Evaluate the original curve from current parameters, even while transport is stopped.
        // Analyzer snapshots can lag edits until the next FFT completes.
        let curve_params = self.params.threshold.curve_params();
        let curve = Curve::new(&curve_params);
        let offsets = [self.params.compressors.upwards.threshold_offset_db.value(),
            self.params.compressors.downwards.threshold_offset_db.value()];
        let thresholds: Vec<_> = (0..192).map(|point| {
            let frequency = 30.0_f32 * (22000.0_f32 / 30.0).powf(point as f32 / 191.0);
            [frequency, curve.evaluate_ln(frequency.ln())]
        }).collect();
        let curve_points: Vec<_> = curve_params.points.iter().enumerate().map(|(index, point)| {
            json!({"index": index + 1, "enabled": point.enabled, "frequency": point.frequency,
                "offset": point.offset_db, "threshold": curve.evaluate_ln(point.frequency.ln())})
        }).collect();
        let mut bins = Vec::with_capacity(192);
        if analyzer.num_bins > 1 {
            for point in 0..192 {
                let frequency = 30.0 * ((rate * 0.5).min(22000.0) / 30.0).powf(point as f32 / 191.0);
                let index = ((frequency / (rate * 0.5)) * (analyzer.num_bins - 1) as f32).round() as usize;
                let index = index.min(analyzer.num_bins - 1);
                // hundredths are below what the graph can show, and shorter in JSON
                let round = |value: f32| (value as f64 * 100.0).round() / 100.0;
                bins.push([round(frequency), round(util::gain_to_db(analyzer.envelope_followers[index].max(1.0e-9))),
                    round(analyzer.gain_difference_db[index]), round(curve.evaluate_ln(frequency.ln()))]);
            }
        }
        let mut reply = json!({ "type": "state", "params": params, "pendingParameters": pending_parameters,
            "sampleRate": rate, "latency": self.window_size(),
            "delta": self.delta_active.load(Ordering::Relaxed), "status": status });
        let sections = [("bins", json!(bins)), ("offsets", json!(offsets)), ("thresholds", json!(thresholds)),
            ("curvePoints", json!(curve_points)),
            ("curveBase", json!([curve_params.intercept, curve_params.center_frequency, curve_params.slope, curve_params.curve]))];
        for (key, value) in sections {
            if sent.sections.get(key) == Some(&value) { continue; }
            reply[key] = value.clone();
            sent.sections.insert(key, value);
        }
        serde_json::to_vec(&reply).unwrap_or_default()
    }
}
