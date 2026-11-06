# mewnet-watch

Утилита первичного discovery и маршрутизации через найденный прокси.

```bash
./build/apps/mewnet-watch/mewnet-watch \
  --db data/watch.db \
  --proxy-port 18085 \
  --batch 70 --concurrency 70 --timeout 3000 \
  https://raw.githubusercontent.com/Pawdroid/Free-servers/main/sub \
  https://raw.githubusercontent.com/ALIILAPRO/v2rayNG-Config/main/server.txt
```

Что делает:

- создаёт/открывает SQLite базу;
- добавляет переданные подписки в группу `All`;
- пока нет рабочего узла, проверяет непроверенные прокси пачками
  (`--batch`, по умолчанию 70) одновременно (`--concurrency`, по умолчанию 70);
- при нахождении рабочего узла добавляет его в `MILIB`, выбирает активным и
  печатает:
  ```text
  PROXY http://127.0.0.1:<port>
  READY
  ```
- после этого продолжает discovery через стандартный rate limiter;
- обновляет не больше одной подписки в час (`--fetch-interval 3600`);
- если непроверенных узлов нет и рабочих не найдено — обновляет следующую
  подписку немедленно, без ожидания часа.

Проверка через найденный прокси:

```bash
curl -x http://127.0.0.1:18085 https://lua.org
```

`mewnet-watch` использует только публичное API `libmewnet_subs` и `libmewnet`.
