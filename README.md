1) Google Sheets + Apps Script (вебхук)

2) Создай Google Sheet (например лист log).

3) Extensions → Apps Script, вставь код:
```
const CFG = {
  LOG_SHEET: 'log',
  REPORT_SHEET: '_report',
  DEVICE_FILTER: 'pinger',
  LABEL_ON: 'Світло було',
  LABEL_OFF: 'Світла не було',
  LABEL_UNK: 'Невідомо',          // <-- додали
};

function doPost(e) {
  const body = (e && e.postData && e.postData.contents) ? e.postData.contents : '';
  let j = {};
  try { j = body ? JSON.parse(body) : {}; } catch (_) {}
  // Команда на отчёт (HA/Cloud Scheduler/ручной запрос)
  if (String(j.action || '') === 'weekly_report') {
    if (!checkSecret_(j.secret)) return text_('bad secret');
    return withLock_(() => {
      sendWeeklyReport_();
      return text_('report sent');
    });
  }
  // Обычный лог от ESP32
  if (!checkSecret_(j.secret)) return text_('bad secret');

  appendEvent_(j);
  return text_('ok');
}

// Чтобы ESP32/клиенты не ловили 405 на GET (редиректы/проверки и т.п.)
function doGet(e) {
  const p = (e && e.parameter) ? e.parameter : {};
  if (String(p.action || '') === 'weekly_report') {
    if (!checkSecret_(p.secret)) return text_('bad secret');
    return withLock_(() => {
      sendWeeklyReport_();
      return text_('report sent');
    });
  }
  return text_('ok');
}

function withLock_(fn) {
  const lock = LockService.getScriptLock();
  lock.waitLock(30000); // до 30с ждём, если уже идёт генерация
  try {
    return fn();
  } finally {
    lock.releaseLock();
  }
}


function text_(s) {
  return ContentService.createTextOutput(String(s))
    .setMimeType(ContentService.MimeType.TEXT);
}

function checkSecret_(secret) {
  const s = PropertiesService.getScriptProperties().getProperty('GS_SECRET') || '';
  if (!s) return true; // если секрет не задан — не проверяем
  return String(secret || '') === String(s);
}

function normalizeState_(s) {
  const v = String(s || '').toUpperCase();
  if (v === 'ON' || v === '1' || v === 'TRUE') return 'ON';
  if (v === 'OFF' || v === '0' || v === 'FALSE') return 'OFF';
  return v || 'UNK';
}

function appendEvent_(j) {
  const ss = SpreadsheetApp.getActiveSpreadsheet();
  const sh = ss.getSheetByName(CFG.LOG_SHEET) || ss.insertSheet(CFG.LOG_SHEET);

  if (sh.getLastRow() === 0) {
    sh.appendRow(['ts', 'iso', 'device', 'state', 'duration']);
  }

  const ts = Number(j.ts || 0);
  const d = ts ? new Date(ts * 1000) : new Date();

  sh.appendRow([
    d,
    String(j.iso || ''),
    String(j.device || ''),
    normalizeState_(j.state),
    String(j.duration || ''),
  ]);
}

function getWeekStartMonday_(d) {
  const x = new Date(d);
  x.setHours(0, 0, 0, 0);
  const dow = x.getDay();          // 0=Sun..6=Sat
  const diffToMon = (dow + 6) % 7; // сколько дней назад был понедельник
  x.setDate(x.getDate() - diffToMon);
  return x; // понедельник 00:00
}

function sendWeeklyReport_() {
  const ss = SpreadsheetApp.getActiveSpreadsheet();

  const generatedAt = new Date();               // час генерації
  const start = getWeekStartMonday_(generatedAt);
  const end = new Date(start); end.setDate(end.getDate() + 7);

  // Дані беремо ТІЛЬКИ до generatedAt (щоб майбутнє було біле)
  const dataEnd = new Date(Math.min(end.getTime(), generatedAt.getTime()));

  const events = loadEvents_(ss, start, dataEnd);
  const built = buildSegments_(events, start, dataEnd);

  const chartBlob = buildChartPng_(ss, built.rowsForChart, start, end, generatedAt);
  telegramSendPhoto_(chartBlob, built.caption);
}

function loadEvents_(ss, start, end) {
  const sh = ss.getSheetByName(CFG.LOG_SHEET);
  if (!sh || sh.getLastRow() < 2) return [];

  const values = sh.getDataRange().getValues();
  const hdr = values[0].map(v => String(v).toLowerCase());

  const colTs = hdr.indexOf('ts');
  const colDev = hdr.indexOf('device');
  const colState = hdr.indexOf('state');
  if (colTs < 0 || colState < 0) return [];

  let lastBefore = null;
  const out = [];

  for (let i = 1; i < values.length; i++) {
    const r = values[i];

    const dev = (colDev >= 0) ? String(r[colDev] || '') : '';
    if (CFG.DEVICE_FILTER && dev !== CFG.DEVICE_FILTER) continue;

    // parse time
    const tsCell = r[colTs];
    let t = null;
    if (tsCell instanceof Date) t = tsCell;
    else {
      const n = Number(tsCell);
      if (!isFinite(n) || n <= 0) continue;
      t = (n > 1e12) ? new Date(n) : new Date(n * 1000); // ms or sec
    }

    const state = normalizeState_(r[colState]);

    if (t < start) {
      if (!lastBefore || t > lastBefore.t) lastBefore = { t, state };
      continue;
    }
    if (t >= end) continue;

    out.push({ t, state });
  }

  if (lastBefore) out.unshift(lastBefore);
  out.sort((a, b) => a.t - b.t);
  return out;
}

// Строим интервалы ON/OFF и готовим строки для TIMELINE чарта так,
// чтобы ось была 0..24 (все дни “ложим” на одну базовую дату, а дни различаем rowLabel)
function buildSegments_(events, start, end) {
  const BASE = new Date(2000, 0, 1, 0, 0, 0, 0);

  function dayLabel_(d) {
    const wd = ['НД','ПН','ВТ','СР','ЧТ','ПТ','СБ'][d.getDay()];
    const dd = String(d.getDate()).padStart(2, '0');
    const mm = String(d.getMonth() + 1).padStart(2, '0');
    return `${wd} (${dd}.${mm})`;
  }

  // endAtDayEnd=true -> 23:59:59.999 (НЕ 02.01.2000 00:00)
  function toBaseTime_(d, endAtDayEnd) {
    const b = new Date(BASE);
    if (endAtDayEnd) {
      b.setHours(23, 59, 59, 0);
    } else {
      b.setHours(d.getHours(), d.getMinutes(), d.getSeconds(), 0);
    }
    return b;
  }

  let curState = 'UNK';
  for (let i = events.length - 1; i >= 0; i--) {
    if (events[i].t < start) { curState = events[i].state; break; }
  }
  if (curState === 'UNK') curState = 'OFF';

  const inWindow = events.filter(e => e.t >= start && e.t < end);

  const raw = [];
  let curT = new Date(start);
  for (const ev of inWindow) {
    if (ev.t > curT) raw.push({ state: curState, a: new Date(curT), b: new Date(ev.t) });
    curState = ev.state;
    curT = new Date(ev.t);
  }
  if (curT < end) raw.push({ state: curState, a: new Date(curT), b: new Date(end) });

  const rowsForChart = [];
  const ONE_DAY = 24 * 3600 * 1000;

  for (const seg of raw) {
    let a = seg.a;
    const b = seg.b;
    while (a < b) {
      const dayStart = new Date(a); dayStart.setHours(0,0,0,0);
      const dayEnd = new Date(dayStart.getTime() + ONE_DAY);
      const partEnd = new Date(Math.min(b.getTime(), dayEnd.getTime()));

      const label = dayLabel_(dayStart);

      const endAtDayEnd =
        partEnd.getTime() === dayEnd.getTime() && partEnd.getTime() > a.getTime();

      const s = toBaseTime_(a, false);
      const e = toBaseTime_(partEnd, endAtDayEnd);

      // ЖЁСТКИЙ CLAMP: диапазон строго внутри 01.01.2000 00:00:00.000 .. 01.01.2000 23:59:59.999
      const base0 = BASE.getTime();
      const baseEnd = base0 + 24 * 3600 * 1000 - 1000; // последний миллисекунд дня

      let sT = s.getTime();
      let eT = e.getTime();

      // если вдруг получилось 02.01.2000 00:00:00 -> прижимаем к 23:59:59.999
      if (sT < base0) sT = base0;
      if (eT > baseEnd) eT = baseEnd;

      // защита от нулевых/битых интервалов
      if (eT > sT) {
        rowsForChart.push([
          label,
          (seg.state === 'ON') ? CFG.LABEL_ON :
          (seg.state === 'OFF') ? CFG.LABEL_OFF : CFG.LABEL_UNK,
          new Date(sT),
          new Date(eT),
        ]);
      }

      a = partEnd;
    }
  }

  // ... твоя статистика/caption без изменений ...
  // return { rowsForChart, caption };

  let onMs = 0, offMs = 0, outages = 0;
  let prev = null;
  for (const seg of raw) {
    const ms = seg.b.getTime() - seg.a.getTime();
    if (seg.state === 'ON') onMs += ms; else offMs += ms;
    if (prev && prev.state === 'ON' && seg.state === 'OFF') outages++;
    prev = seg;
  }

  const caption =
    `Звіт за 7 діб (${formatDate_(start)}–${formatDate_(new Date(end.getTime()-1))})\n` +
    `Світло було: ${fmtDur_(onMs)}\n` +
    `Світла не було: ${fmtDur_(offMs)}\n` +
    `Вимикали: ${outages} раз(и)`;

  return { rowsForChart, caption };
}

function buildChartPng_(ss, rows, start, end, generatedAt) {
  const sh = ss.getSheetByName(CFG.REPORT_SHEET) || ss.insertSheet(CFG.REPORT_SHEET);
  sh.clearContents();
  sh.getRange(1, 1, 1, 4).setValues([['day', 'state', 'start', 'end']]);
  if (rows.length) {
    sh.getRange(2, 1, rows.length, 4).setValues(rows);
    sh.getRange(2, 3, rows.length, 2).setNumberFormat('dd.MM.yyyy HH:mm:ss');
  }

  const title =
    `Графік відключень світла ${fmtDM_(start)} – ${fmtDM_(new Date(end.getTime() - 1))}`;

  return buildStackedBarPng_(rows, title, start, end, generatedAt);
}

function fmtDM_(d) {
  const dd = String(d.getDate()).padStart(2, '0');
  const mm = String(d.getMonth() + 1).padStart(2, '0');
  return `${dd}.${mm}`;
}

function buildStackedBarPng_(rows, title, weekStart, weekEnd, generatedAt) {
  const GREEN = '#57E64B';
  const RED   = '#FF6B5E';
  const GREY  = '#B0B0B0';
  const WHITE = '#FFFFFF';

  const DAY_WD = ['НД','ПН','ВТ','СР','ЧТ','ПТ','СБ'];

  function dayLabelForDate_(d) {
    const wd = DAY_WD[d.getDay()];
    const dd = String(d.getDate()).padStart(2, '0');
    const mm = String(d.getMonth() + 1).padStart(2, '0');
    return `${wd} (${dd}.${mm})`;
  }

  function secOfDay_(d) {
    return d.getHours() * 3600 + d.getMinutes() * 60 + d.getSeconds();
  }

  function endSec_(dt) {
    // 23:59:59 трактуємо як 24:00
    const h = dt.getHours(), m = dt.getMinutes(), s = dt.getSeconds();
    if (h === 23 && m === 59 && s === 59) return 24 * 3600;
    return h * 3600 + m * 60 + s;
  }

  // 1) з rows робимо map: dayLabel -> segments[{kind,aS,bS}]
  // kind: 'ON' | 'OFF' | 'UNK'
  const map = new Map();
  for (const r of rows) {
    const day = String(r[0] || '');
    const stLabel = String(r[1] || '');
    const a = new Date(r[2]);
    const b = new Date(r[3]);

    let aS = secOfDay_(a);
    let bS = endSec_(b);
    if (bS <= aS) continue;

    let kind = 'UNK';
    if (stLabel === CFG.LABEL_ON) kind = 'ON';
    else if (stLabel === CFG.LABEL_OFF) kind = 'OFF';
    else kind = 'UNK';

    if (!map.has(day)) map.set(day, []);
    map.get(day).push({ kind, aS, bS });
  }

  // 2) формуємо 7 днів тижня (включаючи майбутні) -> для кожного робимо повну шкалу 0..24:
  //   [0..cutoff)=дані або UNK, [cutoff..24)=FUTURE(WHITE)
  const genDay0 = new Date(generatedAt); genDay0.setHours(0,0,0,0);

  const dayRows = [];
  let maxSegs = 0;

  for (let i = 0; i < 7; i++) {
    const d0 = new Date(weekStart);
    d0.setDate(d0.getDate() + i);
    d0.setHours(0,0,0,0);

    const label = dayLabelForDate_(d0);

    // cutoffSec: до цього часу день "відомий/минуле", після — FUTURE
    let cutoffSec;
    if (d0.getTime() < genDay0.getTime()) cutoffSec = 24 * 3600;            // минулий день
    else if (d0.getTime() > genDay0.getTime()) cutoffSec = 0;               // майбутній день
    else cutoffSec = secOfDay_(generatedAt);                                // сьогодні: до часу генерації

    // беремо сегменти цього дня (якщо є)
    let segs = (map.get(label) || []).slice();
    segs.sort((x,y) => x.aS - y.aS);

    // обрізаємо сегменти до cutoffSec
    const clipped = [];
    for (const s of segs) {
      if (s.bS <= 0) continue;
      if (s.aS >= cutoffSec) continue;
      clipped.push({
        kind: s.kind,
        aS: Math.max(0, s.aS),
        bS: Math.min(cutoffSec, s.bS),
      });
    }
    clipped.sort((x,y) => x.aS - y.aS);

    // заповнюємо "дірки" UNK до cutoffSec
    const full = [];
    let t = 0;
    for (const s of clipped) {
      if (s.aS > t) full.push({ kind:'UNK', aS:t, bS:s.aS });
      full.push(s);
      t = Math.max(t, s.bS);
    }
    if (t < cutoffSec) full.push({ kind:'UNK', aS:t, bS:cutoffSec });

    // додаємо FUTURE (біле)
    if (cutoffSec < 24 * 3600) {
      full.push({ kind:'FUT', aS:cutoffSec, bS:24*3600 });
    }

    // мерджимо однакові підряд
    const merged = [];
    for (const s of full) {
      const durH = (s.bS - s.aS) / 3600;
      if (durH <= 0) continue;
      const last = merged[merged.length - 1];
      if (last && last.kind === s.kind) last.durH += durH;
      else merged.push({ kind: s.kind, durH });
    }

    maxSegs = Math.max(maxSegs, merged.length);
    dayRows.push({ label, merged });
  }

  // 3) DataTable: day + (ONi, OFFi, UNKi, FUTi) * maxSegs
  const dt = Charts.newDataTable();
  dt.addColumn(Charts.ColumnType.STRING, 'day');
  for (let i = 1; i <= maxSegs; i++) {
    dt.addColumn(Charts.ColumnType.NUMBER, `ON${i}`);
    dt.addColumn(Charts.ColumnType.NUMBER, `OFF${i}`);
    dt.addColumn(Charts.ColumnType.NUMBER, `UNK${i}`);
    dt.addColumn(Charts.ColumnType.NUMBER, `FUT${i}`);
  }

  for (const item of dayRows) {
    const row = [item.label];
    for (let i = 0; i < maxSegs; i++) {
      const seg = item.merged[i];
      if (!seg) {
        row.push(0,0,0,0);
      } else if (seg.kind === 'ON') {
        row.push(seg.durH, 0, 0, 0);
      } else if (seg.kind === 'OFF') {
        row.push(0, seg.durH, 0, 0);
      } else if (seg.kind === 'UNK') {
        row.push(0, 0, seg.durH, 0);
      } else { // FUT
        row.push(0, 0, 0, seg.durH);
      }
    }
    dt.addRow(row);
  }

  // colors per column
  const colors = [];
  for (let i = 1; i <= maxSegs; i++) {
    colors.push(GREEN, RED, GREY, WHITE);
  }

  const height = 170 + dayRows.length * 60;

  const b = Charts.newBarChart()
    .setDataTable(dt)
    .setStacked()
    .setDimensions(1400, height)
    .setTitle(title)
    .setColors(colors);

  if (typeof b.setOption === 'function') {
    b.setOption('legend', { position: 'none' });
    b.setOption('hAxis', { viewWindow: { min: 0, max: 24 }, ticks: [0,4,8,12,16,20,24] });
    b.setOption('chartArea', { left: 160, top: 60, width: '75%', height: '70%' });
    b.setOption('bar', { groupWidth: '70%' });
  }

  return b.build().getAs('image/png').setName('power_week.png');
}

function telegramSendPhoto_(blob, caption) {
  const props = PropertiesService.getScriptProperties();
  const token = props.getProperty('TG_TOKEN');
  const chatId = props.getProperty('TG_CHAT_ID');
  if (!token || !chatId) throw new Error('Set TG_TOKEN and TG_CHAT_ID in Script Properties');

  const url = 'https://api.telegram.org/bot' + token + '/sendPhoto';

  // Telegram принимает файл как multipart/form-data (upload) :contentReference[oaicite:3]{index=3}
  UrlFetchApp.fetch(url, {
    method: 'post',
    payload: {
      chat_id: chatId,
      caption: caption || '',
      photo: blob,
    },
    muteHttpExceptions: true,
  });
}

function telegramSendText_(text) {
  const props = PropertiesService.getScriptProperties();
  const token = props.getProperty('TG_TOKEN');
  const chatId = props.getProperty('TG_CHAT_ID');
  if (!token || !chatId) return;

  const url = 'https://api.telegram.org/bot' + token + '/sendMessage';
  UrlFetchApp.fetch(url, {
    method: 'post',
    payload: { chat_id: chatId, text: String(text || '') },
    muteHttpExceptions: true,
  });
}

function formatDate_(d) {
  const dd = String(d.getDate()).padStart(2,'0');
  const mm = String(d.getMonth()+1).padStart(2,'0');
  const yy = d.getFullYear();
  return `${dd}.${mm}.${yy}`;
}
function fmtDur_(ms) {
  const s = Math.floor(ms / 1000);
  const m = Math.floor(s / 60);
  const h = Math.floor(m / 60);
  const d = Math.floor(h / 24);
  const hh = h % 24;
  const mm = m % 60;
  if (d > 0) return `${d}д ${hh}г ${mm}хв`;
  return `${hh}г ${mm}хв`;
}

```

appsscript.json
'''
{
  "timeZone": "Europe/Kyiv",
  "dependencies": {},
  "exceptionLogging": "STACKDRIVER",
  "runtimeVersion": "V8",
  "webapp": {
    "executeAs": "USER_DEPLOYING",
    "access": "ANYONE_ANONYMOUS"
  },
  "oauthScopes": [
    "https://www.googleapis.com/auth/spreadsheets",
    "https://www.googleapis.com/auth/drive.readonly",
    "https://www.googleapis.com/auth/drive.file",
    "https://www.googleapis.com/auth/script.external_request",
    "https://www.googleapis.com/auth/script.storage"
  ]
}
'''
4) В Apps Script: Project Settings → Script properties:

- TG_TOKEN = токен бота

- TG_CHAT_ID = chat_id/канал

- GS_SECRET = тот же секрет, что в ESP32 (gs_sec)

5) Deploy → New deployment → Web app. Укажи “Execute as me” (чтобы скрипт мог писать в таблицу) и доступ “Anyone…” по ситуации.

6) Скопируй URL вида https://script.google.com/macros/s/.../exec — это и будет вебхук.