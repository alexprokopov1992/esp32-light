'use strict';
// Run: TZ=Europe/Kyiv node tests/test_apps_script.js
const assert = require('node:assert/strict');
const fs = require('node:fs');
const vm = require('node:vm');
const path = require('node:path');
const source = fs.readFileSync(path.join(__dirname, '../Code.gs'), 'utf8');
const RealDate = Date;
class FixedDate extends RealDate {
  constructor(...args) { super(...(args.length ? args : ['2026-11-12T10:00:00Z'])); }
  static now() { return new RealDate('2026-11-12T10:00:00Z').getTime(); }
  static [Symbol.hasInstance](x) { return x instanceof RealDate; }
}

const properties = new Map([['GS_SECRET', 'top-secret'], ['TG_TOKEN', 'tg-token'], ['TG_CHAT_ID', 'chat-id']]);
let rows = [['ts', 'iso', 'device', 'state', 'duration']];
let captions = [];
let failTelegram = false;
let timezone = 'Europe/Kyiv';
let builtChartCalls = [];
const sheet = { getLastRow: () => rows.length, getDataRange: () => ({ getValues: () => rows }) };
const ctx = {
  console, Date: FixedDate, Math, Number, String, JSON, Map, isFinite,
  SpreadsheetApp: { getActiveSpreadsheet: () => ({getSheetByName: (name) => name === 'log' ? sheet : null })},
  PropertiesService: {getScriptProperties: () => ({
    getProperty: (key) => properties.get(key) ?? null,
    setProperty: (key, value) => {properties.set(key, value);}
  })},
  Session: {getScriptTimeZone: () => timezone},
  Utilities: {formatDate: (date, tz, fmt) => {
    assert.equal(tz, 'Europe/Kyiv'); assert.equal(fmt, 'yyyyMMdd');
    return ''+date.getFullYear()+String(date.getMonth()+1).padStart(2, '0')+String(date.getDate()).padStart(2, '0');
  }},
  LockService: {getScriptLock: () => ({ waitLock: () => {}, releaseLock: () => {} })},
  ContentService: {MimeType: {TEXT: 'text'},createTextOutput: (value) => ({value, setMimeType(){return this;}})},
  UrlFetchApp: {fetch: (url, config) => {
    assert.match(url,/\/sendPhoto$/);
    if (failTelegram) return {getResponseCode:()=>429,getContentText:()=>JSON.stringify({ok:false,description:'too many requests'})};
    captions.push(config.payload.caption);
    return {getResponseCode:()=>200,getContentText:()=>JSON.stringify({ok:true})};
  }}
};
vm.createContext(ctx);
vm.runInContext(source, ctx);
ctx.buildChartPng_ = (ss, chartRows, weekStart, weekEnd, dayEnd) => {
  builtChartCalls.push({weekStart, weekEnd, dayEnd, chartRows});
  return {mockPng: true};
};
const ev = (y,mo,d,h,m,state,durationSec) => [new Date(y,mo-1,d,h,m), '', 'pinger', state, durationSec];
function request(date) {return ctx.doGet({parameter:{action:'weekly_report',date,secret:'top-secret'}}).value;}
function expectThrow(fn, phrase) {assert.throws(fn, e=>e.message.includes(phrase));}

// Strict parsing (invalid calendar dates and unfinished today).
assert.equal(ctx.parseReportDay_('2026-11-11').getDate(),11);
expectThrow(()=>ctx.parseReportDay_('2026-02-30'),'Invalid calendar date');
expectThrow(()=>ctx.parseReportDay_('2026-11-12'),'not yet completed');
assert.equal(ctx.doGet({parameter:{action:'weekly_report',date:'2026-11-11',secret:'wrong'}}).value,'bad secret');
assert.equal(captions.length,0);

// Wednesday completed day, with the last known state from Sunday and a
// midnight-spanning ON interval: 18 h ON + 6 h OFF on Wednesday.
rows = [rows[0], ev(2026,11,8,12,0,'ON',0), ev(2026,11,11,18,0,'OFF',78*3600)];
assert.equal(request('2026-11-11'),'report sent 20261111');
assert.ok(captions[0].includes('За добу (11.11.2026):'));
assert.ok(captions[0].includes('Світло було: 18г 0хв\nСвітла не було: 6г 0хв'));
assert.equal(builtChartCalls[0].weekStart.toDateString(),'Mon Nov 09 2026');
assert.equal(builtChartCalls[0].dayEnd.toDateString(),'Thu Nov 12 2026');
assert.equal(builtChartCalls[0].weekEnd.toDateString(),'Mon Nov 16 2026');
assert.equal(request('2026-11-11'),'already sent 20261111');
assert.equal(captions.length,1, 'replay must not send a second Telegram picture');

// Sunday sends a true seven-day weekly report at the following Monday 00:02.
rows = [rows[0], ev(2026,11,1,12,0,'ON',0)];
assert.equal(request('2026-11-08'),'report sent 20261108');
assert.ok(captions.at(-1).includes('Підсумок за тиждень (02.11.2026–08.11.2026)'));
assert.equal(builtChartCalls.at(-1).dayEnd.getTime(), builtChartCalls.at(-1).weekEnd.getTime());
assert.ok(captions.at(-1).includes('Світло було: 7д 0г 0хв'));

// Failed Telegram call is never checkpointed; retry sends exactly once.
failTelegram = true;
expectThrow(()=>request('2026-11-10'),'Telegram sendPhoto failed');
assert.ok(!properties.get('POWER_REPORT_SENT_DAYS_V1').includes('20261110'));
failTelegram = false;
assert.equal(request('2026-11-10'),'report sent 20261110');
assert.equal(request('2026-11-10'),'already sent 20261110');

// Missing events remain unknown, not assumed to mean the power was on.
rows = [rows[0]];
assert.equal(request('2026-11-09'),'report sent 20261109');
assert.ok(captions.at(-1).includes('Невідомо: 24г 0хв'));

// Date boundary event at precisely midnight of the *next* day can validate
// how long the preceding ON state lasted.
rows = [rows[0], ev(2026,11,1,12,0,'ON',0), ev(2026,11,3,0,0,'OFF',36*3600)];
let start = new Date(2026,10,2); let end = new Date(2026,10,3);
let events = ctx.loadEvents_(ctx.SpreadsheetApp.getActiveSpreadsheet(),start,end);
let built = ctx.buildSegments_(events,start,end,start);
assert.ok(built.caption.includes('За добу (02.11.2026):'));
assert.ok(built.caption.includes('Світло було: 24г 0хв'));

// Actual elapsed day length when DST starts/ends in Kyiv.
for (const [month,date,wanted] of [[3,29,23],[10,25,25]]) {
  const day = new Date(2026,month-1,date);
  const next = new Date(day); next.setDate(next.getDate()+1);
  const week = ctx.getWeekStartMonday_(day);
  const prior = new Date(week);prior.setDate(prior.getDate()-1);
  const actual = ctx.buildSegments_([{t:prior,state:'ON',durSec:0}],week,next,day);
  const hourly = `${wanted}г`;
  assert.ok(actual.caption.includes(`Світло було: ${hourly} 0хв`),`${month}/${date}: ${actual.caption}`);
  assert.equal((next.getTime()-day.getTime())/3600000,wanted);
}

timezone = 'UTC';
expectThrow(()=>request('2026-11-07'),'Set the Apps Script project timezone');
console.log('PASS GAS: completed dates, weekly boundary, deduplication, retry, unknown, midnight, DST, timezone');
