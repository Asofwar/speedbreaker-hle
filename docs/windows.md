# Windows

This fork has a Windows build: one `SpeedBreaker.exe`, built for x86-64 Windows 10 and 11. It
started from [rafidwayne's port](https://github.com/SpeedBreakerProject/speedbreaker/pull/39) to
the original project, and plays the same game as the Linux and Steam Deck builds: a race replay
gives the same game state, bit for bit, on Windows, on the Deck and on Linux.

## Playing

1. Download `SpeedBreaker-windows-x86_64.zip` from the
   [Releases page](https://github.com/Asofwar/speedbreaker-hle/releases) and unzip it anywhere you
   can write to (your Games folder, another drive; not Program Files).
2. Start `SpeedBreaker\SpeedBreaker.exe`. The first start asks for your own USA (NTSC-U) disc
   image, `.iso`, or the files extracted from it. It looks in your user folders (Downloads,
   Desktop, Documents, Games) and at the top of every local drive and its `Games` folder;
   *Browse for image...* finds it anywhere else. No game files come with the download.

You need a graphics driver with Vulkan: any NVIDIA, AMD or Intel driver of the last few years.
There is nothing else to install. The exe has no DLLs of its own; `vulkan-1.dll` comes with the
driver.

Windows may warn that the app is from an unknown publisher (SmartScreen): the exe isn't signed.
*More info*, then *Run anyway*.

Where things are:

| What | Where |
|---|---|
| Saves, settings, the installed game | `%LOCALAPPDATA%\SpeedBreaker` |
| Logs (attach one to a bug report) | `%LOCALAPPDATA%\SpeedBreaker\logs` |
| Shader caches (safe to delete) | `%LOCALAPPDATA%\SpeedBreaker\cache` |

Settings, controls and online play work as on the other platforms: F1 (or Back + Start on a
controller) opens the settings, and [docs/online.md](online.md) covers racing with others. With
Connection set to Steam, the game uses `steam_api64.dll` from one of your installed Steam games;
Steam's folder and its other libraries are found from the registry.

Check for Updates doesn't work on Windows yet: get new versions from the Releases page.

## What the port changes

Since upstream's own Windows port (v0.1.9 Windows beta 1) most of the Windows code is upstream's
platform layer, `runtime/platform/win32`: guest memory mapped the way the game expects, the write
watch's page faults, files, threads, timers, crash reports. Upstream builds it with clang-cl and
the MSVC runtime; this fork cross-builds the same code with MinGW-w64 (below), and keeps a little
of its own in `runtime/compat/win`: sockets for the online code on Winsock, the GPU thread's
doorbells, a high-resolution `nanosleep`, and the POSIX headers MinGW lacks.

A couple of things behave differently on Windows and are handled there:

- **Sleeping.** The usual C++ sleep on Windows (winpthreads' `nanosleep`) waits in whole system
  timer ticks of 15.6 ms and doesn't sleep at all below a millisecond. The game's GPU thread
  polls every 0.1 ms and its audio ticks every 5.3 ms, so frames stalled and sound chopped.
  `compat/win/win_sleep.c` replaces it with a high-resolution timer (within about 0.5 ms), the
  process asks for a 1 ms system timer, and the GPU thread's doorbells use `WaitOnAddress`.
- **Saving.** A file is saved by writing a copy and renaming it over the old one. Windows'
  `rename` refuses an existing target, so `compat/win` uses `MoveFileEx` with
  `MOVEFILE_REPLACE_EXISTING` instead.
- **UDP.** Winsock fails the next receive on a UDP socket after an ICMP "port unreachable";
  Linux doesn't. The race's sockets turn that off.

## Building

The Windows build is cross-compiled on Linux (Arch Linux here, the `sb-build` container) from
the same checkout as the Linux build, with clang, lld and mingw-w64:

```bash
pacman -S --needed mingw-w64-gcc clang lld cmake ninja git curl
scripts/setup_tools.sh && scripts/recompile.sh   # as for Linux: tools/ and ppc/
scripts/windows/deps.sh ~/win-deps               # SDL3, Vulkan, glslang, SPIRV-Tools, FFmpeg-XMA
scripts/windows/build.sh ~/win-deps              # build-windows/runtime/SpeedBreaker.exe
scripts/windows/package.sh ~/win-deps build-windows dist/windows
```

`deps.sh` builds the libraries at the Linux build's versions, statically, into the prefix you
give it, and keeps what is already there. `package.sh` makes the zip with the licenses of
everything compiled into the exe. `scripts/steamos/release_hle.sh <tag> build --windows
~/win-deps` puts the zip in a fork release.

To try a build without installing: `NFSMW_GAME_DIR=<folder with default.xex>` points the exe at
a game folder you already have.

## По-русски

У форка есть сборка под Windows: один `SpeedBreaker.exe` для 64-битных Windows 10 и 11. Начало
ей дал [порт rafidwayne](https://github.com/SpeedBreakerProject/speedbreaker/pull/39) для
исходного проекта. Игра та же, что на Linux и Steam Deck: повтор гонки даёт одно и то же
состояние игры бит в бит на Windows, на Деке и на Linux.

Как играть:

1. Скачайте `SpeedBreaker-windows-x86_64.zip` со страницы
   [релизов](https://github.com/Asofwar/speedbreaker-hle/releases) и распакуйте туда, где можно
   писать (папка с играми, другой диск; не Program Files).
2. Запустите `SpeedBreaker\SpeedBreaker.exe`. При первом запуске он попросит образ вашего диска
   (США, NTSC-U): `.iso` или распакованные из него файлы. Ищет в папках пользователя (Загрузки,
   Рабочий стол, Документы, Games) и в корне каждого локального диска и его папке `Games`; если
   образ лежит в другом месте, нажмите *Browse for image...*. Файлов игры в сборке нет.

Нужен драйвер видеокарты с Vulkan, подойдёт любой NVIDIA, AMD или Intel последних лет. Больше
ничего ставить не нужно: своих DLL у игры нет, `vulkan-1.dll` приходит с драйвером. Windows
может предупредить о неизвестном издателе (SmartScreen), потому что exe не подписан: *Подробнее*,
затем *Выполнить в любом случае*.

Сохранения, настройки и установленная игра лежат в `%LOCALAPPDATA%\SpeedBreaker`, логи в
`logs`, кэш шейдеров в `cache` (его можно удалять). Настройки, управление и онлайн такие же,
как на других платформах: F1 (или Back + Start на геймпаде) открывает настройки, про игру по сети
написано в [docs/online.md](online.md). С Connection: Steam игра берёт `steam_api64.dll` из любой
установленной Steam-игры, папку Steam и её библиотеки находит по реестру. Проверка обновлений на
Windows пока не работает, новые версии берите со страницы релизов.

Сборка: кросс-компиляция на Linux тем же деревом, что и Linux-сборка (команды выше, раздел
*Building*).
