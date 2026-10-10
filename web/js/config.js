// The Config tab: the board's settings as a form (applied in chunks, saved on request), import and export.
import { $, esc, download } from './util.js';
import { S } from './state.js';
import { toast } from './ui.js';
import { call, request } from './serial.js';
import { GROUPS, HELP, MUST_MATCH, SELECTS } from './settings.js';
import { refreshInfo } from './status.js';

export async function loadConfig() {
  const res = await call('config.get');
  S.meta = res.meta;
  S.params = res.params;
  renderConfig();
  // Remote-writable params come from the board's meta, so firmware changes to P_REMOTE follow automatically.
  const sel = $('remParam');
  const keep = sel.value;
  sel.replaceChildren(...S.meta.filter((m) => m.remote).map((m) => new Option(m.name, m.name)));
  if (keep && S.meta.some((m) => m.remote && m.name === keep)) sel.value = keep;
  syncRemValue();
}

// Show the selected remote param's range on the value box (the board still validates it).
export function syncRemValue() {
  const m = S.meta.find((x) => x.name === $('remParam').value);
  const el = $('remValue');
  if (!m) return;
  el.min = m.min;
  el.max = m.max;
  el.placeholder = `${m.min}–${m.max}`;
}

export function renderConfig() {
  const byName = Object.fromEntries(S.meta.map((m) => [m.name, m]));
  const form = $('cfgForm');
  form.innerHTML = '';
  for (const [title, names] of GROUPS) {
    const card = document.createElement('div');
    card.className = 'card';
    card.innerHTML = `<div class="label">${title}</div>`;
    for (const name of names) {
      const m = byName[name];
      if (!m) continue;
      const row = document.createElement('div');
      row.className = 'field';
      const id = esc(`p_${name}`); // names come from the board's meta
      let input;
      if (!SELECTS[name] && m.min === 0 && m.max === 1) {
        input = `<input id="${id}" type="checkbox" class="toggle" role="switch">`;
      } else if (SELECTS[name]) {
        const opts = SELECTS[name];
        input = `<select id="${id}">${opts.map(([v, t]) => `<option value="${v}">${t}</option>`).join('')}</select>`;
      } else {
        input = `<input id="${id}" type="number" min="${esc(m.min)}" max="${esc(m.max)}" step="1">`;
      }
      const help = HELP[name]
        ? `<button type="button" class="info" aria-label="About ${esc(name)}" aria-describedby="${id}_tip">i</button>`
          + `<span class="tip" role="tooltip" id="${id}_tip">${esc(HELP[name])}</span>`
        : '';
      // Info icon in its own column at the far right, after the entry field (an empty cell keeps rows aligned).
      const star = MUST_MATCH.has(name) ? '<span class="must" title="Must match on both boards" aria-label="must match on both boards">*</span>' : '';
      row.innerHTML = `<label for="${id}">${esc(name)}${star}</label>${input}${help || '<span></span>'}`;
      card.appendChild(row);
      const el = row.querySelector('input, select');
      setField(el, S.params[name]);
      el.addEventListener('input', () => {
        const v = fieldValue(el);
        row.classList.toggle('dirty', v !== S.params[name]);
        if (el.type === 'number') el.setAttribute('aria-invalid', String(!Number.isInteger(v) || v < m.min || v > m.max || el.value === ''));
        updateDirtyCount();
      });
      if (el.type === 'number') el.addEventListener('keydown', (e) => { if (e.key === 'Enter') $('btnCfgApply').click(); });
    }
    form.appendChild(card);
  }
  updateDirtyCount();
}

// Edits in the form not yet applied to the board.
export const dirtyCount = () => Object.keys(formValues(true)).length;
export function updateDirtyCount() {
  const n = dirtyCount();
  $('btnCfgApply').textContent = n ? `Apply (${n})` : 'Apply';
  markConfigTab();
}

// A dot on the Config tab while edits are unapplied or applied settings are unsaved, seen from any tab.
export function markConfigTab() {
  const t = $('tabbtn-config');
  const n = S.meta.length ? dirtyCount() : 0;
  const pending = n > 0 || S.appliedUnsaved;
  t.classList.toggle('pending', pending);
  t.title = n ? `${n} unapplied edit${n === 1 ? '' : 's'}` : S.appliedUnsaved ? 'Applied settings not saved to flash yet' : '';
}

export function markUnsaved(on) {
  S.appliedUnsaved = on;
  const b = $('btnCfgSave');
  b.textContent = on ? 'Save to flash •' : 'Save to flash';
  b.title = on ? 'Applied settings are running but not saved yet: a reboot or power cut loses them' : '';
  markConfigTab();
}

// On/off parameters are checkbox toggles; everything else carries its value in .value.
export function fieldValue(el) {
  return el.type === 'checkbox' ? Number(el.checked) : Number(el.value);
}

export function setField(el, v) {
  if (el.type === 'checkbox') el.checked = !!v;
  else el.value = v;
}

export function formValues(onlyDirty) {
  const out = {};
  for (const m of S.meta) {
    const el = $(`p_${m.name}`);
    if (!el) continue;
    const v = fieldValue(el);
    if (!onlyDirty || v !== S.params[m.name]) out[m.name] = v;
  }
  return out;
}

const CFG_CHUNK = 8;

export async function applyConfig() {
  const changes = formValues(true);
  if (!Object.keys(changes).length) return toast('No changes.');
  for (const [k, v] of Object.entries(changes)) {
    const m = S.meta.find((x) => x.name === k);
    if (!Number.isInteger(v) || v < m.min || v > m.max) return toast(`${k} must be an integer ${m.min}–${m.max}`, 'err');
  }
  // These take effect at once, and the boards only hear each other while they all match.
  const radio = Object.keys(changes).filter((k) => MUST_MATCH.has(k));
  if (radio.length && !confirm(`Changing ${radio.join(', ')} takes the link down until the other board has the same `
      + `value${radio.length === 1 ? '' : 's'}. Connect that board next and change ${radio.length === 1 ? 'it' : 'them'} there too. Continue?`)) return;
  // Send at most CFG_CHUNK params per request so a full import stays well inside the board's line buffer.
  const entries = Object.entries(changes);
  const applied = [], errors = [];
  let reboot = false;
  let unsure = null; // params from the chunk that got no reply onwards: the board may or may not have them
  for (let i = 0; i < entries.length; i += CFG_CHUNK) {
    const chunk = Object.fromEntries(entries.slice(i, i + CFG_CHUNK));
    let res;
    try {
      res = await request('config.set', { params: chunk });
    } catch (e) {
      unsure = { keys: entries.slice(i).map(([k]) => k), error: e.message };
      break; // don't send more to a board that stopped answering
    }
    applied.push(...(res.applied || []));
    errors.push(...(res.errors || []));
    // A request-level error (no per-param list) rejects the whole chunk.
    if (!res.ok && !res.errors?.length) errors.push(...Object.keys(chunk).map((k) => `${k} (${res.error || 'failed'})`));
    reboot ||= !!res.reboot_required;
  }
  // Show what the board has now, even after a failure part-way: earlier chunks are applied.
  try {
    await loadConfig();
  } catch (e) {
    markUnsaved(true);
    return toast(`Applying stopped (${unsure?.error || e.message}) and the board didn't answer config.get: reconnect and check the settings.`, 'err');
  }
  const took = unsure ? unsure.keys.filter((k) => S.params[k] === changes[k]) : [];
  if (applied.length || took.length) markUnsaved(true);
  if (unsure) {
    // Keep the edits the board doesn't have as unapplied edits, so Apply can send them again.
    const left = unsure.keys.filter((k) => S.params[k] !== changes[k]);
    for (const k of left) {
      const el = $(`p_${k}`);
      if (!el) continue;
      setField(el, changes[k]);
      el.dispatchEvent(new Event('input'));
    }
    const got = [...applied, ...took];
    return toast(`Applying stopped: ${unsure.error}. ${got.length ? `The board has ${got.join(', ')}. ` : ''}`
      + `${left.length ? `Not applied: ${left.join(', ')} (still in the form; Apply again).` : ''}`, 'err');
  }
  const done = applied.length ? `Applied ${applied.join(', ')}. ${reboot ? 'Save and reboot for role change.' : 'Remember to Save.'}` : '';
  if (errors.length) toast(`Rejected: ${errors.join(', ')}. ${done}`, 'err');
  else toast(done || 'Nothing changed.');
}

export async function importConfig(file) {
  let data;
  try { data = JSON.parse(await file.text()); } catch { return toast('Not a valid JSON file.', 'err'); }
  const src = data?.params || data;
  if (!src || typeof src !== 'object') return toast('No GateLink settings found in that file.', 'err');
  // A file exported from the other board would turn this one into its twin: keep this board's role, unless it has
  // none yet (restoring a blank board).
  const roleName = (v) => SELECTS.role.find(([n]) => n === v)?.[1] ?? v;
  const keepRole = Number.isInteger(src.role) && S.params.role && src.role !== S.params.role;
  let found = 0;
  for (const m of S.meta) {
    const el = $(`p_${m.name}`);
    if (m.name === 'role' && keepRole) continue;
    if (el && Number.isInteger(src[m.name])) {
      setField(el, src[m.name]);
      el.dispatchEvent(new Event('input'));
      found++;
    }
  }
  if (!found) return toast('No GateLink settings found in that file.', 'err');
  const n = dirtyCount();
  const kept = keepRole ? ` Kept this board’s role (${roleName(S.params.role)}); the file is from a ${roleName(src.role)} board.` : '';
  toast(`Imported ${found} setting${found === 1 ? '' : 's'}; ${n ? `${n} differ${n === 1 ? 's' : ''} from the board. Review, then Apply and Save.` : 'all match the board already.'}${kept}`);
}

export async function reloadConfig() {
  const n = dirtyCount();
  if (n && !confirm(`Discard ${n} unapplied edit${n === 1 ? '' : 's'} and reload from the board?`)) return;
  await loadConfig();
}

export async function saveConfig() {
  // Save writes what the board is running; edits still in the form would silently be left out.
  const n = dirtyCount();
  if (n) return toast(`${n} edit${n === 1 ? ' isn’t' : 's aren’t'} applied yet. Apply first, then Save.`, 'err');
  await call('config.save');
  markUnsaved(false);
  toast('Saved to flash.');
}

export async function resetConfig() {
  if (!confirm('Erase config and key and restore defaults?')) return;
  await call('config.reset');
  markUnsaved(false); // reset saves the defaults
  await loadConfig();
  await refreshInfo();
  toast('Defaults restored. Reboot to apply role.');
}

// Exports what the board is running (applied, saved or not), not edits still in the form.
export function exportConfig() {
  download(`gatelink-${S.role}-config-${new Date().toISOString().slice(0, 10)}.json`, JSON.stringify({ role: S.role, params: S.params }, null, 2));
  const n = dirtyCount();
  if (n) toast(`Exported the board’s settings. ${n} unapplied edit${n === 1 ? ' isn’t' : 's aren’t'} included: Apply first to export ${n === 1 ? 'it' : 'them'}.`);
}
