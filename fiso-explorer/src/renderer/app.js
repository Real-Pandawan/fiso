// FISO Explorer — renderer. SPDX-License-Identifier: Apache-2.0
'use strict';

const api = window.fiso;
const S = {
  settings: { lang: 'en' },
  appInfo: null,
  view: 'home',
  docs: [], // { path, info, album }
  current: null, // path of the image on screen
  sel: new Set(), // selected track numbers
  anchor: null,
  verified: {}, // path -> verification report
  create: null,
  player: { path: null, rate: 44100, tracks: [], cover: null },
};

// ------------------------------------------------------------------ helpers
const $ = (s, r = document) => r.querySelector(s);
const $$ = (s, r = document) => [...r.querySelectorAll(s)];
const esc = (s) =>
  String(s ?? '').replace(/[&<>"']/g, (c) => ({ '&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;', "'": '&#39;' })[c]);
function t(k, v = {}) {
  const d = I18N[S.settings.lang] || I18N.en;
  return (d[k] ?? I18N.en[k] ?? k).replace(/\{(\w+)\}/g, (m, n) => (n in v ? v[n] : m));
}
const isMac = () => S.appInfo?.platform === 'darwin';
const revealLabel = () => t(isMac() ? 'reveal' : 'reveal.folder');
const LANGS = [
  ['en', 'English'],
  ['fr', 'Français'],
  ['es', 'Español'],
  ['de', 'Deutsch'],
  ['pt', 'Português'],
  ['it', 'Italiano'],
  ['nl', 'Nederlands'],
  ['ja', '日本語'],
];
function fmtDur(sec) {
  sec = Math.max(0, Math.round(sec || 0));
  const h = Math.floor(sec / 3600),
    m = Math.floor((sec % 3600) / 60),
    s = sec % 60;
  return h ? `${h}:${String(m).padStart(2, '0')}:${String(s).padStart(2, '0')}` : `${m}:${String(s).padStart(2, '0')}`;
}
function fmtBytes(b) {
  const u = t('u.bytes').split(',');
  let i = 0;
  while (b >= 1000 && i < 3) {
    b /= 1000;
    i++;
  }
  return `${b.toLocaleString(S.settings.lang, { maximumFractionDigits: i ? 1 : 0 })} ${u[i]}`;
}
const pct = (x) => `${(x * 100).toLocaleString(S.settings.lang, { maximumFractionDigits: 1 })} %`;
function msf(samples, rate) {
  const sectors = Math.floor((samples * 75) / rate);
  const p = (n) => String(n).padStart(2, '0');
  return `${p(Math.floor(sectors / 4500))}:${p(Math.floor(sectors / 75) % 60)}:${p(sectors % 75)}`;
}
const dirname = (p) => p.replace(/[\\/][^\\/]*$/, '');
const basename = (p) => p.replace(/^.*[\\/]/, '');
const doc = () => S.docs.find((d) => d.path === S.current);
let jobSeq = 0;
const newJob = () => `j${Date.now()}-${++jobSeq}`;

function toast(msg, bad = false) {
  const el = document.createElement('div');
  el.className = 'toast' + (bad ? ' bad' : '');
  el.textContent = msg;
  $('#toast-root').appendChild(el);
  setTimeout(() => el.remove(), bad ? 6000 : 3200);
}

// ------------------------------------------------------------------ modal
function modal({ title, body, foot, wide, onClose }) {
  const back = document.createElement('div');
  back.className = 'modal-back';
  back.innerHTML = `<div class="modal ${wide ? 'wide' : ''}" role="dialog" aria-modal="true">
    <div class="modal-h"><h2>${esc(title)}</h2><button class="btn ghost icon-btn" data-x aria-label="${esc(t('close'))}">${icon('x')}</button></div>
    <div class="modal-b"></div>${foot !== false ? '<div class="modal-f"></div>' : ''}</div>`;
  const b = $('.modal-b', back);
  if (typeof body === 'string') b.innerHTML = body;
  else if (body) b.appendChild(body);
  const close = () => {
    back.remove();
    document.removeEventListener('keydown', onKey);
    onClose && onClose();
  };
  const onKey = (e) => {
    if (e.key === 'Escape') close();
  };
  document.addEventListener('keydown', onKey);
  $('[data-x]', back).onclick = close;
  if (foot)
    foot.forEach((f) => {
      const btn = document.createElement('button');
      btn.className = 'btn ' + (f.primary ? 'primary' : '');
      btn.textContent = f.label;
      if (f.id) btn.id = f.id;
      btn.onclick = () => (f.onClick ? f.onClick(close, btn) : close());
      $('.modal-f', back).appendChild(btn);
    });
  $('#modal-root').appendChild(back);
  return { el: back, body: b, close };
}

// Engine messages start with a stable identifier such as "[no_media]".
function humanError(msg) {
  const m = /\[([a-z_]+)\]\s*(.*)$/s.exec(String(msg || ''));
  if (m && (I18N[S.settings.lang] || I18N.en)['e.' + m[1]]) return { text: t('e.' + m[1]), detail: m[2] };
  return { text: String(msg || '').replace(/^\[[a-z_]+\]\s*/, ''), detail: '' };
}
function warnText(msg) {
  const m = /^\[([a-z_]+)\]/.exec(String(msg || ''));
  if (m && (I18N[S.settings.lang] || I18N.en)['w.' + m[1]]) return t('w.' + m[1]);
  return humanError(msg).text;
}
function errorModal(msg) {
  const h = humanError(msg);
  modal({
    title: t('error'),
    body: `<p style="white-space:pre-wrap">${esc(h.text)}</p>${h.detail ? `<p class="hint" style="white-space:pre-wrap">${esc(t('detail'))}${t('sep')}${esc(h.detail)}</p>` : ''}`,
    foot: [{ label: t('ok'), primary: true }],
  });
}

// Runs a long operation with a progress dialog and a working Cancel button.
async function runJob(label, fn) {
  const job = newJob();
  let cancelled = false;
  const m = modal({
    title: label,
    body: `<div class="job-phase"><span id="jp">${esc(t('job.wait'))}</span><span id="jpc" class="num"></span></div><div class="progress"><div id="jbar"></div></div>`,
    foot: [
      {
        label: t('job.cancel'),
        onClick: () => {
          cancelled = true;
          api.cancel(job);
        },
      },
    ],
  });
  $('[data-x]', m.el).hidden = true;
  const onProg = (p) => {
    if (p.jobId !== job || !m.el.isConnected) return;
    const f = p.total ? p.done / p.total : 0;
    $('#jp', m.el).textContent = t('job.' + p.phase);
    $('#jpc', m.el).textContent = pct(f);
    $('#jbar', m.el).style.width = `${(f * 100).toFixed(1)}%`;
  };
  progressListeners.add(onProg);
  try {
    return await fn(job);
  } catch (e) {
    if (cancelled || /cancel/i.test(e.message)) toast(t('job.cancelled'));
    else errorModal(e.message);
    return null;
  } finally {
    progressListeners.delete(onProg);
    m.close();
  }
}
const progressListeners = new Set();
api.on('job:progress', (p) => progressListeners.forEach((f) => f(p)));

// ------------------------------------------------------------------ navigation & sidebar
function go(view) {
  S.view = view;
  render();
}

// Top bar: the logo goes back to the home screen; settings and help open on the right.
function renderTopbar() {
  document.body.classList.toggle('mac', isMac());
  const home = $('#tb-home');
  home.title = t('nav.home');
  home.onclick = () => go('home');
  const btn = (id, ic, label, view) => {
    const b = $(id);
    b.innerHTML = icon(ic);
    b.title = label;
    b.setAttribute('aria-label', label);
    b.classList.toggle('on', S.view === view);
    b.onclick = () => go(S.view === view ? 'home' : view);
  };
  btn('#tb-settings', 'gear', t('nav.settings'), 'settings');
  btn('#tb-help', 'help', t('nav.help'), 'help');
}

// Title of a page that is closed with a cross (settings, help): back to the home screen.
function closableTitle(title) {
  return `<div class="page-head"><h1 class="page-title">${esc(title)}</h1>
    <button class="btn ghost icon-btn" id="page-close" title="${esc(t('nav.close'))}" aria-label="${esc(t('nav.close'))}">${icon('x')}</button></div>`;
}
function bindClose(v) {
  $('#page-close', v).onclick = () => go('home');
}

function albumDiscs(g, current) {
  const byNum = new Map(g.discs.map((d) => [d.number, d]));
  const out = [];
  for (let n = 1; n <= Math.max(g.disc_total, ...g.discs.map((d) => d.number)); n++) {
    const d = byNum.get(n);
    const label = t('ex.disc', { n, t: g.disc_total });
    if (!d) out.push(`<span class="disc-pill missing">${icon('disc')}${esc(label)} · ${esc(t('album.missing'))}</span>`);
    else {
      const sub = (d.tracks === 1 ? t('ex.track1') : t('ex.tracks', { n: d.tracks })) + (d.subtitle ? ' · ' + d.subtitle : '');
      out.push(
        `<button class="disc-pill ${d.path === current ? 'on' : ''}" data-disc="${esc(d.path)}">${icon('disc')}${esc(label)} <span class="hint">${esc(sub)}</span></button>`,
      );
    }
  }
  return out.join('');
}

function render() {
  document.documentElement.lang = S.settings.lang;
  renderTopbar();
  const v = $('#view');
  if (S.view === 'explorer' && !doc()) S.view = 'home';
  ({ home: renderHome, explorer: renderExplorer, create: renderCreate, settings: renderSettings, help: renderHelp })[S.view](v);
  renderTopbar();
}

// ------------------------------------------------------------------ home
function renderHome(v) {
  const card = (id, ic, title, desc) => `
    <button class="card action" data-act="${id}">
      <div class="ic">${icon(ic)}</div><div><h3>${esc(title)}</h3><p>${esc(desc)}</p></div></button>`;
  v.innerHTML = `<div>
    <div class="hero"><h1>${esc(t('home.title'))}</h1><p>${esc(t('home.lead'))}</p></div>
    <div class="actions">
      ${card('create', 'plus', t('home.create'), t('home.create.d'))}
      ${card('open', 'folder', t('home.open'), t('home.open.d'))}
      ${card('extract', 'extract', t('home.extract'), t('home.extract.d'))}
      ${card('convert', 'disc', t('cv.title'), t('cv.home.d'))}
    </div>
    ${
      S.docs.length
        ? `<div class="section-title" style="margin-top:28px">${esc(t('nav.recent'))}</div><div class="open-docs">${S.docs
            .map((d) => {
              const m = d.info.meta;
              return `<button class="card open-doc" data-doc="${esc(d.path)}">${icon('disc')}<span class="lbl"><b>${esc(m.album.title)}</b><span class="sub">${esc(t('ex.disc', { n: m.disc.number, t: m.disc.total }))} · ${esc(basename(d.path))}</span></span></button>`;
            })
            .join('')}</div>`
        : ''
    }
    <div class="home-foot">${icon('info')}<span>${esc(t('home.drop'))}</span></div>
    ${S.appInfo && !S.appInfo.binOk ? `<p class="error-text" style="margin-top:16px">${esc(t('err.nobin', { p: S.appInfo.bin }))}</p>` : ''}
  </div>`;
  $$('[data-act]', v).forEach(
    (b) =>
      (b.onclick = async () => {
        const a = b.dataset.act;
        if (a === 'create') go('create');
        else if (a === 'open') openDialog();
        else if (a === 'extract') {
          const p = await openDialog();
          if (p) extractDialog();
        } else if (a === 'convert') convertCdDialog();
      }),
  );
  $$('[data-doc]', v).forEach(
    (b) =>
      (b.onclick = () => {
        S.current = b.dataset.doc;
        S.sel.clear();
        go('explorer');
      }),
  );
}

// ------------------------------------------------------------------ opening images
async function openDialog() {
  try {
    const paths = await api.dialogs.openFiso();
    let last = null;
    for (const p of paths) last = (await openImage(p, true)) || last;
    if (last) {
      S.current = last;
      S.sel.clear();
      go('explorer');
    }
    return last;
  } catch (e) {
    errorModal(e.message);
    return null;
  }
}

async function openImage(path, quiet) {
  try {
    const info = await api.info(path);
    const existing = S.docs.find((d) => d.path === path);
    const d = existing || { path };
    d.info = info;
    if (!existing) S.docs.push(d);
    d.album = await api.album(path).catch(() => null);
    S.current = path;
    S.sel.clear();
    if (!quiet) go('explorer');
    return path;
  } catch (e) {
    errorModal(`${basename(path)}\n\n${e.message}`);
    return null;
  }
}

function closeCurrent() {
  if (!S.current) return;
  if (S.player.path === S.current) stopPlayer(true);
  S.docs = S.docs.filter((d) => d.path !== S.current);
  S.current = S.docs[S.docs.length - 1]?.path || null;
  go(S.current ? 'explorer' : 'home');
}

// ------------------------------------------------------------------ explorer
function trackTitle(tr) {
  return tr.title || t('track', { n: tr.number });
}

function renderExplorer(v) {
  const d = doc();
  const i = d.info,
    m = i.meta,
    a = i.audio;
  const front = i.covers.find((c) => c.type === 3) || i.covers[0];
  const back = i.covers.find((c) => c.type === 4 && c !== front);
  const year = (m.album.date || '').slice(0, 4);
  const ver = S.verified[d.path];
  const audioState = ver ? (ver.intact_audio.ok ? 'ok' : 'bad') : 'na';
  const validState = ver ? (ver.valid_file.ok ? 'ok' : 'bad') : 'ok';
  const ripState =
    (ver
      ? ver.verified_rip.state
      : m.provenance?.rip?.verified && m.provenance?.rip?.verification
        ? 'verified'
        : 'not_verified') === 'verified'
      ? 'ok'
      : 'na';
  const badge = (st, label, val) =>
    `<span class="badge ${st === 'ok' ? 'ok' : st === 'bad' ? 'bad' : ''}">${icon(st === 'ok' ? 'check' : st === 'bad' ? 'alert' : 'info')}${esc(label)}${t('sep')}${esc(val)}</span>`;
  const chLabel = a.channels === 2 ? t('ch.stereo') : a.channels === 1 ? t('ch.mono') : t('ch.n', { n: a.channels });
  const nTracks = i.tracks.length;
  const playing = S.player.path === d.path ? S.player.currentTrack : null;

  v.innerHTML = `<div>
    <div class="album-head">
      <div class="cover">${front ? `<img src="${front.dataUrl}" alt="">` : icon('disc', 'placeholder')}
        ${back ? `<div class="cover-back" id="show-back" title="${esc(t('meta.back'))}"><img src="${back.dataUrl}" alt=""></div>` : ''}</div>
      <div>
        <div class="eyebrow">${esc(t('ex.disc', { n: m.disc.number, t: m.disc.total }))}${m.disc.subtitle ? ' — ' + esc(m.disc.subtitle) : ''}</div>
        <h1 class="album-title">${esc(m.album.title)}</h1>
        <div class="album-artist">${esc(m.album.artist || '')}</div>
        <div class="chips">
          ${year ? `<span class="badge">${esc(year)}</span>` : ''}
          ${m.album.genre ? `<span class="badge">${esc(m.album.genre)}</span>` : ''}
          <span class="badge">${esc(nTracks === 1 ? t('ex.track1') : t('ex.tracks', { n: nTracks }))}</span>
          <span class="badge num">${fmtDur(a.duration_seconds)}</span>
        </div>
        <div class="toolbar">
          <button class="btn primary" id="b-play">${icon('play')}${esc(t('ex.play'))}</button>
          <button class="btn" id="b-extract">${icon('extract')}${esc(t('ex.extract'))}</button>
          <button class="btn" id="b-verify">${icon('shield')}${esc(t('ex.verify'))}</button>
          <button class="btn" id="b-meta">${icon('edit')}${esc(t('ex.meta'))}</button>
          <button class="btn" id="b-cd" title="${esc(t('cv.title'))}">${icon('burn')}${esc(t('ex.tocd'))}</button>
        </div>
      </div>
    </div>

    <div class="facts">
      <div class="fact"><div class="k">${esc(t('ex.codec'))}</div><div class="v">FLAC · ${(a.sample_rate / 1000).toLocaleString(S.settings.lang)} kHz · ${a.bits_per_sample} bits · ${esc(chLabel)}</div></div>
      <div class="fact"><div class="k">${esc(t('ex.size'))}</div><div class="v num">${fmtBytes(i.sizes.file_bytes)}</div></div>
      <div class="fact"><div class="k">${esc(t('ex.pcm'))}</div><div class="v num">${fmtBytes(i.sizes.pcm_bytes)}</div></div>
      <div class="fact"><div class="k">${esc(t('ex.ratio'))}</div><div class="v num">${pct(i.sizes.ratio)}</div></div>
      ${i.musicbrainz_disc_id ? `<div class="fact" style="grid-column:1/-1"><div class="k">${esc(t('ex.mbid'))}</div><div class="v mono" style="font-size:11.5px">${esc(i.musicbrainz_disc_id)}</div></div>` : ''}
      <div class="fact" style="grid-column:1/-1"><div class="k">${esc(t('ex.integrity'))}</div><div class="v integrity">
        ${badge(validState, t('ex.valid'), validState === 'ok' ? t('st.yes') : t('st.no'))}
        ${badge(audioState, t('ex.audio'), audioState === 'na' ? t('st.unchecked') : audioState === 'ok' ? t('st.yes') : t('st.no'))}
        ${badge(ripState, t('ex.rip'), ripState === 'ok' ? t('st.yes') : t('st.noref'))}
        ${ver ? `<button class="btn ghost small" id="b-report">${esc(t('ver.title'))}</button>` : ''}
      </div></div>
    </div>

    ${d.album && (d.album.discs.length > 1 || d.album.disc_total > 1) ? `<div class="section-title">${esc(t('album.discs'))}${d.album.method === 'heuristic' ? ` · <span style="text-transform:none;letter-spacing:0">${esc(t('album.guess'))}</span>` : ''}</div><div class="discs">${albumDiscs(d.album, d.path)}</div>` : ''}

    <div class="tracks-wrap">
      <div class="card" style="overflow:hidden"><table class="tracks"><thead><tr>
        <th class="tno">#</th><th>${esc(t('col.title'))}</th><th class="r">${esc(t('col.length'))}</th><th class="r">${esc(t('col.pregap'))}</th><th class="r">${esc(t('col.size'))}</th></tr></thead><tbody>
        ${i.htoa ? `<tr class="htoa"><td class="tno num">0</td><td colspan="4">${esc(t('htoa', { d: fmtDur(i.htoa_samples / a.sample_rate) }))}</td></tr>` : ''}
        ${i.tracks
          .map(
            (
              tr,
            ) => `<tr data-t="${tr.number}" class="${S.sel.has(tr.number) ? 'sel' : ''} ${playing === tr.number ? 'playing' : ''}">
          <td class="tno num">${playing === tr.number ? icon('volume') : tr.number}</td>
          <td><div class="t-title">${esc(trackTitle(tr))}</div>${tr.artist && tr.artist !== m.album.artist ? `<div class="t-artist">${esc(tr.artist)}</div>` : ''}</td>
          <td class="r num">${fmtDur(tr.duration_seconds)}</td>
          <td class="r num">${tr.pregap_samples ? fmtDur(tr.pregap_samples / a.sample_rate) : '—'}</td>
          <td class="r num">${fmtBytes(tr.compressed_bytes)}</td></tr>`,
          )
          .join('')}
      </tbody></table></div>
      <div class="card details" id="details"></div>
    </div>
  </div>`;

  renderDetails();
  $('#b-play').onclick = () => playTrack(d, [...S.sel].sort((x, y) => x - y)[0] || i.tracks[0].number);
  $('#b-extract').onclick = () => extractDialog();
  $('#b-verify').onclick = () => verifyCurrent();
  $('#b-meta').onclick = () => editDialog();
  $('#b-cd').onclick = () => convertCdDialog(d.path);
  if ($('#b-report')) $('#b-report').onclick = () => verifyReport(S.verified[d.path]);
  if ($('#show-back'))
    $('#show-back').onclick = () =>
      modal({ title: t('meta.back'), body: `<img src="${back.dataUrl}" style="width:100%;border-radius:8px">`, foot: false });
  $$('.discs [data-disc]', v).forEach((b) => (b.onclick = () => openImage(b.dataset.disc)));
  $$('tbody tr[data-t]', v).forEach((row) => {
    const n = +row.dataset.t;
    row.onclick = (e) => {
      if (e.shiftKey && S.anchor != null) {
        const [lo, hi] = [Math.min(S.anchor, n), Math.max(S.anchor, n)];
        if (!(e.metaKey || e.ctrlKey)) S.sel.clear();
        for (let k = lo; k <= hi; k++) S.sel.add(k);
      } else if (e.metaKey || e.ctrlKey) {
        S.sel.has(n) ? S.sel.delete(n) : S.sel.add(n);
        S.anchor = n;
      } else {
        S.sel = new Set([n]);
        S.anchor = n;
      }
      $$('tbody tr[data-t]', v).forEach((r) => r.classList.toggle('sel', S.sel.has(+r.dataset.t)));
      renderDetails();
    };
    row.ondblclick = () => playTrack(d, n);
  });
}

function renderDetails() {
  const el = $('#details');
  if (!el) return;
  const d = doc();
  const i = d.info;
  if (S.sel.size !== 1) {
    el.innerHTML = `<h4>${esc(t('details'))}</h4><p class="hint">${esc(t('details.none'))}</p>`;
    return;
  }
  const tr = i.tracks.find((x) => x.number === [...S.sel][0]);
  const rate = i.audio.sample_rate;
  const flags = [
    tr.pre_emphasis && t('details.pre'),
    tr.copy_permitted && t('details.dcp'),
    tr.four_channel && t('details.4ch'),
  ].filter(Boolean);
  el.innerHTML = `<h4>${esc(t('details'))} — ${tr.number}</h4>
    <div class="kv">
      <span class="k">${esc(t('col.title'))}</span><span>${esc(trackTitle(tr))}</span>
      <span class="k">${esc(t('col.length'))}</span><span class="num">${fmtDur(tr.duration_seconds)} (${msf(tr.length, rate)})</span>
      <span class="k">${esc(t('details.start'))}</span><span class="num">${tr.start.toLocaleString(S.settings.lang)}</span>
      <span class="k">${esc(t('details.length'))}</span><span class="num">${tr.length.toLocaleString(S.settings.lang)}</span>
      <span class="k">${esc(t('col.pregap'))}</span><span class="num">${tr.pregap_samples ? msf(tr.pregap_samples, rate) : '—'}</span>
      <span class="k">${esc(t('col.isrc'))}</span><span class="mono">${esc(tr.isrc || '—')}</span>
      <span class="k">${esc(t('details.flags'))}</span><span>${esc(flags.join(', ') || '—')}</span>
      <span class="k">${esc(t('col.size'))}</span><span class="num">${fmtBytes(tr.compressed_bytes)}</span>
    </div>
    <div class="section-title" style="margin-top:14px">${esc(t('details.indexes'))}</div>
    <ul class="idx-list">${tr.indexes.map(([n, pos]) => `<li><span>INDEX ${String(n).padStart(2, '0')}</span><span class="num">${msf(pos, rate)}</span></li>`).join('')}</ul>`;
}

// ------------------------------------------------------------------ player (one continuous WAV stream of the disc → gap-less)
const audio = $('#audio');

async function playTrack(d, number) {
  const P = S.player;
  try {
    if (P.path !== d.path) {
      const r = await api.player(d.path);
      P.path = d.path;
      P.rate = r.rate;
      P.tracks = d.info.tracks;
      P.album = d.info.meta.album;
      P.cover = (d.info.covers.find((c) => c.type === 3) || d.info.covers[0])?.dataUrl || null;
      audio.src = r.url;
    }
    const tr = P.tracks.find((x) => x.number === number) || P.tracks[0];
    const seekTo = tr.start / P.rate;
    const doSeek = () => {
      audio.currentTime = seekTo;
      audio.play().catch((e) => toast(e.message, true));
    };
    if (audio.readyState >= 1) doSeek();
    else audio.addEventListener('loadedmetadata', doSeek, { once: true });
    if (audio.readyState < 1) audio.load();
    renderPlayer();
  } catch (e) {
    errorModal(e.message);
  }
}

function stopPlayer(unload) {
  audio.pause();
  if (unload) {
    audio.removeAttribute('src');
    audio.load();
    S.player.path = null;
    $('#player').hidden = true;
  } else if (S.player.tracks.length) audio.currentTime = S.player.tracks[0].start / S.player.rate;
  renderPlayer();
}

function currentTrackAt(sec) {
  const P = S.player;
  const s = sec * P.rate;
  let cur = P.tracks[0];
  for (const tr of P.tracks) if (tr.start <= s) cur = tr;
  return cur;
}

function renderPlayer() {
  const P = S.player;
  const el = $('#player');
  if (!P.path) {
    el.hidden = true;
    return;
  }
  el.hidden = false;
  const tr = currentTrackAt(audio.currentTime || 0);
  if (!el.dataset.built) {
    el.dataset.built = '1';
    el.innerHTML = `
      <div class="p-now"><span id="p-cover"></span><div class="txt"><div class="t1" id="p-t1"></div><div class="t2" id="p-t2"></div></div></div>
      <div class="p-center">
        <div class="p-buttons">
          <button class="btn icon-btn" id="p-prev">${icon('prev')}</button>
          <button class="btn icon-btn main-btn" id="p-play"></button>
          <button class="btn icon-btn" id="p-stop">${icon('stop')}</button>
          <button class="btn icon-btn" id="p-next">${icon('next')}</button>
        </div>
        <div class="p-seek"><span class="num" id="p-pos">0:00</span><input type="range" id="p-seek" min="0" max="1000" value="0"><span class="num" id="p-len">0:00</span></div>
      </div>
      <div class="p-vol">${icon('volume')}<input type="range" id="p-vol" min="0" max="100" value="${Math.round(audio.volume * 100)}"></div>`;
    $('#p-play').onclick = () => (audio.paused ? audio.play() : audio.pause());
    $('#p-stop').onclick = () => stopPlayer(false);
    $('#p-prev').onclick = () => {
      const c = currentTrackAt(audio.currentTime);
      const into = audio.currentTime - c.start / S.player.rate;
      const idx = S.player.tracks.indexOf(c);
      const target = into > 3 || idx === 0 ? c : S.player.tracks[idx - 1];
      audio.currentTime = target.start / S.player.rate;
    };
    $('#p-next').onclick = () => {
      const idx = S.player.tracks.indexOf(currentTrackAt(audio.currentTime));
      if (idx + 1 < S.player.tracks.length) audio.currentTime = S.player.tracks[idx + 1].start / S.player.rate;
    };
    $('#p-seek').oninput = (e) => {
      const c = currentTrackAt(audio.currentTime);
      audio.currentTime = (c.start + (c.length * e.target.value) / 1000) / S.player.rate;
    };
    $('#p-vol').oninput = (e) => (audio.volume = e.target.value / 100);
  }
  $('#p-cover').innerHTML = P.cover ? `<img src="${P.cover}" alt="">` : `<span class="ph">${icon('music')}</span>`;
  $('#p-t1').textContent = `${tr.number}. ${trackTitle(tr)}`;
  $('#p-t2').textContent = [P.album?.artist, P.album?.title].filter(Boolean).join(' — ');
  $('#p-play').innerHTML = icon(audio.paused ? 'play' : 'pause');
  const into = (audio.currentTime || 0) - tr.start / P.rate;
  const len = tr.length / P.rate;
  $('#p-pos').textContent = fmtDur(into);
  $('#p-len').textContent = fmtDur(len);
  if (document.activeElement !== $('#p-seek')) $('#p-seek').value = Math.round((into / len) * 1000) || 0;
  if (P.currentTrack !== tr.number) {
    P.currentTrack = tr.number;
    if (S.view === 'explorer' && S.current === P.path) renderExplorer($('#view'));
  }
}
['timeupdate', 'play', 'pause', 'ended'].forEach((ev) => audio.addEventListener(ev, renderPlayer));
audio.addEventListener('error', () => {
  if (S.player.path) toast(t('pl.failed') + ' ' + (audio.error?.message || ''), true);
});

// ------------------------------------------------------------------ verification
async function verifyCurrent() {
  const d = doc();
  if (!d) return;
  const r = await runJob(t('ex.verify') + ' — ' + basename(d.path), (job) => api.verify(d.path, job));
  if (!r) return;
  S.verified[d.path] = r;
  render();
  verifyReport(r);
}

function verifyReport(r) {
  const st = (
    cls,
    title,
    desc,
    errors,
    extra = '',
  ) => `<div class="state ${cls}"><div class="ic">${icon(cls === 'ok' ? 'check' : cls === 'bad' ? 'alert' : 'info')}</div>
    <div><b>${esc(title)}</b><p>${esc(desc)}</p>${errors?.length ? `<ul>${errors.map((e) => `<li>${esc(e)}</li>`).join('')}</ul>` : ''}${extra}</div></div>`;
  const vf = r.valid_file,
    ia = r.intact_audio,
    vr = r.verified_rip;
  const segs = (ia.segments || [])
    .map(
      (s) =>
        `<span class="badge ${s.flac_md5_ok && s.pcm_sha256_ok ? 'ok' : 'bad'}">${s.kind === 'htoa' ? 'HTOA' : s.track}</span>`,
    )
    .join(' ');
  modal({
    title: t('ver.title'),
    body: `<div class="report">
      ${st(vf.ok ? 'ok' : 'bad', `${t('ex.valid')}${t('sep')}${vf.ok ? t('st.yes') : t('st.no')}`, t('ver.valid.d'), vf.errors)}
      ${st(
        !ia.checked ? 'na' : ia.ok ? 'ok' : 'bad',
        `${t('ex.audio')}${t('sep')}${!ia.checked ? t('st.unchecked') : ia.ok ? t('st.yes') : t('st.no')}`,
        t('ver.audio.d'),
        ia.errors,
        segs
          ? `<div style="margin-top:8px;display:flex;gap:4px;flex-wrap:wrap"><span class="hint">${esc(t('ver.segments'))} :</span> ${segs}</div>`
          : '',
      )}
      ${st(vr.state === 'verified' ? 'ok' : 'na', `${t('ex.rip')}${t('sep')}${vr.state === 'verified' ? t('ver.verified', { m: vr.method }) : t('st.noref')}`, t('ver.rip.d'))}
    </div>`,
    foot: [
      ...(doc()?.info.cdda && vr.state !== 'verified'
        ? [
            {
              label: t('ar.check'),
              onClick: (c) => {
                c();
                arCurrent();
              },
            },
          ]
        : []),
      { label: t('ok'), primary: true },
    ],
  });
}

// ------------------------------------------------------------------ extraction
async function extractDialog() {
  const d = doc();
  if (!d) return;
  const sel = [...S.sel].sort((a, b) => a - b);
  const multi = d.info.meta.disc.total > 1;
  const st = {
    format: 'flac',
    scope: sel.length ? 'sel' : 'all',
    template: S.settings.template || (multi ? 'Disc {disc} - {track:02} - {title}' : '{track:02} - {title}'),
    outDir: S.settings.outDir || dirname(d.path),
    htoa: false,
    overwrite: false,
  };
  const formats = [
    ['flac', 'xt.flac', 'xt.flac.d'],
    ['wav', 'xt.wav', 'xt.wav.d'],
    ['cover', 'xt.cover', ''],
    ['meta', 'xt.meta', ''],
  ];
  const body = document.createElement('div');
  const draw = () => {
    const perTrack = st.format === 'flac' || st.format === 'wav';
    body.innerHTML = `
      <div class="section-title">${esc(t('xt.format'))}</div>
      <div class="opt-list">${formats.map(([id, l, dsc]) => `<label class="opt ${st.format === id ? 'on' : ''}"><input type="radio" name="fmt" value="${id}" ${st.format === id ? 'checked' : ''}><div><b>${esc(t(l))}</b>${dsc ? `<span>${esc(t(dsc))}</span>` : ''}</div></label>`).join('')}</div>
      ${
        perTrack
          ? `
        <div class="section-title" style="margin-top:18px">${esc(t('xt.scope'))}</div>
        <div class="seg"><button data-scope="all" class="${st.scope === 'all' ? 'on' : ''}">${esc(t('xt.all'))}</button><button data-scope="sel" class="${st.scope === 'sel' ? 'on' : ''}" ${sel.length ? '' : 'disabled'}>${esc(t('xt.sel', { n: sel.length }))}</button></div>
        <label class="field" style="margin-top:14px">${esc(t('xt.template'))}<input class="input mono" id="x-tpl" value="${esc(st.template)}"><span class="hint">${esc(t('xt.tokens'))}</span></label>
        ${d.info.htoa ? `<label class="check" style="margin-top:10px"><input type="checkbox" id="x-htoa" ${st.htoa ? 'checked' : ''}>${esc(t('xt.htoa'))}</label>` : ''}`
          : ''
      }
      <div class="section-title" style="margin-top:18px">${esc(t('xt.dest'))}</div>
      <div class="output-row"><span class="path grow">${esc(st.outDir)}</span><button class="btn small" id="x-dir">${esc(t('set.choose'))}</button></div>
      <label class="check" style="margin-top:12px"><input type="checkbox" id="x-ow" ${st.overwrite ? 'checked' : ''}>${esc(t('xt.overwrite'))}</label>`;
    $$('input[name=fmt]', body).forEach(
      (r) =>
        (r.onchange = () => {
          st.format = r.value;
          draw();
        }),
    );
    $$('[data-scope]', body).forEach(
      (b) =>
        (b.onclick = () => {
          st.scope = b.dataset.scope;
          draw();
        }),
    );
    if ($('#x-tpl', body)) $('#x-tpl', body).oninput = (e) => (st.template = e.target.value);
    if ($('#x-htoa', body)) $('#x-htoa', body).onchange = (e) => (st.htoa = e.target.checked);
    $('#x-ow', body).onchange = (e) => (st.overwrite = e.target.checked);
    $('#x-dir', body).onclick = async () => {
      const p = await api.dialogs.chooseDir(st.outDir);
      if (p) {
        st.outDir = p;
        draw();
      }
    };
  };
  draw();
  modal({
    title: `${t('xt.title')} — ${d.info.meta.album.title}`,
    body,
    foot: [
      { label: t('cancel') },
      {
        label: t('xt.go'),
        primary: true,
        onClick: async (close) => {
          close();
          const opts = {
            format: st.format,
            outDir: st.outDir,
            template: st.template,
            includeHtoa: st.htoa,
            overwrite: st.overwrite,
            tracks: st.scope === 'sel' && (st.format === 'flac' || st.format === 'wav') ? sel : [],
          };
          const r = await runJob(t('xt.title') + ' — ' + basename(d.path), (job) => api.extract(d.path, opts, job));
          if (!r) return;
          modal({
            title: t('xt.done', { n: r.files.length }),
            body: `<ul class="idx-list">${r.files.map((f) => `<li><span class="path">${esc(basename(f))}</span></li>`).join('')}</ul>
               ${r.warnings?.length ? `<div class="warn-box" style="margin-top:12px">${esc(t('rep.warnings'))}<ul>${r.warnings.map((w) => `<li>${esc(w)}</li>`).join('')}</ul></div>` : ''}`,
            foot: [
              {
                label: revealLabel(),
                onClick: (c) => {
                  if (r.files[0]) api.shell.show(r.files[0]);
                  c();
                },
              },
              { label: t('ok'), primary: true },
            ],
          });
        },
      },
    ],
  });
}

// ------------------------------------------------------------------ metadata form (create + edit)
class MetaForm {
  constructor(state, opts) {
    this.s = state;
    this.o = opts; // { mode, discid, toc, onChange }
    this.mb = {
      artist: state.album.artist || '',
      album: state.album.title || '',
      results: null,
      release: null,
      medium: null,
      busy: false,
      covers: true,
      msg: '',
      thumbs: {},
    };
    this.el = document.createElement('div');
    this.draw();
  }
  valid() {
    const n = +this.s.disc.number,
      tt = +this.s.disc.total;
    return Number.isInteger(n) && Number.isInteger(tt) && n >= 1 && tt >= 1 && n <= tt && tt <= 999;
  }
  changed() {
    this.o.onChange && this.o.onChange();
  }
  field(label, key, obj = this.s.album, cls = '') {
    return `<label class="field ${cls}">${esc(label)}<input class="input" data-k="${key}" data-o="${obj === this.s.disc ? 'disc' : 'album'}" value="${esc(obj[key] ?? '')}"></label>`;
  }
  draw() {
    const s = this.s,
      ok = this.valid();
    const pick = (kind, label) => {
      const c = s[kind];
      return `<div class="cover-pick"><div class="lbl">${esc(label)}</div>
        <div class="thumb" data-pick="${kind}">${c?.dataUrl ? `<img src="${c.dataUrl}" alt="">` : icon('image')}</div>
        <div class="row" style="margin-top:6px"><button class="btn small" data-pick="${kind}">${esc(t('meta.pick'))}</button>${c ? `<button class="btn ghost small" data-rm="${kind}">${esc(t('meta.remove'))}</button>` : ''}</div></div>`;
    };
    this.el.innerHTML = `<div class="meta-layout">
      <div>
        <div class="grid2">
          ${this.field(t('meta.album'), 'title')}${this.field(t('meta.artist'), 'artist')}
        </div>
        <div class="grid3" style="margin-top:12px">${this.field(t('meta.genre'), 'genre')}${this.field(t('meta.date'), 'date')}${this.field(t('meta.label'), 'label')}</div>
        <div class="disc-box">
          <label class="field">${esc(t('meta.disctotal'))}<input class="input num-in ${ok ? '' : 'err'}" type="number" min="1" max="999" id="mf-total" value="${esc(s.disc.total)}"></label>
          <label class="field">${esc(t('meta.discnum'))}<div class="row"><input class="input num-in ${ok ? '' : 'err'}" type="number" min="1" max="999" id="mf-num" value="${esc(s.disc.number)}"><span class="of num">/ ${esc(s.disc.total)}</span></div></label>
          ${this.field(t('meta.subtitle'), 'subtitle', s.disc, 'grow')}
          <button class="btn small" id="mf-join">${icon('link')}${esc(t('meta.join'))}</button>
          ${ok ? '' : `<div class="error-text" style="flex-basis:100%">${esc(t('meta.err.disc'))}</div>`}
          ${s.joined ? `<div class="hint" style="flex-basis:100%">${esc(t('meta.joined', { a: s.joined }))}</div>` : ''}
        </div>
        <div class="section-title">${esc(t('meta.tracks'))}</div>
        <div class="tracks-edit"><table>${s.tracks
          .map(
            (tr, i) => `<tr><td class="n num">${tr.number}</td>
          <td><input class="input" data-tt="${i}" value="${esc(tr.title || '')}" placeholder="${esc(t('track', { n: tr.number }))}"></td>
          <td style="width:34%"><input class="input" data-ta="${i}" value="${esc(tr.artist || '')}" placeholder="${esc(t('meta.trackartist'))}"></td></tr>`,
          )
          .join('')}</table></div>
      </div>
      <div>
        <div class="covers-pick">${pick('front', t('meta.front'))}${pick('back', t('meta.back'))}</div>
        <div class="mb" style="margin-top:18px" id="mf-mb"></div>
      </div></div>`;
    $$('[data-k]', this.el).forEach(
      (inp) =>
        (inp.oninput = () => {
          (inp.dataset.o === 'disc' ? s.disc : s.album)[inp.dataset.k] = inp.value;
          this.changed();
        }),
    );
    $$('[data-tt]', this.el).forEach(
      (inp) =>
        (inp.oninput = () => {
          s.tracks[+inp.dataset.tt].title = inp.value;
        }),
    );
    $$('[data-ta]', this.el).forEach(
      (inp) =>
        (inp.oninput = () => {
          s.tracks[+inp.dataset.ta].artist = inp.value;
        }),
    );
    const num = (id, key) =>
      ($(id, this.el).onchange = (e) => {
        s.disc[key] = parseInt(e.target.value, 10) || 0;
        this.draw();
        this.changed();
      });
    num('#mf-total', 'total');
    num('#mf-num', 'number');
    $$('[data-pick]', this.el).forEach(
      (b) =>
        (b.onclick = async () => {
          try {
            const img = await api.dialogs.openImage();
            if (img) {
              s[b.dataset.pick] = img;
              s[b.dataset.pick + 'Changed'] = true;
              this.draw();
            }
          } catch (e) {
            errorModal(e.message);
          }
        }),
    );
    $$('[data-rm]', this.el).forEach(
      (b) =>
        (b.onclick = () => {
          s[b.dataset.rm] = null;
          s[b.dataset.rm + 'Changed'] = true;
          this.draw();
        }),
    );
    $('#mf-join', this.el).onclick = () => this.join();
    this.drawMb();
  }
  async join() {
    const paths = await api.dialogs.openFiso();
    if (!paths.length) return;
    try {
      const info = await api.info(paths[0]);
      const m = info.meta;
      this.s.album = { ...m.album };
      this.s.disc.total = m.disc.total;
      if (this.s.disc.number === m.disc.number && m.disc.number < m.disc.total) this.s.disc.number = m.disc.number + 1;
      this.s.joined = m.album.title;
      this.draw();
      this.changed();
    } catch (e) {
      errorModal(e.message);
    }
  }
  drawMb() {
    const b = this.mb,
      el = $('#mf-mb', this.el);
    const rel = b.release;
    el.innerHTML = `<div class="section-title" style="margin:0 0 8px">${icon('search', '')} ${esc(t('mb.title'))}</div>
      ${this.o.discid ? `<button class="btn small" id="mb-id" style="width:100%;justify-content:center">${icon('disc')}${esc(t('mb.identify'))}</button><div style="height:10px"></div>` : ''}
      <div style="display:flex;flex-direction:column;gap:6px">
        <input class="input" id="mb-ar" placeholder="${esc(t('mb.artist'))}" value="${esc(b.artist)}">
        <input class="input" id="mb-al" placeholder="${esc(t('mb.album'))}" value="${esc(b.album)}">
        <button class="btn small" id="mb-go" ${b.busy ? 'disabled' : ''}>${esc(b.busy ? t('mb.loading') : t('mb.search'))}</button>
      </div>
      ${b.msg ? `<p class="hint" style="margin:8px 0 0">${esc(b.msg)}</p>` : ''}
      ${
        b.results
          ? b.results.length
            ? `<p class="hint" style="margin:10px 0 0">${esc(t('mb.choose'))}</p><div class="mb-results">${b.results
                .map((r, i) => {
                  const counts = (r.tracks || []).filter((n) => n != null);
                  const total = counts.reduce((a2, n) => a2 + n, 0);
                  const fits = counts.includes(this.s.tracks.length) && this.s.tracks.length > 0;
                  const thumb = b.thumbs[r.id];
                  return `<button class="mb-r ${rel?.id === r.id ? 'on' : ''}" data-r="${i}">
          <span class="mb-thumb" data-thumb="${esc(r.id)}">${thumb ? `<img src="${thumb}" alt="">` : icon('disc')}</span>
          <span class="mb-txt"><b>${esc(r.title)}</b><span>${esc(r.artist)}</span>
          <span>${esc([r.date, r.country, r.format, r.label, r.catalog].filter(Boolean).join(' · '))}</span>
          <span class="mb-count"><span class="badge ${fits ? 'ok' : ''}">${esc(counts.length > 1 ? t('mb.discs', { d: counts.length, t: counts.join(' + ') }) : t('mb.tracks', { n: total || '?' }))}</span>${fits ? ` <span class="hint">${esc(t('mb.fits'))}</span>` : ''}</span></span></button>`;
                })
                .join('')}</div>`
            : `<p class="hint">${esc(t('mb.none'))}</p>`
          : ''
      }
      ${
        rel
          ? `<div style="margin-top:10px">
        ${rel.media.length > 1 ? `<label class="field">${esc(t('mb.disc'))}<select class="input" id="mb-med">${rel.media.map((m, i) => `<option value="${i}" ${b.medium === i ? 'selected' : ''}>${esc(t('ex.disc', { n: m.position, t: rel.media.length }))}${m.title ? ' — ' + esc(m.title) : ''} (${m.tracks.length})</option>`).join('')}</select></label>` : ''}
        ${rel.media[b.medium] && rel.media[b.medium].tracks.length !== this.s.tracks.length ? `<p class="error-text">${esc(t('mb.mismatch', { a: rel.media[b.medium].tracks.length, b: this.s.tracks.length }))}</p>` : ''}
        <label class="check" style="margin:8px 0"><input type="checkbox" id="mb-cov" ${b.covers ? 'checked' : ''}>${esc(t('mb.covers'))}</label>
        <button class="btn primary small" id="mb-apply" style="width:100%;justify-content:center" ${b.busy ? 'disabled' : ''}>${esc(t('mb.apply'))}</button></div>`
          : ''
      }`;
    $('#mb-ar', el).oninput = (e) => (b.artist = e.target.value);
    $('#mb-al', el).oninput = (e) => (b.album = e.target.value);
    for (const r of b.results || []) {
      if (r.id in b.thumbs) continue;
      b.thumbs[r.id] = null;
      api.mb
        .thumb(r.id)
        .then((url) => {
          b.thumbs[r.id] = url;
          const slot = this.el.querySelector(`[data-thumb="${CSS.escape(r.id)}"]`);
          if (slot && url) slot.innerHTML = `<img src="${url}" alt="">`;
        })
        .catch(() => {});
    }
    const search = (fn) => this.runSearch(fn);
    $('#mb-go', el).onclick = () => search(() => api.mb.search(b.artist, b.album));
    if ($('#mb-id', el)) $('#mb-id', el).onclick = () => search(() => api.mb.discid(this.o.discid, this.o.toc));
    $$('[data-r]', el).forEach(
      (btn) =>
        (btn.onclick = async () => {
          b.busy = true;
          this.drawMb();
          try {
            b.release = await api.mb.release(b.results[+btn.dataset.r].id);
            let med = b.release.media.findIndex((m) => this.o.discid && m.discids.includes(this.o.discid));
            if (med < 0)
              med = Math.max(
                0,
                b.release.media.findIndex((m) => m.position === +this.s.disc.number),
              );
            b.medium = med;
          } catch (e) {
            b.msg = e.message;
          }
          b.busy = false;
          this.drawMb();
        }),
    );
    if ($('#mb-med', el))
      $('#mb-med', el).onchange = (e) => {
        b.medium = +e.target.value;
        this.drawMb();
      };
    if ($('#mb-cov', el)) $('#mb-cov', el).onchange = (e) => (b.covers = e.target.checked);
    if ($('#mb-apply', el)) $('#mb-apply', el).onclick = () => this.apply();
  }
  async runSearch(fn) {
    const b = this.mb;
    b.busy = true;
    b.msg = '';
    b.release = null;
    this.drawMb();
    try {
      b.results = await fn();
      // releases with the same track count as the disc come first
      const n = this.s.tracks.length;
      if (n && b.results) b.results.sort((x, y) => (y.tracks || []).includes(n) - (x.tracks || []).includes(n));
    } catch (e) {
      b.msg = e.message;
      b.results = null;
    }
    b.busy = false;
    this.drawMb();
  }
  identify() {
    if (this.o.discid) this.runSearch(() => api.mb.discid(this.o.discid, this.o.toc));
  }
  async apply() {
    const b = this.mb,
      r = b.release,
      med = r.media[b.medium];
    const s = this.s;
    s.album = {
      ...s.album,
      title: r.title,
      artist: r.artist,
      date: r.date || s.album.date || '',
      genre: r.genre || s.album.genre || '',
      label: r.label || '',
      catalog_number: r.catalog || '',
      country: r.country || '',
      barcode: r.barcode || '',
      musicbrainz: {
        release_id: r.id,
        release_group_id: r.releaseGroupId,
        album_artist_id: r.albumArtistId,
        ...(this.o.discid && med.discids.includes(this.o.discid) ? { disc_id: this.o.discid } : {}),
      },
    };
    s.disc.total = r.media.length || 1;
    s.disc.number = med.position;
    if (med.title) s.disc.subtitle = med.title;
    s.tracks.forEach((tr, i) => {
      const mt = med.tracks[i];
      if (!mt) return;
      tr.title = mt.title;
      tr.artist = mt.artist && mt.artist !== r.artist ? mt.artist : '';
      tr.musicbrainz = { recording_id: mt.recordingId, track_id: mt.trackId };
    });
    if (b.covers) {
      b.busy = true;
      this.drawMb();
      for (const kind of ['front', 'back']) {
        try {
          const c = await api.mb.cover(r.id, kind);
          if (c) {
            s[kind] = c;
            s[kind + 'Changed'] = true;
          }
        } catch {
          /* no artwork */
        }
      }
      b.busy = false;
    }
    b.msg = t('mb.applied');
    b.release = null;
    b.results = null;
    this.draw();
    this.changed();
  }
  // META object, pictures to add and picture types to remove
  collect() {
    const clean = (o) =>
      Object.fromEntries(
        Object.entries(o).filter(([, v]) => v !== '' && v != null && !(typeof v === 'object' && !Object.keys(v).length)),
      );
    const s = this.s;
    const meta = {
      ...(s.base || {}),
      schema: 'fiso.meta/1',
      album: clean({ ...s.album }),
      disc: clean({ number: +s.disc.number, total: +s.disc.total, subtitle: s.disc.subtitle }),
      tracks: s.tracks.map((tr) =>
        clean({ ...(tr.extra || {}), number: tr.number, title: tr.title, artist: tr.artist, musicbrainz: tr.musicbrainz }),
      ),
    };
    if (!meta.album.title) meta.album.title = t('untitled');
    const pictures = [],
      remove = [];
    for (const [kind, type] of [
      ['front', 3],
      ['back', 4],
    ]) {
      if (!s[kind + 'Changed']) continue;
      if (s[kind]?.path) pictures.push({ type, path: s[kind].path });
      else remove.push(type);
    }
    return { meta, pictures, remove };
  }
}

function formStateFromMeta(m, covers) {
  const front = covers?.find((c) => c.type === 3);
  const back = covers?.find((c) => c.type === 4);
  const { album, disc, tracks, ...base } = m;
  return {
    base,
    album: { ...album },
    disc: { number: disc.number, total: disc.total, subtitle: disc.subtitle || '' },
    tracks: tracks.map(({ number, title, artist, musicbrainz, ...extra }) => ({
      number,
      title: title || '',
      artist: artist || '',
      musicbrainz,
      extra,
    })),
    front: front ? { dataUrl: front.dataUrl } : null,
    back: back ? { dataUrl: back.dataUrl } : null,
  };
}

async function editDialog() {
  const d = doc();
  if (!d) return;
  const state = formStateFromMeta(d.info.meta, d.info.covers);
  let saveBtn;
  const form = new MetaForm(state, {
    mode: 'edit',
    discid: d.info.musicbrainz_disc_id,
    toc: d.info.musicbrainz_toc,
    onChange: () => saveBtn && (saveBtn.disabled = !form.valid()),
  });
  const body = document.createElement('div');
  body.innerHTML = `<p class="hint" style="margin:0 0 14px">${icon('info')} ${esc(t('meta.edit.note'))}</p>`;
  body.appendChild(form.el);
  const m = modal({
    title: t('meta.edit.title'),
    body,
    wide: true,
    foot: [
      { label: t('cancel') },
      {
        label: t('meta.save'),
        primary: true,
        id: 'mf-save',
        onClick: async (close) => {
          if (!form.valid()) return;
          const { meta, pictures, remove } = form.collect();
          close();
          const ok = await runJob(t('meta.edit.title'), (job) =>
            api.edit(d.path, { meta, pictures, remove_picture_types: remove }, job),
          );
          if (ok) {
            delete S.verified[d.path];
            if (S.player.path === d.path) stopPlayer(true);
            await openImage(d.path);
          }
        },
      },
    ],
  });
  saveBtn = $('#mf-save', m.el);
}

// ------------------------------------------------------------------ create
function stripTrackName(name) {
  return name
    .replace(/\.(flac|wav)$/i, '')
    .replace(/^\s*(?:disc\s*\d+\s*[-_.]\s*)?\d{1,3}\s*[-_.)\s]\s*/i, '')
    .trim();
}

function renderCreate(v) {
  const C =
    S.create ||
    (S.create = {
      kind: null,
      files: [],
      cue: null,
      out: null,
      compression: 8,
      reencode: false,
      cd: {
        drives: null,
        device: '',
        info: null,
        loading: false,
        error: '',
        mode: S.settings.ripMode || 'secure',
        offset: 0,
        offsetMsg: '',
        identified: false,
      },
      form: {
        album: { title: '', artist: '' },
        disc: { number: 1, total: 1, subtitle: '' },
        tracks: [],
        front: null,
        back: null,
      },
    });
  const CD = C.cd;
  v.innerHTML = `<div>
    <h1 class="page-title">${esc(t('cr.title'))}</h1>
    <section class="card step"><div class="step-h"><span class="step-n">1</span><h2>${esc(t('cr.source'))}</h2></div>
      <div class="tiles">
        <button class="tile ${C.kind === 'cd' ? 'on' : ''}" id="src-cd">${icon('disc')}<div><b>${esc(t('cr.cd'))}</b><span>${esc(t('cr.cd.d'))}</span></div></button>
        <button class="tile ${C.kind === 'files' ? 'on' : ''}" id="src-files">${icon('music')}<div><b>${esc(t('cr.files'))}</b><span>${esc(t('cr.files.d'))}</span></div></button>
        <button class="tile ${C.kind === 'cue' ? 'on' : ''}" id="src-cue">${icon('file')}<div><b>${esc(t('cr.cue'))}</b><span>${esc(t('cr.cue.d'))}</span></div></button>
      </div>
      <div id="src-list"></div>
    </section>
    <section class="card step"><div class="step-h"><span class="step-n">2</span><h2>${esc(t('cr.meta'))}</h2></div><div id="form-host"></div></section>
    <section class="card step"><div class="step-h"><span class="step-n">3</span><h2>${esc(t('cr.output'))}</h2></div>
      <div class="row" style="align-items:flex-end;gap:18px;flex-wrap:wrap">
        <label class="field">${esc(t('cr.compression'))}<select class="input" id="cr-level" style="width:120px">${[8, 7, 6, 5, 4, 3, 2, 1, 0].map((n) => `<option ${C.compression === n ? 'selected' : ''}>${n}</option>`).join('')}</select></label>
        <span class="hint grow">${esc(t('cr.compression.d'))}</span>
      </div>
      ${
        C.kind === 'cd'
          ? ''
          : `<label class="check" style="margin-top:14px;align-items:flex-start"><input type="checkbox" id="cr-reencode" style="margin-top:3px" ${C.reencode ? 'checked' : ''}><span>${esc(t('cr.reencode'))}<br><span class="hint">${esc(t('cr.reencode.d'))}</span></span></label>`
      }
      <div class="output-row" style="margin-top:16px"><span class="path grow">${esc(C.out || t('cr.nooutput'))}</span><button class="btn" id="cr-out">${esc(t('cr.choose'))}</button></div>
      <div class="row" style="justify-content:flex-end;margin-top:18px"><button class="btn primary" id="cr-go">${icon('disc')}${esc(C.kind === 'cd' ? t('cd.go') : t('cr.go'))}</button></div>
    </section></div>`;

  const ready = () => (C.kind === 'cd' ? !!CD.info : C.kind === 'files' ? C.files.length > 0 : !!C.cue);
  const updateGo = () => {
    $('#cr-go').disabled = !(C.kind && ready() && C.out && form.valid());
  };
  const form = new MetaForm(C.form, {
    mode: 'create',
    onChange: updateGo,
    discid: C.kind === 'cd' ? CD.info?.musicbrainz_disc_id : null,
    toc: CD.info?.musicbrainz_toc,
  });
  $('#form-host').appendChild(form.el);
  if (C.kind === 'cd' && CD.info?.musicbrainz_disc_id && !CD.identified) {
    CD.identified = true;
    form.identify();
  }
  const driveLabel = (d) => {
    const m = d.media || {};
    const media = m.state !== 'present' ? t('cd.media.none') : m.blank ? t('cd.media.blank') : m.type || 'CD';
    return `${d.name} — ${media}`;
  };
  const drawList = () => {
    const el = $('#src-list');
    if (C.kind === 'files') {
      el.innerHTML = `<div class="srclist">${C.files
        .map(
          (
            f,
            i,
          ) => `<div class="srcrow"><span class="n num">${i + 1}</span><span class="grow path">${esc(f.name)}</span><span class="hint num">${fmtBytes(f.size)}</span>
        <button class="btn ghost icon-btn" data-up="${i}" ${i ? '' : 'disabled'}>${icon('up')}</button><button class="btn ghost icon-btn" data-down="${i}" ${i < C.files.length - 1 ? '' : 'disabled'}>${icon('down')}</button><button class="btn ghost icon-btn" data-del="${i}" title="${esc(t('meta.remove'))}">${icon('trash')}</button></div>`,
        )
        .join('')}
        <div class="srcrow"><button class="btn small" id="src-add">${icon('plus')}${esc(t('cr.add'))}</button>
          ${C.files.length ? `<button class="btn small" id="src-clear">${icon('clear')}${esc(t('cr.clear'))}</button>` : ''}
          <span class="hint grow">${esc(t(C.reencode ? 'cr.reencode.on' : 'cr.passthrough'))}</span></div></div>`;
      $('#src-add').onclick = addFiles;
      if ($('#src-clear'))
        $('#src-clear').onclick = () =>
          modal({
            title: t('cr.clear'),
            body: `<p>${esc(t('cr.clear.q', { n: C.files.length }))}</p>`,
            foot: [
              { label: t('cancel') },
              {
                label: t('cr.clear'),
                primary: true,
                onClick: (c) => {
                  c();
                  C.files = [];
                  C.form.tracks = [];
                  render();
                },
              },
            ],
          });
      const move = (i, j) => {
        [C.files[i], C.files[j]] = [C.files[j], C.files[i]];
        syncTracks();
      };
      $$('[data-up]', el).forEach((b) => (b.onclick = () => move(+b.dataset.up, +b.dataset.up - 1)));
      $$('[data-down]', el).forEach((b) => (b.onclick = () => move(+b.dataset.down, +b.dataset.down + 1)));
      $$('[data-del]', el).forEach(
        (b) =>
          (b.onclick = () => {
            C.files.splice(+b.dataset.del, 1);
            syncTracks();
          }),
      );
    } else if (C.kind === 'cue') {
      el.innerHTML = `<div class="srclist"><div class="srcrow">${icon('file')}<span class="grow path">${esc(C.cue ? C.cue.path : t('cr.nosource'))}</span><button class="btn small" id="src-cue2">${esc(t('cr.choosecue'))}</button></div></div>`;
      $('#src-cue2').onclick = chooseCue;
    } else if (C.kind === 'cd') {
      const drives = (CD.drives || []).filter((d) => d.can_read !== false);
      const i = CD.info;
      el.innerHTML = `<div class="cdbox">
        <div class="row" style="flex-wrap:wrap">
          <label class="field grow">${esc(t('cd.drive'))}<select class="input" id="cd-dev" ${drives.length ? '' : 'disabled'}>${drives.map((d) => `<option value="${esc(d.id)}" ${d.id === CD.device ? 'selected' : ''}>${esc(driveLabel(d))}</option>`).join('')}</select></label>
          <button class="btn" id="cd-ref" style="align-self:flex-end">${icon('refresh')}${esc(t('cd.refresh'))}</button>
          <button class="btn" id="cd-ej" style="align-self:flex-end" ${CD.device ? '' : 'disabled'}>${icon('eject')}${esc(t('cd.eject'))}</button>
        </div>
        <div class="cd-status">${
          CD.loading
            ? `<span class="hint">${esc(t('cd.reading'))}</span>`
            : CD.error
              ? `<span class="error-text">${esc(CD.error)}</span>`
              : CD.drives && !drives.length
                ? `<span class="hint">${esc(t('cd.nodrive'))}</span>`
                : !i
                  ? `<span class="hint">${esc(t('cd.nodisc'))}</span>`
                  : `<div class="chips" style="margin:0"><span class="badge accent">${icon('disc')}${esc(t('cd.summary', { n: i.audio_tracks, d: fmtDur(i.seconds) }))}</span>
              ${i.cdtext?.album_title ? `<span class="badge">${esc(t('cd.cdtext'))}${t('sep')}${esc(i.cdtext.album_title)}</span>` : ''}
              ${i.mcn ? `<span class="badge mono">MCN ${esc(i.mcn)}</span>` : ''}</div>
             ${i.has_data_tracks ? `<p class="hint" style="margin:8px 0 0">${esc(t('cd.data'))}</p>` : ''}
             ${i.htoa_sectors > 0 ? `<p class="hint" style="margin:4px 0 0">${esc(t('cd.htoa', { d: fmtDur(i.htoa_sectors / 75) }))}</p>` : ''}`
        }</div>
        ${
          i
            ? `<div class="grid2" style="margin-top:14px">
          <div><div class="section-title">${esc(t('cd.mode'))}</div>
            <div class="seg" id="cd-mode"><button data-v="secure" class="${CD.mode === 'secure' ? 'on' : ''}">${esc(t('cd.secure'))}</button><button data-v="fast" class="${CD.mode === 'fast' ? 'on' : ''}">${esc(t('cd.fast'))}</button></div>
            <p class="hint" style="margin:6px 0 0">${esc(t(CD.mode === 'fast' ? 'cd.fast.d' : 'cd.secure.d'))}</p></div>
          <div><div class="section-title">${esc(t('cd.offset'))}</div>
            <div class="row"><input class="input num-in" style="width:96px" type="number" id="cd-off" value="${CD.offset}"><button class="btn small" id="cd-offfind">${icon('search')}${esc(t('cd.offset.find'))}</button></div>
            <p class="hint" style="margin:6px 0 0">${esc(CD.offsetMsg || t('cd.offset.d'))}</p></div>
        </div>
        <label class="check" style="margin-top:12px"><input type="checkbox" id="cd-ar" ${S.settings.accurateRip !== false ? 'checked' : ''}>${esc(t('cd.ar'))}</label>`
            : ''
        }
      </div>`;
      $('#cd-ref').onclick = () => loadDrives(true);
      if ($('#cd-dev'))
        $('#cd-dev').onchange = (e) => {
          CD.device = e.target.value;
          loadInfo();
        };
      $('#cd-ej').onclick = async () => {
        try {
          await api.cd.eject(CD.device);
          CD.info = null;
          toast(t('eject.done'));
          render();
        } catch (e) {
          errorModal(e.message);
        }
      };
      $$('#cd-mode [data-v]').forEach(
        (b) =>
          (b.onclick = () => {
            CD.mode = b.dataset.v;
            api.settings.set({ ripMode: CD.mode }).then((s) => (S.settings = s));
            drawList();
          }),
      );
      if ($('#cd-off'))
        $('#cd-off').onchange = (e) => {
          CD.offset = parseInt(e.target.value, 10) || 0;
          const name = currentDrive()?.name;
          if (name)
            api.settings
              .set({ readOffsets: { ...(S.settings.readOffsets || {}), [name]: CD.offset } })
              .then((s) => (S.settings = s));
        };
      if ($('#cd-offfind'))
        $('#cd-offfind').onclick = async () => {
          const d = currentDrive();
          try {
            const r = d ? await api.ar.offset(d.vendor || '', d.product || d.name) : null;
            if (r) {
              CD.offset = r.offset;
              CD.offsetMsg = t('cd.offset.found', { n: r.name, o: (r.offset > 0 ? '+' : '') + r.offset, s: r.submits });
              api.settings
                .set({ readOffsets: { ...(S.settings.readOffsets || {}), [d.name]: r.offset } })
                .then((s) => (S.settings = s));
            } else CD.offsetMsg = t('cd.offset.none');
          } catch (e) {
            CD.offsetMsg = e.message;
          }
          drawList();
        };
      if ($('#cd-ar'))
        $('#cd-ar').onchange = (e) => api.settings.set({ accurateRip: e.target.checked }).then((s) => (S.settings = s));
    } else el.innerHTML = '';
  };
  const currentDrive = () => (CD.drives || []).find((d) => d.id === CD.device);
  const loadDrives = async (force) => {
    CD.error = '';
    try {
      const r = await api.cd.drives();
      CD.drives = r.drives || [];
      if (r.platform === 'unsupported') CD.error = t('e.platform');
    } catch (e) {
      CD.error = e.message;
      CD.drives = [];
    }
    const withDisc = CD.drives.filter((d) => d.media?.readable);
    if (!CD.drives.some((d) => d.id === CD.device) || force)
      CD.device = (withDisc.find((d) => d.id === S.settings.defaultReader) || withDisc[0] || CD.drives[0] || {}).id || '';
    if (CD.device && withDisc.some((d) => d.id === CD.device)) await loadInfo();
    else {
      CD.info = null;
      render();
    }
  };
  const loadInfo = async () => {
    CD.loading = true;
    CD.error = '';
    CD.info = null;
    CD.identified = false;
    render();
    try {
      const i = await api.cd.info(CD.device);
      CD.info = i;
      const name = currentDrive()?.name || i.drive;
      CD.offset = (S.settings.readOffsets || {})[name] ?? 0;
      CD.offsetMsg = '';
      const ct = i.cdtext || {};
      const audio = i.tracks.filter((x) => x.audio && x.number >= i.first_audio_track).slice(0, i.audio_tracks);
      C.form.tracks = audio.map((x) => {
        const tt = (ct.tracks || []).find((y) => y.number === x.number) || {};
        return {
          number: x.number,
          title: tt.title || '',
          artist: tt.performer && tt.performer !== ct.album_performer ? tt.performer : '',
        };
      });
      if (ct.album_title && !C.form.album.title) C.form.album.title = ct.album_title;
      if (ct.album_performer && !C.form.album.artist) C.form.album.artist = ct.album_performer;
    } catch (e) {
      CD.error = humanError(e.message).text;
    }
    CD.loading = false;
    render();
  };
  const syncTracks = () => {
    const old = C.form.tracks;
    if (C.kind === 'files')
      C.form.tracks = C.files.map((f, i) => ({
        number: i + 1,
        title: old[i]?.edited ? old[i].title : stripTrackName(f.name),
        artist: old[i]?.artist || '',
      }));
    render();
  };
  const addFiles = async () => {
    const ps = await api.dialogs.openSources('files');
    if (!ps.length) return;
    C.files.push(...(await api.peekFiles(ps)));
    syncTracks();
  };
  const chooseCue = async () => {
    const ps = await api.dialogs.openSources('cue');
    if (!ps.length) return;
    try {
      const p = await api.peekCue(ps[0]);
      C.cue = { path: ps[0], peek: p };
      const audio = p.tracks.filter((x) => (x.type || 'AUDIO').toUpperCase() === 'AUDIO');
      C.form.tracks = audio.map((x) => ({
        number: x.number,
        title: x.title,
        artist: x.artist && x.artist !== p.artist ? x.artist : '',
      }));
      if (p.album && !C.form.album.title) C.form.album.title = p.album;
      if (p.artist && !C.form.album.artist) C.form.album.artist = p.artist;
      if (p.date && !C.form.album.date) C.form.album.date = p.date;
      if (p.genre && !C.form.album.genre) C.form.album.genre = p.genre;
      if (audio.length !== p.tracks.length) toast(t('cr.cue.data'), true);
      render();
    } catch (e) {
      errorModal(e.message);
    }
  };
  const switchTo = (k) => {
    if (C.kind !== k) {
      C.form.tracks = [];
    }
    C.kind = k;
    if (k === 'files' && C.files.length) syncTracks();
  };
  $('#src-cd').onclick = () => {
    switchTo('cd');
    render();
    if (!CD.drives || !CD.info) loadDrives(false);
  };
  $('#src-files').onclick = () => {
    switchTo('files');
    if (!C.files.length) addFiles();
    render();
  };
  $('#src-cue').onclick = () => {
    switchTo('cue');
    if (!C.cue) chooseCue();
    else chooseCueKeep();
    render();
  };
  const chooseCueKeep = () => {
    const p = C.cue.peek;
    C.form.tracks = p.tracks
      .filter((x) => (x.type || 'AUDIO').toUpperCase() === 'AUDIO')
      .map((x) => ({ number: x.number, title: x.title, artist: x.artist && x.artist !== p.artist ? x.artist : '' }));
  };
  $('#cr-level').onchange = (e) => (C.compression = +e.target.value);
  if ($('#cr-reencode'))
    $('#cr-reencode').onchange = (e) => {
      C.reencode = e.target.checked;
      drawList();
    };
  $('#cr-out').onclick = async () => {
    const a = C.form.album;
    const name =
      [a.artist, a.title || 'Album'].filter(Boolean).join(' - ') +
      (+C.form.disc.total > 1 ? ` - Disc ${C.form.disc.number}` : '');
    const p = await api.dialogs.saveFiso(name);
    if (p) {
      C.out = p.toLowerCase().endsWith('.fiso') ? p : p + '.fiso';
      render();
    }
  };
  $('#cr-go').onclick = () => doCreate(form);
  drawList();
  updateGo();
}

function arHtml(res) {
  if (!res) return '';
  const r = res.result || {};
  if (!r.in_database)
    return `<div class="state na"><div class="ic">${icon('info')}</div><div><b>${esc(t('ar.title'))}</b><p>${esc(t('ar.absent'))}</p></div></div>`;
  const ok = r.all_matched;
  return `<div class="state ${ok ? 'ok' : 'bad'}"><div class="ic">${icon(ok ? 'check' : 'alert')}</div><div><b>${esc(t('ar.title'))}</b>
    <p>${esc(ok ? t('ar.ok', { c: r.min_confidence }) : t('ar.partial'))}${res.marked ? ' ' + esc(t('ar.marked')) : ''}</p>
    <ul class="idx-list" style="margin-top:6px;color:var(--text-2)">${(r.tracks || []).map((x) => `<li><span>${esc(t('track', { n: x.number }))}</span><span class="${x.match ? '' : 'error-text'}">${esc(x.match ? t('ar.match', { c: x.confidence }) + ' · ' + x.match : t('ar.nomatch'))}</span></li>`).join('')}</ul></div></div>`;
}

async function runAccurateRip(path) {
  return runJob(t('ar.check'), (job) => api.ar.check(path, job));
}

async function doCreate(form) {
  const C = S.create;
  if (!form.valid()) return;
  const { meta, pictures } = form.collect();
  delete meta.provenance;
  let r;
  let ar = null;
  if (C.kind === 'cd') {
    const opts = {
      device: C.cd.device,
      output: C.out,
      overwrite: true,
      mode: C.cd.mode,
      read_offset: C.cd.offset,
      compression: C.compression,
      meta,
      pictures,
      detect_pregaps: true,
      read_htoa: true,
    };
    r = await runJob(t('cd.go'), (job) => api.cd.rip(opts, job));
    if (!r) return;
    if (S.settings.accurateRip !== false) ar = await runAccurateRip(r.output);
  } else {
    const plan = {
      output: C.out,
      overwrite: true,
      compression: C.compression,
      flac_passthrough: !C.reencode,
      meta,
      pictures,
      source: C.kind === 'files' ? { kind: 'files', files: C.files.map((f) => f.path) } : { kind: 'cue', path: C.cue.path },
    };
    r = await runJob(t('cr.title'), (job) => api.create(plan, job));
    if (!r) return;
  }
  const v = r.verify;
  const allOk = v && v.valid_file.ok && v.intact_audio.ok;
  const rip = r.rip;
  modal({
    title: rip ? t('rip.title') : t('rep.title'),
    body: `<div class="report">
      <div class="big-stats">
        <div><div class="v num">${fmtBytes(r.pcm_bytes)}</div><div class="k">${esc(t('rep.pcm'))}</div></div>
        <div><div class="v num">${fmtBytes(r.fiso_bytes)}</div><div class="k">${esc(t('rep.fiso'))}</div></div>
        <div><div class="v num">${pct(r.ratio)}</div><div class="k">${esc(t('rep.ratio'))}</div></div>
      </div>
      <div class="integrity">
        <span class="badge ${allOk ? 'ok' : 'bad'}">${icon(allOk ? 'check' : 'alert')}${esc(t('ex.valid'))} · ${esc(t('ex.audio'))}</span>
        ${r.flac_passthrough_tracks?.length ? `<span class="badge">${esc(t('rep.pass', { n: r.flac_passthrough_tracks.length }))}</span>` : ''}
        ${r.encoding && !r.encoding.copied_tracks ? `<span class="badge">${esc(t('rep.encoded', { e: r.encoding.encoder, n: r.encoding.compression }))}</span>` : ''}
      </div>
      ${
        rip
          ? `<div class="integrity">
        <span class="badge ${rip.unrecovered_sectors.length ? 'bad' : 'ok'}">${icon(rip.unrecovered_sectors.length ? 'alert' : 'check')}${esc(rip.unrecovered_sectors.length ? t('rip.errors', { n: rip.unrecovered_sectors.length }) : t('rip.errors.none'))}</span>
        <span class="badge">${esc(t('rip.retried', { n: rip.retried_sectors }))}</span>
        <span class="badge">${esc(rip.c2 ? t('rip.c2') : t('rip.noc2'))}</span>
        <span class="badge mono">${esc(t('cd.offset'))} ${rip.read_offset > 0 ? '+' : ''}${rip.read_offset}</span></div>`
          : ''
      }
      ${arHtml(ar)}
      ${r.warnings?.length ? `<div class="warn-box">${esc(t('rep.warnings'))}<ul>${r.warnings.map((w) => `<li>${esc(warnText(w))}</li>`).join('')}</ul></div>` : ''}
      <div class="path">${esc(r.output)}</div></div>`,
    foot: [
      {
        label: revealLabel(),
        onClick: (c) => {
          api.shell.show(r.output);
          c();
        },
      },
      {
        label: t('rep.open'),
        primary: true,
        onClick: async (c) => {
          c();
          const vr = ar?.marked ? { state: 'verified', method: '' } : null;
          await openImage(r.output);
          S.verified[r.output] = v;
          if (vr) {
            const d = doc();
            S.verified[r.output] = {
              ...v,
              verified_rip: { state: 'verified', method: d?.info.meta.provenance?.rip?.verification || 'AccurateRip' },
            };
          }
          render();
        },
      },
    ],
  });
  S.create = null;
}

// ------------------------------------------------------------------ audio CD export
// "Convert a FISO image to a burnable audio CD image": writes a BIN/CUE that any burning
// software can write. Never applied during creation; the FISO image is never modified.
async function convertCdDialog(path) {
  if (!path) {
    const ps = await api.dialogs.openFiso();
    if (!ps?.length) return;
    path = ps[0];
  }
  let info;
  try {
    info = await api.info(path);
  } catch (e) {
    errorModal(e.message);
    return;
  }
  const a = info.audio || {};
  const name = path.split(/[\\/]/).pop();
  const isCdFmt = a.sample_rate === 44100 && a.bits_per_sample === 16 && a.channels === 2;
  const f = `${+(a.sample_rate / 1000).toFixed(1)} kHz / ${a.bits_per_sample} bits / ${a.channels === 1 ? t('ch.mono') : a.channels === 2 ? t('ch.stereo') : t('ch.n', { n: a.channels })}`;
  const title = t('cv.title');
  if (a.channels > 2) {
    modal({
      title,
      body: `<p><b>${esc(name)}</b> — ${esc(f)}</p><p class="error-text">${esc(t('e.multichannel'))}</p>`,
      foot: [{ label: t('ok'), primary: true }],
    });
    return;
  }
  const st = { outDir: S.settings.outDir || path.replace(/[\\/][^\\/]*$/, '') };
  const body = document.createElement('div');
  let goBtn = null;
  const draw = () => {
    body.innerHTML = `<p><b>${esc(name)}</b> — <span class="mono">${esc(f)}</span></p>
      <p>${esc(t('cv.d'))}</p>
      <ul class="hint" style="margin:8px 0 0 18px;padding:0">
        ${info.cdda ? `<li>${esc(t('cv.asis'))}</li>` : `<li>${esc(t('cv.align'))}</li>`}
        ${isCdFmt ? '' : `<li>${esc(t('cv.conv', { f }))}</li>`}
        <li>${esc(t('cv.keep'))}</li></ul>
      ${isCdFmt ? '' : `<label class="check" style="margin-top:12px"><input type="checkbox" id="cv-ok">${esc(t('cv.consent'))}</label>`}
      <div class="section-title" style="margin-top:16px">${esc(t('cv.dest'))}</div>
      <div class="output-row"><span class="path grow">${esc(st.outDir)}</span><button class="btn small" id="cv-dir">${esc(t('cr.choose'))}</button></div>`;
    $('#cv-dir', body).onclick = async () => {
      const p = await api.dialogs.chooseDir(st.outDir);
      if (p) {
        st.outDir = p;
        draw();
      }
    };
    const cb = $('#cv-ok', body);
    if (cb)
      cb.onchange = () => {
        if (goBtn) goBtn.disabled = !cb.checked;
      };
    if (goBtn) goBtn.disabled = !!cb && !cb.checked;
  };
  draw();
  const m = modal({
    title,
    body,
    foot: [
      { label: t('cancel') },
      {
        label: t('cv.go'),
        primary: true,
        onClick: async (c) => {
          c();
          const run = (overwrite) =>
            runJob(title, async (job) => {
              try {
                return await api.extract(
                  path,
                  { format: 'bin-cue', outDir: st.outDir, allowConversion: !isCdFmt, overwrite },
                  job,
                );
              } catch (e) {
                if (!overwrite && /already exists/i.test(e.message)) return { exists: true };
                throw e;
              }
            });
          let r = await run(false);
          if (r?.exists) {
            const ok = await new Promise((res) =>
              modal({
                title,
                body: `<p>${esc(t('cv.exists'))}</p>`,
                foot: [
                  {
                    label: t('cancel'),
                    onClick: (x) => {
                      x();
                      res(false);
                    },
                  },
                  {
                    label: t('cv.replace'),
                    primary: true,
                    onClick: (x) => {
                      x();
                      res(true);
                    },
                  },
                ],
              }),
            );
            if (!ok) return;
            r = await run(true);
          }
          if (!r) return;
          const cue = (r.files || []).find((x) => /\.cue$/i.test(x)) || r.files?.[0];
          modal({
            title,
            body: `<div class="report">
          <div class="state ok"><div class="ic">${icon('check')}</div><div><b>${esc(t('cv.done'))}</b><p>${esc(t('cv.done.d'))}</p></div></div>
          ${r.warnings?.length ? `<div class="warn-box">${esc(t('rep.warnings'))}<ul>${r.warnings.map((w) => `<li>${esc(warnText(w))}</li>`).join('')}</ul></div>` : ''}
          ${(r.files || []).map((x) => `<div class="path">${esc(x)}</div>`).join('')}</div>`,
            foot: [
              { label: t('ok') },
              {
                label: revealLabel(),
                primary: true,
                onClick: (c2) => {
                  api.shell.show(cue);
                  c2();
                },
              },
            ],
          });
        },
      },
    ],
  });
  goBtn = [...m.el.querySelectorAll('button.primary')].pop();
  draw();
}

// ------------------------------------------------------------------ AccurateRip and eject (menu actions)
async function arCurrent() {
  const d = doc();
  if (!d) return;
  if (!d.info.cdda) {
    errorModal(t('ar.notrip'));
    return;
  }
  const r = await runAccurateRip(d.path);
  if (!r) return;
  const prev = S.verified[d.path];
  if (r.marked) {
    await openImage(d.path);
    if (prev)
      S.verified[d.path] = {
        ...prev,
        verified_rip: { state: 'verified', method: doc()?.info.meta.provenance?.rip?.verification || 'AccurateRip' },
      };
    render();
  }
  modal({ title: t('ar.title'), body: `<div class="report">${arHtml(r)}</div>`, foot: [{ label: t('ok'), primary: true }] });
}

async function ejectDefault() {
  try {
    await api.cd.eject(S.settings.defaultReader || '');
    toast(t('eject.done'));
  } catch (e) {
    errorModal(e.message);
  }
}

// ------------------------------------------------------------------ settings
function renderSettings(v) {
  const st = S.settings;
  v.innerHTML = `<div class="settings">${closableTitle(t('set.title'))}<div class="card" style="padding:4px 22px">
    <div class="set-row"><span class="k">${esc(t('set.version'))}</span><span><b>FISO Explorer ${esc(S.appInfo?.version || '')}</b>${S.appInfo?.engineVersion ? `<span class="hint"> · ${esc(t('set.engine.v', { v: S.appInfo.engineVersion }))}</span>` : ''}</span></div>
    <div class="set-row"><span class="k">${esc(t('set.lang'))}</span><select class="input" id="s-lang" style="max-width:260px">${LANGS.map(([c, n]) => `<option value="${c}" ${st.lang === c ? 'selected' : ''}>${esc(n)}</option>`).join('')}</select></div>
    <div class="set-row"><span class="k">${esc(t('set.theme'))}</span><div class="seg" id="s-theme">${['system', 'light', 'dark'].map((x) => `<button data-v="${x}" class="${st.theme === x ? 'on' : ''}">${esc(t('set.' + x))}</button>`).join('')}</div></div>
    <div class="set-row"><span class="k">${esc(t('set.outdir'))}</span><div class="output-row"><span class="path grow">${esc(st.outDir || t('set.default'))}</span><button class="btn small" id="s-out">${esc(t('set.choose'))}</button></div></div>
    <div class="set-row"><span class="k">${esc(t('set.template'))}</span><input class="input mono" id="s-tpl" placeholder="{track:02} - {title}" value="${esc(st.template || '')}"></div>
    <div class="set-row"><span class="k">${esc(t('set.reader'))}</span><select class="input" id="s-reader"><option value="">${esc(t('set.auto'))}</option></select></div>
    <div class="set-row"><span class="k">${esc(t('set.ar'))}</span><label class="check"><input type="checkbox" id="s-ar" ${st.accurateRip !== false ? 'checked' : ''}><span class="hint">${esc(t('set.ar.d'))}</span></label></div>
    ${isMac() ? `<div class="set-row"><span class="k">${esc(t('set.thumbs'))}</span><div><span class="hint">${esc(t('set.thumbs.d'))}</span><div class="row" style="margin-top:8px"><button class="btn small" id="s-thumbs">${esc(t('set.thumbs.fix'))}</button><span class="hint" id="s-thumbs-r"></span></div></div></div>` : ''}
    <div class="set-row"><span class="k">${esc(t('set.cache'))}</span><div><button class="btn small" id="s-cache">${esc(t('set.clear'))}</button></div></div>
    <div class="set-row"><span class="k">${esc(t('set.engine'))}</span><span class="path">${esc(S.appInfo?.bin || '')}</span></div>
  </div></div>`;
  bindClose(v);
  const save = async (patch) => {
    S.settings = await api.settings.set(patch);
    render();
  };
  $('#s-lang').onchange = (e) => save({ lang: e.target.value });
  if ($('#s-thumbs'))
    $('#s-thumbs').onclick = async () => {
      const out = $('#s-thumbs-r');
      out.textContent = t('set.thumbs.working');
      try {
        const r = await api.thumbs.register();
        out.textContent = r.skipped
          ? t('set.thumbs.dev')
          : r.ok
            ? t('set.thumbs.ok')
            : t('set.thumbs.bad') + (r.quarantined ? ' ' + t('set.thumbs.quarantine') : '');
      } catch (e) {
        out.textContent = e.message;
      }
    };
  $$('#s-theme [data-v]').forEach((b) => (b.onclick = () => save({ theme: b.dataset.v })));
  $('#s-out').onclick = async () => {
    const p = await api.dialogs.chooseDir(st.outDir);
    if (p) save({ outDir: p });
  };
  $('#s-tpl').onchange = (e) => save({ template: e.target.value.trim() });
  $('#s-cache').onclick = async () => {
    await api.settings.clearCache();
    toast(t('set.cleared'));
  };
  $('#s-ar').onchange = (e) => save({ accurateRip: e.target.checked });
  $('#s-reader').onchange = (e) => save({ defaultReader: e.target.value });
  api.cd
    .drives()
    .then((r) => {
      for (const d of r.drives || []) {
        if (!$('#s-reader')) return;
        $('#s-reader').insertAdjacentHTML(
          'beforeend',
          `<option value="${esc(d.id)}" ${st.defaultReader === d.id ? 'selected' : ''}>${esc(d.name)}</option>`,
        );
      }
    })
    .catch(() => {});
}

function renderHelp(v) {
  const L = S.settings.lang;
  v.innerHTML = `<div class="prose">${closableTitle(t('help.title'))}
    <div class="row"><button class="btn" id="h-spec">${icon('file')}${esc(t('help.spec'))}</button><button class="btn" id="h-site">${icon('link')}${esc(t('help.site'))}</button></div>
    ${HELP[L] || HELP.en}
    <h3>${esc(t('help.limits'))}</h3><ul>${(LIMITS[L] || LIMITS.en).map((x) => `<li>${esc(x)}</li>`).join('')}</ul></div>`;
  bindClose(v);
  $('#h-spec').onclick = () => api.shell.spec();
  $('#h-site').onclick = () => api.shell.site();
}

function aboutDialog() {
  const i = S.appInfo || {};
  modal({
    title: t('about.title'),
    body: `<div style="display:flex;gap:16px;align-items:center"><img src="brand-icon.png" alt="" width="56" height="56">
      <div><h2>FISO Explorer ${esc(i.version || '')}</h2><p class="hint" style="margin:4px 0 0">${esc(t('about.ref'))}</p></div></div>
      <p class="hint" style="margin-top:16px">© 2026 Anthony Gentil — Retro-Spare · Apache-2.0<br>libFLAC © Xiph.Org Foundation (BSD-3-Clause) · Electron (MIT)<br>${esc(t('about.data'))}</p>`,
    foot: [{ label: t('ok'), primary: true }],
  });
}

// ------------------------------------------------------------------ drag & drop, menu, keyboard
let dragDepth = 0;
window.addEventListener('dragenter', (e) => {
  e.preventDefault();
  dragDepth++;
  $('#dropzone').hidden = false;
});
window.addEventListener('dragleave', () => {
  if (--dragDepth <= 0) {
    dragDepth = 0;
    $('#dropzone').hidden = true;
  }
});
window.addEventListener('dragover', (e) => e.preventDefault());
window.addEventListener('drop', async (e) => {
  e.preventDefault();
  dragDepth = 0;
  $('#dropzone').hidden = true;
  const paths = [...e.dataTransfer.files].map((f) => api.pathOf(f)).filter(Boolean);
  const fisos = paths.filter((p) => /\.fiso$/i.test(p));
  const audioFiles = paths.filter((p) => /\.(flac|wav)$/i.test(p));
  if (fisos.length) {
    for (const p of fisos) await openImage(p, true);
    S.current = fisos[fisos.length - 1];
    go('explorer');
  } else if (S.view === 'create' && audioFiles.length) {
    S.create.kind = 'files';
    S.create.files.push(...(await api.peekFiles(audioFiles)));
    S.create.form.tracks = S.create.files.map((f, i) => ({ number: i + 1, title: stripTrackName(f.name), artist: '' }));
    render();
  } else if (paths.length) toast(t('err.notfiso'), true);
});

api.on('open-files', async (paths) => {
  for (const p of paths) await openImage(p, true);
  if (paths.length) {
    S.current = paths[paths.length - 1];
    go('explorer');
  }
});

api.on('menu', (a) => {
  const needDoc = ['extract', 'verify', 'meta', 'close', 'accuraterip'];
  if (needDoc.includes(a) && !doc()) return;
  ({
    open: openDialog,
    create: () => go('create'),
    close: closeCurrent,
    extract: extractDialog,
    verify: verifyCurrent,
    meta: editDialog,
    settings: () => go('settings'),
    help: () => go('help'),
    about: aboutDialog,
    tocd: () => convertCdDialog(doc()?.path),
    accuraterip: arCurrent,
    eject: ejectDefault,
    rip: () => {
      S.create = null;
      go('create');
      $('#src-cd')?.click();
    },
  })[a]?.();
});

document.addEventListener('keydown', (e) => {
  if (e.target.matches('input, textarea, select') || $('.modal-back')) return;
  if (e.key === 'Escape' && (S.view === 'settings' || S.view === 'help')) {
    go('home');
    return;
  }
  if (e.code === 'Space' && S.player.path) {
    e.preventDefault();
    audio.paused ? audio.play() : audio.pause();
  }
});

// ------------------------------------------------------------------ start
(async () => {
  S.settings = await api.settings.get();
  S.appInfo = await api.appInfo();
  render();
})();
