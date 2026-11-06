# Сборка mewnet

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

Ядро mihomo зафиксировано в `libs/mewnet/go.mod` (`v1.19.33`). Тег `main` не используется:
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
export GOPATH="$HOME/.cache/mewnet/gopath"
export GOCACHE="$HOME/.cache/mewnet/gocache"
export GOMODCACHE="$GOPATH/pkg/mod"
```

## Сборка

```bash
make build
```

Эквивалентная команда:

```bash
cd libs/mewnet
CGO_ENABLED=1 mise exec -- go build \
  -p=2 \
  -trimpath \
  -buildmode=c-shared \
  -ldflags "-s -w -X github.com/metacubex/mihomo/constant.Version=v1.19.33" \
  -o ../../dist/libmewnet.so \
  ./cmd/mewnet
```

C-библиотека подписок:

```bash
make subs
# dist/libmewnet.so — Go-ядро + C-подписки + SQLite в одной библиотеке
# build/apps/mewnet-subs/mewnet-subs
```

GUI на Odin/raylib:

```bash
make gui
# build/mewnet-gui
```

Результат:

- `dist/libmewnet.so` — библиотека;
- `dist/libmewnet.h` — заголовок, сгенерированный cgo;
- `libs/mewnet/cmd/mewnet/mewnet.h` и `mewnet_types.h` — публичные заголовки
  (используйте их; `mewnet.h` подключает `mewnet_types.h`).

`-p=2` ограничивает параллелизм: mihomo большой, на машинах с 1–2 ГБ RAM
сборка с большим `-p` может упасть по памяти.

## Использование из C

```bash
cc app.c -Ilibs/mewnet/cmd/mewnet -Ldist -lmewnet -Wl,-rpath,'$ORIGIN' -o app
```

`$ORIGIN` нужен, чтобы `libmewnet.so` находилась рядом с бинарником. Иначе
задайте `LD_LIBRARY_PATH=dist`.

## Использование из Python

```python
import ctypes
lib = ctypes.CDLL("dist/libmewnet.so")
```

Полный пример — в `README.md` и `tests/python/test_api.py`.

## Кросс-компиляция под Windows (mingw)

Нужен `x86_64-w64-mingw32-gcc` (в окружении уже есть).

```bash
make build-mingw-core
# dist/windows/mewnet.dll
# dist/windows/libmewnet.dll.a
# dist/windows/mewnet.def

make subs-mingw
# dist/windows/mewnet.dll
# build-mingw/build/apps/mewnet-subs/mewnet-subs.exe
```

`subs-mingw` использует `cmake/toolchains/mingw-x86_64.cmake`.
Тесты под Windows не собираются (`MEWNET_SUBS_BUILD_TESTS=OFF`).
Запуск под Wine не проверялся.

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
make test-c-ping MEWNET_TEST_VLESS='vless://...'  # ping из C через реальную ссылку
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
| `mewnet is already initialized` | в процессе уже живёт экземпляр; вызовите `mewnet_shutdown` |
| `undefined reference to mewnet_*` | не подключён `-lmewnet` или не тот `include` |
| `libmewnet.so: cannot open shared object file` | добавьте `-Wl,-rpath` или `LD_LIBRARY_PATH` |
| e2e: `blocked target ... blackholing` | Xray 26.x блокирует loopback; в тестовом конфиге есть `finalRules: allow` |
