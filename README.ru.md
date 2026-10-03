<p align="center">
  <img src="docs/images/panel-photo.jpg" alt="tablo на столе: курсы, лимиты Claude и Codex, воздух, почта и погода на e-ink панели" width="820">
</p>

<h1 align="center">tablo</h1>

<p align="center">
  Автономный e-ink дашборд для TRMNL 7.5" DIY Kit (ESP32-S3).<br>
  Курсы валют и крипты, лимиты Claude Code и Codex, воздух в комнате, непрочитанная почта, погода. Сервер не нужен.
</p>

<p align="center">
  <a href="https://github.com/pavellunev/tablo/actions/workflows/ci.yml"><img src="https://github.com/pavellunev/tablo/actions/workflows/ci.yml/badge.svg" alt="CI"></a>
  <a href="https://github.com/pavellunev/tablo/releases"><img src="https://img.shields.io/github/v/release/pavellunev/tablo?include_prereleases" alt="Release"></a>
  <a href="LICENSE"><img src="https://img.shields.io/badge/license-MIT-blue.svg" alt="MIT"></a>
</p>

<p align="center">
  <a href="README.md">English</a> · Русский
</p>

## Возможности

- **Автономный.** ESP32 сам ходит в Binance, ЦБ РФ, Anthropic, OpenAI, Open-Meteo, Home Assistant и в почту по IMAP, сам рисует кадр. Без домашнего сервера, облака и приложения-посредника.
- **Лимиты Claude Code и Codex на столе.** Остаток пятичасового и недельного окна, время до сброса. OAuth-токены обновляются на устройстве.
- **Честный экран.** Устаревшее помечено, отказавший источник показывает причину, ноль не рисуется вместо отсутствия данных.
- **Три дашборда** переключаются кнопками на плате, собираются из виджетов на странице настройки.
- **Настройка с телефона.** Точка доступа с QR при первом включении, дальше страница в домашней сети. Интерфейс на английском или русском.
- **Бережно к e-ink.** Частичное обновление только при изменениях, полное раз в час.
- **Готовая прошивка** в [Releases](https://github.com/pavellunev/tablo/releases), хостовые тесты и рендер кадров без устройства.

## Что на экране

<p align="center">
  <img src="docs/images/dashboard-desk.png" alt="Заводской дашборд: рынки, лимиты AI и воздух, почта, сегодня" width="800">
</p>

| Блок | Показывает | Откуда |
|---|---|---|
| **Рынки** | BTC с графиком за сутки, USD и EUR с изменением за день | Binance, ЦБ РФ |
| **Лимиты** | Остаток пятичасового и недельного окна Claude, недельного окна Codex, время до сброса | OAuth usage-эндпоинты Anthropic и OpenAI |
| **Воздух** | CO₂ и TVOC с графиком за 6 часов и состоянием «свежо / норма / проветрить» | Home Assistant |
| **Почта** | Счётчик непрочитанных и последние четыре письма | IMAP |
| **Сегодня** | Температура и описание погоды для вашего города | Open-Meteo, координаты и часовой пояс определяются автоматически |

Если источники отвалились, каждый блок остаётся на экране и объясняет причину:

<p align="center">
  <img src="docs/images/dashboard-failures.png" alt="Все источники отвалились: каждый блок показывает причину" width="800">
</p>

## Железо

[Seeed Studio TRMNL 7.5" OG DIY Kit](https://www.seeedstudio.com/TRMNL-7-5-Inch-OG-DIY-Kit-p-6481.html)
(в России есть [на Ozon](https://www.ozon.ru/product/seeed-studio-trmnl-byod-7-5-og-diy-nabor-dlya-e-ink-monohromnyy-e-ink-displey-800x480-xiao-esp32-s3-2887044566/)):
XIAO ESP32-S3 (8 МБ flash, 8 МБ PSRAM), монохромная e-ink панель 7,5" 800×480, три кнопки, батарея с замером заряда.
Подставка на фото: [L-образный корпус с MakerWorld](https://makerworld.com/ru/models/1625065-trmnl-7-5-og-diy-kit-l-shape).

## Установка

### Готовая прошивка

Скачайте `tablo-<версия>-full.bin` из [Releases](https://github.com/pavellunev/tablo/releases) и прошейте по адресу `0x0`:

```bash
pip install esptool
esptool.py --chip esp32s3 --port /dev/ttyACM0 write_flash 0x0 tablo-<версия>-full.bin
```

Без командной строки: откройте [web.esphome.io](https://web.esphome.io) в Chrome, подключите плату по USB и выберите тот же файл.

### Сборка из исходников

Нужны [PlatformIO](https://platformio.org/) и Python 3.

```bash
pio run                    # собрать
pio run -t upload          # прошить
pio run -t uploadfs        # залить страницу настройки
./scripts/verify.sh        # сборка + хостовые тесты
```

`firmware/src/secrets.h` опционален. Скопируйте его из `secrets.h.example`, если хотите зашить Wi-Fi и токены в образ; иначе всё вводится на странице настройки.

## Настройка

**Первое включение.** Без известной сети устройство поднимает точку доступа `tablo-setup` и показывает пароль и QR-код. Наведите камеру телефона, страница настройки откроется сама.

<p align="center">
  <img src="docs/images/setup-access-point.png" alt="Экран точки доступа: имя сети, пароль и QR" width="800">
</p>

**Дальше.** В домашней сети страница доступна по `http://tablo-setup.local/` (имя устройства меняется там же). Источники настраиваются карточками: Claude, Codex, почта, Home Assistant, курсы, погода и город. Секреты вводятся один раз и наружу больше не показываются. Удержание кнопки 1 три секунды поднимает точку доступа принудительно.

**Доступы.**

| Источник | Что ввести | Где взять |
|---|---|---|
| Claude | OAuth access + refresh token | Вход в Claude Code: `~/.claude/.credentials.json` (macOS: связка ключей, элемент «Claude Code-credentials») |
| Codex | OAuth access + refresh token | Вход в Codex CLI: `~/.codex/auth.json` |
| Почта | IMAP-сервер, адрес, пароль приложения | Пароли приложений у провайдера (Gmail: Безопасность → Пароли приложений) |
| Home Assistant | URL, долгосрочный токен | Профиль HA → Безопасность → Токены долгосрочного доступа |
| Курсы, погода | ничего | публичные API |

Usage-эндпоинты Anthropic и OpenAI доступны не из всех регионов. Устройство пишет это на экране, а не рисует нули.

## Дашборды и виджеты

Три дашборда переключаются кнопками 1–3. Каждый собирается на странице настройки из палитры виджетов с предпросмотром.

| Размер | Ширина | Смысл |
|---|---|---|
| `S` | 202 px | узкая колонка, как «Сегодня» |
| `M` | 296 px | средняя, как «Рынки» |
| `flex` | остаток | делит остаток ряда с другими гибкими |

Скрытый виджет не оставляет дыры, соседи забирают его место. Базовый набор: рынки, лимиты, воздух, лимиты + воздух, почта, сегодня, показатель (любой слот крупно) и подпись.

Добавить виджет: один файл плюс строка в реестре. Виджет объявляет, какие слоты данных ему нужны и как часто, а устройство опрашивает источник только если его данные нужны видимому виджету. См. [docs/widgets.md](docs/widgets.md).

## Разработка

```
источники ──► коннекторы ──► слоты ──► виджеты ──► кадр 800×480
Binance,      HTTP/JSON,      btc,      markets,     1-бит канва,
Anthropic,    OAuth-refresh,  limit.*,  limits,      Terminus +
OpenAI,       IMAP-диалог,    co2,      air, mail,   IBM Plex Mono,
Open-Meteo,   разбор          mail.*    today, …     частичное обновление
```

- `scripts/verify.sh` собирает прошивку и гоняет хостовые тесты (слоты, конфиг, раскладка, расписание опроса, кнопки). `--fast` пропускает тесты.
- `tools/render_frame/build_and_run.sh` снимает все экраны в PNG тем же кодом раскладки, что работает на панели: три дашборда, экраны включения и точки доступа, сценарии отказов.
- `tools/compare_frame.py` сверяет каркас кадра с эталонным макетом.
- CI гоняет ту же сборку и тесты на каждый push. Тег `v*` публикует релиз с образами `full`, `firmware` и `littlefs`.

Документация:

- [docs/decisions.md](docs/decisions.md) — решения и их цена: рендер на устройстве, OAuth и IMAP без посредника, настройка из любой сети, HTTPS с закреплёнными корнями.
- [docs/architecture.md](docs/architecture.md) — слоты, коннекторы, раскладка.
- [docs/widgets.md](docs/widgets.md) — как добавить виджет.
- [docs/constructor.md](docs/constructor.md) — дашборды, палитра, мастер источников.

## Лицензия

MIT. Шрифты Terminus и IBM Plex Mono — SIL OFL 1.1, см. `firmware/assets/`.
