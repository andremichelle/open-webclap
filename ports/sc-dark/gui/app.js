import {createPresetUI} from './presets.mjs';
import {curveLayout} from './curve-layout.mjs';
import {SpectrumAnimation} from './spectrum-animation.mjs';
import {PointDragging} from './point-drag.mjs';
import {createHelpUI} from './help.mjs';
import {hasInlineHelp} from './help-content.mjs';
const encode = new TextEncoder(), decode = new TextDecoder();
const send = message => window.parent.postMessage(encode.encode(JSON.stringify(message)).buffer, '*');
const presets = createPresetUI({send, status: text => { document.querySelector('#status').textContent = text; }});
const elements = new Map();
let state, parentOrigin;
let activePoint;
const markers = new Map();
const canvas = document.querySelector('canvas'), context = canvas.getContext('2d');
const spectrum = new SpectrumAnimation(), reducedMotion = matchMedia('(prefers-reduced-motion: reduce)');
let animationFrame = 0;
const pointDragging = new PointDragging({send, getState: () => state, invalidate, select: selectPoint,
    getPlot: () => curveLayout(canvas.clientWidth, canvas.clientHeight, state.thresholds, state.offsets)});
const help = createHelpUI({beforeOpen: () => pointDragging.finish(true)});
function invalidate() {
    if (!animationFrame && !document.hidden) animationFrame = requestAnimationFrame(paintFrame);
}
function paintFrame(now) {
    animationFrame = 0;
    pointDragging.flush();
    const moving = spectrum.step(now, reducedMotion.matches);
    draw();
    if (moving) invalidate();
}
function selectPoint(index) {
    activePoint = index;
    for (const [key, section] of groups) if (key.startsWith('point-')) section.classList.toggle('selected', key === `point-${index}`);
    for (const [key, marker] of markers) marker.setAttribute('aria-pressed', String(key === index));
}
const controls = document.querySelector('#controls');
const groups = new Map();
for (const [key, name] of [['threshold', 'Threshold & character'], ['downwards', 'Downward compression'], ['upwards', 'Upward compression'], ['global', 'Output & timing']]) {
    const section = document.createElement('section'); section.className = `group ${key}`;
    const title = document.createElement('h2'); title.textContent = name; section.append(title); controls.append(section); groups.set(key, section);
}
const classify = param => param.id.startsWith('down') ? 'downwards' : param.id.startsWith('up') ? 'upwards' : param.group === 'global' ? 'global' : 'threshold';
function createControl(param) {
    const control = document.createElement('div'); control.className = 'control';
    const label = document.createElement('label');
    label.textContent = param.name.replace(/^(Downwards|Upwards|Threshold) /, '').replace('Global Threshold', 'Threshold').replace('Hi-Freq Rolloff', 'HF rolloff').replace('SC Channel Link', 'SC link');
    label.title = param.name;
    const labelRow = document.createElement('div'); labelRow.className = 'control-label';
    labelRow.append(label);
    if (hasInlineHelp(param.id)) labelRow.append(help.button(param.id, param.name));
    control.append(labelRow);
    const id = `p-${param.id}`; label.htmlFor = id;
    const set = value => { send({op: 'begin', id: param.id}); send({op: 'set', id: param.id, value}); send({op: 'end', id: param.id}); };
    const isToggle = param.steps === 1;
    let input, value;
    if (isToggle) {
        input = document.createElement('input'); input.type = 'checkbox'; input.onchange = () => set(input.checked ? 1 : 0);
    } else if (param.choices.length > 0) {
        input = document.createElement('select');
        param.choices.forEach((text, index) => { const option = document.createElement('option'); option.textContent = text; option.value = index / param.steps; input.append(option); });
        input.onchange = () => set(Number(input.value));
    } else {
        value = document.createElement('input'); value.type = 'text'; value.setAttribute('aria-label', `${param.name} exact value`);
        value.onchange = () => send({op: 'text', id: param.id, text: value.value});
        value.onkeydown = event => { if (event.key === 'Enter') value.blur(); if (event.key === 'Escape') { value.value = elements.get(param.id).param.text; value.blur(); } };
        control.append(value);
        input = document.createElement('input'); input.type = 'range'; input.min = '0'; input.max = '1'; input.step = param.steps ? String(1 / param.steps) : '0.001';
        let editing = false;
        const begin = () => { if (!editing) { editing = true; send({op: 'begin', id: param.id}); } };
        const end = () => { if (editing) { editing = false; send({op: 'end', id: param.id}); } };
        input.onpointerdown = event => { begin(); input.setPointerCapture(event.pointerId); };
        input.oninput = () => { begin(); send({op: 'set', id: param.id, value: Number(input.value)}); };
        input.onpointerup = end; input.onpointercancel = end; input.onlostpointercapture = end; input.onkeyup = end; input.onblur = end;
        input.ondblclick = () => send({op: 'reset', id: param.id});
    }
    input.id = id; control.append(input);
    const point = /curve_point_(enabled|frequency|offset)_(\d+)/.exec(param.id);
    let parent;
    if (point) {
        label.textContent = {enabled: 'Enabled', frequency: 'Frequency', offset: 'Offset'}[point[1]];
        const key = `point-${point[2]}`;
        if (!groups.has(key)) { const section = document.createElement('section'); section.className = 'group'; section.dataset.point = point[2]; const title = document.createElement('h2'); title.textContent = `Point ${point[2]}`; section.append(title); document.querySelector('#points').append(section); groups.set(key, section); }
        parent = groups.get(key);
        control.addEventListener('focusin', () => selectPoint(Number(point[2])));
    } else { parent = groups.get(classify(param)); }
    parent.append(control); elements.set(param.id, {input, value, param});
}
function merge(previous, message) {
    const params = previous?.params.map(param => ({...param})) ?? [];
    for (const change of message.params) {
        const index = params.findIndex(param => param.id === change.id);
        if (index < 0) params.push(change); else params[index] = {...params[index], ...change};
    }
    return {...previous, ...message, params};
}
window.addEventListener('message', event => {
    if (event.source !== window.parent || !(event.data instanceof ArrayBuffer)) return;
    if (parentOrigin !== undefined && event.origin !== parentOrigin) return;
    let message;
    try { message = JSON.parse(decode.decode(event.data)); } catch { return; }
    if (presets.receive(message)) return;
    if (message.type !== 'state' || !Array.isArray(message.params)) return;
    // a reply carries only what changed since the last one, sections it leaves out are unchanged
    parentOrigin = event.origin; state = merge(state, message);
    if (message.bins) spectrum.update(message.bins, performance.now(), reducedMotion.matches);
    pointDragging.reconcile(state, performance.now());
    presets.updateState(state);
    for (const param of state.params) {
        if (!elements.has(param.id)) createControl(param);
        const element = elements.get(param.id); element.param = param;
        if (document.activeElement !== element.input) {
            if (element.input.type === 'checkbox') element.input.checked = param.value >= 0.5;
            else element.input.value = element.input.tagName === 'SELECT' ? Math.round(param.value * param.steps) / param.steps : param.value;
        }
        const previewing = pointDragging.preview && ['frequency', 'offset'].some(field => param.id === `curve_point_${field}_${pointDragging.preview.index}`);
        if (element.value && document.activeElement !== element.value && !previewing) element.value.value = param.text;
    }
    for (const point of state.curvePoints ?? []) groups.get(`point-${point.index}`)?.classList.toggle('enabled', point.enabled);
    document.querySelector('#point-count').textContent = `${(state.curvePoints ?? []).filter(point => point.enabled).length} / 8 enabled`;
    document.querySelector('#delta').setAttribute('aria-pressed', String(message.delta));
    document.querySelector('#latency').textContent = `${message.sampleRate.toFixed(0)} Hz · ${(message.latency / message.sampleRate * 1000).toFixed(1)} ms FFT latency`;
    const status = document.querySelector('#status');
    if (message.status) { status.textContent = message.status; document.querySelector('#match').disabled = false; }
    else if (status.textContent.startsWith('Connecting')) status.textContent = 'SC Dark audio engine. Use a low listening level.';
    invalidate();
});
document.querySelector('#delta').onclick = () => send({op: 'delta'});
document.querySelector('#match').onclick = () => {
    send({op: 'match'}); document.querySelector('#status').textContent = 'Play audio for 5 seconds. Match level is active.';
    document.querySelector('#match').disabled = true;
    setTimeout(() => { document.querySelector('#match').disabled = false; }, 15000);
};
function draw() {
    const bounds = canvas.getBoundingClientRect(), dpr = window.devicePixelRatio || 1;
    const pixelWidth = Math.round(bounds.width * dpr), pixelHeight = Math.round(bounds.height * dpr);
    // Do not reallocate the canvas backing store on every animation frame.
    if (canvas.width !== pixelWidth) canvas.width = pixelWidth;
    if (canvas.height !== pixelHeight) canvas.height = pixelHeight;
    context.setTransform(dpr, 0, 0, dpr, 0, 0); context.clearRect(0, 0, bounds.width, bounds.height);
    const width = bounds.width, height = bounds.height;
    const visual = pointDragging.display(state);
    const {left, right, top, bottom, ticks, x, y, gainY} = pointDragging.drag?.plot ?? curveLayout(width, height, visual?.thresholds, visual?.offsets);
    context.font = '10px system-ui'; context.lineWidth = 1;
    for (const frequency of [30, 100, 300, 1000, 3000, 10000, 20000]) {
        context.strokeStyle = '#ffffff12'; context.beginPath(); context.moveTo(x(frequency), top); context.lineTo(x(frequency), bottom); context.stroke();
        context.fillStyle = '#929292'; context.textAlign = 'center'; context.fillText(frequency >= 1000 ? `${frequency / 1000}k` : frequency, x(frequency), height - 5);
    }
    for (const db of ticks) {
        context.strokeStyle = '#ffffff0b'; context.beginPath(); context.moveTo(left, y(db)); context.lineTo(right, y(db)); context.stroke();
        context.fillStyle = '#777'; context.textAlign = 'right'; context.fillText(db, left - 7, y(db) + 3);
    }
    context.strokeStyle = '#ffffff35'; context.beginPath(); context.moveTo(left, gainY(0)); context.lineTo(right, gainY(0)); context.stroke();
    context.fillStyle = '#aaa'; context.textAlign = 'right'; context.fillText('0 dB', left - 7, gainY(0) + 3);
    const line = (color, data, value) => {
        context.strokeStyle = color; context.lineWidth = 1.5; context.beginPath();
        data.forEach((bin, index) => { const px = x(bin[0]), py = value(bin); if (index === 0) context.moveTo(px, py); else context.lineTo(px, py); }); context.stroke();
    };
    if (spectrum.bins.length) {
        line('#bfc2c2', spectrum.bins, bin => y(bin[1]));
        line('#c692e4', spectrum.bins, bin => gainY(bin[2]));
    }
    if (!visual) return;
    line('#e6b23a', visual.thresholds ?? [], bin => y(bin[1] + visual.offsets[1]));
    line('#6ac8d8', visual.thresholds ?? [], bin => y(bin[1] + visual.offsets[0]));
    if (visual.curvePoints?.some(point => point.enabled)) {
        context.setLineDash([3, 4]); line('#dddddd80', visual.thresholds, bin => y(bin[1])); context.setLineDash([]);
    }
    for (const point of visual.curvePoints ?? []) {
        if (!markers.has(point.index)) {
            const marker = document.createElement('button'); marker.className = 'point-marker'; marker.textContent = point.index;
            marker.addEventListener('focus', () => selectPoint(point.index));
            pointDragging.bind(marker, point.index);
            marker.onclick = () => {
                selectPoint(point.index); document.querySelector('#curve-details').open = true;
                elements.get(`curve_point_frequency_${point.index}`)?.input.focus({preventScroll: true});
                groups.get(`point-${point.index}`)?.scrollIntoView({block: 'nearest'});
            };
            document.querySelector('#point-markers').append(marker); markers.set(point.index, marker);
        }
        const marker = markers.get(point.index); marker.hidden = !point.enabled;
        marker.style.left = `${x(point.frequency)}px`; marker.style.top = `${y(point.threshold)}px`;
        marker.title = `Point ${point.index}: ${point.frequency.toFixed(0)} Hz, ${point.offset >= 0 ? '+' : ''}${point.offset.toFixed(1)} dB offset`;
        marker.setAttribute('aria-label', marker.title);
        marker.setAttribute('aria-pressed', String(activePoint === point.index));
        if (pointDragging.preview?.index === point.index) {
            for (const [field, text] of [['frequency', `${point.frequency.toFixed(0)} Hz`], ['offset', `${point.offset.toFixed(1)} dB`]]) {
                const value = elements.get(`curve_point_${field}_${point.index}`)?.value;
                if (value && document.activeElement !== value) value.value = text;
            }
        }
    }
}
new ResizeObserver(() => { pointDragging.finish(true); invalidate(); }).observe(canvas);
new ResizeObserver(([entry]) => {
    document.querySelector('main').style.setProperty('--analyzer-height', `${entry.target.getBoundingClientRect().height}px`);
}).observe(document.querySelector('.analyzer'));
reducedMotion.addEventListener('change', () => {
    spectrum.update(state?.bins ?? [], performance.now(), reducedMotion.matches); invalidate();
});
document.addEventListener('visibilitychange', () => {
    if (document.hidden) { cancelAnimationFrame(animationFrame); animationFrame = 0; }
    else { spectrum.time = performance.now(); send({op: 'poll'}); invalidate(); }
});
setInterval(() => { if (!document.hidden) send({op: 'poll'}); }, 100);
send({op: 'poll', full: true});
