// Browser tests for the web console (npm run test:browser): the real page in Chromium against fake boards
// (fake-serial.js). See tests/web/README.md.
import { existsSync } from 'node:fs';
import { chromium, defineConfig, devices } from '@playwright/test';

const PORT = 47123; // unusual, so it can't be another project's dev server
// Without the pinned browser, a Chromium already on the machine: the cloud sessions' image has an older revision
// there (TODO.md), and the tests pass on it.
const FALLBACK = '/opt/pw-browsers/chromium';
const executablePath = !existsSync(chromium.executablePath()) && existsSync(FALLBACK) ? FALLBACK : undefined;

export default defineConfig({
  testDir: './browser',
  outputDir: './test-results',
  timeout: 30000,
  fullyParallel: true,
  retries: process.env.CI ? 1 : 0,
  reporter: process.env.CI ? [['list'], ['html', { outputFolder: 'report', open: 'never' }]] : 'list',
  use: {
    baseURL: `http://127.0.0.1:${PORT}`,
    trace: 'retain-on-failure',
  },
  projects: [{ name: 'chromium', use: { ...devices['Desktop Chrome'], launchOptions: { executablePath } } }],
  webServer: {
    command: `node serve.js ${PORT}`,
    cwd: import.meta.dirname,
    url: `http://127.0.0.1:${PORT}/index.html`,
    reuseExistingServer: false,
  },
});
