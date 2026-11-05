# Сборка milib

## Требования

| Компонент | Версия |
|---|---|
| Linux | x86_64, glibc |
| Go | 1.27.3 (mise) |
| CGO | clang 23.1.4 (mise) |
| CMake | 4.4.5 (mise) |
| SQLite | vendored amalgamation 3.54.1 |
| Python (тесты) | 3.10+ |
| Xray (только e2e) | 26.x |

Ядро mihomo зафиксировано в `go.mod` (`v1.19.33`). Тег `main` не используется:
в нём лежит другой проект.

## Toolchain через mise

Версии зафиксированы в `.mise.toml`:

```toml
[tools]
go = "1.27.3"
odin = "dev-2026-11a"
android-sdk = "23.0"
clang = "23.1.4"
cmake = "4.4.5"
```

```bash
make deps
# или
mise install
```

Кэши Go можно держать вне домашнего каталога:

```bash
export GOPATH="$HOME/.cache/milib/gopath"
export GOCACHE="$HOME/.cache/milib/gocache"
export GOMODCACHE="$GOPATH/pkg/mod"
```

## Сборка

```bash
make build
```

Эквивалентная команда:

```bash
CGO_ENABLED=1 mise exec -- go build \
  -p=2 \
  -trimpath \
  -buildmode=c-shared \
  -ldflags "-s -w -X github.com/metacubex/mihomo/constant.Version=v1.19.33" \
  -o dist/libmilib.so \
  ./cmd/milib
```

C-библиотека подписок:

```bash
make subs
# build/subscriptions/libmilib_subs.so
# build/subscriptions/milib-subs
```

GUI на Odin/raylib:

```bash
make gui
# build/milib-gui
```

Результат:

- `dist/libmilib.so` — библиотека;
- `dist/libmilib.h` — заголовок, сгенерированный cgo;
- `include/milib.h` и `include/milib_types.h` — публичные заголовки
  (используйте их; `milib.h` подключает `milib_types.h`).

`-p=2` ограничивает параллелизм: mihomo большой, на машинах с 1–2 ГБ RAM
сборка с большим `-p` может упасть по памяти.

## Использование из C

```bash
cc app.c -Iinclude -Ldist -lmilib -Wl,-rpath,'$ORIGIN' -o app
```

`$ORIGIN` нужен, чтобы `libmilib.so` находилась рядом с бинарником. Иначе
задайте `LD_LIBRARY_PATH=dist`.

## Использование из Python

```python
import ctypes
lib = ctypes.CDLL("dist/libmilib.so")
```

Полный пример — в `README.md` и `tests/python/test_api.py`.

## Тесты

```bash
make test-unit       # Go-тесты задач + базовый ABI
make test-links      # SharedVPNLink без сети
make test-stress     # 100 циклов add/remove
make test-c          # C-заголовок и линковка
make xray            # скачать Xray в /tmp/opencode/xray
make test-e2e        # одиночный VLESS + mixed inbound
make test-e2e-links  # 4 прокси, hot-swap, конкурентные пинги
make test-c-ping-local                 # ping из C через локальный Xray
make test-c-ping MILIB_TEST_VLESS='vless://...'  # ping из C через реальную ссылку
make test-subs       # C-тесты подписок (base64, SHA-256, rating)
make test-e2e-subs   # локальный Xray + HTTP-подписка + SQLite/rating
```

## Особенности

- TUN не включён: сборка без тега `with_gvisor`;
- `dlclose` для `.so` запрещён — Go runtime не выгружается;
- один экземпляр mihomo на процесс;
- glibc-совместимость: собрано на Fedora 45 / glibc 2.44; для старых
  дистрибутивов собирайте в более старом base image;
- распространение `.so` требует соблюдения GPL-3.0 (лицензия mihomo).

## Диагностика

| Симптом | Причина |
|---|---|
| `mixed inbound failed to listen` | порт занят, задайте `mixed_port: 0` |
| `milib is already initialized` | в процессе уже живёт экземпляр; вызовите `milib_shutdown` |
| `undefined reference to milib_*` | не подключён `-lmilib` или не тот `include` |
| `libmilib.so: cannot open shared object file` | добавьте `-Wl,-rpath` или `LD_LIBRARY_PATH` |
| e2e: `blocked target ... blackholing` | Xray 26.x блокирует loopback; в тестовом конфиге есть `finalRules: allow` |
