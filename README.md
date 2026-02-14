1) Google Sheets + Apps Script (вебхук)

2) Создай Google Sheet (например лист log).

3) Extensions → Apps Script, вставь код:
```
const CFG = {
  LOG_SHEET: 'log',
  REPORT_SHEET: '_report',
  // если нужно только одно устройство — впиши его id, иначе оставь ''
  DEVICE_FILTER: 'pinger',
  // подписи в легенде
  LABEL_ON: 'Світло було',
  LABEL_OFF: 'Світла не було',
};

function doPost(e) {
  const body = (e && e.postData && e.postData.contents) ? e.postData.contents : '';
  let j = {};
  try { j = body ? JSON.parse(body) : {}; } catch (_) {}

  // Команда на отчёт (HA/Cloud Scheduler/ручной запрос)
  if (String(j.action || '') === 'weekly_report') {
    if (!checkSecret_(j.secret)) return text_('bad secret');
    sendWeeklyReport_();
    return text_('report sent');
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
    sendWeeklyReport_();
    return text_('report sent');
  }
  return text_('ok');
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

function sendWeeklyReport_() {
  const ss = SpreadsheetApp.getActiveSpreadsheet();

  // Берём последние 7 ПОЛНЫХ суток: [end-7d .. end), где end = сегодня 00:00
  const end = new Date();
  end.setHours(0, 0, 0, 0);
  const start = new Date(end);
  start.setDate(start.getDate() - 7);

  const events = loadEvents_(ss, start, end);
  if (!events.length) {
    telegramSendText_('Немає даних для звіту за останні 7 діб.');
    return;
  }

  const built = buildSegments_(events, start, end);

  const chartBlob = buildChartPng_(ss, built.rowsForChart);
  telegramSendPhoto_(chartBlob, built.caption);
}

function loadEvents_(ss, start, end) {
  const sh = ss.getSheetByName(CFG.LOG_SHEET);
  if (!sh || sh.getLastRow() < 2) return [];

  const values = sh.getDataRange().getValues(); // includes header
  const hdr = values[0].map(v => String(v).toLowerCase());

  const colTs = hdr.indexOf('ts');
  const colDev = hdr.indexOf('device');
  const colState = hdr.indexOf('state');

  if (colTs < 0 || colState < 0) return [];

  // захватываем “чуть раньше”, чтобы понять состояние на границе start
  const startMinus = new Date(start);
  startMinus.setDate(startMinus.getDate() - 2);

  const out = [];
  for (let i = 1; i < values.length; i++) {
    const r = values[i];
    const dev = (colDev >= 0) ? String(r[colDev] || '') : '';
    if (CFG.DEVICE_FILTER && dev !== CFG.DEVICE_FILTER) continue;

    const tsCell = r[colTs];
    let t = null;
    if (tsCell instanceof Date) t = tsCell;
    else {
      const n = Number(tsCell);
      if (!isFinite(n) || n <= 0) continue;
      t = (n > 1e12) ? new Date(n) : new Date(n * 1000); // ms or sec
    }

    if (t < startMinus || t >= end) continue;

    out.push({
      t: t,
      state: normalizeState_(r[colState]),
    });
  }

  out.sort((a, b) => a.t - b.t);
  return out;
}

// Строим интервалы ON/OFF и готовим строки для TIMELINE чарта так,
// чтобы ось была 0..24 (все дни “ложим” на одну базовую дату, а дни различаем rowLabel)
function buildSegments_(events, start, end) {
  // базовая дата для оси времени (важно: одна и та же для всех дней)
  const BASE = new Date(2000, 0, 1, 0, 0, 0, 0);

  function dayLabel_(d) {
    const wd = ['НД','ПН','ВТ','СР','ЧТ','ПТ','СБ'][d.getDay()];
    const dd = String(d.getDate()).padStart(2, '0');
    const mm = String(d.getMonth() + 1).padStart(2, '0');
    return `${wd} (${dd}.${mm})`;
  }

  function toBaseTime_(d, isEndMidnightNextDay) {
    const b = new Date(BASE);
    b.setHours(d.getHours(), d.getMinutes(), d.getSeconds(), 0);
    if (isEndMidnightNextDay) b.setDate(b.getDate() + 1); // 24:00 как 00:00 следующего дня
    return b;
  }

  // состояние на start: последнее событие до start
  let curState = 'UNK';
  for (let i = events.length - 1; i >= 0; i--) {
    if (events[i].t < start) { curState = events[i].state; break; }
  }
  if (curState === 'UNK') curState = 'OFF'; // можно поменять на ON, если тебе так логичнее

  // события внутри окна
  const inWindow = events.filter(e => e.t >= start && e.t < end);

  // raw segments (для статистики)
  const raw = [];
  let curT = new Date(start);

  for (const ev of inWindow) {
    if (ev.t > curT) raw.push({ state: curState, a: new Date(curT), b: new Date(ev.t) });
    curState = ev.state;
    curT = new Date(ev.t);
  }
  if (curT < end) raw.push({ state: curState, a: new Date(curT), b: new Date(end) });

  // split by days, then map to BASE time for chart
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

      const endIsMidnightNextDay =
        (partEnd.getHours() === 0 && partEnd.getMinutes() === 0 && partEnd.getSeconds() === 0) &&
        (partEnd.getTime() === dayEnd.getTime()) &&
        (partEnd.getTime() > a.getTime());

      rowsForChart.push([
        label,
        (seg.state === 'ON') ? CFG.LABEL_ON : CFG.LABEL_OFF,
        toBaseTime_(a, false),
        toBaseTime_(partEnd, endIsMidnightNextDay),
      ]);

      a = partEnd;
    }
  }

  // статистика для подписи
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

function buildChartPng_(ss, rows) {
  const sh = ss.getSheetByName(CFG.REPORT_SHEET) || ss.insertSheet(CFG.REPORT_SHEET);
  sh.clearContents();

  // Данные для Timeline chart: RowLabel | BarLabel | Start | End
  sh.getRange(1, 1, 1, 4).setValues([['day', 'state', 'start', 'end']]);
  if (rows.length) sh.getRange(2, 1, rows.length, 4).setValues(rows);

  const range = sh.getRange(1, 1, rows.length + 1, 4);

  // Встроенный chart через Spreadsheet service:
  // builder.setChartType(Charts.ChartType.*) :contentReference[oaicite:1]{index=1}
  const chart = sh.newChart()
    .setChartType(Charts.ChartType.TIMELINE)
    .addRange(range)
    .setNumHeaders(1)
    .setOption('timeline', { groupByRowLabel: true, showBarLabels: false })
    .setOption('hAxis', { format: 'H' }) // показываем только часы
    .setOption('colors', ['#4CAF50', '#F44336']) // ON / OFF
    .setOption('legend', { position: 'bottom' })
    .setPosition(1, 6, 0, 0)
    .build();

  // chart.getAs('image/png') :contentReference[oaicite:2]{index=2}
  return chart.getAs('image/png').setName('power_week.png');
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
4) В Apps Script: Project Settings → Script properties:

- TG_TOKEN = токен бота

- TG_CHAT_ID = chat_id/канал

- GS_SECRET = тот же секрет, что в ESP32 (gs_sec)

5) Deploy → New deployment → Web app. Укажи “Execute as me” (чтобы скрипт мог писать в таблицу) и доступ “Anyone…” по ситуации.

6) Скопируй URL вида https://script.google.com/macros/s/.../exec — это и будет вебхук.