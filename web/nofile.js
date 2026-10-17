// Opened from a file, browsers won't load the page's ES modules: say how to open it rather than leave it dead.
// A classic script (not a module), so it still runs from a file; its own file so the page's CSP needs no inline script.
if (location.protocol === 'file:') {
  document.getElementById('unsupportedMsg').textContent = 'This page can’t run from a file. Open the hosted console, '
    + 'or serve this folder (python -m http.server 8000 -d web) and open http://localhost:8000 in Chrome or Edge.';
  document.getElementById('unsupported').hidden = false;
}
