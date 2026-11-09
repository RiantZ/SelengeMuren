# Память ассистента

Копия заметок, которые ассистент (Claude Code) хранит между сессиями по этой теме, на 2026-11-09.
Оригиналы лежат вне репозитория, в каталоге памяти проекта; язык и формат сохранены, ссылки [[...]] —
имена других заметок.

## viewer-ui-architecture

Product name: SelengeMuren (the GitHub repo is RiantZ/SelengeMuren); p8 is the older/internal name.
Chat "Архитектура сервера" (2026-11-09) opened the UI client design.

Requirements (user, 2026-11-09): the client connects to the Go server over the network; it lists and deletes
streams, changes server settings, and shows logs, traces and metrics live at 30-120 fps (cardio-monitor
scrolling, not Datadog-style "pick a range, then render"). Also: an own filter DSL, search, views synced in time
across logs/traces/metrics of one or many apps, hotkeys, MCP later. Files go to the server for offline
reading, so the client never parses p8 data itself. Key question: browser or a cross-platform app such as Qt.

My analysis (2026-11-09, not accepted yet): the browser is enough if views render on the GPU only (WebGL2 as
the base) and the server computes the view for the current zoom: M4 (min/max/first/last per pixel column) for
metrics, visible lines for logs, sub-pixel spans merged. The view is recomputed from raw data on every zoom,
so [[metrics-raw-no-aggregation]] still holds. The live right edge is the merged frontier F from
doc/live_stream_ordering.md §5. I leaned to A (TS + WebGL2 served by the Go server, WebSocket; Electron later if
needed) over Qt 6 and one-codebase C++/ImGui (Tracy) or Rust/egui+wgpu (Rerun).

Browser facts verified by web search on 2026-11-09:
- Safari caps page rendering at 60 Hz on ProMotion by default ("Prefer Page Rendering Updates near 60fps");
  per WebKit bugs the flag has no effect in WKWebView (Tauri on Mac).
- Ctrl/Cmd+W/T/N belong to the browser outside fullscreen; Keyboard Lock works only in fullscreen.
- WebGPU and SharedArrayBuffer need a secure context; SAB also needs cross-origin isolation (MDN).
- Memory64: Chrome 133+, Firefox 134+, not Safari. WebTransport: Baseline since Safari 26.4 (March 2026).
  WebGPU is not on by default on Linux everywhere.
- Precedents: the Perfetto UI added a native trace_processor accelerator, and Rerun added on-demand streaming to
  go past the 4 GiB Wasm limit. Both started with all data inside the viewer. Tracy has a WASM web demo.

Deployment (user, 2026-11-09): both, on a developer's machine and centrally for a team. Implications I gave:
one binary with two profiles (local: 127.0.0.1, no auth, open files by path; team: TLS, auth/roles for delete
and settings, file upload); the server serves the UI, so UI and server versions never diverge (Tracy needs
equal viewer and client versions, README.md:153); view tiles on a fixed time grid are shared by all viewers;
ingest must never wait for queries, because a stalled sink makes log writers block (p8_log.cpp:436); on one
host all apps use one system clock (kit/src/time/*), across hosts alignment depends on NTP/PTP.

Scale (user, 2026-11-09): one server instance first, a cluster later; the user's reason: on lau/sorting-testing one
goroutine merges about as fast as one client generates, so many clients overload one instance. My refinement from
the branch's tests/sorting/doc/results.md: that parity is against the null sink; 10 GbE carries ~22 M logs/s or
~52 M metrics/s = 0.1-0.2 of a core of merge (Y 156-236 M logs/s, 263-432 M metrics/s), 100 GbE = 1-2 cores;
merge only, storage and index costs are not measured yet. The real wall is disk volume: 10 GbE nonstop = 4.5 TB/h.
Proposed: the stream is the unit of work and of sharding; cross-stream work only in views and queries
(scatter-gather of screen-sized data in a cluster); the §5 frontier needs no node clock sync.

AGREED (user, 2026-11-09): web client. The viewer never takes all data: it sends a request (time, scale, duration)
and gets a bounded window, usually tens or hundreds of KB. Filtering, aggregation for display and all other
processing are the server's job; the client only controls and displays. The retention question went unanswered.

Facts for the user's follow-up questions (verified 2026-11-09):
- Tabs: BroadcastChannel in Safari 15.4+, SharedWorker in Safari 16+; rAF is paused in background tabs (MDN);
  performance.timeOrigin syncs a window and a worker (MDN), cross-tab is not documented.
- Hotkeys: Chromium reserves close/new tab and window, incognito, restore tab, next/previous tab and exit; its source
  says "In Apps mode, no keys are reserved" (installed PWA), and in fullscreen everything except fullscreen and exit
  goes to the page. Firefox copied Chrome's list (bug 1074971). Safari has no docs; community sources say
  Cmd+N/W/T/Q, Ctrl+Tab and Cmd+digits are browser-owned.
- Protocols: WebSocketStream is Chromium-only; classic WebSocket has no receive backpressure; browsers speak
  HTTP/2 only over TLS.

WebGL2 POC (2026-11-09, at the user's request, modelled on a screenshot of their old Telemetry viewer: stacked
counter panels, time axis on top, timestamps below, markers on raw points; drag with the left button pans, the wheel
zooms). Saved at the user's request on branch lau/ui-experiment (from main c78e644) in tests/ui: poc/ (index.html,
app.js, signals.js, server_sim.js), scripts/ (serve.py on 127.0.0.1:8765, smoke_check.sh via playwright-cli),
doc/ (README run guide, conclusions, memory copy, screenshots). The "server" is server_sim.js in a Web Worker:
it sends raw points, or M4 when points are denser than 2 per pixel; one request in flight, latest wins.
Measured in headless Chrome 156 (60 Hz cap, DPR 1, M4 Max): 9 counters JS 0.1 ms and GPU 0.3-0.6 ms per frame,
0 janks; the whole hour in M4 is 115-117k segments, GPU 1.0 ms; 72 counters at 500 kHz is 1.29M segments,
GPU 3-5 ms (two runs), and each server reply is 10.3 MB, because it grows with counters x pixels.
Safari and Firefox were not run.

DISPLAY MODEL (user, 2026-11-09, corrected me): the time scale is discrete, every X px (e.g. 16) is one time unit.
The server takes all raw samples of every visible counter in each cell and computes one value per cell (average or
p99, depending on the display type), then sends only those points. Example: 10 ms window, X = 16, 160 px -> 10 points
(0..9 ms). So a reply is always KB to hundreds of KB (counters x cells x ~4 B; time is the cell index).
My POC's 10.3 MB came from my own choice (M4 per pixel column, 1-2 columns per pixel, +-50% margins), not from the
architecture; tests/ui/doc/conclusions.md, README.md and memory.md on lau/ui-experiment were corrected on
2026-11-09 (the POC code itself still uses M4, by the user's choice). Server consequences I raised: long windows
need block summaries next to the raw data; avg/min/max/count merge exactly, p99 does not (raw pass or a mergeable
sketch such as DDSketch / t-digest); avg hides short spikes, hence max/p99 views.

**Why:** the user picks the client stack from this analysis. Repeating these searches every session wastes time.
**How to apply:** retention is still open, so don't push it. The user declined a hotkey tester page
(2026-11-09), don't offer it again. Don't push a choice ([[p8-user-working-style]]). Re-check the browser
facts if months have passed.

## metrics-raw-no-aggregation

Metrics will not be aggregated into intervals (user, 2026-10-31). The user can change the window size
at any moment (zoom) and inspect metrics at the smallest time scale, down to micro- and nanoseconds.

**Why:** the user's stated use case is arbitrary zoom over raw samples; fixed bucketing would lose it.
**How to apply:** never propose interval aggregation or "close the bucket at the watermark" as a way to
skip ordering metrics; metrics need the same full time-ordered merge as logs, and storage/index designs
must keep every raw sample addressable by time. Related: [[reader-ordering-brainstorm]]

## Связанные заметки (не копировались)

- reader-ordering-brainstorm — порядок записей на сервере и замеры слияния;
- p8-high-performance-lens — оценивать решения по стоимости на горячем пути продюсера;
- p8-user-working-style — как пользователь хочет вести работу.
