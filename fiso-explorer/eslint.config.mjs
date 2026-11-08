import globals from 'globals';
export default [
  {
    files: ['**/main.js', '**/preload.js', 'scripts/**/*.js'],
    languageOptions: { ecmaVersion: 2023, sourceType: 'commonjs', globals: { ...globals.node } },
    rules: { 'no-unused-vars': ['error', { args: 'none', caughtErrors: 'none' }], 'no-undef': 'error' },
  },
  {
    files: ['**/renderer/**/*.js'],
    languageOptions: {
      ecmaVersion: 2023,
      sourceType: 'script',
      globals: { ...globals.browser, api: 'readonly', I18N: 'writable', HELP: 'writable', LIMITS: 'writable', icon: 'readonly' },
    },
    rules: { 'no-unused-vars': ['error', { args: 'none', caughtErrors: 'none', vars: 'local' }], 'no-undef': 'error' },
  },
];
