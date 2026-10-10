// Small helpers shared by the console's modules. Nothing here touches the page until it is called, so the pure ones
// are also unit-tested under Node (tests/web/unit).

export const $ = (id) => document.getElementById(id);
// Escape text for innerHTML (element content and quoted attributes).
export const esc = (t) => String(t).replace(/&/g, '&amp;').replace(/</g, '&lt;').replace(/>/g, '&gt;').replace(/"/g, '&quot;');
export const cap = (t) => t[0].toUpperCase() + t.slice(1);
export const yesNo = (v) => (v ? 'yes' : 'no');
export const pill = (on) => `<span class="pill ${on ? 'on' : ''}">${on ? 'ON' : 'off'}</span>`;
export const fmtNum = (v, d = 1) => (v === null || v === undefined ? '—' : Number(v).toFixed(d).replace(/\.0+$/, ''));
export const sum = (arr, f) => arr.reduce((a, b) => a + (f(b) || 0), 0);
export const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
export const hex8 = (n) => n.toString(16).toUpperCase().padStart(8, '0');

export function fmtDur(ms) {
  if (ms < 0) return 'never';
  const s = Math.floor(ms / 1000);
  if (s < 60) return `${s}s`;
  const m = Math.floor(s / 60);
  if (m < 60) return `${m}m ${s % 60}s`;
  const h = Math.floor(m / 60);
  if (h < 48) return `${h}h ${m % 60}m`;
  return `${Math.floor(h / 24)}d ${h % 24}h`;
}

// Compares "x.y.z" versions: negative, zero or positive.
export function cmpVer(a, b) {
  const pa = String(a).split('.').map(Number);
  const pb = String(b).split('.').map(Number);
  for (let i = 0; i < 3; i++) if ((pa[i] || 0) !== (pb[i] || 0)) return (pa[i] || 0) - (pb[i] || 0);
  return 0;
}

// Greedy word wrap for SVG text (no auto-wrap there); `max` is in characters.
export function wrapText(text, max) {
  const lines = [];
  let line = '';
  for (const w of text.split(' ')) {
    if (line && line.length + 1 + w.length > max) {
      lines.push(line);
      line = w;
    } else line = line ? `${line} ${w}` : w;
  }
  return line ? [...lines, line] : lines;
}

// Clean tick values for a [lo, hi] range, about three of them.
export function niceScale(lo, hi) {
  const span = Math.max(hi - lo, 1);
  const step = [1, 2, 5, 10, 20, 25, 50, 100].find((s) => span / s <= 3) || 200;
  const a = Math.floor(lo / step) * step, b = Math.ceil(hi / step) * step;
  const ticks = [];
  for (let v = a; v <= b + 1e-9; v += step) ticks.push(v);
  return { lo: a, hi: b === a ? a + step : b, ticks };
}

export function download(name, text, type = 'application/json') {
  const a = document.createElement('a');
  a.href = URL.createObjectURL(new Blob([text], { type }));
  a.download = name;
  a.click();
  setTimeout(() => URL.revokeObjectURL(a.href), 1000);
}
