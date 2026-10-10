// Browser tests for the web console (npm run test:browser): the real page in Chromium against fake boards
// (fake-serial.js). See tests/web/README.md.
import { defineConfig, devices } from '@playwright/test';

const PORT = 47123; // unusual, so it can't be another project's dev server

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
  projects: [{ name: 'chromium', use: { ...devices['Desktop Chrome'] } }],
  webServer: {
    command: `node serve.js ${PORT}`,
    cwd: import.meta.dirname,
    url: `http://127.0.0.1:${PORT}/index.html`,
    reuseExistingServer: false,
  },
});
