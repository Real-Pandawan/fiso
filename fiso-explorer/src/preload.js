// SPDX-License-Identifier: Apache-2.0
'use strict';
const { contextBridge, ipcRenderer, webUtils } = require('electron');

const call = async (ch, ...args) => {
  const r = await ipcRenderer.invoke(ch, ...args);
  if (!r.ok) throw new Error(r.error);
  return r.value;
};

contextBridge.exposeInMainWorld('fiso', {
  appInfo: () => call('app:info'),
  settings: {
    get: () => call('settings:get'),
    set: (s) => call('settings:set', s),
    clearCache: () => call('settings:clearCache'),
  },
  thumbs: { register: () => call('thumbs:register') },
  dialogs: {
    openFiso: () => call('dialog:openFiso'),
    openSources: (kind) => call('dialog:openSources', kind),
    openImage: () => call('dialog:openImage'),
    chooseDir: (d) => call('dialog:chooseDir', d),
    saveFiso: (name) => call('dialog:saveFiso', name),
  },
  info: (p) => call('fiso:info', p),
  verify: (p, job) => call('fiso:verify', p, job),
  extract: (p, o, job) => call('fiso:extract', p, o, job),
  create: (plan, job) => call('fiso:create', plan, job),
  edit: (p, e, job) => call('fiso:edit', p, e, job),
  album: (p) => call('fiso:album', p),
  cancel: (job) => call('job:cancel', job),
  batch: {
    chooseFolders: () => call('batch:chooseFolders'),
    scan: (dirs) => call('batch:scan', dirs),
    prepare: (album, outDir, options) => call('batch:prepare', album, outDir, options),
  },
  peekCue: (p) => call('source:peekCue', p),
  peekFiles: (ps) => call('source:peekFiles', ps),
  player: (p) => call('player:open', p),
  cd: {
    drives: () => call('cd:drives'),
    info: (d) => call('cd:info', d),
    eject: (d) => call('cd:eject', d),
    rip: (o, job) => call('cd:rip', o, job),
  },
  ar: { offset: (v, p) => call('ar:offset', v, p), check: (p, job) => call('ar:check', p, job) },
  mb: {
    thumb: (id) => call('mb:thumb', id),
    discid: (id, toc) => call('mb:discid', id, toc),
    search: (artist, album) => call('mb:search', artist, album),
    release: (id) => call('mb:release', id),
    cover: (id, kind) => call('caa:download', id, kind),
  },
  shell: { show: (p) => call('shell:show', p), site: () => call('shell:site'), spec: () => call('shell:spec') },
  pathOf: (file) => webUtils.getPathForFile(file),
  on: (ch, fn) => {
    const allowed = ['menu', 'open-files', 'job:progress'];
    if (allowed.includes(ch)) ipcRenderer.on(ch, (_e, ...a) => fn(...a));
  },
});
