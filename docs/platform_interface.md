# Destinia v1.06: карта платформенного интерфейса (результат этапа 0)

APK SHA-256: `d9d10ec00d6c6852e1fb681c7aafa67bbc3ac5d795b2e436b3f56e6254b020cd` (см. `apk.sha256`).
Все факты проверены запуском оригинального `libdestinia_jni.so` в harness (`harness/`).

## 1. Библиотека

`lib/armeabi/libdestinia_jni.so`: ELF32 ARM, ARMv5TE, Thumb-1 + ARM, soft-float, зависимости `libc libstdc++ libm liblog libdl`.
Таблица символов **не вычищена**: 611 функций с именами (`GameCanvas_*`, `GameUI_*`, `Monster_*`, `MMain_*`, `Zlib_*`, `Sound_*`...), доступны через `.dynsym`.
Размеры: `.text` 0x3818c, `.data` 0xd54, `.bss` 0x48ecc (≈300 КБ). Куча после 120 кадров: ≈0.7 МБ живых, ≈2 МБ пик (суммарно с .bss менее 3 МБ).

### Импорты (всего 28, и это вся граница с ОС)

| Группа | Функции |
|---|---|
| Файлы | `fopen fclose fread fwrite fseek ftell remove` |
| Память | `malloc free memcpy memset` |
| Строки | `strlen strcpy strncmp strstr sprintf` |
| Время | `clock` (используется как единственный источник времени) |
| Лог | `__android_log_print` |
| Защита стека | `__stack_chk_guard` (data), `__stack_chk_fail` |
| ABI-хелперы | `__aeabi_idiv __aeabi_idivmod __aeabi_uidiv __aeabi_uidivmod __aeabi_ldivmod __aeabi_lmul __aeabi_unwind_cpp_pr0/pr1` |

Нет: потоков, сокетов, mmap, GL, звука, динамической загрузки. Поэтому для порта достаточно крошечной libc-прослойки.

## 2. Входы из Java в нативный код (JNI exports)

| Функция | Сигнатура (Java) | Смысл |
|---|---|---|
| `GameThread.initGame` | `(byte[] uid, int uidLen, byte[] model, int modelLen, int w, int h, byte isKorean)` | Инициализация. w=400, h=240. Внутри вызывает `setJNIClassMethod` и `WIPI_init`. Возвращает 3 |
| `GameThread.jniRun` | `(byte[] frame, int frameNo) -> int` | Один тик + отрисовка. `frame` = 192000 байт RGB565 LE. Возврат: задержка до следующего кадра в мс (**0 = выход из игры**, 1000 = пауза на загрузку, обычно 60) |
| `GameThread.jniTouch` | `(int x, int y, int type)` | type 0=down, 1=up, 2=move (-> `MMain_touch*`), **3 = нажатие клавиши 0x15 (Menu)**. Координаты в системе 400×240 |
| `GameThread.bannerShow` | `() -> int` | Состояние рекламы: 0 нет, 1 баннер, 2 реклама без баннера, 5 запрос GC, 10 экран «grade». На Switch игнорируем |
| `GameThread.setGamePlaying(byte)` | | Реакция на экран рейтинга (только корейская версия) |
| `GameThread.setEarnedCoin(int)` | | Начисление валюты из рекламы/Tapjoy. Игнорируем |
| `GameThread.setUniqueID(byte[], int)` | | Идентификатор устройства |
| `GameThread.procCommMsg / procCommRead` | `(int,int)->int`, `(byte[])->int` | Сетевые события. На Switch не вызываются |
| `CommManager.commCheck / commGetConType` | | Сетевой автомат (сервер `211.172.246.21:2031`, cash-сервер `218.145.70.36:31604`, оба мертвы). Не вызываем |

## 3. Колбэки нативного кода в Java (`setJNIClassMethod`: `FindClass` + `GetStaticMethodID`)

Все **статические**. Нужно реализовать на стороне порта:

| Класс.метод | Сигнатура | Реализация на Switch |
|---|---|---|
| `GameThread.getAssetResSize` | `([B)I` | Размер файла из `assets/` по имени (имя без пути, регистр как в APK) |
| `GameThread.getAssetRes` | `([B)[B` | Содержимое файла из `assets/` |
| `GameThread.playSound` | `(I)V` | id<100 = эффект `eff{id}.ogg` (50 = `eff50`), id≥100 = фон `bg{id}.mp3` (зациклен) |
| `GameThread.setSoundVolume` | `(III)V` | (тип 0=BG/1=SFX, текущая, максимум); в harness приходит 3/5 |
| `GameThread.playVib` | `(I)V` | Вибрация (мс). По желанию HID-вибрация |
| `GameThread.logEvent` | `([B)V` | Flurry-аналитика. No-op |
| `GameThread.gotoURL` | `([B)V` | Открыть ссылку. No-op |
| `CommManager.buyCash` | `(I)V` | Запуск покупки Cash. Решение по магазину пока открыто (см. §6) |
| `CommManager.close` | `()V` | Закрытие сокета. No-op |

Из реально встреченных за прогон 2400 кадров вызывались только `getAssetRes/Size` (239 раз), `playSound` и `setSoundVolume`.

### Минимальный набор JNIEnv/JavaVM

Использованные функции: `FindClass, NewGlobalRef, GetStaticMethodID, CallStaticIntMethod, CallStaticObjectMethod, CallStaticVoidMethod, GetArrayLength, GetByteArrayElements/ReleaseByteArrayElements (+ Region-варианты), GetJavaVM`, плюс `JavaVM->GetEnv/AttachCurrentThread`. Остальные записи таблицы никогда не вызываются (harness логирует каждый неизвестный вызов, их не было). Полноценная JVM не нужна.

## 4. Экран и ввод

- Framebuffer: **400×240, RGB565 little-endian, 192000 байт**. Нативный код пишет в массив, переданный в `jniRun`.
- `MC_GRP_GET_FRAME_BUFFER_*`, `MC_grpFlushLcd`: заглушки в самой библиотеке, отдельной реализации не требуют.
- Игра рисует **собственные экранные элементы управления** (виртуальный джойстик, кнопки, «SKIP», «назад») прямо в framebuffer. Для режима «только геймпад» их нужно либо скрыть, либо перекрыть своими. Решение в этапе 3.
- Ввод внутри игры клавишный: `MMain_keyPressed(int)` / `MMain_keyReleased(int)` (экспортированы). Касание переводится в код клавиши функциями `getKeyPadIdx` + таблицей `TOUCH_KEY` @0x443c8, и зависит от состояния экрана (`_getTouchState`).
- Подтверждённые коды: **0x11 = OK/выбор** (титул, меню, диалоги, выбор персонажа), 0x15 = Menu (через `jniTouch type 3`).
- Таблица `TOUCH_KEY` (35 значений): `2c 0f 10 0d 0e 11 1b 02 08 0b 04 0a 0c 0c 01 0c 02 03 04 05 06 07 08 09 0a 01 14 15 25 26 27 28 29 2a 2b`. Полная семантика кодов (направления, действия, умения) устанавливается в этапе 2 разбором `MMain_keyPressed` → обработчиков канвасов.

## 5. Файлы и время

- Сохранения: `fopen` с относительными именами в рабочей папке: `msave.dat` (метаданные), `save0.dat..save2.dat` (слоты). Порт должен отобразить их в `sdmc:/switch/destinia/`. Запись и чтение проверены.
- Время: только `clock()`. Игра ожидает микросекунды (CLOCKS_PER_SEC = 1 000 000, как в bionic). Harness использует **виртуальные часы**: после каждого `jniRun` время растёт на возвращённую задержку. Это даёт детерминизм.
- Все файлы ассетов и ресурсов приходят через `getAssetRes`: имена как на диске в `assets/` (`title.wmb`, `script_en.sdt`...).

## 6. Выводы для порта

1. Платформенный слой **очень узкий**: 28 импортов, 12 JNI-входов, 9 Java-колбэков, из них критичны 5.
2. Звук целиком на стороне платформы: нужны только декодеры OGG (эффекты) и MP3 (8 фоновых треков).
3. Объём памяти малый (менее 3 МБ), производительность ядра на Switch не должна быть проблемой (игра рассчитана на телефоны 2011 года).
4. Открытые вопросы: (а) судьба экранных кнопок в framebuffer; (б) покупка Cash (`buyCash`) и режим без сети; (в) коды клавиш для геймпада.

## 7. Воспроизведение (harness)

```
/usr/bin/python3 -I harness/run_frames.py --frames 700 --every 100 --script harness/scripts/newgame3.txt
/usr/bin/python3 -I harness/probe_keys.py          # поиск кодов клавиш
/usr/bin/python3 -I harness/contact.py out.png work/frames 80 100 120   # монтаж кадров
```

Эталон: `docs/golden/newgame3_700f.md5` (md5 каждого из 700 кадров сценария `newgame3.txt`). Проверено: два независимых прогона дают идентичные хеши.
Зависимости: `/usr/bin/python3` с `unicorn`, `capstone`, `Pillow`; распакованный APK в `work/apk/`.
