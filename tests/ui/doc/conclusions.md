# UI‑эксперимент: выводы

Итоги обсуждения «Архитектура сервера» от 2026‑11‑09 и замеры WebGL2‑прототипа из `tests/ui/poc`.
Как запускать — в [README.md](README.md).

## Принятые решения

- **Клиент — web.** UI раздаёт сам Go‑сервер, поэтому версии клиента и сервера не расходятся. Для сравнения:
  вьюер Tracy должен быть ровно той же версии, что и клиент (корневой `README.md`).
- **Клиент не берёт все данные.** Он отправляет запрос (время, масштаб, длительность) и получает ограниченное
  окно, обычно десятки–сотни КБ.
- **Обработка — на сервере:** фильтрация, агрегация для показа, поиск, DSL. Клиент только управляет и
  отображает. MCP работает через тот же серверный движок.
- **Вид для показа.** Пока сырых точек не больше двух на пиксель, сервер шлёт их как есть. Дальше он шлёт M4:
  первую, минимальную, максимальную и последнюю точку каждой колонки пикселя. Линия получается та же, что из
  сырых данных (Jugel et al., PVLDB 2014). Хранение при этом не агрегируется: вид пересчитывается из сырых
  данных при каждом зуме, поэтому зум до наносекунд работает.
- **Развёртывание — один бинарник, два профиля.** У разработчика сервер слушает 127.0.0.1 без авторизации и
  открывает файлы по пути. Командный сервер работает по TLS, удалять потоки и менять настройки можно только с
  правами, файлы приходят загрузкой.
- **Сначала один сервер, кластер потом.** Поток — единица работы и шардирования: приём, слияние, хранение и
  индекс у каждого потока свои. Общая работа нескольких потоков — только виды и запросы. В кластере это сбор с
  узлов данных размером с экран, распределённой сортировки нет.
- **Приём важнее зрителей.** Если запрос затормозит приём, обратное давление дойдёт до приложений: писатель
  логов ждёт свободный буфер, а не выбрасывает запись (`engine/p8_log.cpp:436`). Приём и запросы — раздельные
  ресурсы, у запросов есть лимиты.

## Что первым упрётся у одного сервера

По замерам слияния на `lau/sorting-testing` (`tests/sorting/doc/results.md`) одна горутина сливает 156–236 М
логов/с и 263–432 М метрик/с. 10 GbE несёт ~22 М логов/с или ~52 М метрик/с, то есть слияние занимает
0,1–0,2 ядра на каждые 10 GbE приёма и 1–2 ядра на 100 GbE. Хранение и индекс ещё не мерили.

Раньше процессора кончится место на диске: 10 GbE без перерыва — 4,5 ТБ сырых данных в час. Вопрос лимитов
хранения (автоматически или только ручным удалением потоков) открыт.

## Ограничения браузеров (проверено в ноябре 2026)

| Ограничение | Кого касается | Обход |
|---|---|---|
| Safari по умолчанию рисует страницы на 60 Гц на ProMotion‑экранах | Safari и WKWebView (Tauri на Mac) | Chrome/Edge, Electron или снять флаг |
| `Ctrl/Cmd+W/T/N`, их `Shift`‑варианты, `Ctrl+Tab`, `Cmd+Q` забирает браузер | обычная вкладка | установить UI как приложение (PWA) в Chrome/Edge или полноэкранный режим |
| WebGPU и SharedArrayBuffer — только в secure context (HTTPS или localhost) | UI по `http://<IP в LAN>` | HTTPS; без него работают WebGL2 и WebSocket |
| wasm32 ограничен 4 ГБ, Memory64 в Safari нет | только если держать данные в клиенте | данные на сервере |
| WebGPU включён по умолчанию не везде на Linux | — | базовый уровень — WebGL2 |

Прецеденты «данные на сервере»: Perfetto UI добавил нативный `trace_processor`, когда упёрся в память вкладки,
а Rerun ради того же стал подгружать данные с сервера. У Tracy есть web‑демо на WASM.

## Вкладки, хоткеи, протоколы

- **Синхронизация вкладок.** Надёжнее всего одна вкладка с панелями: одно состояние времени и один цикл
  отрисовки. Между вкладками — `BroadcastChannel` (Safari с 15.4): по нему идут изменения состояния
  `{ t_left_ns, duration_ns, live, cursor_ns }`, а не кадры. Общий хаб и одно соединение на все вкладки даёт
  `SharedWorker` (Safari с 16). Скрытая вкладка не рисует, браузер ставит `requestAnimationFrame` на паузу.
- **Хоткеи.** Свободны одиночные клавиши, стрелки, `+/−`, `Home/End`, `/` и многие `Ctrl/Cmd`‑сочетания
  (`F`, `K`, `S`, `Z`). Буквенные хоткеи нужно привязывать к `event.code`, иначе на ЙЦУКЕН они не сработают.
- **Протоколы.** Управление — HTTP и JSON. Данные — один WebSocket на вкладку: запросы окон, отмена устаревших
  запросов, live‑подписка. WebTransport есть во всех браузерах с Safari 26.4, на него можно перейти позже.
  SSE не подходит, он передаёт только текст; чистого TCP в браузере нет.

## Замеры прототипа

Условия: headless Chrome 156, Apple M4 Max, DPR 1, холст 1586×750; headless рисует 60 Гц.

| Сценарий | JS на кадр | GPU на кадр | Рывки | Ответ сервера |
|---|---|---|---|---|
| 9 счётчиков, окно 2,73 с, сырые точки и маркеры | 0,13 мс | 0,25–0,37 мс | 0 из 600 кадров | 16–20 КБ |
| Вся запись (~1 ч), всё в M4, 115 тыс. сегментов | 0,14 мс | 1,06 мс | 0 | 918 КБ |
| 72 счётчика × 500 кГц, 1,29 млн сегментов | 0,15 мс | 3,1–5,0 мс | 0 | 10,3 МБ |
| Зум до 226 мкс (147 нс на пиксель) при ×10 000 | 0,16 мс | 1,0 мс | 0 | 24 КБ |

![Вся запись в M4](img/zoom_out.png)

- В рабочих сценариях кадр стоит десятые доли миллисекунды JS и до миллисекунды GPU. До бюджета 120 Гц
  (8,3 мс) далеко.
- Сдвиг не ждёт сервер: пока ответ с запасом покрывает окно, график сдвигается на GPU без новых запросов.
- Стоимость кадра растёт с числом сегментов, то есть со счётчиками × пикселями, а не с частотой данных.
- Ответ сервера растёт так же: для 9 счётчиков — десятки КБ сырых точек или около мегабайта M4 на окно с
  запасом, для 72 счётчиков — 10 МБ. Резервы: ровно одна колонка M4 на пиксель (сейчас 1–2), две точки на
  колонку вместо четырёх, меньший запас окна, более компактные числа.
- Кадры «без данных у края» бывают только при быстром зуме, когда анимация обгоняет ответы, потому что в полёте
  один запрос.

## Что не проверено

- 120 Гц на ProMotion, Safari, Firefox, DPR 2, GPU на Windows и Linux.
- Настоящий Go‑сервер, сеть, протокол поверх WebSocket, live при реальном приёме.
- Логи и трейсы: прототип рисует только метрики.
- Подписи рисует Canvas2D; при большом числе подписей это может стать заметным.

## Источники

- [WebGPU is now supported in major browsers (web.dev)](https://web.dev/blog/webgpu-supported-major-browsers), [gpuweb implementation status](https://github.com/gpuweb/gpuweb/wiki/implementation-status)
- [WebKit bug 173434: 120 Hz rAF](https://bugs.webkit.org/show_bug.cgi?id=173434), [WebKit bug 272226: флаг 60 fps и PWA](https://bugs.webkit.org/show_bug.cgi?id=272226), [MacRumors: 120 Гц в Safari](https://www.macrumors.com/how-to/enable-smoother-120hz-browsing-in-safari/)
- [Chromium: browser_command_controller.cc](https://raw.githubusercontent.com/chromium/chromium/main/chrome/browser/ui/browser_command_controller.cc), [Mozilla bug 1074971](https://www.bugzilla.mozilla.org/show_bug.cgi?id=1074971), [Keyboard Lock API](https://developer.chrome.com/docs/capabilities/web-apis/keyboard-lock)
- [MDN: WebGPU API](https://developer.mozilla.org/en-US/docs/Web/API/WebGPU_API), [MDN: SharedArrayBuffer](https://developer.mozilla.org/en-US/docs/Web/JavaScript/Reference/Global_Objects/SharedArrayBuffer), [MDN: Secure contexts](https://developer.mozilla.org/en-US/docs/Web/Security/Defenses/Secure_Contexts)
- [SpiderMonkey: Memory64](https://spidermonkey.dev/blog/2025/01/15/is-memory64-actually-worth-using.html), [WebKit bug 300538: Memory64](https://bugs.webkit.org/show_bug.cgi?id=300538)
- [WebTransport is now Baseline](https://webrtc.ventures/2026/04/webtransport-is-now-baseline-what-it-means-for-real-time-media/), [Chrome: WebSocketStream](https://developer.chrome.com/docs/capabilities/web-apis/websocketstream)
- [caniuse: BroadcastChannel](https://caniuse.com/broadcastchannel), [web-features: Shared workers](https://web-platform-dx.github.io/web-features-explorer/features/shared-workers/), [MDN: requestAnimationFrame](https://developer.mozilla.org/en-US/docs/Web/API/Window/requestAnimationFrame)
- [Perfetto: large traces](https://perfetto.dev/docs/visualization/large-traces), [Rerun ARCHITECTURE.md](https://github.com/rerun-io/rerun/blob/main/ARCHITECTURE.md), [Tracy README](https://cdn.jsdelivr.net/gh/wolfpld/tracy@master/README.md)
- [M4: A Visualization-Oriented Time Series Data Aggregation (PVLDB 2014)](https://datavis.cs.columbia.edu/files/papers/m4.pdf)
