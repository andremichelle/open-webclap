//! OpenRB-338's engine (`orb-engine`, external/openrb-338) behind a small C API for the CLAP plugins in
//! `plugin/openrb.cpp`: the TB-303 and the four effects, each a device of its own. The engine is used as it is, at
//! its own rate (44.1 kHz) and in its own blocks (64 frames); the plugin resamples to and from the host.
//!
//! Every handle is created and freed here; the plugin calls the functions from its audio thread (process) and its
//! main thread (state, parameters), never both at once for the same handle.

use std::sync::OnceLock;

use orb_engine::comp::Compressor;
use orb_engine::delay::Delay;
use orb_engine::dist::Distortion;
use orb_engine::frame::{BLOCK, Block, BlockContext, ChannelDevice, InsertEffect, SendEffect, SongDevice};
use orb_engine::pcf::{self, Pcf};
use orb_engine::process::ProcessState;
use orb_engine::seq;
use orb_engine::tables::Tables;
use orb_engine::tb303::{self, Tb303};

/// The engine's rate.
pub const RATE: u32 = 44_100;

/// The tables every device reads, built once (`Tables::new` computes them, bit-identical everywhere).
fn tables() -> &'static Tables {
    static TABLES: OnceLock<Tables> = OnceLock::new();
    TABLES.get_or_init(Tables::new)
}

fn context(tick: i32, tempo: u32) -> BlockContext<'static> {
    BlockContext {
        tables: tables(),
        meter_index: 0,
        tempo,
        tick,
        stack_word: None,
    }
}

/// # Safety
/// `p` points to [`BLOCK`] floats.
unsafe fn block<'a>(p: *mut f32) -> &'a mut Block {
    unsafe { &mut *(p as *mut Block) }
}

fn boxed<T>(value: T) -> *mut T {
    Box::into_raw(Box::new(value))
}

/// # Safety
/// `p` came from [`boxed`] and is not used afterwards.
unsafe fn free<T>(p: *mut T) {
    if !p.is_null() {
        drop(unsafe { Box::from_raw(p) });
    }
}

#[unsafe(no_mangle)]
pub extern "C" fn orb_block_frames() -> u32 {
    BLOCK as u32
}

#[unsafe(no_mangle)]
pub extern "C" fn orb_rate() -> u32 {
    RATE
}

/// Builds the tables now rather than in the first process call.
#[unsafe(no_mangle)]
pub extern "C" fn orb_init() {
    tables();
}

/// The shuffled tick of the sequencer (`seq::shuffled`): the second 16th of every 8th moves later by up to 21 ticks.
#[unsafe(no_mangle)]
pub extern "C" fn orb_shuffled(tick: i32, shuffle: u8) -> i32 {
    seq::shuffled(tick, shuffle)
}

/// The mixer's level curve, `T[0..127]`: the delay's send uses it as the mixer's send knob does.
#[unsafe(no_mangle)]
pub extern "C" fn orb_level(value: u8) -> f32 {
    tables().level[value.min(127) as usize]
}

// --- TB-303 --------------------------------------------------------------------------------------------------------

/// A TB-303 played by notes or by its own patterns.
pub struct Bass {
    device: Tb303,
}

/// The steps' bytes: note, slide, accent, up, down, rest.
pub const STEP: usize = 6;

#[unsafe(no_mangle)]
pub extern "C" fn orb_bass_new() -> *mut Bass {
    boxed(Bass {
        device: Tb303::new(),
    })
}

/// # Safety
/// `bass` came from [`orb_bass_new`].
#[unsafe(no_mangle)]
pub unsafe extern "C" fn orb_bass_free(bass: *mut Bass) {
    unsafe { free(bass) }
}

/// Controller `ctrl` (`tb303::TUNE` .. `tb303::WAVEFORM`, the pattern `tb303::PATTERN`) to `value`, as a song's track
/// sets it.
///
/// # Safety
/// `bass` came from [`orb_bass_new`].
#[unsafe(no_mangle)]
pub unsafe extern "C" fn orb_bass_set(bass: *mut Bass, ctrl: u8, value: u8) {
    let b = unsafe { &mut *bass };
    b.device.set_param(ctrl, value);
    b.device.params = b.device.live;
}

/// The sound model: false "2.0", true "Vintage" (a song-wide setting in the original).
///
/// # Safety
/// `bass` came from [`orb_bass_new`].
#[unsafe(no_mangle)]
pub unsafe extern "C" fn orb_bass_set_vintage(bass: *mut Bass, vintage: bool) {
    unsafe { &mut *bass }.device.set_sound_model(vintage);
}

/// A note: `semis` from the 303's lowest C (MIDI 36 at the middle tune), accented or not; `glide` slides from the
/// note that sounds (a legato note), the original's slide flag on the step before.
///
/// # Safety
/// `bass` came from [`orb_bass_new`].
#[unsafe(no_mangle)]
pub unsafe extern "C" fn orb_bass_note_on(bass: *mut Bass, semis: i32, accent: bool, glide: bool) {
    let d = &mut unsafe { &mut *bass }.device;
    // the pitch table holds (tune + semis) from -48 to 47; the tune reaches -12..12
    let semis = semis.clamp(-36, 35);
    // the note byte and the octave bytes as a step writes them: note + 12 * up - 12 * down
    let down = if semis < 0 { (-semis + 11) / 12 } else { 0 };
    let note = (semis + 12 * down) as u8;
    d.slide = glide;
    d.note_on([note, 0, accent as u8, 0, down as u8, 0]);
}

/// The gate closes, the note releases.
///
/// # Safety
/// `bass` came from [`orb_bass_new`].
#[unsafe(no_mangle)]
pub unsafe extern "C" fn orb_bass_note_off(bass: *mut Bass) {
    unsafe { &mut *bass }.device.silence();
}

/// The transport starts at `tick` (192 per quarter note): the patterns play from the next step.
///
/// # Safety
/// `bass` came from [`orb_bass_new`].
#[unsafe(no_mangle)]
pub unsafe extern "C" fn orb_bass_start(bass: *mut Bass, tick: i32) {
    let d = &mut unsafe { &mut *bass }.device;
    d.relocate(tick, tick);
    d.start(tick);
    d.gate_off = -1;
    d.last_pos = 0;
}

/// The transport stops: what sounds releases.
///
/// # Safety
/// `bass` came from [`orb_bass_new`].
#[unsafe(no_mangle)]
pub unsafe extern "C" fn orb_bass_stop(bass: *mut Bass) {
    unsafe { &mut *bass }.device.stop();
}

/// The sequencer's step at `tick`: the pattern plays its step there (and changes at a bar line).
///
/// # Safety
/// `bass` came from [`orb_bass_new`].
#[unsafe(no_mangle)]
pub unsafe extern "C" fn orb_bass_step(bass: *mut Bass, tick: i32, shuffle: u8, tempo: u32) {
    let d = &mut unsafe { &mut *bass }.device;
    d.step(tick, seq::shuffled(tick, shuffle), tempo as i32);
}

/// One block of 64 frames into `out`.
///
/// # Safety
/// `bass` came from [`orb_bass_new`], `out` holds 64 floats.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn orb_bass_process(bass: *mut Bass, out: *mut f32) {
    let b = unsafe { &mut *bass };
    b.device.process(unsafe { block(out) }, &context(0, 120_000));
}

/// Silence: every voice state cleared (the original's reset).
///
/// # Safety
/// `bass` came from [`orb_bass_new`].
#[unsafe(no_mangle)]
pub unsafe extern "C" fn orb_bass_reset(bass: *mut Bass) {
    unsafe { &mut *bass }.device.reset();
}

/// The 32 patterns, 98 bytes each (shuffle flag, length, 16 steps of note, slide, accent, up, down, rest), to read and
/// write in place.
///
/// # Safety
/// `bass` came from [`orb_bass_new`].
#[unsafe(no_mangle)]
pub unsafe extern "C" fn orb_bass_patterns(bass: *mut Bass) -> *mut u8 {
    unsafe { &mut *bass }.device.patterns.as_mut_ptr()
}

#[unsafe(no_mangle)]
pub extern "C" fn orb_bass_pattern_bytes() -> u32 {
    tb303::PATTERN_LEN as u32
}

#[unsafe(no_mangle)]
pub extern "C" fn orb_bass_pattern_count() -> u32 {
    tb303::PATTERNS as u32
}

/// The pattern playing, and the step it played last (255 when the transport never played).
///
/// # Safety
/// `bass` came from [`orb_bass_new`].
#[unsafe(no_mangle)]
pub unsafe extern "C" fn orb_bass_position(bass: *const Bass, pattern: *mut u8, step: *mut u8) {
    let d = &unsafe { &*bass }.device;
    unsafe {
        *pattern = d.cur_pattern;
        *step = d.live[9];
    }
}

/// Whether the gate is open (a note sounds).
///
/// # Safety
/// `bass` came from [`orb_bass_new`].
#[unsafe(no_mangle)]
pub unsafe extern "C" fn orb_bass_gate(bass: *const Bass) -> bool {
    unsafe { &*bass }.device.gate != 0.0
}

// --- Distortion ----------------------------------------------------------------------------------------------------

pub struct Dist {
    device: Distortion,
}

#[unsafe(no_mangle)]
pub extern "C" fn orb_dist_new() -> *mut Dist {
    boxed(Dist {
        device: Distortion::new(),
    })
}

/// # Safety
/// `dist` came from [`orb_dist_new`].
#[unsafe(no_mangle)]
pub unsafe extern "C" fn orb_dist_free(dist: *mut Dist) {
    unsafe { free(dist) }
}

/// Controller `ctrl` (`dist::AMOUNT`, `dist::SHAPE`) to `value`.
///
/// # Safety
/// `dist` came from [`orb_dist_new`].
#[unsafe(no_mangle)]
pub unsafe extern "C" fn orb_dist_set(dist: *mut Dist, ctrl: u8, value: u8) {
    unsafe { &mut *dist }.device.set_param(ctrl, value);
}

/// One mono block in place.
///
/// # Safety
/// `dist` came from [`orb_dist_new`], `bus` holds 64 floats.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn orb_dist_process(dist: *mut Dist, bus: *mut f32) {
    unsafe { &mut *dist }.device.process(unsafe { block(bus) }, &context(0, 120_000));
}

/// The input meter, 0..127 (the original's meter byte).
///
/// # Safety
/// `dist` came from [`orb_dist_new`].
#[unsafe(no_mangle)]
pub unsafe extern "C" fn orb_dist_meter(dist: *const Dist) -> u8 {
    unsafe { &*dist }.device.live[orb_engine::dist::METER]
}

/// The distortion's transfer curve at `amount` and `shape` (the gains it settles at, as the update sets them): `n`
/// outputs for inputs from -1 to 1 into `out`.
///
/// # Safety
/// `out` holds `n` floats.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn orb_dist_transfer(amount: u8, shape: u8, n: u32, out: *mut f32) {
    use orb_engine::dist;
    let (amount, shape) = (amount.min(127), shape.min(127));
    let t = tables();
    let out = unsafe { std::slice::from_raw_parts_mut(out, n as usize) };
    let last = (n.max(2) - 1) as f64;
    // the update's targets (0x10010CB0) and the block's two paths (0x10010A50)
    let gi = (t.level[amount as usize] as f64 + 0.0625) * 2048.0;
    let go = dist::grid(shape, amount) * 5.0 * 0.00390625;
    let g = 16.0 * t.level[amount as usize] as f64 + 1.0;
    let h = if amount < 90 { g } else { 16.0 * t.level[90] as f64 + 1.0 };
    let th = 1.0 / h;
    let w = &t.dist_shaper[(shape >> 2) as usize];
    for (i, y) in out.iter_mut().enumerate() {
        let x = -1.0 + 2.0 * i as f64 / last;
        *y = if shape == 0 {
            (g * x).clamp(-th, th) as f32
        } else {
            (dist::shape(w, gi * x + 128.0) * go) as f32
        };
    }
}

// --- PCF -----------------------------------------------------------------------------------------------------------

pub struct Filter {
    device: Pcf,
}

#[unsafe(no_mangle)]
pub extern "C" fn orb_pcf_new() -> *mut Filter {
    boxed(Filter { device: Pcf::new() })
}

/// # Safety
/// `pcf` came from [`orb_pcf_new`].
#[unsafe(no_mangle)]
pub unsafe extern "C" fn orb_pcf_free(pcf: *mut Filter) {
    unsafe { free(pcf) }
}

/// Controller `ctrl` (`pcf::FREQUENCY` .. `pcf::MODE`) to `value`. The wave stays within the 55 patterns.
///
/// # Safety
/// `pcf` came from [`orb_pcf_new`].
#[unsafe(no_mangle)]
pub unsafe extern "C" fn orb_pcf_set(pcf: *mut Filter, ctrl: u8, value: u8) {
    let value = if ctrl as usize == pcf::WAVE { value.min(54) } else { value };
    unsafe { &mut *pcf }.device.set_param(ctrl, value);
}

/// One mono block in place at the song's `tick` (192 per quarter note).
///
/// # Safety
/// `pcf` came from [`orb_pcf_new`], `bus` holds 64 floats.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn orb_pcf_process(pcf: *mut Filter, bus: *mut f32, tick: i32) {
    let f = unsafe { &mut *pcf };
    f.device.process(unsafe { block(bus) }, &context(tick.max(0), 120_000));
    // waves 0..54 have a length; nothing faults
    let _ = f.device.faulted();
}

/// The envelope (the cutoff as a filter table index, 0..511) and the step the pattern is at.
///
/// # Safety
/// `pcf` came from [`orb_pcf_new`].
#[unsafe(no_mangle)]
pub unsafe extern "C" fn orb_pcf_status(pcf: *const Filter, env: *mut f32, step: *mut i32) {
    let d = &unsafe { &*pcf }.device;
    unsafe {
        *env = d.env;
        *step = d.step;
    }
}

/// Wave `wave`'s pattern: its length and resolution (steps per bar), and its 32 step velocities into `steps`.
///
/// # Safety
/// `steps` holds 32 bytes, `length` and `resolution` point to u16s.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn orb_pcf_wave(wave: u8, length: *mut u16, resolution: *mut u16, steps: *mut u8) {
    let wave = wave.min(54);
    let (l, r) = pcf::pattern(wave);
    unsafe {
        *length = l;
        *resolution = r;
        for i in 0..32 {
            *steps.add(i) = pcf::velocity(wave, i as i32);
        }
    }
}

// --- Compressor ----------------------------------------------------------------------------------------------------

pub struct Comp {
    device: Compressor,
}

#[unsafe(no_mangle)]
pub extern "C" fn orb_comp_new() -> *mut Comp {
    boxed(Comp {
        device: Compressor::new(),
    })
}

/// # Safety
/// `comp` came from [`orb_comp_new`].
#[unsafe(no_mangle)]
pub unsafe extern "C" fn orb_comp_free(comp: *mut Comp) {
    unsafe { free(comp) }
}

/// Controller `ctrl` (`comp::AMOUNT`, `comp::THRESHOLD`) to `value`.
///
/// # Safety
/// `comp` came from [`orb_comp_new`].
#[unsafe(no_mangle)]
pub unsafe extern "C" fn orb_comp_set(comp: *mut Comp, ctrl: u8, value: u8) {
    unsafe { &mut *comp }.device.set_param(ctrl, value);
}

/// One stereo block in place: linked, one detector over both channels, as on the original's master.
///
/// # Safety
/// `comp` came from [`orb_comp_new`], `l` and `r` hold 64 floats.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn orb_comp_process(comp: *mut Comp, l: *mut f32, r: *mut f32) {
    let c = unsafe { &mut *comp };
    c.device.process_stereo(unsafe { block(l) }, unsafe { block(r) }, &context(0, 120_000));
    // the meters' bytes are refreshed by the mixer update's slot 16 in the original
    c.device.update(&context(0, 120_000));
}

/// The gain meter (`min(G * 18, 127)`) and the input peak meter, 0..127 each.
///
/// # Safety
/// `comp` came from [`orb_comp_new`].
#[unsafe(no_mangle)]
pub unsafe extern "C" fn orb_comp_meters(comp: *const Comp, gain: *mut u8, peak: *mut u8) {
    let d = &unsafe { &*comp }.device;
    unsafe {
        *gain = d.live[orb_engine::comp::GAIN_METER];
        *peak = d.live[orb_engine::comp::PEAK_METER];
    }
}

// --- Delay ---------------------------------------------------------------------------------------------------------

/// The delay with a line of its own (the original shares one line between every delay of the process).
pub struct Echo {
    device: Delay,
    send: Block,
}

#[unsafe(no_mangle)]
pub extern "C" fn orb_delay_new() -> *mut Echo {
    let process = ProcessState::new();
    let mut device = Delay::new(process);
    device.silence();
    boxed(Echo {
        device,
        send: [orb_engine::frame::GUARD; BLOCK],
    })
}

/// # Safety
/// `delay` came from [`orb_delay_new`].
#[unsafe(no_mangle)]
pub unsafe extern "C" fn orb_delay_free(delay: *mut Echo) {
    unsafe { free(delay) }
}

/// Controller `ctrl` (`delay::STEPS` .. `delay::PAN`) to `value`.
///
/// # Safety
/// `delay` came from [`orb_delay_new`].
#[unsafe(no_mangle)]
pub unsafe extern "C" fn orb_delay_set(delay: *mut Echo, ctrl: u8, value: u8) {
    unsafe { &mut *delay }.device.set_param(ctrl, value);
}

/// One block: `send` (64 floats, mono) goes into the line, the line's output is added to `l` and `r` (the dry signal
/// the caller put there). `tempo` is the song's, in thousandths of a BPM.
///
/// # Safety
/// `delay` came from [`orb_delay_new`], the pointers hold 64 floats each.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn orb_delay_process(delay: *mut Echo, send: *const f32, l: *mut f32, r: *mut f32, tempo: u32) {
    let e = unsafe { &mut *delay };
    let input = unsafe { std::slice::from_raw_parts(send, BLOCK) };
    // the mixer adds the channels' sends onto the guard the delay left in its bus
    for (s, &x) in e.send.iter_mut().zip(input) {
        *s = (x as f64 + *s as f64) as f32;
    }
    e.device
        .process(&mut e.send, unsafe { block(l) }, unsafe { block(r) }, &context(0, tempo));
}

/// The delay's length in samples at `tempo`, as the device computes it.
#[unsafe(no_mangle)]
pub extern "C" fn orb_delay_length(tempo: u32, steps: u8, mode: u8) -> u32 {
    orb_engine::delay::length(tempo, steps, mode)
}

/// The line cleared (the original refills it with its denormal guard).
///
/// # Safety
/// `delay` came from [`orb_delay_new`].
#[unsafe(no_mangle)]
pub unsafe extern "C" fn orb_delay_clear(delay: *mut Echo) {
    unsafe { &mut *delay }.device.silence();
}

/// The send's meter, 0..127.
///
/// # Safety
/// `delay` came from [`orb_delay_new`].
#[unsafe(no_mangle)]
pub unsafe extern "C" fn orb_delay_meter(delay: *const Echo) -> u8 {
    unsafe { &*delay }.device.live[orb_engine::delay::METER]
}

// --- the drum kit: the samples the user brings ---------------------------------------------------------------------
// The 808 and 909 play ReBirth's own drum samples, which this port does not ship. The user brings them: the 46 AIFF
// resources of ReBirth.dll (ids 1000..1045, read by the engine's own `SampleSet::from_dll`), or single samples per
// slot (16-bit mono at 44.1 kHz, decoded by the page). The plugin fills the slots with its factory kits
// (ports/openrb/kits) first. A slot without a sample is silent.

use orb_engine::samples::{COUNT as KIT_SLOTS, FIRST_ID, Sample, SampleSet};
use orb_engine::tr808::{self, Tr808};
use orb_engine::tr909::{self, Tr909};
use std::sync::Arc;

/// The 46 slots by resource id from 1000, raw 16-bit audio (no padding).
pub struct Kit {
    slots: Vec<Vec<i16>>,
}

impl Kit {
    fn set(&self) -> Arc<SampleSet> {
        Arc::new(SampleSet::from_samples(self.slots.iter().map(|audio| Sample::from_audio(audio)).collect()))
    }
}

#[unsafe(no_mangle)]
pub extern "C" fn orb_kit_new() -> *mut Kit {
    boxed(Kit {
        slots: vec![Vec::new(); KIT_SLOTS],
    })
}

/// # Safety
/// `kit` came from [`orb_kit_new`].
#[unsafe(no_mangle)]
pub unsafe extern "C" fn orb_kit_free(kit: *mut Kit) {
    unsafe { free(kit) }
}

/// Every slot from the bytes of ReBirth.dll; false (the kit unchanged) when it is not one.
///
/// # Safety
/// `kit` came from [`orb_kit_new`], `bytes` holds `size` bytes.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn orb_kit_load_dll(kit: *mut Kit, bytes: *const u8, size: u32) -> bool {
    let dll = unsafe { std::slice::from_raw_parts(bytes, size as usize) };
    match SampleSet::from_dll(dll) {
        Ok(set) => {
            let k = unsafe { &mut *kit };
            for (i, slot) in k.slots.iter_mut().enumerate() {
                *slot = set.get(FIRST_ID + i as u16).audio().to_vec();
            }
            true
        }
        Err(_) => false,
    }
}

/// Slot `id` (1000..1045) to `frames` samples of `audio`.
///
/// # Safety
/// `kit` came from [`orb_kit_new`], `audio` holds `frames` samples.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn orb_kit_set(kit: *mut Kit, id: u16, audio: *const i16, frames: u32) {
    let k = unsafe { &mut *kit };
    if let Some(slot) = id.checked_sub(FIRST_ID).and_then(|i| k.slots.get_mut(i as usize)) {
        *slot = if frames == 0 { Vec::new() } else { unsafe { std::slice::from_raw_parts(audio, frames as usize) }.to_vec() };
    }
}

/// Slot `id`'s audio and its length in `frames` (0: empty).
///
/// # Safety
/// `kit` came from [`orb_kit_new`], `frames` points to a u32.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn orb_kit_get(kit: *const Kit, id: u16, frames: *mut u32) -> *const i16 {
    let k = unsafe { &*kit };
    match id.checked_sub(FIRST_ID).and_then(|i| k.slots.get(i as usize)) {
        Some(slot) => {
            unsafe { *frames = slot.len() as u32 };
            slot.as_ptr()
        }
        None => {
            unsafe { *frames = 0 };
            std::ptr::null()
        }
    }
}

// --- TR-808 and TR-909 ---------------------------------------------------------------------------------------------

pub enum Drums {
    Tr808(Box<Tr808>),
    Tr909(Box<Tr909>),
}

impl Drums {
    fn device(&mut self) -> &mut dyn ChannelDevice {
        match self {
            Drums::Tr808(d) => d.as_mut(),
            Drums::Tr909(d) => d.as_mut(),
        }
    }
}

/// A drum machine, 808 (`machine` 0) or 909 (1), on `kit`'s samples.
///
/// # Safety
/// `kit` came from [`orb_kit_new`].
#[unsafe(no_mangle)]
pub unsafe extern "C" fn orb_drums_new(machine: u8, kit: *const Kit) -> *mut Drums {
    let set = unsafe { &*kit }.set();
    let process = ProcessState::new();
    boxed(if machine == 0 {
        Drums::Tr808(Box::new(Tr808::new(set, process)))
    } else {
        Drums::Tr909(Box::new(Tr909::new(set, process)))
    })
}

/// # Safety
/// `drums` came from [`orb_drums_new`].
#[unsafe(no_mangle)]
pub unsafe extern "C" fn orb_drums_free(drums: *mut Drums) {
    unsafe { free(drums) }
}

/// The samples of `kit` from now on (what sounds is silenced).
///
/// # Safety
/// `drums` came from [`orb_drums_new`], `kit` from [`orb_kit_new`].
#[unsafe(no_mangle)]
pub unsafe extern "C" fn orb_drums_set_kit(drums: *mut Drums, kit: *const Kit) {
    let set = unsafe { &*kit }.set();
    let mut retired = orb_engine::samples::Retired::default();
    unsafe { &mut *drums }.device().set_samples(&set, &mut retired);
}

/// Controller `ctrl` (the RBS order of the machine) to `value`; the pattern-mode copy follows.
///
/// # Safety
/// `drums` came from [`orb_drums_new`].
#[unsafe(no_mangle)]
pub unsafe extern "C" fn orb_drums_set(drums: *mut Drums, ctrl: u8, value: u8) {
    match unsafe { &mut *drums } {
        Drums::Tr808(d) => {
            d.set_param(ctrl, value);
            d.params = d.live;
        }
        Drums::Tr909(d) => {
            d.set_param(ctrl, value);
            d.params = d.live;
        }
    }
}

/// Instrument `column` (1..11, the pattern's columns) from a note: `accent` the step's accent column, `strong` the
/// 909's louder step value (2: gain 2.0 for 0.8).
///
/// # Safety
/// `drums` came from [`orb_drums_new`].
#[unsafe(no_mangle)]
pub unsafe extern "C" fn orb_drums_hit(drums: *mut Drums, column: u8, accent: bool, strong: bool) {
    let column = column as usize;
    if !(1..=11).contains(&column) {
        return;
    }
    match unsafe { &mut *drums } {
        Drums::Tr808(d) => {
            // as the step function: f32(AC * accent level * 0.03125 + 1)
            d.accent = ((accent as u8 as f64 * d.live[tr808::ACCENT] as f64) * 0.03125 + 1.0) as f32;
            d.trigger(column);
        }
        Drums::Tr909(d) => {
            d.accent = ((accent as u8 as f64 * d.live[tr909::ACCENT] as f64) * 0.03125 + 1.0) as f32;
            d.gain = (d.accent as f64 * if strong { 2.0 } else { 0.8 }) as f32;
            d.trigger(column);
        }
    }
}

/// The transport starts at `tick`.
///
/// # Safety
/// `drums` came from [`orb_drums_new`].
#[unsafe(no_mangle)]
pub unsafe extern "C" fn orb_drums_start(drums: *mut Drums, tick: i32) {
    let d = unsafe { &mut *drums }.device();
    d.relocate(tick, tick);
    d.start(tick);
}

/// # Safety
/// `drums` came from [`orb_drums_new`].
#[unsafe(no_mangle)]
pub unsafe extern "C" fn orb_drums_stop(drums: *mut Drums) {
    unsafe { &mut *drums }.device().stop();
}

/// The sequencer's step at `tick`.
///
/// # Safety
/// `drums` came from [`orb_drums_new`].
#[unsafe(no_mangle)]
pub unsafe extern "C" fn orb_drums_step(drums: *mut Drums, tick: i32, shuffle: u8, tempo: u32) {
    unsafe { &mut *drums }.device().step(tick, seq::shuffled(tick, shuffle), tempo as i32);
}

/// One block of 64 frames into `out`.
///
/// # Safety
/// `drums` came from [`orb_drums_new`], `out` holds 64 floats.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn orb_drums_process(drums: *mut Drums, out: *mut f32) {
    unsafe { &mut *drums }.device().process(unsafe { block(out) }, &context(0, 120_000));
}

/// # Safety
/// `drums` came from [`orb_drums_new`].
#[unsafe(no_mangle)]
pub unsafe extern "C" fn orb_drums_reset(drums: *mut Drums) {
    unsafe { &mut *drums }.device().reset();
}

/// The 32 patterns, 194 bytes each (shuffle, length, 16 steps of 12 columns: accent and the 11 instruments).
///
/// # Safety
/// `drums` came from [`orb_drums_new`].
#[unsafe(no_mangle)]
pub unsafe extern "C" fn orb_drums_patterns(drums: *mut Drums) -> *mut u8 {
    unsafe { &mut *drums }.device().patterns_mut().map_or(std::ptr::null_mut(), |p| p.as_mut_ptr())
}

/// The pattern playing and the step it played last.
///
/// # Safety
/// `drums` came from [`orb_drums_new`].
#[unsafe(no_mangle)]
pub unsafe extern "C" fn orb_drums_position(drums: *mut Drums, pattern: *mut u8, step: *mut u8) {
    let (p, s) = match unsafe { &*drums } {
        Drums::Tr808(d) => (d.cur_pattern, d.live()[29]),
        Drums::Tr909(d) => (d.cur_pattern, d.live()[30]),
    };
    unsafe {
        *pattern = p;
        *step = s;
    }
}
