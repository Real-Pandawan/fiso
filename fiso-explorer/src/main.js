// FISO Explorer — main process
// SPDX-License-Identifier: Apache-2.0
'use strict';

const { app, BrowserWindow, ipcMain, dialog, Menu, protocol, shell, net, nativeTheme } = require('electron');
const path = require('node:path');
const fs = require('node:fs');
const os = require('node:os');
const crypto = require('node:crypto');
const { spawn, execFile } = require('node:child_process');

const APP_VERSION = app.getVersion();
const USER_AGENT = `FISOExplorer/${APP_VERSION} ( https://github.com/Real-Pandawan/fiso )`;
const PROJECT_URL = 'https://github.com/Real-Pandawan/fiso';

protocol.registerSchemesAsPrivileged([
  { scheme: 'fiso-audio', privileges: { standard: true, secure: true, stream: true, supportFetchAPI: true } },
]);

// ------------------------------------------------------------------ paths & settings
// The `fiso` command-line tool does all the audio work; the app only drives it.
// Packaged builds ship it in resources/bin; during development it is taken from a CMake build folder.
const FISO_EXE = process.platform === 'win32' ? 'fiso.exe' : 'fiso';
function fisoBin() {
  if (process.env.FISO_BIN) return process.env.FISO_BIN;
  if (app.isPackaged) return path.join(process.resourcesPath, 'bin', FISO_EXE);
  const candidates = [
    path.join(__dirname, '..', 'bin', FISO_EXE),
    path.join(__dirname, '..', '..', 'build', 'fiso-cli', FISO_EXE),
    path.join(__dirname, '..', '..', 'build', 'fiso-cli', 'Release', FISO_EXE),
  ];
  return candidates.find((p) => fs.existsSync(p)) || candidates[0];
}
function specDir() {
  return app.isPackaged ? path.join(process.resourcesPath, 'spec') : path.join(__dirname, '..', '..', 'spec');
}

const settingsPath = () => path.join(app.getPath('userData'), 'settings.json');
let defaults = {
  lang: 'en',
  theme: 'system',
  outDir: '',
  template: '',
  accurateRip: true,
  ripMode: 'secure',
  readOffsets: {},
  defaultReader: '',
};
let settings = { ...defaults };
function loadSettings() {
  try {
    settings = { ...defaults, ...JSON.parse(fs.readFileSync(settingsPath(), 'utf8')) };
  } catch {
    settings = { ...defaults };
  }
  nativeTheme.themeSource = settings.theme;
}
function saveSettings() {
  fs.mkdirSync(path.dirname(settingsPath()), { recursive: true });
  fs.writeFileSync(settingsPath(), JSON.stringify(settings, null, 2));
}

const tmpRoot = () => {
  const d = path.join(os.tmpdir(), 'fiso-explorer-' + process.pid);
  fs.mkdirSync(d, { recursive: true });
  return d;
};

// ------------------------------------------------------------------ running the fiso tool
const jobs = new Map(); // jobId -> child process

function runFiso(args, { jobId, onProgress } = {}) {
  return new Promise((resolve) => {
    let child;
    try {
      child = spawn(fisoBin(), args, { stdio: ['ignore', 'pipe', 'pipe'] });
    } catch (e) {
      resolve({ code: 9, stdout: '', error: { message: String(e) } });
      return;
    }
    if (jobId) jobs.set(jobId, child);
    let out = '',
      errBuf = '',
      error = null;
    const plain = [];
    child.stdout.on('data', (d) => (out += d));
    child.stderr.on('data', (d) => {
      errBuf += d;
      let i;
      while ((i = errBuf.indexOf('\n')) >= 0) {
        const line = errBuf.slice(0, i).trim();
        errBuf = errBuf.slice(i + 1);
        if (!line) continue;
        try {
          const j = JSON.parse(line);
          if (j.progress && onProgress) onProgress(j.progress);
          else if (j.error) error = j.error;
        } catch {
          plain.push(line);
        }
      }
    });
    child.on('error', (e) => {
      error = { message: `cannot start the fiso tool (${e.message})` };
    });
    child.on('close', (code) => {
      if (jobId) jobs.delete(jobId);
      if (!error && code !== 0 && plain.length) error = { message: plain.join('\n') };
      resolve({ code: code ?? 9, stdout: out, error, warnings: plain });
    });
  });
}

function parseOut(r) {
  try {
    return JSON.parse(r.stdout);
  } catch {
    return null;
  }
}

function progressSender(event, jobId) {
  let last = 0;
  return (p) => {
    const now = Date.now();
    if (now - last < 80 && p.done !== p.total) return;
    last = now;
    if (!event.sender.isDestroyed()) event.sender.send('job:progress', { jobId, ...p });
  };
}

// ------------------------------------------------------------------ audio streaming (gap-less timeline)
const streams = new Map(); // token -> { path, rate, channels, total }

function wavHeader(rate, ch, frames) {
  const data = frames * ch * 2;
  const b = Buffer.alloc(44);
  b.write('RIFF', 0);
  b.writeUInt32LE(36 + data, 4);
  b.write('WAVE', 8);
  b.write('fmt ', 12);
  b.writeUInt32LE(16, 16);
  b.writeUInt16LE(1, 20);
  b.writeUInt16LE(ch, 22);
  b.writeUInt32LE(rate, 24);
  b.writeUInt32LE(rate * ch * 2, 28);
  b.writeUInt16LE(ch * 2, 32);
  b.writeUInt16LE(16, 34);
  b.write('data', 36);
  b.writeUInt32LE(data, 40);
  return b;
}

function handleAudio(request) {
  const url = new URL(request.url);
  const s = streams.get(url.hostname);
  if (!s) return new Response('not found', { status: 404 });
  const frameBytes = s.channels * 2;
  const header = wavHeader(s.rate, s.channels, s.total);
  const size = 44 + s.total * frameBytes;
  let start = 0,
    end = size - 1,
    partial = false;
  const range = request.headers.get('range');
  const m = range && /bytes=(\d*)-(\d*)/.exec(range);
  if (m) {
    partial = true;
    if (m[1]) start = parseInt(m[1], 10);
    if (m[2]) end = Math.min(parseInt(m[2], 10), size - 1);
    if (!m[1] && m[2]) {
      start = size - parseInt(m[2], 10);
      end = size - 1;
    }
    if (start >= size || start > end) return new Response(null, { status: 416, headers: { 'Content-Range': `bytes */${size}` } });
  }
  const length = end - start + 1;
  let child = null;
  const body = new ReadableStream({
    start(controller) {
      let pos = start;
      if (pos < 44) {
        controller.enqueue(new Uint8Array(header.subarray(pos, Math.min(44, end + 1))));
        pos = 44;
      }
      if (pos > end) {
        controller.close();
        return;
      }
      const off = pos - 44;
      const f0 = Math.floor(off / frameBytes);
      const f1 = Math.floor((end - 44) / frameBytes);
      let skip = off - f0 * frameBytes;
      let remaining = end + 1 - pos;
      child = spawn(fisoBin(), ['pcm', s.path, '--start', String(f0), '--count', String(f1 - f0 + 1), '--format', 's16']);
      child.stdout.on('data', (d) => {
        if (skip) {
          const k = Math.min(skip, d.length);
          d = d.subarray(k);
          skip -= k;
        }
        if (d.length > remaining) d = d.subarray(0, remaining);
        remaining -= d.length;
        if (d.length) {
          try {
            controller.enqueue(new Uint8Array(d));
          } catch {
            child.kill();
          }
        }
      });
      child.on('close', () => {
        try {
          controller.close();
        } catch {
          /* already closed */
        }
      });
      child.on('error', (e) => {
        try {
          controller.error(e);
        } catch {
          /* ignore */
        }
      });
    },
    cancel() {
      if (child) child.kill();
    },
  });
  const headers = {
    'Content-Type': 'audio/wav',
    'Accept-Ranges': 'bytes',
    'Content-Length': String(length),
  };
  if (partial) headers['Content-Range'] = `bytes ${start}-${end}/${size}`;
  return new Response(body, { status: partial ? 206 : 200, headers });
}

// ------------------------------------------------------------------ MusicBrainz / Cover Art Archive
const MB = 'https://musicbrainz.org/ws/2';
let mbQueue = Promise.resolve();
let mbLast = 0;
const cachePath = () => path.join(app.getPath('userData'), 'mb-cache.json');
let mbCache = null;
function cacheGet(k) {
  if (!mbCache) {
    try {
      mbCache = JSON.parse(fs.readFileSync(cachePath(), 'utf8'));
    } catch {
      mbCache = {};
    }
  }
  const e = mbCache[k];
  if (e && Date.now() - e.t < 30 * 86400e3) return e.v;
  return undefined;
}
function cacheSet(k, v) {
  mbCache[k] = { t: Date.now(), v };
  try {
    fs.writeFileSync(cachePath(), JSON.stringify(mbCache));
  } catch {
    /* cache is optional */
  }
}

// One request per second at most, as required by the MusicBrainz API rules.
function mbFetch(url) {
  const cached = cacheGet(url);
  if (cached !== undefined) return Promise.resolve(cached);
  const run = async () => {
    for (let attempt = 0; attempt < 3; attempt++) {
      const wait = Math.max(0, mbLast + 1100 - Date.now());
      if (wait) await new Promise((r) => setTimeout(r, wait));
      mbLast = Date.now();
      let res;
      try {
        res = await net.fetch(url, { headers: { 'User-Agent': USER_AGENT, Accept: 'application/json' } });
      } catch (e) {
        throw new Error(`MusicBrainz unreachable — check the Internet connection (${e.message})`);
      }
      if (res.status === 503 || res.status === 429) {
        await new Promise((r) => setTimeout(r, 2000 * (attempt + 1)));
        continue;
      }
      if (res.status === 404) {
        cacheSet(url, null);
        return null;
      }
      if (!res.ok) throw new Error(`MusicBrainz HTTP ${res.status}`);
      const j = await res.json();
      cacheSet(url, j);
      return j;
    }
    throw new Error('MusicBrainz is busy, please retry later');
  };
  const p = mbQueue.then(run, run);
  mbQueue = p.catch(() => {});
  return p;
}

function artistCredit(ac) {
  return (ac || []).map((a) => (a.name || a.artist?.name || '') + (a.joinphrase || '')).join('');
}

function summarizeRelease(r) {
  return {
    id: r.id,
    title: r.title,
    artist: artistCredit(r['artist-credit']),
    date: r.date || '',
    country: r.country || '',
    label: r['label-info']?.[0]?.label?.name || '',
    catalog: r['label-info']?.[0]?.['catalog-number'] || '',
    barcode: r.barcode || '',
    discs: r['media']?.length || r['medium-count'] || 0,
    tracks: (r.media || []).map((m) => m['track-count']),
    format: (r.media || [])
      .map((m) => m.format || '')
      .filter(Boolean)
      .join(' + '),
    score: r.score,
  };
}

async function mbRelease(id) {
  const r = await mbFetch(
    `${MB}/release/${encodeURIComponent(id)}?inc=artist-credits+recordings+labels+release-groups+discids+genres&fmt=json`,
  );
  if (!r) return null;
  return {
    ...summarizeRelease(r),
    releaseGroupId: r['release-group']?.id || '',
    albumArtistId: r['artist-credit']?.[0]?.artist?.id || '',
    genre: r.genres?.[0]?.name || r['release-group']?.genres?.[0]?.name || '',
    media: (r.media || []).map((m) => ({
      position: m.position,
      title: m.title || '',
      format: m.format || '',
      discids: (m.discs || []).map((d) => d.id),
      tracks: (m.tracks || []).map((t) => ({
        position: t.position,
        title: t.title,
        artist: artistCredit(t['artist-credit']),
        recordingId: t.recording?.id || '',
        trackId: t.id,
        length: t.length,
      })),
    })),
  };
}

async function caaDownload(mbid, kind) {
  const list = await mbFetch(`https://coverartarchive.org/release/${encodeURIComponent(mbid)}`).catch(() => null);
  if (!list || !list.images) return null;
  const img = list.images.find((i) => (kind === 'back' ? i.back : i.front)) || null;
  if (!img) return null;
  const url = img.thumbnails?.['1200'] || img.thumbnails?.large || img.image;
  const res = await net.fetch(url, { headers: { 'User-Agent': USER_AGENT } });
  if (!res.ok) throw new Error(`Cover Art Archive HTTP ${res.status}`);
  const buf = Buffer.from(await res.arrayBuffer());
  if (buf.length > 15 * 1024 * 1024) throw new Error('picture too large');
  const isJpeg = buf[0] === 0xff && buf[1] === 0xd8;
  const isPng = buf.subarray(0, 8).equals(Buffer.from([0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a]));
  if (!isJpeg && !isPng) throw new Error('downloaded file is not a JPEG or PNG image');
  const dir = path.join(app.getPath('userData'), 'covers');
  fs.mkdirSync(dir, { recursive: true });
  const file = path.join(dir, `${mbid}-${kind}${isPng ? '.png' : '.jpg'}`);
  fs.writeFileSync(file, buf);
  return { path: file, dataUrl: `data:image/${isPng ? 'png' : 'jpeg'};base64,${buf.toString('base64')}` };
}

// Small front-cover thumbnails for MusicBrainz search results (Cover Art Archive, 250 px).
const thumbMem = new Map();
let thumbActive = 0;
const thumbWaiters = [];
async function caaThumb(mbid) {
  if (!/^[0-9a-f-]{36}$/i.test(mbid)) return null;
  if (thumbMem.has(mbid)) return thumbMem.get(mbid);
  const dir = path.join(app.getPath('userData'), 'thumbs');
  const file = path.join(dir, mbid);
  try {
    const buf = fs.readFileSync(file);
    const v = buf.length ? imageBufferToDataUrl(buf) : null;
    thumbMem.set(mbid, v);
    return v;
  } catch {
    /* not cached yet */
  }
  if (thumbActive >= 4) await new Promise((r) => thumbWaiters.push(r));
  thumbActive++;
  try {
    const res = await net.fetch(`https://coverartarchive.org/release/${mbid}/front-250`, {
      headers: { 'User-Agent': USER_AGENT },
    });
    let v = null,
      buf = Buffer.alloc(0);
    if (res.ok) {
      buf = Buffer.from(await res.arrayBuffer());
      if (buf.length > 2 * 1024 * 1024) buf = Buffer.alloc(0);
      v = buf.length ? imageBufferToDataUrl(buf) : null;
      if (!v) buf = Buffer.alloc(0);
    } else if (res.status !== 404) {
      return null; // temporary failure: do not cache
    }
    fs.mkdirSync(dir, { recursive: true });
    fs.writeFileSync(file, buf); // an empty file remembers "no artwork"
    thumbMem.set(mbid, v);
    return v;
  } catch {
    return null;
  } finally {
    thumbActive--;
    const w = thumbWaiters.shift();
    if (w) w();
  }
}
function imageBufferToDataUrl(buf) {
  const isPng = buf[0] === 0x89 && buf[1] === 0x50;
  const isJpeg = buf[0] === 0xff && buf[1] === 0xd8;
  if (!isPng && !isJpeg) return null;
  return `data:image/${isPng ? 'png' : 'jpeg'};base64,${buf.toString('base64')}`;
}

// AccurateRip: drive read offsets and the checksum database (http://www.accuraterip.com).
const normName = (s) =>
  String(s || '')
    .toUpperCase()
    .replace(/[^A-Z0-9]+/g, ' ')
    .trim();
async function arOffsetFor(vendor, product) {
  const res = await net.fetch('http://www.accuraterip.com/driveoffsets.htm', { headers: { 'User-Agent': USER_AGENT } });
  if (!res.ok) throw new Error(`AccurateRip HTTP ${res.status}`);
  const html = await res.text();
  const want = normName(`${vendor} ${product}`);
  const wantProduct = normName(product);
  let best = null;
  for (const row of html.split(/<tr[^>]*>/i)) {
    const cells = row
      .split(/<td[^>]*>/i)
      .slice(1)
      .map((c) =>
        c
          .replace(/<[^>]+>/g, '')
          .replace(/&nbsp;/g, ' ')
          .trim(),
      );
    if (cells.length < 2) continue;
    const m = /^([+-]?\d+)$/.exec(cells[1].replace(/\s/g, ''));
    if (!m) continue;
    const name = normName(cells[0]);
    const submits = parseInt((cells[2] || '0').replace(/\D/g, ''), 10) || 0;
    const score = name === want ? 3 : name.endsWith(wantProduct) && wantProduct.length > 3 ? 2 : 0;
    if (score && (!best || score > best.score || (score === best.score && submits > best.submits)))
      best = { name: cells[0], offset: parseInt(m[1], 10), submits, score };
  }
  return best;
}

// ------------------------------------------------------------------ helpers for the renderer
function imageDataUrl(file) {
  const buf = fs.readFileSync(file);
  const isPng = buf[0] === 0x89 && buf[1] === 0x50;
  const isJpeg = buf[0] === 0xff && buf[1] === 0xd8;
  if (!isPng && !isJpeg) return null;
  return `data:image/${isPng ? 'png' : 'jpeg'};base64,${buf.toString('base64')}`;
}

function peekCue(file) {
  let text = fs.readFileSync(file);
  let s = text.toString('utf8');
  if (s.includes('�')) s = text.toString('latin1');
  const out = { album: '', artist: '', tracks: [], date: '', genre: '' };
  let cur = null;
  for (const raw of s.split(/\r?\n/)) {
    const line = raw.trim();
    const q = /"(.*)"/.exec(line);
    const val = q ? q[1] : line.split(/\s+/).slice(1).join(' ');
    const cmd = line.split(/\s+/)[0]?.toUpperCase();
    if (cmd === 'TRACK') {
      cur = { number: parseInt(line.split(/\s+/)[1], 10), title: '', artist: '', type: line.split(/\s+/)[2] };
      out.tracks.push(cur);
    } else if (cmd === 'TITLE') {
      if (cur) cur.title = val;
      else out.album = val;
    } else if (cmd === 'PERFORMER') {
      if (cur) cur.artist = val;
      else out.artist = val;
    } else if (cmd === 'REM' && !cur) {
      const parts = line.split(/\s+/);
      if (parts[1]?.toUpperCase() === 'DATE') out.date = parts.slice(2).join(' ');
      if (parts[1]?.toUpperCase() === 'GENRE') out.genre = val.replace(/^GENRE\s+/i, '');
    }
  }
  return out;
}

// ------------------------------------------------------------------ window & menu
let win = null;
let pendingOpen = [];

const MENU_TEXT = {
  fr: {
    file: 'Fichier',
    open: 'Ouvrir une image…',
    create: 'Créer une image…',
    close: 'Fermer',
    tools: 'Outils',
    extract: 'Extraire…',
    tocd: 'Convertir en image CD audio gravable…',
    rip: 'Créer depuis un CD audio…',
    eject: 'Éjecter le disque',
    ar: 'Vérifier avec AccurateRip',
    verify: 'Vérifier l’intégrité',
    meta: 'Modifier les métadonnées…',
    settings: 'Paramètres…',
    help: 'Aide',
    docs: 'Documentation',
    spec: 'Spécification du format FISO',
    site: 'Site du projet',
    about: 'À propos de FISO Explorer',
    edit: 'Édition',
    view: 'Affichage',
    window: 'Fenêtre',
  },
  en: {
    file: 'File',
    open: 'Open Image…',
    create: 'Create Image…',
    close: 'Close',
    tools: 'Tools',
    extract: 'Extract…',
    tocd: 'Convert to a Burnable Audio CD Image…',
    rip: 'Create from an Audio CD…',
    eject: 'Eject Disc',
    ar: 'Verify with AccurateRip',
    verify: 'Verify Integrity',
    meta: 'Edit Metadata…',
    settings: 'Settings…',
    help: 'Help',
    docs: 'Documentation',
    spec: 'FISO Format Specification',
    site: 'Project Website',
    about: 'About FISO Explorer',
    edit: 'Edit',
    view: 'View',
    window: 'Window',
  },
  es: {
    file: 'Archivo',
    open: 'Abrir imagen…',
    create: 'Crear imagen…',
    close: 'Cerrar',
    tools: 'Herramientas',
    extract: 'Extraer…',
    tocd: 'Convertir en imagen de CD de audio grabable…',
    rip: 'Crear desde un CD de audio…',
    eject: 'Expulsar disco',
    ar: 'Verificar con AccurateRip',
    verify: 'Verificar la integridad',
    meta: 'Editar metadatos…',
    settings: 'Ajustes…',
    help: 'Ayuda',
    docs: 'Documentación',
    spec: 'Especificación del formato FISO',
    site: 'Sitio del proyecto',
    about: 'Acerca de FISO Explorer',
    edit: 'Edición',
    view: 'Visualización',
    window: 'Ventana',
  },
  de: {
    file: 'Ablage',
    open: 'Image öffnen…',
    create: 'Image erstellen…',
    close: 'Schließen',
    tools: 'Werkzeuge',
    extract: 'Exportieren…',
    tocd: 'In brennbares Audio-CD-Image umwandeln…',
    rip: 'Von Audio-CD erstellen…',
    eject: 'CD auswerfen',
    ar: 'Mit AccurateRip prüfen',
    verify: 'Integrität prüfen',
    meta: 'Metadaten bearbeiten…',
    settings: 'Einstellungen…',
    help: 'Hilfe',
    docs: 'Dokumentation',
    spec: 'Spezifikation des FISO-Formats',
    site: 'Projektwebsite',
    about: 'Über FISO Explorer',
    edit: 'Bearbeiten',
    view: 'Darstellung',
    window: 'Fenster',
  },
  pt: {
    file: 'Ficheiro',
    open: 'Abrir imagem…',
    create: 'Criar imagem…',
    close: 'Fechar',
    tools: 'Ferramentas',
    extract: 'Extrair…',
    tocd: 'Converter em imagem de CD de áudio gravável…',
    rip: 'Criar a partir de um CD de áudio…',
    eject: 'Ejetar disco',
    ar: 'Verificar com o AccurateRip',
    verify: 'Verificar a integridade',
    meta: 'Editar metadados…',
    settings: 'Definições…',
    help: 'Ajuda',
    docs: 'Documentação',
    spec: 'Especificação do formato FISO',
    site: 'Site do projeto',
    about: 'Acerca do FISO Explorer',
    edit: 'Edição',
    view: 'Visualização',
    window: 'Janela',
  },
  it: {
    file: 'File',
    open: 'Apri immagine…',
    create: 'Crea immagine…',
    close: 'Chiudi',
    tools: 'Strumenti',
    extract: 'Estrai…',
    tocd: 'Converti in immagine CD audio masterizzabile…',
    rip: 'Crea da un CD audio…',
    eject: 'Espelli disco',
    ar: 'Verifica con AccurateRip',
    verify: 'Verifica integrità',
    meta: 'Modifica metadati…',
    settings: 'Impostazioni…',
    help: 'Aiuto',
    docs: 'Documentazione',
    spec: 'Specifica del formato FISO',
    site: 'Sito del progetto',
    about: 'Informazioni su FISO Explorer',
    edit: 'Modifica',
    view: 'Vista',
    window: 'Finestra',
  },
  nl: {
    file: 'Archief',
    open: 'Image openen…',
    create: 'Image maken…',
    close: 'Sluiten',
    tools: 'Gereedschap',
    extract: 'Exporteren…',
    tocd: 'Omzetten naar brandbare audio-cd-image…',
    rip: 'Maken van een audio-cd…',
    eject: 'Schijf uitwerpen',
    ar: 'Controleren met AccurateRip',
    verify: 'Integriteit controleren',
    meta: 'Metadata bewerken…',
    settings: 'Instellingen…',
    help: 'Help',
    docs: 'Documentatie',
    spec: 'Specificatie van het FISO-formaat',
    site: 'Projectwebsite',
    about: 'Over FISO Explorer',
    edit: 'Wijzig',
    view: 'Weergave',
    window: 'Venster',
  },
  ja: {
    file: 'ファイル',
    open: 'イメージを開く…',
    create: 'イメージを作成…',
    close: '閉じる',
    tools: 'ツール',
    extract: '書き出し…',
    tocd: '書き込み可能なオーディオ CD イメージに変換…',
    rip: 'オーディオ CD から作成…',
    eject: 'ディスクを取り出す',
    ar: 'AccurateRip で検証',
    verify: '整合性を検証',
    meta: 'メタデータを編集…',
    settings: '設定…',
    help: 'ヘルプ',
    docs: 'ドキュメント',
    spec: 'FISO フォーマット仕様',
    site: 'プロジェクトのウェブサイト',
    about: 'FISO Explorer について',
    edit: '編集',
    view: '表示',
    window: 'ウインドウ',
  },
};

function send(action) {
  if (win) win.webContents.send('menu', action);
}

function buildMenu() {
  const t = MENU_TEXT[settings.lang] || MENU_TEXT.en;
  const isMac = process.platform === 'darwin';
  const template = [
    ...(isMac
      ? [
          {
            label: app.name,
            submenu: [
              { label: t.about, click: () => send('about') },
              { type: 'separator' },
              { label: t.settings, accelerator: 'Cmd+,', click: () => send('settings') },
              { type: 'separator' },
              { role: 'services' },
              { type: 'separator' },
              { role: 'hide' },
              { role: 'hideOthers' },
              { role: 'unhide' },
              { type: 'separator' },
              { role: 'quit' },
            ],
          },
        ]
      : []),
    {
      label: t.file,
      submenu: [
        { label: t.open, accelerator: 'CmdOrCtrl+O', click: () => send('open') },
        { label: t.create, accelerator: 'CmdOrCtrl+N', click: () => send('create') },
        { type: 'separator' },
        { label: t.close, accelerator: 'CmdOrCtrl+W', click: () => send('close') },
        ...(isMac ? [] : [{ type: 'separator' }, { role: 'quit' }]),
      ],
    },
    {
      label: t.edit,
      submenu: [
        { role: 'undo' },
        { role: 'redo' },
        { type: 'separator' },
        { role: 'cut' },
        { role: 'copy' },
        { role: 'paste' },
        { role: 'selectAll' },
      ],
    },
    {
      label: t.tools,
      submenu: [
        { label: t.extract, accelerator: 'CmdOrCtrl+E', click: () => send('extract') },
        { label: t.tocd, accelerator: 'CmdOrCtrl+B', click: () => send('tocd') },
        // CD drives are supported on macOS only in this version
        ...(isMac
          ? [
              { label: t.rip, accelerator: 'CmdOrCtrl+R', click: () => send('rip') },
              { label: t.eject, accelerator: 'CmdOrCtrl+Shift+E', click: () => send('eject') },
            ]
          : []),
        { type: 'separator' },
        { label: t.verify, accelerator: 'CmdOrCtrl+Shift+V', click: () => send('verify') },
        { label: t.ar, click: () => send('accuraterip') },
        { label: t.meta, accelerator: 'CmdOrCtrl+I', click: () => send('meta') },
        ...(isMac ? [] : [{ type: 'separator' }, { label: t.settings, click: () => send('settings') }]),
      ],
    },
    {
      label: t.view,
      submenu: [
        { role: 'reload', visible: !app.isPackaged },
        { role: 'toggleDevTools', visible: !app.isPackaged },
        { role: 'resetZoom' },
        { role: 'zoomIn' },
        { role: 'zoomOut' },
        { type: 'separator' },
        { role: 'togglefullscreen' },
      ],
    },
    ...(isMac ? [{ label: t.window, role: 'windowMenu' }] : []),
    {
      label: t.help,
      role: 'help',
      submenu: [
        { label: t.docs, click: () => send('help') },
        { label: t.spec, click: () => openSpec() },
        { label: t.site, click: () => shell.openExternal(PROJECT_URL) },
        ...(isMac ? [] : [{ label: t.about, click: () => send('about') }]),
      ],
    },
  ];
  Menu.setApplicationMenu(Menu.buildFromTemplate(template));
}

function openSpec() {
  const name =
    settings.lang === 'fr' && fs.existsSync(path.join(specDir(), 'FISO_FORMAT_v1.0_FR.md'))
      ? 'FISO_FORMAT_v1.0_FR.md'
      : 'FISO_FORMAT_v1.0.md';
  shell.openPath(path.join(specDir(), name));
}

function createWindow() {
  win = new BrowserWindow({
    width: 1180,
    height: 780,
    minWidth: 820,
    minHeight: 560,
    title: 'FISO Explorer',
    titleBarStyle: process.platform === 'darwin' ? 'hiddenInset' : 'default',
    backgroundColor: nativeTheme.shouldUseDarkColors ? '#141416' : '#f5f3ee',
    show: false,
    webPreferences: {
      preload: path.join(__dirname, 'preload.js'),
      contextIsolation: true,
      nodeIntegration: false,
      sandbox: true,
    },
  });
  win.loadFile(path.join(__dirname, 'renderer', 'index.html'));
  win.once('ready-to-show', () => win.show());
  win.webContents.setWindowOpenHandler(() => ({ action: 'deny' }));
  win.webContents.on('will-navigate', (e) => e.preventDefault());
  win.on('closed', () => {
    win = null;
  });
}

function openPaths(paths) {
  const list = paths.filter((p) => p.toLowerCase().endsWith('.fiso'));
  if (!list.length) return;
  if (win && !win.webContents.isLoading()) win.webContents.send('open-files', list);
  else pendingOpen.push(...list);
}

app.on('open-file', (e, p) => {
  e.preventDefault();
  openPaths([p]);
});

// ------------------------------------------------------------------ IPC
function handle(name, fn) {
  ipcMain.handle(name, async (event, ...args) => {
    try {
      return { ok: true, value: await fn(event, ...args) };
    } catch (e) {
      return { ok: false, error: e.message || String(e) };
    }
  });
}

function fisoResult(r) {
  const v = parseOut(r);
  if (r.code !== 0) {
    const err = new Error(r.error?.message || `fiso exited with code ${r.code}`);
    err.code = r.code;
    throw Object.assign(err, { detail: v });
  }
  return v;
}

function registerIpc() {
  handle('app:info', () => ({
    version: APP_VERSION,
    platform: process.platform,
    arch: process.arch,
    bin: fisoBin(),
    binOk: fs.existsSync(fisoBin()),
  }));
  handle('settings:get', () => settings);
  handle('settings:set', (_e, s) => {
    settings = { ...settings, ...s };
    nativeTheme.themeSource = settings.theme;
    saveSettings();
    buildMenu();
    return settings;
  });
  handle('thumbs:register', () => registerThumbnails(true));
  handle('settings:clearCache', () => {
    mbCache = {};
    try {
      fs.unlinkSync(cachePath());
    } catch {
      /* none */
    }
    return true;
  });

  handle('dialog:openFiso', async () => {
    const r = await dialog.showOpenDialog(win, {
      properties: ['openFile', 'multiSelections'],
      filters: [{ name: 'FISO', extensions: ['fiso'] }],
    });
    return r.canceled ? [] : r.filePaths;
  });
  handle('dialog:openSources', async (_e, kind) => {
    const filters =
      kind === 'cue' ? [{ name: 'CUE', extensions: ['cue'] }] : [{ name: 'Audio (FLAC, WAV)', extensions: ['flac', 'wav'] }];
    const r = await dialog.showOpenDialog(win, {
      properties: kind === 'cue' ? ['openFile'] : ['openFile', 'multiSelections'],
      filters,
    });
    return r.canceled ? [] : r.filePaths;
  });
  handle('dialog:openImage', async () => {
    const r = await dialog.showOpenDialog(win, {
      properties: ['openFile'],
      filters: [{ name: 'Images', extensions: ['jpg', 'jpeg', 'png'] }],
    });
    if (r.canceled) return null;
    const dataUrl = imageDataUrl(r.filePaths[0]);
    if (!dataUrl) throw new Error('JPEG or PNG only');
    return { path: r.filePaths[0], dataUrl };
  });
  handle('dialog:chooseDir', async (_e, def) => {
    const r = await dialog.showOpenDialog(win, {
      properties: ['openDirectory', 'createDirectory'],
      defaultPath: def || settings.outDir || app.getPath('music'),
    });
    return r.canceled ? null : r.filePaths[0];
  });
  handle('dialog:saveFiso', async (_e, name) => {
    const r = await dialog.showSaveDialog(win, {
      defaultPath: path.join(settings.outDir || app.getPath('music'), (name || 'Album').replace(/[\/\\:*?"<>|]/g, '_') + '.fiso'),
      filters: [{ name: 'FISO', extensions: ['fiso'] }],
    });
    return r.canceled ? null : r.filePath;
  });

  handle('fiso:info', async (_e, file) => {
    const v = fisoResult(await runFiso(['info', file, '--json']));
    const covers = [];
    if (v.pictures?.length) {
      const dir = path.join(
        tmpRoot(),
        crypto
          .createHash('sha1')
          .update(file + Date.now())
          .digest('hex'),
      );
      const r = await runFiso(['extract', file, '--format', 'cover', '--out', dir, '--json', '--overwrite']);
      const x = parseOut(r);
      for (const f of x?.files || []) {
        const type = /back_cover/.test(f) ? 4 : /front_cover/.test(f) ? 3 : 0;
        const dataUrl = imageDataUrl(f);
        if (dataUrl) covers.push({ type, dataUrl });
      }
      fs.rmSync(dir, { recursive: true, force: true });
    }
    return { ...v, covers, stat: fs.statSync(file).size };
  });
  handle('fiso:verify', async (e, file, jobId) => {
    const r = await runFiso(['verify', file, '--json', '--progress-json'], { jobId, onProgress: progressSender(e, jobId) });
    const v = parseOut(r);
    if (!v) fisoResult(r);
    return v;
  });
  handle('fiso:extract', async (e, file, opts, jobId) => {
    const args = ['extract', file, '--format', opts.format, '--out', opts.outDir, '--json', '--progress-json'];
    if (opts.tracks?.length) args.push('--tracks', opts.tracks.join(','));
    if (opts.template) args.push('--template', opts.template);
    if (opts.includeHtoa) args.push('--include-htoa');
    if (opts.overwrite) args.push('--overwrite');
    if (opts.allowConversion) args.push('--convert');
    return fisoResult(await runFiso(args, { jobId, onProgress: progressSender(e, jobId) }));
  });
  handle('fiso:create', async (e, plan, jobId) => {
    const planFile = path.join(tmpRoot(), `plan-${jobId}.json`);
    fs.writeFileSync(planFile, JSON.stringify(plan));
    try {
      return fisoResult(
        await runFiso(['create', '--plan', planFile, '--json', '--progress-json'], {
          jobId,
          onProgress: progressSender(e, jobId),
        }),
      );
    } finally {
      fs.rmSync(planFile, { force: true });
    }
  });
  handle('fiso:edit', async (e, file, edit, jobId) => {
    const editFile = path.join(tmpRoot(), `edit-${jobId}.json`);
    fs.writeFileSync(editFile, JSON.stringify(edit));
    try {
      fisoResult(
        await runFiso(['meta', 'set', file, '--edit', editFile, '--progress-json'], {
          jobId,
          onProgress: progressSender(e, jobId),
        }),
      );
      return true;
    } finally {
      fs.rmSync(editFile, { force: true });
    }
  });
  handle('fiso:album', async (_e, file) => {
    const dir = path.dirname(file);
    const siblings = fs
      .readdirSync(dir)
      .filter((n) => n.toLowerCase().endsWith('.fiso'))
      .map((n) => path.join(dir, n))
      .slice(0, 60);
    const v = fisoResult(await runFiso(['group', ...siblings, '--json']));
    return (v.groups || []).find((g) => g.discs.some((d) => d.path === file)) || null;
  });
  handle('job:cancel', (_e, jobId) => {
    const c = jobs.get(jobId);
    if (c) c.kill('SIGTERM');
    return !!c;
  });

  handle('source:peekCue', (_e, file) => peekCue(file));
  handle('source:peekFiles', (_e, files) => files.map((f) => ({ path: f, name: path.basename(f), size: fs.statSync(f).size })));

  handle('player:open', async (_e, file) => {
    const v = fisoResult(await runFiso(['info', file, '--json']));
    const token = crypto.randomBytes(8).toString('hex');
    streams.clear();
    streams.set(token, { path: file, rate: v.audio.sample_rate, channels: v.audio.channels, total: v.audio.total_samples });
    return { url: `fiso-audio://${token}/disc.wav`, rate: v.audio.sample_rate };
  });

  handle('mb:thumb', (_e, id) => caaThumb(id));

  handle('cd:drives', async () => fisoResult(await runFiso(['drives', '--json'])));
  handle('cd:info', async (_e, device) => fisoResult(await runFiso(['cd-info', '--device', device || '', '--json'])));
  handle('cd:eject', async (_e, device) => {
    fisoResult(await runFiso(['eject', '--device', device || '']));
    return true;
  });
  handle('cd:rip', async (e, options, jobId) => {
    const f = path.join(tmpRoot(), `rip-${jobId}.json`);
    fs.writeFileSync(f, JSON.stringify(options));
    try {
      return fisoResult(
        await runFiso(['rip', options.output, '--options', f, '--json', '--progress-json'], {
          jobId,
          onProgress: progressSender(e, jobId),
        }),
      );
    } finally {
      fs.rmSync(f, { force: true });
    }
  });
  handle('ar:offset', (_e, vendor, product) => arOffsetFor(vendor, product));
  handle('ar:check', async (e, file, jobId) => {
    const first = fisoResult(
      await runFiso(['accuraterip', file, '--json', '--progress-json'], { jobId, onProgress: progressSender(e, jobId) }),
    );
    let res;
    try {
      res = await net.fetch(first.url, { headers: { 'User-Agent': USER_AGENT } });
    } catch (err) {
      throw new Error(`AccurateRip unreachable — check the Internet connection (${err.message})`);
    }
    if (res.status === 404) return { ...first, result: { in_database: false, tracks: [], all_matched: false } };
    if (!res.ok) throw new Error(`AccurateRip HTTP ${res.status}`);
    const db = path.join(tmpRoot(), `ar-${jobId}.bin`);
    fs.writeFileSync(db, Buffer.from(await res.arrayBuffer()));
    try {
      const r = await runFiso(['accuraterip', file, '--db', db, '--mark', '--json']);
      const v = parseOut(r);
      if (v) return v;
      return fisoResult(r);
    } finally {
      fs.rmSync(db, { force: true });
    }
  });

  handle('mb:discid', async (_e, id, toc) => {
    const j = await mbFetch(
      `${MB}/discid/${encodeURIComponent(id)}?toc=${encodeURIComponent(toc.replace(/ /g, '+'))}&inc=artist-credits+labels&fmt=json`,
    );
    return (j?.releases || []).map(summarizeRelease);
  });
  handle('mb:search', async (_e, artist, album) => {
    const q = [album && `release:"${album.replace(/"/g, '')}"`, artist && `artist:"${artist.replace(/"/g, '')}"`]
      .filter(Boolean)
      .join(' AND ');
    if (!q) return [];
    const j = await mbFetch(`${MB}/release/?query=${encodeURIComponent(q)}&limit=20&fmt=json`);
    return (j?.releases || []).map(summarizeRelease);
  });
  handle('mb:release', (_e, id) => mbRelease(id));
  handle('caa:download', (_e, mbid, kind) => caaDownload(mbid, kind));

  handle('shell:show', (_e, p) => {
    shell.showItemInFolder(p);
    return true;
  });
  handle('shell:site', () => shell.openExternal(PROJECT_URL));
  handle('shell:spec', () => openSpec());
  handle('menu:rebuild', () => buildMenu());
}

// ------------------------------------------------------------------ Finder icons (macOS)
// The Quick Look extension inside the app draws the front cover as the icon of .fiso files.
// macOS normally discovers it when the app is launched; for an app that is not notarized this
// may not happen, so FISO Explorer registers it explicitly (once per version, or on request).
const THUMB_ID = 'fr.retrospare.fiso-explorer.thumbnail';
const LSREGISTER = '/System/Library/Frameworks/CoreServices.framework/Frameworks/LaunchServices.framework/Support/lsregister';
const runTool = (cmd, args) =>
  new Promise((resolve) =>
    execFile(cmd, args, { timeout: 20000 }, (err, out, errOut) =>
      resolve({ ok: !err, out: String(out || '') + String(errOut || '') }),
    ),
  );
async function registerThumbnails(force) {
  if (process.platform !== 'darwin' || !app.isPackaged) return { ok: false, skipped: true };
  if (!force && settings.thumbsRegistered === APP_VERSION) return { ok: true, skipped: true };
  const bundle = path.resolve(path.dirname(app.getPath('exe')), '..', '..');
  const appex = path.join(bundle, 'Contents', 'PlugIns', 'FISOThumbnail.appex');
  if (!fs.existsSync(appex)) return { ok: false, error: 'extension missing' };
  await runTool(LSREGISTER, ['-f', bundle]); // register the app and its document type
  await runTool('/usr/bin/pluginkit', ['-a', appex]); // register the extension
  await runTool('/usr/bin/pluginkit', ['-e', 'use', '-i', THUMB_ID]); // and enable it
  await runTool('/usr/bin/qlmanage', ['-r']); // restart Quick Look
  await runTool('/usr/bin/qlmanage', ['-r', 'cache']); // and drop cached thumbnails
  const check = await runTool('/usr/bin/pluginkit', ['-m', '-v', '-i', THUMB_ID]);
  const registered = check.out.includes(THUMB_ID);
  const enabled = registered && !/^\s*-/m.test(check.out); // pluginkit prefixes disabled plug-ins with "-"
  const quarantined = (await runTool('/usr/bin/xattr', ['-p', 'com.apple.quarantine', bundle])).ok;
  settings.thumbsRegistered = APP_VERSION;
  saveSettings();
  return { ok: registered && enabled, registered, enabled, quarantined, detail: check.out.trim() };
}

// ------------------------------------------------------------------ lifecycle
app.whenReady().then(() => {
  loadSettings();
  protocol.handle('fiso-audio', handleAudio);
  registerIpc();
  buildMenu();
  createWindow();
  registerThumbnails(false).catch(() => {});
  openPaths(process.argv.slice(1));
  win.webContents.on('did-finish-load', () => {
    if (pendingOpen.length) {
      win.webContents.send('open-files', pendingOpen);
      pendingOpen = [];
    }
  });
  app.on('activate', () => {
    if (!win) createWindow();
  });
});

app.on('window-all-closed', () => {
  if (process.platform !== 'darwin') app.quit();
});
app.on('will-quit', () => {
  for (const c of jobs.values()) c.kill('SIGTERM');
  try {
    fs.rmSync(path.join(os.tmpdir(), 'fiso-explorer-' + process.pid), { recursive: true, force: true });
  } catch {
    /* ignore */
  }
});
