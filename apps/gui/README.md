# mewnet GUI (Odin + raylib)

Простая GUI-программа для управления подписками и прокси из SQLite.

## Сборка и запуск

```bash
make gui
./build/mewnet-gui --db data/subs.db
```

Для проверки без ручного скриншота:

```bash
./build/mewnet-gui --db data/subs.db --screenshot /tmp/mewnet-gui.png
```

GUI использует:

- `libmewnet` для всех операций с SQLite;
- `vendor:raylib` из Odin;
- только страницы прокси, а не всю подписку;
- DejaVu Sans + Symbola fallback для Unicode/эмодзи.
  CJK-иероглифы, которых нет в этих шрифтах, могут отображаться как `?`.

## Что можно делать

- смотреть группы подписок; по умолчанию одна группа `All`;
- добавлять подписку в выбранную группу: URL + priority;
- удалять выбранную подписку;
- листать прокси группы страницами по 16;
- видеть имя, протокол, адрес, рейтинг, alive/dead, задержку, manual-флаг;
- выбрать прокси и посмотреть полную share-ссылку;
- добавить ручной прокси в выбранную группу;
- удалить выбранный прокси.

## Память

GUI **не загружает**:

- тело подписки;
- все прокси подписки;
- все прокси группы.

Он держит в памяти:

- список групп и подписок выбранной группы (обычно небольшие);
- одну страницу `mewnet_subs_proxy_summary` без полной ссылки;
- одну выбранную полную запись `mewnet_subs_proxy`.

Поэтому для подписки на 10 000 конфигов используется примерно тот же объём,
что и для 20.

## Библиотечные API для GUI

Добавлены в `libmewnet`:

- `mewnet_subs_load_groups` / `mewnet_subs_add_subscription_in_group`;
- `mewnet_subs_load_proxy_summaries` / `mewnet_subs_unload_proxy_summaries`
  — страница без поля `link`;
- `mewnet_subs_get_proxy` — одна полная запись по id;
- `mewnet_subs_add_proxy` — ручной прокси в подписку (`manual = 1`);
- `mewnet_subs_remove_proxy`;
- `mewnet_subs_count_proxies`.

Ручные прокси не удаляются при следующем fetch, если их нет в подписке:
generation-cleanup удаляет только `manual = 0`.
