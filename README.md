# Silent Hill 1 — Nintendo Switch Port

Форк PC-порта Silent Hill, адаптированный для запуска на **Nintendo Switch** через devkitPro + libnx.

Ветка: `feature/switch-port`

---

## Статус

**Work in Progress.** Инфраструктура сборки готова, порт компилируется, но:
- [x] Компиляция под ARM64 (devkitPro + libnx)
- [x] Подключены SDL2, OpenAL, libjpeg-turbo (статическая линковка)
- [x] OpenGL ES 3.0 рендеринг через PsyCross
- [ ] Тестирование на реальном Switch
- [ ] Полная компиляция всех 42 карт (пока только map0_s00)
- [ ] Оптимизация под 720p / handheld
- [ ] Joy-Con / Pro Controller маппинг
- [ ] FMV-плеер под ARM
- [ ] Сохранение (Memory Card)

---

## Сборка

### Требования

- [devkitPro](https://devkitpro.org) с пакетами:
  ```
  dkp-pacman -S switch-dev switch-cmake ninja
  dkp-pacman -S switch-sdl2 switch-openal-soft switch-libjpeg-turbo
  ```
- CMake 3.16+, Ninja
- Git + submodule

### Сборка

```bash
git clone https://github.com/ChaykaDed/silent-hill-decomp-nx.git
cd silent-hill-decomp-nx
git checkout feature/switch-port
git submodule update --init --recursive

cd pc_port
./build_switch.sh rebuild
```

Результат: `pc_port/build_switch/SilentHillPC.nro`

---

## Управление (Switch)

На текущем этапе — стандартные PSX-кнопки через SDL. Joy-Con mapping будет добавлен позже.

| PSX Button | Switch |
|------------|--------|
| Cross | A |
| Circle | B |
| Triangle | X |
| Square | Y |
| Start | + |
| Select | - |
| D-Pad | D-Pad |
| L1 / R1 | L / R |
| L2 / R2 | ZL / ZR |

---

## Данные

Требуется оригинальный диск Silent Hill (PSX). Поместите BIN-образ в директорию `gamedata/` рядом с NRO-файлом.

---

## Структура

```
├── pc_port/                  # Порт-слой (PC/Switch)
│   ├── CMakeLists.txt        # Сборочная система
│   ├── build_switch.sh       # Скрипт сборки под Switch
│   ├── toolchains/           # Тулчейн devkitPro
│   ├── PsyCross/             # PSX HAL (сабмодуль)
│   └── src/                  # Stubs, переходники, FMV-плеер
├── src/                      # Декомпилированный код игры
│   ├── bodyprog/             # Движок
│   └── maps/                 # Карты
├── include/                  # Заголовки
└── SWITCH_PORT_README.md     # Подробная документация порта
```

---

## Благодарности

- Оригинальная декомпиляция: [Vatuu/silent-hill-decomp](https://github.com/Vatuu/silent-hill-decomp)
- PC-порт: [SlickAmogus/silent-hill-decomp](https://github.com/SlickAmogus/silent-hill-decomp)
- PsyCross: [OpenDriver2/PsyCross](https://github.com/OpenDriver2/PsyCross)
- devkitPro + libnx: [devkitPro](https://devkitpro.org)

Silent Hill © Konami. Репозиторий не содержит игровых ассетов.
