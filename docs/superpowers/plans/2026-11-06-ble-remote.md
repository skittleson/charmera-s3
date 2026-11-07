# Bluetooth Remote Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** A BLE GATT peripheral on the camera that reports live status and accepts look/mode/shoot/time-lapse commands, plus a Web Bluetooth page in `docs/` for GitHub Pages.

**Architecture:** A new `remote` component owns NimBLE: it serves Status (read/notify JSON), Info (read JSON) and Command (write text), queues parsed commands, and notifies Status when the JSON changes. All protocol logic (command parsing, the "allowed while running" rule, JSON building) is pure C in `remote_proto.c` and host-tested. The UI task stays the only owner of the camera, card and screen: each loop it pops one command, runs it through new camera-screen hooks, and publishes a fresh status snapshot.

**Tech Stack:** ESP-IDF v5.5, NimBLE (peripheral only), LVGL 9, Unity host tests, plain HTML/JS with Web Bluetooth.

**Spec:** `docs/superpowers/specs/2026-11-06-ble-remote-design.md`

## Global Constraints

- **Stack:** NimBLE, peripheral role only, 1 connection, memory from PSRAM (`CONFIG_BT_NIMBLE_MEM_ALLOC_MODE_EXTERNAL=y`), preferred MTU 517.
- **Name:** advertise as `Charmera-S3` with the custom 128-bit service UUID.
- **UUIDs** (fixed in firmware and page):
  - service `c4a30000-7b6e-4f0a-9d2e-5a1c8e3f6b20`
  - Status `c4a30001-…`
  - Command `c4a30002-…`
  - Info `c4a30003-…`

  All share the service's tail `-7b6e-4f0a-9d2e-5a1c8e3f6b20`.
- **Standard Battery Service (0x180F)** is served as well.
- **Commands:** UTF-8, one per write: `look <n>`, `mode single`, `mode lapse [n]`, `interval <n>`, `shoot`, `lapse start`, `lapse stop`.
- **Refused during a run:** while a time-lapse is running, `look`, `mode`, `interval` and `shoot` give an error and the run keeps going.
- **Shutter and Start:** `shoot` takes one photo into `100CHARM` in any mode. `lapse start` switches to time-lapse mode if needed, then starts. `lapse stop` stops at once.
- **Persistence:** the mode isn't persisted. Look and interval persist as today. Bluetooth On/Off persists (key `ble`), default On; Off fully deinitialises NimBLE.
- **Remote commands:** they switch to the camera screen if needed and count as activity, so they wake the screen.
- **Errors:** the status carries `"error":"<reason>"` until the next successful command.
- **Page:** `docs/index.html`, `docs/app.js`, `docs/style.css`, `docs/.nojekyll`. No build step, no dependencies. Shows a plain message when `navigator.bluetooth` is missing.

## Review Focus

- **Messy command text.** nRF Connect "Text" writes with a trailing `\n`/`\r\n`, extra spaces, or upper case (`LOOK 3`) should work like `look 3`. (Task 1 test `test_parse_tolerates_whitespace_and_case`.)
- **Bad numbers.** Out-of-range or non-numeric arguments (`look 12`, `look -1`, `look x`, `interval 5`) must be rejected with an error and change nothing. (Task 1 test `test_parse_rejects_bad_numbers`.)
- **Commands during a run.** A running time-lapse plus any changing command must be refused, with the run untouched. (Task 1 test `test_check_refuses_changes_while_running`. Task 3 board check.)
- **JSON too big.** A status JSON that doesn't fit its buffer must be refused, never truncated into invalid JSON. (Task 1 test `test_status_json_too_small_buffer_returns_0`.)
- **Dropped connection.** A disconnect mid-session should restart advertising, and the page should offer to reconnect. (Task 2 and Task 4 board checks.)

---

### Task 1: `remote_proto` — command parser, run rule, JSON (host-tested)

**Files:**
- Create: `components/remote/include/remote_proto.h`, `components/remote/src/remote_proto.c`
- Test: `tests/host/test_remote_proto.c`
- Modify: `tests/host/CMakeLists.txt` (add `add_host_test(remote_proto ${REPO}/components/remote/src/remote_proto.c)` and the `components/remote/include` include dir)

**Interfaces:**
- Produces:
  ```c
  typedef enum { RC_LOOK, RC_MODE_SINGLE, RC_MODE_LAPSE, RC_INTERVAL, RC_SHOOT, RC_LAPSE_START, RC_LAPSE_STOP } remote_cmd_kind_t;
  typedef struct { remote_cmd_kind_t kind; int arg; /* -1 = none */ } remote_cmd_t;
  /* NULL on success, else a short static reason ("unknown command", "bad look", "bad interval"). */
  const char *remote_parse(const char *text, size_t len, int look_count, int interval_count, remote_cmd_t *out);
  /* NULL if allowed, else "time-lapse running" / "not running". */
  const char *remote_check(const remote_cmd_t *c, bool lapse_running);
  typedef enum { REMOTE_CARD_OK, REMOTE_CARD_NONE, REMOTE_CARD_FULL } remote_card_t;
  typedef struct {
      uint32_t photos, left; int battery_pct; bool usb;
      const char *camera;                   /* "OV5640" / "OV2640" / "none" */
      int look; const char *look_name;
      bool lapse_mode; int interval; const char *interval_name;
      bool lapse_running; uint32_t frames, elapsed_s; char folder[9];
      remote_card_t card; bool busy;
      char last[24]; char error[32];        /* "" = none */
  } remote_status_t;
  /* Compact JSON; returns its length, or 0 if it doesn't fit (never truncated). */
  size_t remote_status_json(const remote_status_t *s, char *out, size_t len);
  size_t remote_info_json(const char *version, const char *const *looks, int nlooks,
                          const char *const *intervals, int nintervals, char *out, size_t len);
  ```

- [ ] **Step 1: Write the failing tests** in `tests/host/test_remote_proto.c`:
  - `test_parse_each_command`: `"look 3"` → `{RC_LOOK,3}`; `"mode single"` → `RC_MODE_SINGLE`; `"mode lapse"` → `{RC_MODE_LAPSE,-1}`; `"mode lapse 2"` → `{RC_MODE_LAPSE,2}`; `"interval 4"` → `{RC_INTERVAL,4}`; `"shoot"`, `"lapse start"`, `"lapse stop"`. All with `look_count=12, interval_count=5`, and each returns NULL.
  - `test_parse_tolerates_whitespace_and_case`: `"  LOOK 3\r\n"` → `{RC_LOOK,3}`; `"Shoot\n"` → `RC_SHOOT`. The `len` argument is honoured, so text without a NUL terminator works.
  - `test_parse_rejects_bad_numbers`: `"look 12"`, `"look -1"`, `"look x"`, `"look"` → `"bad look"`; `"interval 5"`, `"mode lapse 9"` → `"bad interval"`; `"dance"`, `""` → `"unknown command"`.
  - `test_check_refuses_changes_while_running`: with `lapse_running=true`, `RC_LOOK`, `RC_MODE_SINGLE`, `RC_MODE_LAPSE`, `RC_INTERVAL`, `RC_SHOOT` and `RC_LAPSE_START` → `"time-lapse running"`, and `RC_LAPSE_STOP` → NULL. With `false`: `RC_LAPSE_STOP` → `"not running"`, everything else NULL.
  - `test_status_json_fields`: fill every field; the output contains `"photos":91`, `"left":120`, `"battery":82`, `"usb":false`, `"camera":"OV5640"`, `"look":3`, `"lookName":"Black & White"`, `"mode":"lapse"`, `"interval":1`, `"intervalName":"10 s"`, `"running":true`, `"frames":12`, `"elapsed":75`, `"folder":"101LAPSE"`, `"card":"ok"`, `"busy":false`, `"last":"IMG_0093"`, `"error":""`. It starts with `{`, ends with `}`, and is under 400 bytes.
  - `test_status_json_escapes_quotes`: `look_name = "a\"b"` appears as `a\"b`.
  - `test_status_json_too_small_buffer_returns_0`: a 40-byte buffer → 0.
  - `test_info_json`: the output contains `"version":"0.1.0"`, `"looks":["Normal","Cool Tone"]` and `"intervals":["5 s","10 s"]`.

- [ ] **Step 2: Run, expect failures**

  Run: `cmake -S tests/host -B tests/host/build && cmake --build tests/host/build && ./tests/host/build/test_remote_proto`

  Expected: the build fails (missing source), or all tests fail against an empty stub.

- [ ] **Step 3: Implement `remote_proto.c`.** Trim and lower-case into a local buffer of up to 32 bytes; parse numbers with `strtol`, rejecting trailing junk. JSON goes through `snprintf` into the buffer, with a small helper that escapes `"` and `\`; return 0 when `snprintf` reports truncation.

- [ ] **Step 4: Run all host tests.** `ctest --test-dir tests/host/build` → `100% tests passed`.

- [ ] **Step 5: Commit** `feat(remote): command parser, run rule and status JSON (host-tested)`.

### Task 2: `remote` component — NimBLE GATT server, Bluetooth setting

**Files:**
- Create: `components/remote/include/remote.h`, `components/remote/remote.c`, `components/remote/CMakeLists.txt` (`SRCS "remote.c" "src/remote_proto.c"`, `INCLUDE_DIRS "include"`, `REQUIRES bt freertos esp_timer`)
- Modify: `sdkconfig.defaults` (add `CONFIG_BT_ENABLED=y`, `CONFIG_BT_NIMBLE_ENABLED=y`, `CONFIG_BT_NIMBLE_ROLE_CENTRAL=n`, `CONFIG_BT_NIMBLE_ROLE_OBSERVER=n`, `CONFIG_BT_NIMBLE_MAX_CONNECTIONS=1`, `CONFIG_BT_NIMBLE_MEM_ALLOC_MODE_EXTERNAL=y`, `CONFIG_BT_NIMBLE_ATT_PREFERRED_MTU=517`), `main/settings.{h,c}` (`bool settings_bluetooth(void)`, default true, key `"ble"`; `void settings_set_bluetooth(bool)`), `main/main.c`, `main/CMakeLists.txt` (`REQUIRES … remote`), `main/ui/screen_settings.c` (new row `ROW_BLUETOOTH` before Format, shows "Bluetooth  On/Off"; hold toggles, saves, and calls `remote_start`/`remote_stop`)

**Interfaces:**
- Consumes: Task 1's `remote_cmd_t`, `remote_parse`.
- Produces:
  ```c
  void remote_set_counts(int looks, int intervals); /* bounds for remote_parse; call before remote_start */
  esp_err_t remote_start(const char *info_json);  /* init NimBLE, register services, advertise; copies info_json */
  void remote_stop(void);                         /* stop + nimble_port_deinit; frees the stack */
  bool remote_is_running(void);
  bool remote_is_connected(void);
  bool remote_take_command(remote_cmd_t *out);    /* non-blocking pop; queue depth 4 */
  void remote_report_error(const char *reason);   /* parse errors from the write callback are stored here and read by the bridge */
  const char *remote_take_error(void);            /* "" if none; clears it */
  void remote_publish(const char *status_json, int battery_pct); /* store; notify if connected and changed; set BAS level */
  ```

- [ ] **Step 1: Write the component.**
  - Copy the `bleprph` pattern (`nimble_port_init`, `ble_svc_gap_init`, `ble_svc_gatt_init`, `ble_svc_bas_init`, `ble_gatts_count_cfg`/`ble_gatts_add_svcs`, sync callback → `ble_hs_util_ensure_addr` + advertise, `nimble_port_freertos_init` host task).
  - The service table has Status (READ|NOTIFY), Command (WRITE|WRITE_NO_RSP) and Info (READ), each with a 0x2901 user-description descriptor ("Status JSON", "Command", "Info JSON").
  - The Command write callback flattens the mbuf (≤64 bytes), runs `remote_parse` with look and interval counts passed into `remote_start` through the info (store the counts in statics set by a `remote_set_counts(int looks, int intervals)` helper, called before `remote_start`), then either queues the command or calls `remote_report_error`.
  - Reads of Status return the last published JSON under a mutex. Notifications use `ble_gatts_notify_custom` with `ble_hs_mbuf_from_flat`.
  - Restart advertising on `BLE_GAP_EVENT_DISCONNECT` and on a failed connect.
- [ ] **Step 2: Wire into boot.**
  - In `main.c`, after `ui_start()`: build the info JSON with `remote_info_json` (looks from `look_name(i)`, intervals from `lapse_interval_name(i)`, version `APP_VERSION`), call `remote_set_counts(LOOK_COUNT, LAPSE_INTERVAL_COUNT)`, and if `settings_bluetooth()`, call `remote_start(info)`.
  - Log `heap_caps_get_free_size(MALLOC_CAP_INTERNAL)` and `heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL)` before and after.
  - Keep the info JSON in a static buffer so Settings can restart Bluetooth.
- [ ] **Step 3: Build from fresh config.** Run `rm sdkconfig && idf.py build`. Expected: build OK, no warnings from `main/` or `components/`. Then confirm `grep CONFIG_BT_NIMBLE_ENABLED=y sdkconfig`.
- [ ] **Step 4: Board check (owner, nRF Connect).**
  - Flash, then in nRF Connect scan, find `Charmera-S3`, connect, and read Info. It should show the JSON with 12 looks and 5 intervals.
  - Write `look 3` as text. The serial log should show the command being queued (temporary `ESP_LOGI` in `remote.c`).
  - Write `look 99`. A read of Status (after Task 3) or the log should show `bad look`.
  - Disconnect, and check the device is advertising again.
  - Record internal free heap before and after from the boot log.
- [ ] **Step 5: Commit** `feat(remote): NimBLE GATT service with status, command and info; Bluetooth setting`.

### Task 3: bridge — run commands and publish status from the UI task

**Files:**
- Modify: `main/ui/ui.h`, `main/ui/ui.c`, `main/ui/screen_camera.c`
- Create: `main/remote_bridge.c`, `main/remote_bridge.h`; add to `main/CMakeLists.txt` SRCS

**Interfaces:**
- Consumes: Task 1 types; Task 2 `remote_take_command`, `remote_take_error`, `remote_publish`, `remote_is_running`.
- Produces:
  ```c
  /* screen_camera.c */
  /* Run a remote command on the camera screen; returns NULL or an error reason. Caller is the UI task. */
  const char *ui_camera_remote(const remote_cmd_t *c);
  /* Camera-screen fields of the status: look, mode, interval, lapse.*, busy, last. */
  void ui_camera_fill_status(remote_status_t *st);
  /* remote_bridge.c (UI task only) */
  void remote_bridge_poll(void);    /* call every UI loop: pop ≤1 command, run it, publish */
  void remote_bridge_publish(void); /* build status + remote_publish; cheap (cached card/battery) */
  ```

- [ ] **Step 1: Camera-screen hooks.**
  - `ui_camera_remote` maps each command to existing code, after running `remote_check(c, s.lapse.mode == LAPSE_RUNNING)`:
    - `RC_LOOK` → `on_look`
    - `RC_MODE_SINGLE` → mode OFF + repaint
    - `RC_MODE_LAPSE` → `on_interval(arg >= 0 ? arg : s.lapse.interval)`
    - `RC_INTERVAL` → save it, and if armed, repaint
    - `RC_SHOOT` → `shoot()`, which takes a normal photo even when armed
    - `RC_LAPSE_START` → arm if needed, then `lapse_start()`
    - `RC_LAPSE_STOP` → `lapse_stop(NULL)`, with no confirm
  - Record the last saved name in `s.last` from `shoot` and `lapse_frame` (`IMG_%04d` or `%s/IMG_%04d`).
  - Call `remote_bridge_publish()` right after setting `s.busy = true`, and after clearing it, so `busy` is seen before the long save.
  - `leave()` also sets `s.lapse.mode = LAPSE_OFF`.
- [ ] **Step 2: Bridge.**
  - `remote_bridge_poll`:
    - If `!remote_is_running()`, return.
    - On a queued command: if the current screen isn't the camera, `switch_to(UI_SCREEN_CAMERA)` (expose a `ui_show_now(id)` in `ui.c` that switches synchronously). Then `ui_note_activity()`, then `err = ui_camera_remote(&c)`. Keep `err` (or clear it on success) in a static for the status.
    - Pull `remote_take_error()` too.
    - Publish at most every 250 ms, or at once after a command.
  - Cache storage info and battery for 5 s inside the bridge, so it never touches SPI or the ADC more often.
  - Fill `camera` from `camera_sensor_name()` (or `"none"`), `card` from `storage_status()` and the remaining count, `battery_pct` with the settings screen's 3.3–4.2 V formula (move `battery_pct` into `board`), and `usb` as `v >= 4.5`.
- [ ] **Step 3: Call `remote_bridge_poll()`** from the `ui_task` loop in `ui.c`, after input handling.
- [ ] **Step 4: Build and host tests.** Run `idf.py build` and `ctest`. Both pass.
- [ ] **Step 5: Board check (owner, nRF Connect).**
  - Enable notify on Status and write each command in turn:
    - `look 5` changes the screen label and `lookName`
    - `mode lapse 2` makes the badge show "every 30 s"
    - `shoot` takes a photo, with `busy` true then false and `last` updated
    - `lapse start` makes `running` true and the frames count up
    - `look 1` during the run gives `"error":"time-lapse running"` and the run continues
    - `lapse stop` stops it
  - From the gallery screen, `shoot` returns to the camera and shoots.
  - From a sleeping screen, `shoot` wakes it.
- [ ] **Step 6: Commit** `feat(remote): run Bluetooth commands and publish live status`.

### Task 4: Web Bluetooth page

**Files:**
- Create: `docs/index.html`, `docs/app.js`, `docs/style.css`, `docs/.nojekyll`

**Interfaces:**
- Consumes: the UUIDs, the JSON field names from Task 1, and the command strings.

- [ ] **Step 1: Build the page.**
  - **Connect:** `requestDevice({filters:[{services:[SERVICE]}], optionalServices:['battery_service']})`. On connect, read Info and fill the look and interval `<select>`s, then `startNotifications` on Status and also read it once.
  - **Status:** parse the JSON from each notification. If it fails to parse, re-read the characteristic.
  - **Cards:** photos, left, battery (with a USB icon), camera, look, mode, and lapse (frames, elapsed, folder), plus an error line.
  - **Controls:**
    - the look select sends `look n`
    - a mode toggle sends `mode single` or `mode lapse n`
    - the interval select sends `interval n`
    - a big Shutter button sends `shoot`
    - a Start button sends `lapse start`
    - a Stop button sends `lapse stop` after `confirm()`
  - **Disabled controls:** grey out the look, mode, interval and Shutter controls while `running` or `busy`, and grey out everything while disconnected.
  - **Reconnect:** on `gattserverdisconnected`, show "Disconnected" with a Reconnect button.
  - **Unsupported browser:** when `!navigator.bluetooth`, show "Use Chrome or Edge on desktop or Android. iPhone browsers don't support Web Bluetooth."
  - **Layout:** a phone-width single column, with system fonts and dark colours matching the camera UI (`#101010` background, `#ffc400` accent).
- [ ] **Step 2: Local check (owner).** Run `python3 -m http.server -d docs 8000` and open `http://localhost:8000` in Chrome; localhost counts as a secure context.
  - Connect to the camera and check every card and control against the board.
  - Turn the camera's Bluetooth off in Settings: the page should show Disconnected. Turn it back on, then Reconnect.
- [ ] **Step 3: Commit** `feat(web): Web Bluetooth remote page for GitHub Pages`.

### Task 5: Release gate, docs, publish

**Files:**
- Modify: `README.md`, `docs/build-decisions.md`, `docs/acceptance-2026-11-06.md`

- [ ] **Step 1: Memory gate (board).**
  - With the page connected, take photos on the OV5640: a Normal photo (sensor JPEG), a Film Frame (re-encoded) and a 10 s time-lapse of 3 frames.
  - The serial log must show no allocation or `cam_hal` buffer failures.
  - Record the internal heap numbers.
- [ ] **Step 2: Docs.**
  - **README:** add a "📶 Bluetooth remote" section with the page link `https://skittleson.github.io/charmera-s3/`, nRF Connect usage (UUIDs and the command list), and the browser support note. Add the Bluetooth setting to Features, and a Bluetooth line to the status table.
  - **Build decisions:** record the designer calls.
- [ ] **Step 3: Host tests and build.** Both pass, then commit.
- [ ] **Step 4: Publish.** Merge to `main` and push as `skittleson` (switch `gh` back afterwards). Offer to enable Pages with `gh api repos/skittleson/charmera-s3/pages -X POST -f source[branch]=main -f source[path]=/docs`, and ask before running it.
