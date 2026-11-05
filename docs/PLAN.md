# milib — FFI-обёртка mihomo для Linux

Статус: реализовано; см. `README.md`, `docs/BUILD.md` и `docs/TEST_REPORT.md`.

## 1. Зафиксированные решения

| Вопрос | Решение |
|---|---|
| Источник ядра | mihomo **v1.19.33**, commit `3f27ef39f8c96bbfbc8b0b9bb10f5c9ef60149d8` |
| Потребитель | C ABI + Python `ctypes` для тестов |
| Трафик | локальный mixed SOCKS/HTTP inbound на `127.0.0.1` |
| Тестовый VLESS | локальный Xray на `127.0.0.1` |
| Add/remove | hot-swap через `tunnel.UpdateProxies` |
| Лицензия проекта | GPL-3.0 (как у mihomo) |

### Почему именно v1.19.33

На момент планирования ветка `main` репозитория `MetaCubeX/mihomo` содержит
Python-проект, а не Go-ядро. Тег `v1.19.33` содержит корректный Go-исходник и
доступен через Go module proxy. Поэтому:

- версия фиксируется в `go.mod` и `go.sum`;
- `@latest` и `@main` не используются;
- для релизной сборки допускается `go mod vendor` или `third_party/mihomo`.

В `go.mod` mihomo есть `replace google.golang.org/protobuf => ...`.
`replace` из зависимости не применяется, поэтому его нужно продублировать в
корневом `go.mod` проекта.

## 2. Границы первой версии

Входит:

- Linux x86_64, glibc;
- `c-shared` `.so` + C-заголовок;
- init/shutdown;
- add/remove/list/select прокси;
- delay-тест через прокси (HTTP HEAD, как «пинг» в Clash UI);
- async delay-задачи;
- локальный mixed SOCKS/HTTP inbound;
- Xray-based e2e-тесты;
- C и Python тесты, отчёт.

Не входит:

- TUN и системный VPN;
- несколько независимых экземпляров ядра в одном процессе;
- ICMP ping;
- Android/iOS;
- хранение/импорт подписок;
- GUI.

## 3. Архитектура

```text
milib/
├── cmd/milib/              package main, cgo, //export
│   ├── main.go
│   ├── api.go
│   ├── bridge.h
│   └── bridge.c            C-trampoline для callback'ов
├── internal/core/
│   ├── core.go             lifecycle, состояние, блокировки
│   ├── config.go           базовая конфигурация
│   ├── proxies.go          add/remove/list/select
│   ├── group.go            пересборка MILIB/GLOBAL
│   ├── delay.go            URLTest sync/async
│   └── tasks.go            реестр задач, отмена, TTL
├── include/milib.h
├── tests/
│   ├── c/test_api.c
│   ├── python/test_api.py
│   └── e2e/
│       ├── run.py
│       ├── xray-vless.json.tmpl
│       └── target_204.py
├── Makefile
├── LICENSE                 GPL-3.0
└── docs/
```

### Один экземпляр на процесс

mihomo хранит состояние в глобальных `tunnel`, `resolver`, `listener`,
`cachefile`. Несколько независимых ядер в одном процессе корректно не
поддерживаются.

Правила:

- одновременно активен один handle;
- `milib_init` при уже активном handle возвращает ошибку;
- `milib_shutdown` останавливает listeners, закрывает прокси и задачи;
- повторный `milib_init` после `shutdown` тестируется отдельно; если окажется
  нестабильным — документируется как one-shot на процесс;
- `dlclose` для `.so` запрещён: Go runtime не выгружается.

### Инициализация

1. `constant.SetHomeDir(home)` до любого обращения к mihomo.
2. Загрузить базовый YAML.
3. Добавить/проверить группу `MILIB` (`type: select`, `proxies: [DIRECT]`).
4. Добавить `MATCH,MILIB`, если в правилах нет `MATCH`.
5. Выбрать `mixed-port`:
   - `options.mixed_port > 0` — использовать его;
   - `options.mixed_port == -1` — inbound выключен;
   - `options.mixed_port == 0` или не задан — подобрать свободный порт;
   - значение из YAML используется, если options не задаёт своё.
6. `config.Parse()` → `hub.ApplyConfig(cfg)`.

Базовая конфигурация по умолчанию:

```yaml
allow-lan: false
bind-address: 127.0.0.1
mode: rule
log-level: warning
ipv6: false
proxies: []
proxy-groups:
  - name: MILIB
    type: select
    proxies: [DIRECT]
rules:
  - MATCH,MILIB
```

### Hot-swap add/remove

Правила mihomo хранят **имя** адаптера, а не указатель, поэтому замена группы в
`tunnel.Proxies()` не ломает `MATCH,MILIB`.

Алгоритм добавления:

1. `convert.ConvertsV2Ray(link)` → `[]map[string]any`
   (поддерживается одиночная ссылка, список строк и base64-подписка).
2. Для каждого элемента:
   - проверить `type`, `server`, `port`;
   - определить имя: fragment → `type-server-port` → суффикс при коллизии;
   - защитить имена `DIRECT`, `REJECT`, `GLOBAL`, `MILIB`;
   - `adapter.ParseProxy(mapping)` для валидации.
3. Построить новый map: копия `tunnel.Proxies()` + новые прокси.
4. Пересобрать `MILIB`:
   - `provider.NewHealthCheck(proxies, C.DefaultTestURL, 5000, 0, true, nil)`;
   - `provider.NewCompatibleProvider("MILIB", proxies, hc)`;
   - `outboundgroup.NewSelector(...)`;
   - обернуть `adapter.NewProxy(selector)`.
5. Пересобрать `GLOBAL`, если он был создан автоматически.
6. `tunnel.UpdateProxies(newProxies, newProviders)`.
7. Закрыть старые group/provider; сохранить выбранный элемент.
8. Обновить внутренний реестр (`name`, `type`, `link`, `source`, `added_at`).

Удаление:

1. найти прокси в реестре (встроенные удалять нельзя);
2. запомнить текущий selection группы;
3. построить map без прокси и пересобрать группу;
4. `tunnel.UpdateProxies`;
5. закрыть соединения, в chain которых есть удаляемое имя
   (`statistic.DefaultManager`);
6. закрыть прокси и старые group/provider;
7. если удалён выбранный — выбрать `DIRECT` (или первый оставшийся).

Fallback: полный `executor.ApplyConfig(cfg, false)`, если меняется структура
конфигурации. Он медленнее и подвешивает туннель, поэтому в add/remove не
используется.

### Delay-тест

`C.Proxy.URLTest(ctx, url, expectedStatus)`:

- возвращает задержку в мс;
- `0`/ошибка — неуспех;
- timeout задаётся через `context.WithTimeout`;
- по умолчанию URL `https://www.gstatic.com/generate_204`, expected `204`;
- для локального теста — `http://127.0.0.1:<port>/204`;
- это HTTP-задержка, не ICMP.

`milib_test_link` парсит ссылку, создаёт временный прокси, тестирует и закрывает
его, не добавляя в состояние.

### Потоки и callback'и

- все публичные функции thread-safe, кроме `init/shutdown` (вызывать из одного
  потока);
- async-задачи выполняются в Go-горутинах;
- C callback вызывается через trampoline в `bridge.c`;
- callback не должен вызывать `milib_shutdown` и не должен бросать C++
  исключения;
- строки в callback живут только на время вызова;
- `milib_shutdown` отменяет активные задачи и ждёт их завершения.

### Ошибки и память

- сложные операции возвращают JSON `{"ok":true,...}` / `{"ok":false,"error":"..."}`;
- строки выделяются через `C.CString` и освобождаются `milib_free_string`;
- все `//export`-функции оборачиваются в `recover()`;
- panic в фоновой горутине mihomo не перехватывается — это ограничение
  in-process библиотеки.

## 4. C API

```c
#ifndef MILIB_H
#define MILIB_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef uint64_t milib_handle;

typedef void (*milib_task_cb)(int64_t task_id,
                              const char* result_json,
                              void* user);

int   milib_init(const char* home_dir,
                 const char* config_yaml,
                 const char* options_json,
                 milib_handle* out_handle,
                 char** out_error);

void  milib_shutdown(milib_handle h);

char* milib_add_proxy(milib_handle h,
                      const char* link_or_blob,
                      const char* options_json);

char* milib_remove_proxy(milib_handle h, const char* name);
char* milib_list_proxies(milib_handle h);
char* milib_select(milib_handle h, const char* group, const char* name);
char* milib_status(milib_handle h);

char* milib_test_delay(milib_handle h,
                       const char* name,
                       const char* options_json);

char* milib_test_link(milib_handle h,
                      const char* link,
                      const char* options_json);

int64_t milib_test_delay_async(milib_handle h,
                               const char* name,
                               const char* options_json,
                               milib_task_cb cb,
                               void* user);

char* milib_task_poll(milib_handle h, int64_t task_id);
void  milib_task_free(milib_handle h, int64_t task_id);
int   milib_task_cancel(milib_handle h, int64_t task_id);

const char* milib_version(void);
void        milib_free_string(char* p);

#ifdef __cplusplus
}
#endif
#endif
```

### JSON-схемы

`milib_init` options:

```json
{ "mixed_port": 0, "log_level": "warning" }
```

`milib_add_proxy` options:

```json
{ "name": "custom-name", "select": true, "skip_cert_verify": false }
```

`milib_test_delay` options:

```json
{ "url": "https://www.gstatic.com/generate_204",
  "timeout_ms": 5000,
  "expected": "204" }
```

`milib_list_proxies`:

```json
{ "ok": true, "group": "MILIB", "selected": "node-1",
  "proxies": [
    { "name": "node-1", "type": "VLESS",
      "server": "example.com", "port": 443,
      "source": "link", "alive": true,
      "delay": 123, "last_test_url": "https://www.gstatic.com/generate_204" }
  ] }
```

`milib_status`:

```json
{ "ok": true, "version": "0.1.0", "mihomo_version": "v1.19.33",
  "running": true, "mode": "rule",
  "mixed_port": 34567, "socks_port": 0, "http_port": 0,
  "proxies": 3, "goroutines": 42, "rss_bytes": 73400320,
  "uptime_ms": 120000 }
```

## 5. Сборка

Go в системе отсутствует. Установка без root:

```bash
# Go 1.26.0 — версия из CI mihomo
curl -LO https://go.dev/dl/go1.26.0.linux-amd64.tar.gz
tar -C "$HOME/.local" -xzf go1.26.0.linux-amd64.tar.gz
export PATH="$HOME/.local/go/bin:$PATH"

export GOPATH="$HOME/.cache/milib/gopath"
export GOCACHE="$HOME/.cache/milib/gocache"
export GOMODCACHE="$GOPATH/pkg/mod"
```

Сборка:

```bash
CGO_ENABLED=1 go build \
  -trimpath \
  -buildmode=c-shared \
  -ldflags "-s -w -X github.com/metacubex/mihomo/constant.Version=v1.19.33" \
  -o dist/libmilib.so \
  ./cmd/milib
```

Особенности:

- по умолчанию без тега `with_gvisor` (TUN/Tailscale не нужны);
- при необходимости TUN — отдельная сборка с `-tags with_gvisor`;
- памяти мало (~1.2 ГБ свободно), собирать с `-p=2`;
- glibc с Fedora 45 может не подойти для старых дистрибутивов; для поставки
  собирать в более старом base-образе;
- артефакты: `.so`, `include/milib.h`, sha256, `milib.pc`, примеры C/Python.

## 6. Тестирование

### 6.1 Unit / ABI

- валидная VLESS-ссылка: add → list → remove;
- пустая/битая/не-VLESS ссылка: ошибка, состояние не меняется;
- дубликаты имён и fragment'ов: уникальные имена;
- удаление несуществующего/встроенного: ошибка;
- удаление выбранного: fallback на `DIRECT`;
- 100 циклов add/remove: FD, goroutine, RSS;
- C-тест с `#include "milib.h"`;
- Python `ctypes`: `NULL`, неверные handle, освобождение строк, callback;
- опционально `-race`.

### 6.2 E2E с локальным Xray

Инструменты:

- Xray prebuilt, версия пинится, бинарник не коммитится;
- локальный HTTP-сервер, отдающий `204` на `GET`/`HEAD`;
- mihomo с mixed-port на свободном порту.

Сценарии:

| # | Сценарий | Ожидание |
|---|---|---|
| 1 | add VLESS TCP без TLS → select → delay(local 204) | delay > 0 |
| 2 | delay(`https://www.gstatic.com/generate_204`) | delay > 0 при наличии сети |
| 3 | `curl -x socks5h://... http://127.0.0.1:.../204` | 204, Xray access log содержит соединение |
| 4 | остановить Xray → delay/curl | понятная ошибка, без crash |
| 5 | remove прокси → прямой запрос | работает через DIRECT |
| 6 | «мёртвый» прокси (blackhole/недоступный порт) | delay timeout, good-прокси работает |
| 7 | VLESS + TLS self-signed (`allowInsecure`) | delay и трафик работают |
| 8 | реальная ссылка из `MILIB_TEST_VLESS` | delay и трафик работают |

Реальная ссылка передаётся только через env/файл, не попадает в логи и git.

### 6.3 Критерии приёмки

- `.so` грузится из C и Python;
- add/remove/list/select консистентны;
- delay возвращает мс через прокси и ошибку при недоступности;
- HTTP-запрос через mixed-port реально проходит через VLESS (подтверждено
  access-логом Xray);
- удаление прокси возвращает трафик на DIRECT;
- нет падений на невалидных входах;
- нет неограниченного роста FD/goroutine/RSS на 100 циклах;
- `docs/TEST_REPORT.md` содержит команды, числа и ограничения.

## 7. Этапы

- [x] **0. Toolchain** — Go 1.26, cgo hello-world `.so`, проверка памяти.
- [x] **1. Каркас** — модуль, pin mihomo, init/shutdown, загрузка `.so` из Python.
- [x] **2. Конфиг и inbound** — базовый YAML, mixed-port, `status`, `list`.
- [x] **3. Hot-swap** — add/remove/select, имена, ошибки, очистка ресурсов.
- [x] **4. Delay** — sync/async, `test_link`, отмена задач.
- [x] **5. E2E** — Xray + локальный 204 + негативные сценарии.
- [ ] **6. TLS/REALITY и stress** — stress (100 циклов) выполнен; TLS-вариант,
  реальная ссылка и `-race` ещё не проверялись.
- [x] **7. Документация** — Makefile, README, `TEST_REPORT.md`, LICENSE.
- [x] **8. SharedVPNLink** — объект-ссылка, per-link SOCKS5 с авторизацией,
  ping/speed/exit_ip, конкурентные задачи, hot-swap; e2e на 4 прокси.

Оценка: MVP — ~1 рабочий день, полный вариант — 1.5–2 дня.

## 8. Риски

1. **Supply chain**: main mihomo содержит не тот проект. Pin `v1.19.33`,
   `go.sum`, при необходимости vendor.
2. **GPL-3.0**: распространение `.so` с mihomo требует соблюдения GPL.
   Проект выбран как open-source GPL-3.0.
3. **Один экземпляр на процесс**: следствие глобального состояния mihomo.
4. **Panic в фоновой горутине** роняет процесс; для недоверенных конфигов
   нужен отдельный процесс.
5. **Сигналы**: Go runtime ставит обработчики; возможны конфликты с JVM/Node.
6. **VLESS-конвертер**: `allowInsecure` для VLESS не мапится — патчим в wrapper.
7. **glibc/архитектура**: сборка на Fedora 45; для старых систем — отдельный
   base image.
8. **Память при сборке**: mihomo большой; `-p=2`, при OOM — swap или другая
   машина.
