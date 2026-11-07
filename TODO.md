# TODO

Krótka lista tego, co faktycznie zostało. Tu wpisujemy tylko rzeczy do
zrobienia i fakty potrzebne, żeby ich nie sprawdzać od nowa; zamknięta pozycja
znika stąd (historia jest w commitach).

## Otwarte — firmware

- [ ] **BLE + Sampler + pitched na długiej sesji** — dryf `psram_largest`
      (niżej „Fragmentacja”) przy BLE + pitched bez resetu. Wolnego PSRAM na
      launcherze jest mało — pilnować po każdej zmianie obrazu.
- [ ] **MIDI Clock, część STM** (`docs/subsystems/midi-clock.md` „Open”):
      (a) tempo hosta omija rescale sekwencera i może trafić do
      `sequences.json` przy nagrywaniu pętli z DAW — ścieżka tempa bez zapisu;
      (b) klik dryfuje od DAW ~0,08 beatu / 4 min przy 90 BPM — phase-lock do
      beatów hosta; (c) gdy zegar dojdzie do portu USB STM, host dostanie
      własny zegar drugim portem — rozstrzygnąć w specu; (d) dowód, że
      `STM_FW_UPDATE ... FORCE` przy chodzącym zegarze jest bezpieczny.
- [ ] Ustawienia płytki ławki potrafią wrócić do domyślnych (`USB_GUARD 1`,
      BLE off), choć nic w hil nie woła `SETTINGS_RESET`; sprawdzić
      `bench.py recover`/UART.
- [ ] **IMU: 6D, wybudzenie z LIGHT i double tap w graniu — przy płytce.**
      Sprawdzić, czy granie na padach nie przełącza Advanced w samplerze i czy
      double tap trafia za każdym razem; zostaje też 6D przy obracaniu
      i podniesienie płytki z LIGHT. Dowód: `bench.py hil cdc IMU` + `ENC_GROUP`.
- [ ] **IMU przestaje odpowiadać po kilku minutach pracy i nie wraca.** Gałąź
      odzyskiwania w sterowniku działa (re-init przechodzi), ale odczyt danych
      dalej pada — więc to stan samego układu albo konfiguracja gubiona przy
      re-incie, nie zwykły konflikt na szynie. Następny krok: zrzucić
      `WHO_AM_I`, `CTRL1_XL/CTRL2_G` i `STATUS_REG` w momencie porażki.
- [ ] back_churn: Sampler `~130 B/visit` internal (16 uderzeń + back), równo
      co rundę; 1 uderzenie × 6 wizyt = 0 B. Następny trop: głos grający przy
      zamykaniu Samplera (FILE/lock w internal).
- [ ] **Późny start 40–260 ms** raz na swap kitu: hit w pad będący właśnie
      przebudowywany jest przytrzymany i odegrany po reloadzie (`late_hits`).
      Okno = WipeOut + SetupSample (~96 KB tempowanego odczytu), na 4-bit SD
      ~4× krótsze. Do decyzji: próg `kit_swap_audio --max-late-ms` per ławka
      albo dalsze skracanie okna.
- [ ] **„Późny start” hitu podczas ładowania kitu** — `platform_noteOn` →
      `SampleStreamPlayer_NoteOn` z tasku UART/hil_pad czeka do 1,1 s na SD,
      gdy loader zajmuje kartę; port powinien odłożyć hit zamiast blokować
      (`main/gui/gui.cpp`, `sample_player_port.cpp`).
- [ ] **`sdmmc_read_sectors: not enough mem` raz, w boocie po OTA**, gdy
      ładował się kit: 512 B z `MALLOC_CAP_DMA` nie dostało bloku w tamtej
      chwili (`int_min` 43 kB, więc nie brak internal ogółem). Obserwować po
      każdym OTA (`smoke` po `flash` łapie to w `errors`).
- [ ] **Klatki LVGL do 385 ms podczas grania w Samplerze** (`LVGL_STATS`
      `refr_max_us`, `CDC_STATS maxcmd=PAD_PRESS` = pętla prio 1 wywłaszczona
      przez render). Zmierzyć, co unieważnia ekran przy hicie.
- [ ] **`espressif/esp-now` jest kompilowany i nigdy nie inicjalizowany.**
      W obrazie to 260 B, więc wycięcie nie odda PSRAM; decyzja: wyciąć
      zależność czy dopisać inicjalizację (ESP-NOW jest na liście funkcji
      sprzętu).
- [ ] 4 hit/s × 2 s tony (8 głosów streamujących) — zmierzyć pasmo karty na
      4-bit SD.
- [ ] **Próg `sampler_record` vs rozmiar kroku.** Krok 3 s przy 2 hit/s to
      6 trafień, więc `min_audible 0.9` wymaga detektora bez ani jednego pudła.
      Opcje: minimalna liczba trafień, zanim krok jest oceniany, albo werdykt
      z sumy po krokach. Przy kitach z długimi samplami sprawdzić najpierw
      detektor onsetów.
- [ ] **ESP wiesza się przy wyjściu z profilu audio po wielu minutach w nim.**
      Ścieżka SysEx w profilu audio jest zdrowa; zawiesza się dopiero
      przełączenie profilu: log kończy się na `usbd_uac: UAC Device
      deinitialized`, a `usb_audio_bridge_deinit()` nigdy nie dochodzi do
      swojego `Deinitialized` — czyli `usb_audio_port_stop()` albo
      `switch_i2s_to_default()` (`main/usb_audio_bridge.c`). Czynnikiem jest
      długość pobytu, nie strumień.
- [ ] ESP potrafi przestać nadawać na USB MIDI OUT (RX i CDC żyją,
      `usb_drop=0` w `PAD_STATS`); ratunek `console --reset`. Ramki SysEx
      ponad 256 B są cięte z założenia (`SysExMaxSize = 256` w
      `main/midiCallbacks.cpp`) — dłuższe wymagają podniesienia stałej albo
      dzielenia po stronie hosta.
- [ ] **Fragmentacja `psram_largest`**: świeży launcher ma largest ~249 kB, po
      sesji z Samplerem/pitched spada do ~112 kB i nie wraca mimo
      `LoadMainScreen()` zrzucającego cache obrazów; free wraca normalnie.
      Zbadać arenę/alokator obrazów (`main/lvgl_mem_psram.c`, `engine_arena.cpp`).
- [ ] Tor USB audio: opisy „FIFO count steruje hostem” w
      `usb_audio_port.h`, `UsbAudioPort.cpp` i `docs/subsystems/audio-routing.md`
      są nieaktualne (feedback liczy się z cykli CPU); kontrola końca
      `.flash.text` względem granicy strony PSRAM po buildzie (przekroczenie
      jest ciche: +64 kB PSRAM); `uac_device_deinit()` najpierw odcina ścieżkę
      ISR (`spk_task_handle = NULL` pod `s_mux` przed `vTaskDelete`).

## Wymaga rąk na ławce (nie zrobi tego żaden HIL)

- [ ] **Haptyka DRV2605**: auto-kalibracja potrafi skończyć się `DIAG=1 OC=1`
      (haptyka jest wtedy wyłączana). Sprawdzić LRA / nastawy (`RATEDV 0x3F`,
      `CLAMPV 0x89`, biblioteka 6).
- [ ] **Dotyk** — zero pokrycia HIL; przeklikać appki palcem (churny testują
      tylko enkoder).
- [ ] **Gesty guzika power palcem** wg tabeli w `docs/subsystems/power-button.md`;
      potwierdzić na płytce po in-field update STM (`STM_FW_INFO`).
- [ ] **Głośność uchem** (mapowanie z zaokrąglaniem, kroki ±0,9 dB) — jeśli dół
      skali pusty, następna dźwignia to zakres/krzywa.
- [ ] Webflasher: choosery Web Serial/USB na telefonie (Chrome/Android).
- [ ] IMU → MIDI: przechylić płytkę i posłuchać w DAW.

## Appki i narzędzia

- [ ] Migracja Samplera i Sequencera na `REGISTER_APP_PL`.
- [ ] DAW Control: Ableton Remote Script na tym samym protokole; tryby
      `steps`/`clips`.
- [ ] **Flash partycji assets zaraz po systemowej**: reset w oknie trial =
      rollback, więc assets przed obrazem albo po confirm — sprawdzić na płytce.
- [ ] crosspad-pc: `audio_level` (port 19840) zwraca 0.0 dla obu strumieni
      także przy kicie samplerowym; VU w simie do sprawdzenia.

## Backlog techniczny

**Silnik pitched:** GUI (badge w selektorze, edytor stref, MIDI-in
note→engine); mip-strefy anty-aliasowe z FFT przy ładowaniu; SIMD pętli
Hermite (>18 głosów); strefa >0,5 s przy resamplingu pada na scratch PSRAM
(resampler blokowy do pliku/strumienia); `kMaxBankBytes` walidować per
alokacja vs realny largest block po selektorze.

**Audio / USB:** strumień z hosta jako węzeł miksera (dziś tryb USB-audio
parkuje mikser RT: w DAW pady nieme bez `0x1D 06 01`, a z wznowionym
mikserem dwóch pisarzy na I2S TX); pojedyncze trzaski w części cykli
`audio_loopback` (WARN); underruny streamingu przy wielu głosach z różnych
plików w prebuilt silniku.

**Kit browser:** render listy ~3 µs/px w silniku SW (opcje: esp_lvgl_port
z SIMD dla LVGL 9.3, mniej obiektów na wiersz, leniwe kciuki w oknie ±2
ekrany, wirtualizacja przy >150 kitów); cache indeksu kitów na SD.

**HIL:** `d2spikes`/`pitch_cmp` jako kernele `crosspad-hil analyze` +
scenariusz `audio_fidelity` na kicie HIL TONE; `sampler_record --rates`
tylko int; usunąć shimy `tools/hil_*.py`.

**Framework / cleanup:** Sequencer `lv_Sequencer_destroy` zdublowane bloki;
4 wzorce grupy enkodera → jeden portable.

**CI / infra:** CI dla PC i testów core.

**STM / produkcja:** `REG_IDENT_PCB_REV` z OTP (zapisać rewizję przy
produkcji, żeby gate PCB nie polegał na buildzie STM).

## Fakty (żeby nie sprawdzać od nowa)

- `crosspad-hil run smoke` w maszynie wirtualnej Windows kończy się exit 2
  „No boot banner on the console port”, choć płytka odpowiada przez CDC:
  konsola przez passthrough USB w QEMU nie daje banera po resecie — smoke rób
  na hoście.
- Zanim ruszysz ławkę, sprawdź, czy DAW (np. FL pod Wine) nie trzyma karty
  (`pgrep -f FL64|wine`, `pw-link -l`): grab karty/portu rozwala żywą sesję.
- audio_loopback pada, gdy węzły DAW PipeWire trzymają kartę: sink CrossPada
  jest zawieszony w profilu audio, więc odtworzenie tonu wisi 25 s. exit=2
  (ławka), nie firmware — zatrzymać usługę węzłów DAW na czas scenariusza.
- `AUDIO_LEVEL`/VU/`MIX_LVL` peak to peak z OSTATNIEGO bloku renderu (~1,5 ms),
  nie akumulat. Pojedynczy krótki hit z CDC (latencja ~100 ms) wybrzmiewa przed
  odczytem → miernik 0 mimo że grało. Mierz przy CIĄGŁYM dźwięku (STM_VPAD),
  albo patrz na `SMPL_PEAK` (read-and-clear, akumuluje do odczytu).
- Nagranie przez PipeWire (karta zajęta przez węzły DAW) chodzi na zegarze grafu,
  nie karty: ~0,1 s dryfu na 16 s. Floor/ciszę mierz na początku nagrania.
- „No reply within 2 s” przy małych `maxcmd_us`/`maxgap_us` = pętla CDC (prio 1,
  core 0) nie dostaje CPU: `maxgap` liczy się tylko, gdy kolejka już czekała.
  Patrz `LVGL_STATS render_us` (render ciągły) i kto siedzi na core 0 z prio > 1.
- `CONFIG_HEAP_TASK_TRACKING` z BT w buildzie kończy każdy boot paniką
  `LoadProhibited` przy inicjalizacji koegzystencji BT — atrybucja sterty per
  zadanie tą drogą jest zamknięta.
- Blok 69 632 B w internal RAM to pula `CONFIG_SPIRAM_MALLOC_RESERVE_INTERNAL`
  (64 KiB). Obniżanie rezerwy jest ryzykowne — z niej czerpią I2S, SDMMC i USB.
- Bootloopy, które wymagały flasha przez UART: `LV_CACHE_DEF_SIZE` < splash;
  `LV_OS_FREERTOS` + 2 draw units; skan kitów przed alokacją puli silnika.
  Ratunek: `idf.py -p <PORT> flash` przez most STM (konsola STM VCP
  `0x0483:5740`, DTR/RTS resetuje ESP).
- Buildy w worktree TYLKO z `-DSDKCONFIG=…` — untracked `sdkconfig` daje obraz
  rev1 i wygląda jak padły STM.
- Web Bluetooth z Chrome na Linuksie: (1) jest wyłączony domyślnie —
  `navigator.bluetooth` undefined do `chrome://flags/#enable-web-bluetooth`
  (Playwright: `--enable-features=WebBluetooth,WebBluetoothNewPermissionsBackend`,
  drugi daje `getDevices()`). (2) bluetoothd zjada serwis BLE MIDI (port ALSA
  „CrossPad Bluetooth”) i nie eksportuje go po D-Bus — Chrome i bleak widzą
  tylko serwis kontrolny `7e400001`; companion łączy przez niego. `BLE_START 1`
  tuż po `BLE_STOP` odmawia przez chwilę — pollować.
