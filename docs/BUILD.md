# Сборка milib

## Требования

| Компонент | Версия |
|---|---|
| Linux | x86_64, glibc |
| Go | 1.26+ |
| CGO | gcc или clang |
| Python (тесты) | 3.10+ |
| Xray (только e2e) | 26.x |

Ядро mihomo зафиксировано в `go.mod` (`v1.19.33`). Тег `main` не используется:
в нём лежит другой проект.

## Установка Go без root

```bash
curl -LO https://go.dev/dl/go1.26.0.linux-amd64.tar.gz
tar -C "$HOME/.local" -xzf go1.26.0.linux-amd64.tar.gz
export PATH="$HOME/.local/go/bin:$PATH"
```

Кэши можно держать вне домашнего каталога:

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
CGO_ENABLED=1 go build \
  -p=2 \
  -trimpath \
  -buildmode=c-shared \
  -ldflags "-s -w -X github.com/metacubex/mihomo/constant.Version=v1.19.33" \
  -o dist/libmilib.so \
  ./cmd/milib
```

Результат:

- `dist/libmilib.so` — библиотека;
- `dist/libmilib.h` — заголовок, сгенерированный cgo;
- `include/milib.h` — публичный заголовок (используйте его).

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
make test-unit       # базовый ABI
make test-links      # SharedVPNLink без сети
make test-stress     # 100 циклов add/remove
make test-c          # C-заголовок и линковка
make xray            # скачать Xray в /tmp/opencode/xray
make test-e2e        # одиночный VLESS + mixed inbound
make test-e2e-links  # 4 прокси, hot-swap, конкурентные пинги
make test-c-ping-local                 # ping из C через локальный Xray
make test-c-ping MILIB_TEST_VLESS='vless://...'  # ping из C через реальную ссылку
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
