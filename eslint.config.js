// ESLint for the web console and its tests (npm run lint; CI runs it). Bug-finding rules, little style: the
// recommended set, plus the checks that catch mistakes in a no-build ES-module page.
import js from '@eslint/js';
import globals from 'globals';

export default [
  { ignores: ['node_modules/', 'web/firmware/', 'tests/web/test-results/', 'tests/web/report/'] },
  js.configs.recommended,
  {
    files: ['web/**/*.js'],
    languageOptions: { ecmaVersion: 2023, sourceType: 'module', globals: { ...globals.browser } },
    rules: {
      'no-unused-vars': ['error', { args: 'none', caughtErrors: 'none' }],
      'no-empty': ['error', { allowEmptyCatch: true }],
      eqeqeq: ['error', 'smart'],
      'no-implicit-globals': 'error',
      'no-restricted-globals': ['error', 'name', 'status', 'event'], // window.* that read like local variables
      'prefer-const': 'error',
      'no-var': 'error',
      'no-shadow': ['error', { builtinGlobals: false }],
    },
  },
  {
    files: ['tests/web/**/*.js', 'eslint.config.js'],
    languageOptions: { ecmaVersion: 2023, sourceType: 'module', globals: { ...globals.node, ...globals.browser } },
    rules: { 'no-unused-vars': ['error', { args: 'none', caughtErrors: 'none' }] },
  },
];
