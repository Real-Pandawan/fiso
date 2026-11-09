// Batch creation: finds album folders, reads their FLAC tags and artwork, and completes the
// metadata with MusicBrainz. Runs in the main process. SPDX-License-Identifier: Apache-2.0
'use strict';
const fs = require('node:fs');
const path = require('node:path');
const crypto = require('node:crypto');

const AUDIO = /\.(flac|wav)$/i;
const IMAGE = /\.(jpe?g|png)$/i;
const COVER_NAMES = ['cover', 'folder', 'front', 'albumart', 'album'];
const MAX_DEPTH = 8;
const MAX_PICTURE = 15 * 1024 * 1024;

// ------------------------------------------------------------------ FLAC metadata
// Reads the Vorbis comments and the front-cover picture of a FLAC file (metadata blocks only).
function readFlacTags(file) {
  const out = { tags: {}, picture: null };
  let fd;
  try {
    fd = fs.openSync(file, 'r');
    const head = Buffer.alloc(4);
    if (fs.readSync(fd, head, 0, 4, 0) !== 4 || head.toString('latin1') !== 'fLaC') return out;
    let pos = 4;
    for (let guard = 0; guard < 128; guard++) {
      const h = Buffer.alloc(4);
      if (fs.readSync(fd, h, 0, 4, pos) !== 4) break;
      const last = (h[0] & 0x80) !== 0;
      const type = h[0] & 0x7f;
      const len = h.readUIntBE(1, 3);
      pos += 4;
      if (type === 4 || (type === 6 && len <= MAX_PICTURE + 4096)) {
        const b = Buffer.alloc(len);
        if (fs.readSync(fd, b, 0, len, pos) !== len) break;
        if (type === 4) parseVorbisComment(b, out.tags);
        else {
          const pic = parsePicture(b);
          // keep the front cover, or the first picture when there is no front cover
          if (pic && (pic.type === 3 || !out.picture)) out.picture = pic;
        }
      }
      pos += len;
      if (last) break;
    }
  } catch {
    /* unreadable file: no tags */
  } finally {
    if (fd !== undefined) fs.closeSync(fd);
  }
  return out;
}

function parseVorbisComment(b, tags) {
  let p = 0;
  const u32 = () => {
    const v = b.readUInt32LE(p);
    p += 4;
    return v;
  };
  const vendor = u32();
  p += vendor;
  const n = u32();
  for (let i = 0; i < n && p + 4 <= b.length; i++) {
    const len = u32();
    const s = b.toString('utf8', p, p + len);
    p += len;
    const eq = s.indexOf('=');
    if (eq <= 0) continue;
    const k = s.slice(0, eq).toUpperCase();
    if (!(k in tags)) tags[k] = s.slice(eq + 1).trim();
  }
}

function parsePicture(b) {
  let p = 0;
  const u32 = () => {
    const v = b.readUInt32BE(p);
    p += 4;
    return v;
  };
  const type = u32();
  const mimeLen = u32();
  const mime = b.toString('latin1', p, p + mimeLen);
  p += mimeLen;
  const description = u32();
  p += description;
  p += 16; // width, height, depth, colours
  const len = u32();
  const data = b.subarray(p, p + len);
  if (data.length !== len || !imageKind(data)) return null;
  return { type, mime, data };
}

function imageKind(buf) {
  if (buf[0] === 0xff && buf[1] === 0xd8) return 'jpg';
  if (buf.subarray(0, 8).equals(Buffer.from([0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a]))) return 'png';
  return null;
}

// ------------------------------------------------------------------ folders
const natural = new Intl.Collator(undefined, { numeric: true, sensitivity: 'base' }).compare;

// Every folder that directly contains FLAC or WAV files, searched below the given folders.
function findAlbumFolders(roots) {
  const found = [];
  const seen = new Set();
  const walk = (dir, depth) => {
    let real;
    try {
      real = fs.realpathSync.native(dir);
    } catch {
      return;
    }
    if (seen.has(real) || depth > MAX_DEPTH) return;
    seen.add(real);
    let entries;
    try {
      entries = fs.readdirSync(dir, { withFileTypes: true });
    } catch {
      return;
    }
    const audio = entries.filter((e) => e.isFile() && AUDIO.test(e.name) && !e.name.startsWith('.'));
    if (audio.length) found.push({ dir, files: audio.map((e) => path.join(dir, e.name)).sort(natural) });
    for (const e of entries.filter((x) => x.isDirectory() && !x.name.startsWith('.')).sort((a, b) => natural(a.name, b.name)))
      walk(path.join(dir, e.name), depth + 1);
  };
  for (const r of roots) walk(r, 0);
  return found;
}

function folderCover(dir) {
  let names;
  try {
    names = fs.readdirSync(dir).filter((n) => IMAGE.test(n) && !n.startsWith('.'));
  } catch {
    return null;
  }
  const stem = (n) => n.replace(/\.[^.]+$/, '').toLowerCase();
  const named = COVER_NAMES.map((c) => names.find((n) => stem(n) === c)).find(Boolean);
  const pick =
    named ||
    names
      .map((n) => ({ n, size: fs.statSync(path.join(dir, n)).size }))
      .filter((x) => x.size <= MAX_PICTURE)
      .sort((a, b) => b.size - a.size)[0]?.n;
  if (!pick) return null;
  const file = path.join(dir, pick);
  const head = Buffer.alloc(8);
  const fd = fs.openSync(file, 'r');
  fs.readSync(fd, head, 0, 8, 0);
  fs.closeSync(fd);
  return imageKind(head) ? file : null;
}

const firstNumber = (s) => {
  const m = /^\s*(\d+)/.exec(s || '');
  return m ? +m[1] : 0;
};
const totalOf = (s) => {
  const m = /\/\s*(\d+)/.exec(s || '');
  return m ? +m[1] : 0;
};
const stripTrackName = (name) =>
  name
    .replace(/\.[^.]+$/, '')
    .replace(/^\s*\d+\s*[-._ ]\s*/, '')
    .trim();

// Splits a folder into discs (DISCNUMBER tags) and builds one album description per disc.
function describeFolder(folder, coverDir) {
  const tracks = folder.files.map((file) => {
    const isFlac = /\.flac$/i.test(file);
    const { tags, picture } = isFlac ? readFlacTags(file) : { tags: {}, picture: null };
    return { file, tags, picture };
  });
  const byDisc = new Map();
  for (const tr of tracks) {
    const d = firstNumber(tr.tags.DISCNUMBER) || 1;
    if (!byDisc.has(d)) byDisc.set(d, []);
    byDisc.get(d).push(tr);
  }
  const discs = [...byDisc.keys()].sort((a, b) => a - b);
  const localCover = folderCover(folder.dir);
  return discs.map((number) => {
    const list = byDisc.get(number);
    list.sort((a, b) => firstNumber(a.tags.TRACKNUMBER) - firstNumber(b.tags.TRACKNUMBER) || natural(a.file, b.file));
    const tag = (k) => list.map((x) => x.tags[k]).find(Boolean) || '';
    const albums = new Set(list.map((x) => x.tags.ALBUM).filter(Boolean));
    const artist = tag('ALBUMARTIST') || tag('ALBUM ARTIST') || tag('ARTIST');
    const total =
      totalOf(tag('DISCNUMBER')) || firstNumber(tag('DISCTOTAL')) || firstNumber(tag('TOTALDISCS')) || Math.max(...discs);
    let front = null;
    const embedded = list.find((x) => x.picture?.type === 3) || list.find((x) => x.picture);
    if (localCover) front = localCover;
    else if (embedded) {
      const ext = imageKind(embedded.picture.data);
      const name = crypto.createHash('sha1').update(embedded.picture.data).digest('hex') + '.' + ext;
      fs.mkdirSync(coverDir, { recursive: true });
      front = path.join(coverDir, name);
      if (!fs.existsSync(front)) fs.writeFileSync(front, embedded.picture.data);
    }
    const folderName = path.basename(folder.dir);
    return {
      id: crypto
        .createHash('sha1')
        .update(folder.dir + '#' + number)
        .digest('hex')
        .slice(0, 16),
      dir: folder.dir,
      files: list.map((x) => x.file),
      tagged: !!tag('ALBUM'),
      taggedTitles: list.map((x) => !!x.tags.TITLE),
      mixed: albums.size > 1,
      album: {
        title: tag('ALBUM') || folderName,
        artist,
        date: tag('DATE') || tag('YEAR'),
        genre: tag('GENRE'),
        label: tag('LABEL') || tag('ORGANIZATION'),
        catalog_number: tag('CATALOGNUMBER'),
        barcode: tag('BARCODE') || tag('UPC'),
      },
      disc: { number, total: Math.max(total, number), subtitle: tag('DISCSUBTITLE') },
      tracks: list.map((x, i) => ({
        number: i + 1,
        title: x.tags.TITLE || stripTrackName(path.basename(x.file)),
        artist: x.tags.ARTIST && x.tags.ARTIST !== artist ? x.tags.ARTIST : '',
      })),
      front,
    };
  });
}

// ------------------------------------------------------------------ MusicBrainz
const norm = (s) =>
  String(s || '')
    .normalize('NFKD')
    .replace(/[̀-ͯ]/g, '')
    .toLowerCase()
    .replace(/&/g, ' and ')
    .replace(/[^a-z0-9]+/g, ' ')
    .trim();
const core = (s) => norm(String(s || '').replace(/[([][^)\]]*[)\]]/g, ' '));
const sameText = (a, b) => !!a && !!b && (norm(a) === norm(b) || core(a) === core(b));

// Completes an album description with MusicBrainz. Tags always win: MusicBrainz only fills what
// the files do not say. Returns the reasons to check the album by hand (empty when certain).
async function completeWithMusicBrainz(a, mb) {
  const n = a.tracks.length;
  const results = await mb.search(a.album.artist, a.album.title);
  const fits = (r) => {
    if (r.tracks.length > 1 && a.disc.total > 1) return r.tracks[a.disc.number - 1] === n;
    return r.tracks.includes(n);
  };
  const candidates = results.filter(fits);
  if (!results.length) return { reasons: ['mb_none'] };
  if (!candidates.length) return { reasons: ['mb_tracks'] };
  const best = candidates.sort((x, y) => (y.score || 0) - (x.score || 0))[0];
  const certain =
    (best.score || 0) >= 90 && sameText(best.title, a.album.title) && (!a.album.artist || sameText(best.artist, a.album.artist));
  if (!certain) return { reasons: ['mb_uncertain'], release: best };
  const rel = await mb.release(best.id);
  if (!rel) return { reasons: ['mb_none'] };
  const med =
    (rel.media.length > 1 && a.disc.total > 1 ? rel.media.find((m) => m.position === a.disc.number) : null) ||
    rel.media.find((m) => m.tracks.length === n);
  if (!med || med.tracks.length !== n) return { reasons: ['mb_tracks'] };
  const al = a.album;
  al.artist ||= rel.artist;
  al.date ||= rel.date;
  al.genre ||= rel.genre;
  al.label ||= rel.label;
  al.catalog_number ||= rel.catalog;
  al.country ||= rel.country;
  al.barcode ||= rel.barcode;
  al.musicbrainz = { release_id: rel.id, release_group_id: rel.releaseGroupId, album_artist_id: rel.albumArtistId };
  if (rel.media.length > a.disc.total) a.disc.total = rel.media.length;
  if (!a.disc.subtitle && med.title) a.disc.subtitle = med.title;
  a.tracks.forEach((tr, i) => {
    const mt = med.tracks[i];
    if (!a.taggedTitles?.[i]) tr.title = mt.title;
    if (!tr.artist && mt.artist && mt.artist !== al.artist) tr.artist = mt.artist;
    tr.musicbrainz = { recording_id: mt.recordingId, track_id: mt.trackId };
  });
  if (!a.front) {
    const c = await mb.cover(rel.id, 'front').catch(() => null);
    if (c) a.front = c.path;
  }
  const back = await mb.cover(rel.id, 'back').catch(() => null);
  if (back) a.back = back.path;
  return { reasons: [], release: rel };
}

// ------------------------------------------------------------------ plan
const safeName = (s) =>
  String(s)
    .replace(/[\/\\:*?"<>|\x00-\x1f]/g, '_')
    .replace(/\s+/g, ' ')
    .trim()
    .slice(0, 180);

function outputName(a) {
  const base = [a.album.artist, a.album.title].filter(Boolean).join(' - ') || 'Album';
  return safeName(base + (a.disc.total > 1 ? ` - Disc ${a.disc.number}` : '')) + '.fiso';
}

const clean = (o) =>
  Object.fromEntries(
    Object.entries(o).filter(([, v]) => v !== '' && v != null && !(typeof v === 'object' && !Object.keys(v).length)),
  );

// Turns an album description into a create plan for the engine.
function createPlan(a, outDir, options) {
  const meta = {
    schema: 'fiso.meta/1',
    album: clean({ ...a.album }),
    disc: clean({ number: a.disc.number, total: a.disc.total, subtitle: a.disc.subtitle }),
    tracks: a.tracks.map((tr) => clean({ number: tr.number, title: tr.title, artist: tr.artist, musicbrainz: tr.musicbrainz })),
  };
  const pictures = [];
  if (a.front) pictures.push({ type: 3, path: a.front });
  if (a.back) pictures.push({ type: 4, path: a.back });
  return {
    output: path.join(outDir, outputName(a)),
    overwrite: false,
    compression: options.compression ?? 8,
    flac_passthrough: !options.reencode,
    meta,
    pictures,
    source: { kind: 'files', files: a.files },
  };
}

module.exports = { readFlacTags, findAlbumFolders, describeFolder, completeWithMusicBrainz, createPlan, outputName };
