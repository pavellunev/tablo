# Виджеты: протокол, реестр, как добавить свой

Экран собирается из виджетов. Каждый виджет — один файл `widgets/w_<type>.cpp`
с одной записью `Spec`: спецификация → видимость → отрисовка → регистрация.
Дашборд (`config::Dashboard`, `config.h`) хранит только тип, размер и пару
опций — сам виджет знает, какие слоты ему нужны и как себя рисовать.

## Протокол (`firmware/src/widgets/widget.h`)

```cpp
namespace widgets {

enum class Size : uint8_t { kS, kM, kFlex };  // widgets/types.h

struct Instance {           // то, что лежит в дашборде
    String type;            // ключ в реестре, "markets"
    Size size = Size::kFlex;
    bool divider = false;   // вертикальная линия 2px слева от виджета
    String slot;            // metric: какой слот показывать
    String label;           // metric/text: подпись
};

struct Spec {
    const char* type;
    const char* title;                 // «Рынки» — для палитры на странице
    Size default_size;
    int16_t min_width;                 // меньше — страница размер не предлагает
    const char* const* slots;          // nullptr-terminated; metric — из Instance::slot
    uint32_t refresh_seconds;          // как часто виджету нужны свежие данные
    bool (*visible)(const slots::Store&, const Instance&);
    void (*draw)(canvas::Canvas&, const slots::Store&, const layout::DeviceInfo&,
                 layout::Rect, const Instance&);
};

const Spec* find(const char* type);            // nullptr — неизвестный тип
const Spec* const* all(size_t* count);         // реестр — для /api/widgets и палитры
void required_slots(const Instance&, std::vector<String>& out);

}
```

`widgets::Size` — три токена ширины, не пиксели произвольной величины:

| Токен | Ширина | Откуда |
|---|---|---|
| `S` | 202 px | «Сегодня» в эталоне |
| `M` | 296 px | «Рынки» в эталоне |
| `flex` | остаток ряда, поровну между flex-виджетами | гибкие колонки эталона |

Правило ряда (`layout::layout_row`, `layout.cpp`): видимые виджеты с `S`/`M`
берут свои пиксели, `flex` делят остаток поровну между собой. Если среди
видимых нет ни одного `flex` — видимые делят весь ряд поровну (так Рынки в
эталоне занимают всю ширину верхнего ряда, когда правая колонка пропала: один
видимый — «поровну» на одного и есть вся ширина). Невидимый виджет (у него
`visible()` вернул `false` — обычно потому, что источник отвалился) места не
резервирует, сосед получает освободившееся пространство.

## Шаги, чтобы добавить виджет

1. **Спека.** Реши, какие слоты виджету нужны (`btc`, `co2`, …), какой у него
   `default_size`/`min_width`, как часто ему нужны свежие данные
   (`refresh_seconds` — это войдёт в `widgets::compute_demand`, опрос источника
   подстроится под самый требовательный виджет).
2. **`visible(store, instance)`** — чистая функция: есть ли хоть один из нужных
   слотов (`layout::has_data(store.find("..."))`). Источник отвалился целиком —
   `visible` возвращает `false`, виджет не рисуется и не резервирует место.
3. **`draw(canvas, store, device, rect, instance)`** — рисование внутри `rect`,
   который посчитала раскладка. Общие примитивы (эйброу-заголовок, шкала-
   полоска, спарклайн, обрезка текста по ширине, инверсная плашка, бейдж
   состояния) — в `widgets/prims.h` (`namespace widgets::prims`), не дублируй
   их внутри своего файла. Имена функций/данных внутри анонимного namespace
   файла — с префиксом типа виджета (`markets_visible`, `kMarketsSlots`, …):
   тесты (`test_layout`, `test_slots`) собирают все `widgets/w_*.cpp` в одну
   единицу трансляции через `#include`, и безымянный namespace в C++ один на
   всю единицу трансляции — одинаковые имена в разных файлах столкнулись бы
   переопределением при таком объединении (при обычной раздельной компиляции,
   `env:xiao-esp32s3`, каждый `.cpp` — своя единица трансляции, и коллизии не
   было бы, но тесты подключают исходники, а не линкуют отдельно).
4. **Строка в реестре** (`widgets/registry.cpp`): `extern const Spec kXxxSpec;`
   рядом с остальными плюс запись в `kRegistry[]`. `extern` на самом
   определении `Spec` в твоём файле обязателен — без него `const` на уровне
   namespace получает внутреннюю линковку по умолчанию в C++, и `registry.cpp`
   не находит символ при раздельной компиляции (поймано на реальной сборке).
5. **Лёгкая таблица типов** (`widgets/types.cpp`, `kTypes[]`): добавь
   `{type, default_size, min_width}` — этим пользуется `config.cpp` при
   разборе дашборда с формы (проверка `type`/`size`), не тяня в себя код
   отрисовки. Числа должны совпадать с твоим `Spec` — тест `test_layout`
   (`test_widgets_registry_matches_types_table`) это перепроверяет при каждой
   сборке.
6. **Палитра на странице подхватывает сама.** `GET /api/widgets` (`portal.cpp`)
   отдаёт весь реестр (`type`, `title`, `default_size`, `min_width`, `slots[]`,
   `refresh`) — новый виджет появляется в палитре без единой правки
   `index.html`.

## Базовый набор

Перенос существующего кода отрисовки (`layout.cpp` до дашбордов) без
изменения пикселей — обмер `reference/cockpit-reference.png` не трогался.

| type | title | default | min | slots | refresh |
|---|---|---|---|---|---|
| `markets` | Рынки | M | 202 | btc, usd_rub, eur_rub | 300 |
| `limits` | Лимиты AI | flex | 202 | limit.claude.5h/.week/.reset, limit.codex(.reset) | 300 |
| `air` | Воздух | flex | 202 | co2, tvoc | 300 |
| `limits_air` | Лимиты + Воздух | flex | 202 | объединение `limits` и `air` | 300 |
| `mail` | Почта | flex | 202 | mail.unread (+ mail.N.\*, N=1..4, тем же коннектором) | 900 |
| `today` | Сегодня | S | 202 | weather.temp/.summary, event.N.at/.title (N=1..3) | 1800 |
| `metric` | Показатель | S | 120 | `Instance::slot` — любой один слот | 300 |
| `text` | Подпись | S | 120 | — (виден всегда, без данных) | 0 |

`limits`/`air` — общая `widgets::prims::draw_limits_and_air` с подавлением
своей половины (`show_limits`/`show_air`), `limits_air` включает обе:
переиспользуется один и тот же код рисования, не дублируясь по файлам.
`metric` — эйброу как подпись (или сам слот, если подписи нет), значение
`PlexMono28` через `layout::format_value` с дельтой (`widgets::prims::
format_delta`, если `delta != 0`), спарклайн — если `history_len ≥ 2`. `text`
— подпись крупно `Terminus20` полужирным, без данных.

## Потребности виджетов управляют опросом

`widgets::compute_demand(settings)` (`widgets/demand.h`) проходит по всем
инстансам **всех трёх дашбордов** (не только активного — переключение кнопкой
должно показать данные сразу, а не ждать первого опроса после смены экрана),
для каждого нужного слота находит через `connectors::provides(connector, slot)`,
какой коннектор его даёт, и берёт минимальный `refresh_seconds` среди всех
виджетов, которым нужен этот коннектор. `connectors::poll_due(settings, store,
now, demand)` коннектор без потребности не опрашивает вовсе; для нужного —
эффективный интервал `max(demand.refresh_seconds(id), Connector::interval)`:
виджет просит чаще, источник ограничивает снизу (Claude — не чаще раза в
300 с из-за TLS, почта — 900).

## Пример: «список дел» из TODO API одним файлом

```cpp
// widgets/w_todo.cpp
#include "widget.h"
#include "prims.h"

namespace widgets {
namespace {

const char* const kTodoSlots[] = {"todo.count", nullptr};

bool todo_visible(const slots::Store& store, const Instance&) {
    return layout::has_data(store.find("todo.count"));
}

void todo_draw(canvas::Canvas& c, const slots::Store& store, const layout::DeviceInfo& d,
               layout::Rect r, const Instance&) {
    prims::draw_eyebrow(c, r, "ДЕЛА");
    String value = layout::format_value(store.find("todo.count"), d.now, 0, "");
    fonts::draw_text(c, fonts::PlexMono28, r.x, r.y + 40, value.c_str(), canvas::Color::Black);
}

}  // namespace

extern const Spec kTodoSpec = {
    "todo", "Дела", Size::kS, 120, kTodoSlots, 900, &todo_visible, &todo_draw,
};

}  // namespace widgets
```

Плюс строка `{"todo", Size::kS, 120}` в `widgets/types.cpp`, `extern const
Spec kTodoSpec;` + запись в `kRegistry[]` в `widgets/registry.cpp` — источник
данных (`kind: "http"`, `map: [{"slot": "todo.count", "source": "..."}]`)
владелец заводит на странице настройки сам, без единой правки прошивки под
конкретный сервис.
