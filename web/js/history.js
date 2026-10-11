// The Tools tab's link history card: the board's hourly link record (hist.get, docs/console.md#link-history),
// fetched page by page, drawn as one SVG of stacked panels on a shared time axis so one crosshair and tooltip serve
// them all. The here/peer colours (--chart-here/--chart-peer) were chosen with a colour-blind palette check.
import { $, esc, fmtDur, fmtNum, sum, niceScale } from './util.js';
import { S } from './state.js';
import { request } from './serial.js';
import { SNR_FLOOR } from './survey.js'; // SX127x demodulation limit per SF, dB

// The board's hourly link record (hist.get, docs/console.md#link-history), fetched page by page. Buckets count
// from the last hist.clear as if without a break: bucket i started now_s − i × period_s seconds before the reply,
// unless the board reset after it (firmware 0.14.0 keeps the history across resets; `boot` changes where one came):
// then it started earlier, by however long the reset took.
let hist = null; // { buckets, period, nowS, current, fetchedAt, peer, sf, role, persist, lastReset }
let histAt = -1; // bucket (index into hist.buckets) under the crosshair, -1 = none
let histLayout = null;
let histNote = null; // the card's text before any history is loaded

export async function loadHistory() {
  if (histNote === null) histNote = $('histNote').textContent;
  const buckets = [];
  let from = null, res;
  $('histPlot').classList.add('loading'); // keep the old chart while the pages come in
  try {
    for (;;) {
      res = await request('hist.get', from === null ? {} : { from });
      if (!res.ok) {
        throw new Error(res.error === 'unknown cmd' ? 'This firmware has no link history (0.4.0 or later needed).' : res.error || 'hist.get failed');
      }
      buckets.push(...res.rows.map((r) => Object.fromEntries(res.fields.map((f, i) => [f, r[i]]))));
      // A bucket closing while we page moves `current` on: keep going until the last page reaches it.
      if (!res.rows.length || res.rows[res.rows.length - 1][0] >= res.current) break;
      from = res.rows[res.rows.length - 1][0] + 1;
    }
  } finally {
    $('histPlot').classList.remove('loading');
  }
  hist = {
    buckets, period: res.period_s, nowS: res.now_s, current: res.current, fetchedAt: Date.now(),
    peer: S.role === 'house', sf: S.params.sf, role: S.role, persist: res.persist === true,
    lastReset: lastReset(buckets),
  };
  histAt = -1;
  renderHistory();
}

export function clearHistoryView() {
  hist = null;
  histAt = -1;
  histLayout = null;
  $('histBody').hidden = true;
  $('btnHistCsv').disabled = true;
  $('btnHistLoad').textContent = 'Load history';
  if (histNote !== null) $('histNote').textContent = histNote;
}

// Indexes i where the board reset between bucket i - 1 and bucket i (their `boot` differs; older firmware has none).
const resetsBetween = (B) => B.flatMap((b, i) => (i > 0 && b.boot !== undefined && B[i - 1].boot !== undefined
  && b.boot !== B[i - 1].boot ? [i] : []));
// The first bucket after the last reset (0: none in view). Buckets before it started earlier than histStart says.
const lastReset = (B) => resetsBetween(B).pop() ?? 0;
const beforeReset = (i) => i < hist.lastReset;
const histStart = (b) => hist.fetchedAt - (hist.nowS - b.idx * hist.period) * 1000;
// Seconds the bucket covers: all of its period, or up to now for the one in progress.
const histSpan = (b) => (b.idx < hist.current ? hist.period : Math.max(1, hist.nowS - b.idx * hist.period));
const fmtClock = (ms) => new Date(ms).toLocaleTimeString([], { hour: '2-digit', minute: '2-digit' });
const fmtDay = (ms) => new Date(ms).toLocaleDateString([], { weekday: 'short' });

// Buckets are hours unless hist.clear set another period (the e2e suite uses 60 s).
const histPeriodText = () => (hist.period % 60 ? `${hist.period} s` : `${hist.period / 60} min`);
const histBuckets = (n) => (hist.period === 3600 ? `${n} hour${n === 1 ? '' : 's'}` : `${n} bucket${n === 1 ? '' : 's'}`);

function renderHistory() {
  const B = hist.buckets;
  $('histBody').hidden = !B.length;
  $('btnHistCsv').disabled = !B.length;
  const first = B.length ? histStart(B[0]) : hist.fetchedAt;
  $('histNote').textContent = B.length
    ? `${histBuckets(B.length)}${hist.period === 3600 ? '' : ` of ${histPeriodText()}`} from ${hist.lastReset ? 'before ' : ''}${fmtDay(first)} ${fmtClock(first)} to now (the ${hist.role} board’s record; up to four days, ${hist.persist ? 'kept across resets: a dashed line marks each, and buckets before one started earlier than shown' : 'lost on reset'}). Loaded ${fmtClock(hist.fetchedAt)}.`
    : 'No history yet.';
  if (!B.length) return;
  renderHistTiles();
  const peerName = hist.role === 'house' ? 'at gate' : 'at house';
  $('histLegend').innerHTML = (hist.peer
    ? `<span><span class="sw line here"></span> here (${esc(hist.role)})</span><span><span class="sw line peer"></span> ${peerName}</span>`
    : '')
    + '<span><span class="sw band"></span> range to the worst value</span>'
    + '<span><span class="sw heat"></span> more problems = darker</span>'
    + (resetsBetween(B).length ? '<span><span class="sw reset"></span> board reset</span>' : '');
  drawHistory();
  renderHistTable();
}

function tile(label, value, sub) {
  return `<div class="tile"><div class="t-label">${esc(label)}</div><div class="t-value">${esc(value)}</div><div class="t-sub">${esc(sub)}</div></div>`;
}

function renderHistTiles() {
  const B = hist.buckets, peer = hist.peer;
  const span = sum(B, histSpan), down = sum(B, (b) => b.down_s);
  const up = 100 * (1 - down / span);
  const availability = tile('Link up', down ? `${up.toFixed(up > 99.9 ? 2 : 1)} %` : '100 %',
    down ? `down ${fmtDur(down * 1000)} of ${fmtDur(span * 1000)}` : `no downtime in ${fmtDur(span * 1000)}`);

  // Worst SNR margin: the lowest per-frame SNR in any bucket, either direction.
  const floor = SNR_FLOOR[hist.sf];
  let worst = null;
  for (const b of B) {
    for (const [v, side] of [[b.snr_min, 'here'], [peer ? b.peer_snr_min : null, hist.role === 'house' ? 'at gate' : 'at house']]) {
      if (v !== null && v !== undefined && (!worst || v < worst.v)) worst = { v, side, at: histStart(b) };
    }
  }
  const margin = floor === undefined || !worst
    ? tile('Worst SNR', worst ? `${fmtNum(worst.v)} dB` : '—', worst ? `${worst.side}, ${fmtDay(worst.at)} ${fmtClock(worst.at)}` : 'no frames received')
    : tile('Worst SNR margin', `${fmtNum(worst.v - floor)} dB`,
      `above the SF${hist.sf} limit (${floor} dB), ${worst.side}, ${fmtDay(worst.at)} ${fmtClock(worst.at)}`);

  const tx = sum(B, (b) => b.tx), retries = sum(B, (b) => b.retries);
  const resends = tile('Resends', tx ? `${((100 * retries) / tx).toFixed(1)} %` : '—',
    `${retries} of ${tx} frames sent here` + (peer ? `; the gate resent ${sum(B, (b) => b.peer_retries)}` : ''));

  const lost = (b) => b.giveups + (peer ? b.peer_giveups : 0);
  const crc = sum(B, (b) => b.crc_err + (peer ? b.peer_crc_err : 0));
  const hit = B.filter((b) => lost(b) > 0).length;
  const lostTile = tile('Messages lost', String(sum(B, lost)),
    `in ${hit} of ${histBuckets(B.length)}; ${crc} CRC error${crc === 1 ? '' : 's'}`);
  $('histTiles').innerHTML = availability + margin + resends + lostTile;
}

const HIST_PANELS = [
  { title: 'Received signal (dBm)', avg: 'rssi_avg', band: 'rssi_min' },
  { title: 'SNR margin (dB)', avg: 'snr_avg', band: 'snr_min', margin: true },
  { title: 'Noise floor (dBm)', avg: 'noise_avg', band: 'noise_max' },
];
const HIST_ROWS = [
  ['Resends', (b, p) => b.retries + (p ? b.peer_retries : 0)],
  ['Give-ups', (b, p) => b.giveups + (p ? b.peer_giveups : 0)],
  ['CRC errors', (b, p) => b.crc_err + (p ? b.peer_crc_err : 0)],
  ['Down (min)', (b) => b.down_s / 60],
];

function drawHistory() {
  const svg = $('histChart');
  const B = hist.buckets, n = B.length;
  const series = [['here', ''], ...(hist.peer ? [['peer', 'peer_']] : [])];
  const floor = SNR_FLOOR[hist.sf];
  const W = Math.max(300, Math.round(svg.parentElement.clientWidth));
  const padL = 70, padR = 10, plotW = W - padL - padR;
  // Capped bucket width, newest at the right edge: a young history doesn't stretch an hour across the card.
  const bw = Math.min(plotW / n, 28), x0 = padL + plotW - n * bw;
  const cx = (i) => x0 + (i + 0.5) * bw;
  const PH = 84, TITLE = 28, GAP = 14;
  let y = 0, out = '';
  const panels = [];

  for (const p of HIST_PANELS) {
    const shift = p.margin && floor !== undefined ? floor : 0;
    const title = p.margin ? (floor === undefined ? 'SNR (dB)' : `SNR margin above the SF${hist.sf} limit (dB)`) : p.title;
    const val = (b, pre, k) => (b[pre + k] === null || b[pre + k] === undefined ? null : b[pre + k] - shift);
    const vals = B.flatMap((b) => series.flatMap(([, pre]) => [val(b, pre, p.avg), val(b, pre, p.band)])).filter((v) => v !== null);
    if (p.margin) vals.push(0);
    if (!vals.length) vals.push(0);
    const sc = niceScale(Math.min(...vals), Math.max(...vals));
    const top = y + TITLE;
    const py = (v) => top + PH - ((v - sc.lo) / (sc.hi - sc.lo)) * PH;
    out += `<text class="ptitle" x="0" y="${y + 13}">${esc(title)}</text>`;
    for (const t of sc.ticks) {
      out += `<line class="${t === 0 && p.margin ? 'zero' : 'grid'}" x1="${padL}" x2="${W - padR}" y1="${py(t)}" y2="${py(t)}"/>`
        + `<text x="${padL - 6}" y="${py(t) + 4}" text-anchor="end">${t}</text>`;
    }
    if (p.margin) out += `<text x="${W - padR}" y="${py(0) - 4}" text-anchor="end">limit</text>`;
    for (const [cls, pre] of series) {
      // Runs of buckets with data: a band from the average to the worst value, and the average as a line.
      let run = [];
      const flush = () => {
        if (!run.length) return;
        const avg = run.map((i) => `${cx(i).toFixed(1)},${py(val(B[i], pre, p.avg)).toFixed(1)}`);
        const band = run.map((i) => `${cx(i).toFixed(1)},${py(val(B[i], pre, p.band)).toFixed(1)}`).reverse();
        if (run.length > 1) {
          out += `<polygon class="band ${cls}" points="${avg.concat(band).join(' ')}"/>`
            + `<polyline class="ln ${cls}" points="${avg.join(' ')}"/>`;
        } else {
          out += `<line class="ln ${cls}" x1="${cx(run[0]) - 3}" x2="${cx(run[0]) + 3}" y1="${avg[0].split(',')[1]}" y2="${avg[0].split(',')[1]}"/>`;
        }
        run = [];
      };
      B.forEach((b, i) => (val(b, pre, p.avg) === null ? flush() : run.push(i)));
      flush();
    }
    panels.push({ val, py, series, p });
    y = top + PH + GAP;
  }

  // Problem strip: one row per counter, each shaded against its own busiest bucket (values in the tooltip/table).
  out += `<text class="ptitle" x="0" y="${y + 13}">Problems per ${hist.period === 3600 ? 'hour' : `${histPeriodText()} bucket`}</text>`;
  const RH = 14, rowTop = y + TITLE, gap = bw >= 6 ? 2 : 0;
  HIST_ROWS.forEach(([label, f], r) => {
    const ry = rowTop + r * (RH + 3);
    const vals = B.map((b) => f(b, hist.peer));
    const max = Math.max(...vals);
    out += `<text x="${padL - 6}" y="${ry + RH - 3}" text-anchor="end">${esc(label)}</text>`
      + `<rect class="track" x="${padL}" y="${ry}" width="${plotW}" height="${RH}" rx="2"/>`;
    vals.forEach((v, i) => {
      if (!v) return;
      const pct = Math.round(30 + (70 * v) / max);
      out += `<rect x="${(x0 + i * bw + gap / 2).toFixed(1)}" y="${ry}" width="${Math.max(1, bw - gap).toFixed(1)}" height="${RH}" rx="2"`
        + ` style="fill: color-mix(in oklab, var(--chart-heat) ${pct}%, var(--surface))"/>`;
    });
  });
  y = rowTop + HIST_ROWS.length * (RH + 3) + 4;

  // Resets between buckets: a dashed line at the boundary. The buckets before one are drawn closer to now than they
  // were (the time the board was down isn't known).
  for (const i of resetsBetween(B)) {
    const x = (x0 + i * bw).toFixed(1);
    out += `<line class="reset" x1="${x}" x2="${x}" y1="18" y2="${y}"/>`;
  }

  // Time axis: bucket start times, spaced so labels don't touch, counted back from the newest.
  const k = [1, 2, 3, 4, 6, 8, 12, 24, 48, 96].find((s) => s * bw >= 100) || n; // room for "Mon 08:43 PM"
  let lastDay = '';
  for (let i = 0; i < n; i++) {
    if ((n - 1 - i) % k) continue;
    const t = histStart(B[i]), day = fmtDay(t);
    const label = day !== lastDay ? `${day} ${fmtClock(t)}` : fmtClock(t);
    lastDay = day;
    const x = Math.min(Math.max(cx(i), x0 + 40), W - padR - 40);
    out += `<text x="${x}" y="${y + 12}" text-anchor="middle">${esc(label)}</text>`;
  }
  const H = y + 18;

  svg.setAttribute('viewBox', `0 0 ${W} ${H}`);
  svg.setAttribute('height', H);
  svg.innerHTML = `${out}<g id="histHover"></g><rect class="hit" x="${x0}" y="0" width="${n * bw}" height="${H}"/>`;
  histLayout = { W, x0, bw, cx, H, panels, chartBottom: y };
  drawHistHover();
}

function drawHistHover() {
  const g = $('histHover'), tip = $('histTip');
  if (!g || !histLayout) return;
  if (histAt < 0 || histAt >= hist.buckets.length) {
    g.innerHTML = '';
    tip.hidden = true;
    return;
  }
  const { cx, panels, chartBottom } = histLayout;
  const b = hist.buckets[histAt], x = cx(histAt);
  let s = `<line class="cross" x1="${x}" x2="${x}" y1="18" y2="${chartBottom}"/>`;
  for (const { val, py, series, p } of panels) {
    for (const [cls, pre] of series) {
      const v = val(b, pre, p.avg);
      if (v !== null) s += `<circle class="dot ${cls}" cx="${x}" cy="${py(v)}" r="4"/>`;
    }
  }
  g.innerHTML = s;
  renderHistTip(b);
  // Beside the crosshair, on whichever side has room.
  const w = tip.offsetWidth, plot = $('histPlot').clientWidth;
  const px = (x / histLayout.W) * plot;
  tip.style.left = `${px + 14 + w > plot ? Math.max(0, px - 14 - w) : px + 14}px`;
}

// Built with textContent: values lead, the series name follows.
function renderHistTip(b) {
  const tip = $('histTip');
  tip.replaceChildren();
  const add = (cls, text, parent = tip) => {
    const el = document.createElement('div');
    el.className = cls;
    el.textContent = text;
    parent.appendChild(el);
    return el;
  };
  const row = (key, value, label) => {
    const r = add('r', '');
    const k = document.createElement('span');
    k.className = `key ${key}`;
    const v = document.createElement('b');
    v.textContent = value;
    const l = document.createElement('span');
    l.textContent = label;
    r.append(k, v, l);
  };
  const t = histStart(b), end = t + histSpan(b) * 1000;
  add('tt', `${fmtDay(t)} ${fmtClock(t)}–${fmtClock(end)}${b.idx === hist.current ? ' (so far)' : ''}`);
  if (beforeReset(hist.buckets.indexOf(b))) add('note', 'Before a reset: it was earlier than this');
  const floor = SNR_FLOOR[hist.sf];
  const peerName = hist.role === 'house' ? 'at gate' : 'at house';
  const sides = [['here', '', 'here'], ...(hist.peer ? [['peer', 'peer_', peerName]] : [])];
  add('sec', 'Signal · SNR');
  for (const [key, pre, name] of sides) {
    const m = b[`${pre}snr_min`];
    row(key, `${fmtNum(b[`${pre}rssi_avg`])} dBm`, `${name}, worst ${fmtNum(b[`${pre}rssi_min`])}`);
    row(key, `${fmtNum(b[`${pre}snr_avg`])} dB`, `SNR ${name}, worst ${fmtNum(m)}${floor !== undefined && m !== null ? ` (margin ${fmtNum(m - floor)})` : ''}`);
  }
  add('sec', 'Noise floor');
  for (const [key, pre, name] of sides) row(key, `${fmtNum(b[`${pre}noise_avg`])} dBm`, `${name}, peak ${fmtNum(b[`${pre}noise_max`])}`);
  add('sec', 'Traffic');
  const both = (here, peer) => (hist.peer ? `${here} here · ${peer} gate` : String(here));
  row('', `${b.tx} / ${b.rx}`, 'frames sent / received');
  row('', both(b.retries, b.peer_retries), 'resends');
  row('', both(b.giveups, b.peer_giveups), 'give-ups');
  row('', both(b.crc_err, b.peer_crc_err), 'CRC errors');
  row('', b.down_s ? fmtDur(b.down_s * 1000) : '0', 'link down');
  tip.hidden = false;
}

function histPointer(e) {
  if (!histLayout) return;
  const r = $('histChart').getBoundingClientRect();
  const x = ((e.clientX - r.left) / r.width) * histLayout.W;
  const i = Math.floor((x - histLayout.x0) / histLayout.bw);
  const next = i >= 0 && i < hist.buckets.length ? i : -1;
  if (next !== histAt) {
    histAt = next;
    drawHistHover();
  }
}

function histKey(e) {
  if (!hist?.buckets.length) return;
  const n = hist.buckets.length;
  const at = histAt < 0 ? n - 1 : histAt;
  const next = { ArrowLeft: at - 1, ArrowRight: at + 1, Home: 0, End: n - 1 }[e.key];
  if (e.key === 'Escape') histAt = -1;
  else if (next === undefined) return;
  else histAt = Math.min(n - 1, Math.max(0, histAt < 0 ? n - 1 : next));
  e.preventDefault();
  drawHistHover();
}

function renderHistTable() {
  const peer = hist.peer, peerName = hist.role === 'house' ? 'gate' : 'house';
  const cols = [
    ['Start', (b, i) => `${beforeReset(i) ? 'before ' : ''}${fmtDay(histStart(b))} ${fmtClock(histStart(b))}`],
    ['RSSI avg / worst (dBm)', (b) => `${fmtNum(b.rssi_avg)} / ${fmtNum(b.rssi_min)}`],
    ['SNR avg / worst (dB)', (b) => `${fmtNum(b.snr_avg)} / ${fmtNum(b.snr_min)}`],
    ['Noise avg / peak (dBm)', (b) => `${fmtNum(b.noise_avg)} / ${fmtNum(b.noise_max)}`],
    ...(peer ? [
      [`RSSI at ${peerName}`, (b) => `${fmtNum(b.peer_rssi_avg)} / ${fmtNum(b.peer_rssi_min)}`],
      [`SNR at ${peerName}`, (b) => `${fmtNum(b.peer_snr_avg)} / ${fmtNum(b.peer_snr_min)}`],
      [`Noise at ${peerName}`, (b) => `${fmtNum(b.peer_noise_avg)} / ${fmtNum(b.peer_noise_max)}`],
    ] : []),
    ['TX / RX', (b) => `${b.tx} / ${b.rx}`],
    ['Resends', (b) => (peer ? `${b.retries} + ${b.peer_retries}` : b.retries)],
    ['Give-ups', (b) => (peer ? `${b.giveups} + ${b.peer_giveups}` : b.giveups)],
    ['CRC errors', (b) => (peer ? `${b.crc_err} + ${b.peer_crc_err}` : b.crc_err)],
    ['Down', (b) => (b.down_s ? fmtDur(b.down_s * 1000) : '0')],
  ];
  const rows = hist.buckets.map((b, i) => [b, i]).reverse();
  const resets = resetsBetween(hist.buckets); // newest first: a dashed line under the first bucket after each reset
  $('histTable').innerHTML = `<thead><tr>${cols.map(([h]) => `<th>${esc(h)}</th>`).join('')}</tr></thead><tbody>`
    + rows.map(([b, i]) => `<tr${resets.includes(i) ? ' class="reset"' : ''}>${cols.map(([, f]) => `<td>${esc(f(b, i))}</td>`).join('')}</tr>`).join('')
    + `</tbody>${peer ? `<caption class="muted small" style="caption-side: bottom; text-align: left">Pairs “a + b”: here + ${peerName}.</caption>` : ''}`;
}

export function historyCsv() {
  const fields = Object.keys(hist.buckets[0]);
  const pad = (v) => String(v).padStart(2, '0');
  const stamp = (ms) => {
    const d = new Date(ms);
    return `${d.getFullYear()}-${pad(d.getMonth() + 1)}-${pad(d.getDate())} ${pad(d.getHours())}:${pad(d.getMinutes())}`;
  };
  return [['start', ...fields].join(','),
    ...hist.buckets.map((b) => [stamp(histStart(b)), ...fields.map((f) => (b[f] === null ? '' : b[f]))].join(','))].join('\n') + '\n';
}

export const historyRole = () => hist?.role;
export const historyLoaded = () => !!hist?.buckets.length;

// Pointer, keyboard and resize handling for the chart.
export function initHistory() {
  const chart = $('histChart');
  chart.addEventListener('pointermove', histPointer);
  chart.addEventListener('pointerdown', histPointer); // touch: tap a bucket
  chart.addEventListener('pointerleave', () => { histAt = -1; drawHistHover(); });
  chart.addEventListener('keydown', histKey);
  chart.addEventListener('blur', () => { histAt = -1; drawHistHover(); });
  // Redraw at the new width (text stays at its real size); also when the Tools tab is first shown.
  let histWidth = 0;
  new ResizeObserver(() => {
    const w = $('histPlot').clientWidth; // a redraw changes the height only, which mustn't trigger another
    if (w && w !== histWidth && hist?.buckets.length) drawHistory();
    histWidth = w;
  }).observe($('histPlot'));
}
