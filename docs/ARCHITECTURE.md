# RCP2027 system architecture

## Operating modes

Offline and online are separate operating modes; neither silently falls back to
the other.

- **Offline:** the terminal loads active employees from `employees.csv` on the
  SD card and writes attendance and diagnostics only to that card.
- **Online:** the terminal loads employees through the authenticated Apps Script
  API and sends attendance and diagnostic rows to Google Sheets. A failed
  request is reported and the punch is not reported as successful or queued to
  local attendance CSV.
- Optional image files are saved to `/photos` on the SD card. Images are not
  uploaded by the current Apps Script API.

The desktop application edits CSV files in offline mode. In online mode it
reads the tables from Apps Script and requires confirmation before replacing
the employee or attendance sheet with its edited table.
In offline mode, attendance and log views load from an explicitly selected CSV
file on the SD card; manual attendance corrections are saved back to that file.
The desktop app initializes missing or empty CSV files with their headers when
a card folder is selected. The terminal likewise creates missing
`employees.csv`, `attendance.csv`, and `logs.csv` files. If `config.json` is
missing, the terminal creates a default offline configuration; this permits
first-time offline enrollment without prepopulated employee data.

## Shared files and CSV schemas

Files use UTF-8, a comma delimiter, a header row, and standard CSV double-quote
escaping. Empty files with just a header are valid. CSV values containing line
breaks are supported. IDs are strings; times are ISO 8601 with an explicit
UTC offset.

The terminal's font setting offers Unicode efont and the distinct FreeSans,
FreeSerif, and FreeMono GFX typefaces. The selected font is stored in NVS.
Unicode efont supports the interface's localized diacritics; the GFX typefaces
are limited to basic Latin/ASCII and may omit accented letters. Text sizing is
scaled per font family to keep the layout usable. The home-screen clock and
date use increased text sizes.

The idle home screen displays one of 50 built-in motivational phrases in each
supported language. A random phrase is selected once per RTC date, stored in
NVS, and kept stable across screen redraws and restarts; the previous day's
phrase is avoided when selecting a new one. The card-scan prompt remains
visible below the phrase.

### `config.json` (schema version 1)

The desktop application writes these keys:

```json
{
  "schema_version": 1,
  "mode": "offline",
  "language": "pl",
  "wifi_ssid": "",
  "wifi_password": "",
  "timezone_offset_minutes": 60,
  "apps_script_url": "",
  "api_token": "",
  "employees_sheet_url": "",
  "attendance_sheet_url": "",
  "logs_sheet_url": "",
  "camera_enabled": false
}
```

The Wi-Fi password and API token are plain text on removable media. Restrict
physical access to the card and regenerate credentials if it is lost. Do not
commit a device-specific configuration file to a public repository.

### `employees.csv`

Columns in exact order:

`employee_id,card_uid,first_name,last_name,active,relay_access`

`active` and `relay_access` accept `true`/`false` (case-insensitive) or `1`/`0`.
Employee IDs and card UIDs must be present and unique. Card UIDs are compared
case-insensitively. Relay-authorized cards get an on-screen choice between
entry, exit, and a time-limited door unlock.
After a successful entry or exit selection, the terminal displays the localized
greeting and employee name on a confirmation screen for three seconds before
returning to the home screen.

### `attendance.csv`

Columns in exact order:

`event_id,timestamp,employee_id,card_uid,event_type,photo_file,source,notes`

`event_type` is `ENTRY` or `EXIT`. The terminal chooses the next event from the
employee's most recent attendance event. A relay action is a system log, not a
work-time punch. Manual corrections retain the event ID and use the same
validated schema.

### `logs.csv`

Columns in exact order:

`event_id,timestamp,level,code,message`

## Online Google Sheets setup

Google Sheets CSV/export links are not a write API. The terminal uses the
included `google_apps_script/Code.gs` web app for authenticated reads and
writes. Install it in a Google Apps Script project, set script properties, and
deploy it as a web app that executes as its owner. The endpoint is public but
all operations require a high-entropy `RCP_API_TOKEN`; keep this token private.
Generate at least 32 random characters with the desktop **Generuj token** button.

Set these script properties to spreadsheet IDs parsed from the corresponding
Google Sheets URLs:

- `RCP_API_TOKEN`
- `EMPLOYEES_SHEET_ID`
- `ATTENDANCE_SHEET_ID`
- `LOGS_SHEET_ID`

The first tab in each sheet must have the exact CSV header above. The token
entered in the desktop application and saved to the device configuration must
match. Use **Testuj API** to check API authorization, script execution access,
sheet IDs, and headers. Keep the Apps Script deployment URL ending in `/exec`.

The firmware verifies TLS against the Google Trust Services Root R1 certificate.
Only use the supplied Apps Script HTTPS endpoint; arbitrary certificate hosts
are not configurable.

## Hardware

The firmware target is the custom PlatformIO board definition
`esp32-s3-n16r8.json` (16 MB Flash, 8 MB PSRAM). The display is a 3.5-inch
ILI9488, 320 × 480, with an XPT2046 touch controller. The display setup uses LovyanGFX on SPI3, landscape rotation, and GPIO3 as the
shared external peripheral reset. The SD card shares the display/touch SPI
lines and uses Arduino's `HSPI` selection for the ESP32-S3 SPI3 peripheral.
Display, touch, and SD access must remain sequential on this shared bus.
Touch coordinates are calibrated from four on-screen targets using raw
XPT2046 samples. The resulting affine transform is stored in ESP32 NVS and
applied directly in the landscape 480 × 320 UI coordinate space. The guided
calibration runs on first boot when no valid transform is stored and can be
started again from the PIN-protected settings screen. If a calibration target
is not touched within 30 seconds, boot continues using LovyanGFX's existing
touch transform. The home screen updates the clock text region only, not the
full screen, to avoid visible flashing during the once-per-second clock refresh.

The camera pin assignments below are read from the board/pinout image provided
for this project and match the OV2640 signal layout. Camera reset and power
down are not connected. GPIO3 is labelled JTAG_EN on the board and is not a
camera data signal; the terminal uses it as the shared RFID/display reset
because that is how the working hardware test is wired.

| Function | GPIO |
| --- | --- |
| RTC I2C SDA / SCL | 1 / 2 |
| Display/touch/SD SPI SCK, MOSI, MISO | 14 / 21 / 47 |
| ILI9488 CS / DC | 39 / 40 |
| XPT2046 CS | 41 |
| SD CS | 46 |
| MFRC522 SPI SCK, MOSI, MISO, CS, RST | 20 / 43 / 38 / 19 / 3 |
| Relay / buzzer | 45 / 42 |
| OV2640 XCLK, SIOD, SIOC | 15 / 4 / 5 |
| OV2640 VSYNC, HREF, PCLK | 6 / 7 / 13 |
| OV2640 Y2…Y9 | 11, 9, 8, 10, 12, 18, 17, 16 |

The screen reset is pulsed through GPIO3 as in the supplied test sketch. Confirm
the shared reset wiring on the actual board before operating the relay. The
relay is initialized open-drain and inactive and receives a fixed 3-second
unlock pulse.

## Device employee enrollment

When an unknown RFID card is read, the terminal offers enrollment. The operator
must confirm and enter the existing settings PIN before entering employee ID,
first name, last name, and relay permission on the touch keyboard. New employees
are active by default. Offline enrollment appends to `employees.csv`; online
enrollment calls the authenticated Apps Script `registerEmployee` action, which
checks duplicate employee IDs and card UIDs under a script lock before appending
to the employee sheet.

The PIN-protected settings menu also provides an employee list. Operators can
toggle each active employee's relay permission or remove the employee. Removal
sets `active` to false, preserving attendance history; offline updates replace
`employees.csv` through a temporary file and backup, while online updates use
the authenticated `updateEmployee` Apps Script action. Adding from this list
starts a card-scan prompt before PIN-protected enrollment. After updating
`Code.gs`, redeploy the Apps Script web app so terminals can use the new action.

Photo mode can be enabled from settings and is persisted both in NVS and
`config.json` when the SD card is available. Camera initialization failures are
reported and logged; photo mode remains selected so it can be retried on the
next boot. Every firmware log event is written to `/logs.csv` on the SD card
regardless of operating mode. In online mode it is also sent to the configured
Google logs sheet; failure to upload does not discard the SD copy. Camera
initialization error codes are included in the local log message.

## Current implementation boundary

The firmware implements boot/display, touch menu, protected settings, RTC time
adjustment, SD configuration and offline CSV attendance, RFID lookup and
enrollment, relay access, optional OV2640 photo capture, and the Apps Script
online request contract. Online photos, DST rule databases,
camera/display hardware verification, and end-to-end Google deployment testing
require the physical device and project-owned Google resources.
