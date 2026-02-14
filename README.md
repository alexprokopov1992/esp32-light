1) Google Sheets + Apps Script (вебхук)

2) Создай Google Sheet (например лист log).

3) Extensions → Apps Script, вставь код:
```
const SHEET_NAME = "log";
const SECRET = "my_shared_secret";
function doPost(e) {
  try {
    const data = JSON.parse((e.postData && e.postData.contents) || "{}");

    if (SECRET && data.secret !== SECRET) {
      return ContentService.createTextOutput("forbidden").setMimeType(ContentService.MimeType.TEXT);
    }

    const ss = SpreadsheetApp.getActiveSpreadsheet();
    const sh = ss.getSheetByName(SHEET_NAME) || ss.insertSheet(SHEET_NAME);

    const ts = data.ts ? new Date(Number(data.ts) * 1000) : new Date();

    sh.appendRow([
      ts,                 // время события (Date)
      data.iso || "",     // строкой (опционально)
      data.state || "",   // "ON"/"OFF"
      data.device || "",  // имя девайса
      data.duration || "" // длительность (опционально)
    ]);

    return ContentService
      .createTextOutput(JSON.stringify({ ok: true }))
      .setMimeType(ContentService.MimeType.JSON);

  } catch (err) {
    return ContentService
      .createTextOutput(JSON.stringify({ ok: false, error: String(err) }))
      .setMimeType(ContentService.MimeType.JSON);
  }
}
```
4) Deploy → New deployment → Web app. Укажи “Execute as me” (чтобы скрипт мог писать в таблицу) и доступ “Anyone…” по ситуации.

5) Скопируй URL вида https://script.google.com/macros/s/.../exec — это и будет вебхук.