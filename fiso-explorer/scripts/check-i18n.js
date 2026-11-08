#!/usr/bin/env node
// Checks that every translation defines exactly the keys of the English reference,
// with the same {placeholders}, and that help and limitation texts exist.
//   npm run check:i18n
// SPDX-License-Identifier: Apache-2.0
'use strict';
const fs = require('node:fs');
const path = require('node:path');

const dir = path.join(__dirname, '..', 'src', 'renderer', 'i18n');
const window = (global.window = {});
require(path.join(dir, 'en.js'));
for (const f of fs.readdirSync(dir).sort()) if (f !== 'en.js' && f.endsWith('.js')) require(path.join(dir, f));

const ref = window.I18N.en;
const placeholders = (s) => (s.match(/\{[a-z0-9:]+\}/g) || []).sort().join(' ');
let errors = 0;
for (const [lang, dict] of Object.entries(window.I18N)) {
  const report = (msg) => {
    console.error(`${lang}: ${msg}`);
    errors++;
  };
  for (const k of Object.keys(ref)) {
    if (!(k in dict)) report(`missing "${k}"`);
    else if (placeholders(ref[k]) !== placeholders(dict[k])) report(`placeholders differ in "${k}"`);
  }
  for (const k of Object.keys(dict)) if (!(k in ref)) report(`unknown key "${k}"`);
  if (!window.HELP[lang]) report('no help text');
  if (!Array.isArray(window.LIMITS[lang]) || window.LIMITS[lang].length !== window.LIMITS.en.length)
    report('limitations list differs');
}
if (errors) {
  console.error(`${errors} problem(s)`);
  process.exit(1);
}
console.log(`${Object.keys(window.I18N).length} languages, ${Object.keys(ref).length} strings each: OK`);
