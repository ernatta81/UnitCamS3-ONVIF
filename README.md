# UnitCamS3 – RTSP / ONVIF

Firmware PlatformIO per **M5Stack UnitCamS3** (ESP32-S3 + OV2640) che trasforma la camera in una IP camera di rete:

- **RTSP** – `rtsp://<ip>:554/mjpeg/1`, video **MJPEG su RTP** (RFC 2435), trasporto UDP o TCP interleaved, fino a 4 client contemporanei
- **ONVIF Profile S (minimo)** – WS-Discovery + servizi Device/Media: la camera compare nella ricerca automatica degli NVR/VMS, che ricavano da soli l'URL RTSP e quello dello snapshot
- **HTTP** – pagina web di configurazione con anteprima live, snapshot JPEG `/api/v1/capture`, stream MJPEG `/api/v1/stream` (API camera riprese dal demo ufficiale)

Basato su [UnitCamS3-UserDemo](https://github.com/m5stack/UnitCamS3-UserDemo): stessa board, stesse impostazioni flash/PSRAM, stesso pinout camera e stesse librerie async.

## Aprire il progetto in VS Code

1. Installa l'estensione **PlatformIO IDE** (VS Code la propone da sola, è in `.vscode/extensions.json`).
2. `File → Open Folder…` → seleziona la cartella `UnitCamS3-ONVIF` (quella con `platformio.ini`).
3. Dalla barra di PlatformIO: **Build**, poi **Upload** (stessa procedura di caricamento usata per il demo), poi **Monitor** a 115200 baud.

Oppure da terminale:

```bash
pio run -t upload
```

```bash
pio device monitor
```

## Configurazione Wi-Fi

Al primo avvio (o se la rete salvata non è raggiungibile entro 30 s) la camera crea un access point aperto **`UnitCamS3-xxxxxx`**:

1. Collegati all'AP e apri `http://192.168.4.1/`
2. Inserisci SSID e password della tua rete, risoluzione, qualità, fps e (consigliato) una password di accesso → **Save and reboot**
3. La camera si collega alla rete. La trovi su `http://unitcams3.local/` (mDNS) oppure leggi l'IP dal monitor seriale:

```
web:   http://192.168.1.50/
rtsp:  rtsp://192.168.1.50:554/mjpeg/1
onvif: http://192.168.1.50/onvif/device_service
```

Se l'AP di setup resta inutilizzato per 5 minuti e una rete è già salvata, la camera si riavvia e riprova (utile se il router era solo spento).

In alternativa puoi precompilare `data/config.json` e caricarlo con **Upload Filesystem Image**:

| Chiave        | Default     | Note                                                                 |
|---------------|-------------|----------------------------------------------------------------------|
| `wifiSsid`    | `""`        |                                                                      |
| `wifiPass`    | `""`        |                                                                      |
| `hostname`    | `unitcams3` | nome mDNS e nome ONVIF                                               |
| `frameSize`   | `VGA`       | `QVGA` `CIF` `HVGA` `VGA` `SVGA` `XGA` `HD` `SXGA` `UXGA`             |
| `jpegQuality` | `12`        | 6 = qualità massima, 63 = file più piccoli                           |
| `fps`         | `15`        | fps massimi dello stream RTSP (1–30)                                 |
| `vflip`       | `true`      | come il demo                                                         |
| `hmirror`     | `true`      | come il demo                                                         |
| `authUser`    | `admin`     | utente per pagina web, RTSP e ONVIF                                  |
| `authPass`    | `""`        | vuoto = accesso libero, altrimenti protezione attiva                 |

## Password

Dalla sezione **Access** della pagina web imposti utente e password, che valgono per tutto:

- **Pagina web, snapshot, MJPEG**: HTTP Digest (il browser chiede le credenziali).
- **RTSP**: Digest (anche Basic è accettato). Nei client l'URL diventa `rtsp://admin:password@<ip>:554/mjpeg/1`.
- **ONVIF**: WS-Security UsernameToken (PasswordDigest o PasswordText) oppure HTTP Digest. Nell'NVR inserisci le stesse credenziali quando aggiungi la camera. Come da specifica ONVIF, `GetSystemDateAndTime`, `GetCapabilities`, `GetServices`, `GetServiceCapabilities` e `GetHostname` rispondono anche senza login.

Con la password vuota (o la casella *No password, open access*) tutto torna ad accesso libero.

**Password dimenticata**: ricarica il filesystem con **Upload Filesystem Image** (usa `data/config.json`, con `authPass` vuota). La configurazione torna quella del file, quindi va reinserita anche la rete Wi-Fi, a meno che non la scrivi nel file prima di caricarlo.

## Test rapido

- **VLC**: `Media → Apri flusso di rete` → `rtsp://<ip>:554/mjpeg/1`
- **ffmpeg / ffplay**:

```bash
ffplay -rtsp_transport tcp rtsp://admin:password@192.168.1.50:554/mjpeg/1
```

- **ONVIF Device Manager** (Windows): la camera deve comparire da sola nella lista; inserisci utente e password in alto a sinistra (senza password impostata va bene qualsiasi valore).

## Compatibilità e limiti (importante)

- **Il video è MJPEG, non H.264.** L'ESP32-S3 non ha un encoder H.264 hardware, quindi la camera manda JPEG. Funziona con VLC, ffmpeg, Blue Iris, Frigate (in ingresso), Synology Surveillance Station, ONVIF Device Manager, Home Assistant *Generic Camera*.
  Molti NVR commerciali (Hikvision, Dahua, UniFi Protect…) e l'integrazione **ONVIF** di Home Assistant accettano **solo H.264/H.265**: con questi la camera viene trovata ma il video non parte.
  Soluzione: mettere in mezzo [go2rtc](https://github.com/AlexxIT/go2rtc) (o ffmpeg) che legge l'RTSP MJPEG e lo ripubblica in H.264, per esempio:

  ```yaml
  streams:
    unitcams3: ffmpeg:rtsp://192.168.1.50:554/mjpeg/1#video=h264
  ```

- **Autenticazione** disattivata finché non imposti una password. Anche con la password attiva il traffico non è cifrato (niente HTTPS/RTSPS): il video viaggia in chiaro, quindi la camera va comunque tenuta su una rete fidata e non esposta su Internet. Per semplicità non c'è controllo anti-replay sul timestamp dei token WS-Security.
- **Un solo profilo** ONVIF (JPEG, risoluzione corrente). Le modifiche di encoder via ONVIF (`SetVideoEncoderConfiguration`) vengono accettate ma ignorate: risoluzione/qualità/fps si cambiano dalla pagina web.
- Niente audio, PTZ, eventi, imaging service.
- Risoluzione massima `UXGA` (1600x1200): RTP/JPEG non supporta lati oltre 2040 px.
- Più client e risoluzioni alte saturano rapidamente il Wi-Fi: per un uso continuo `VGA` a 10–15 fps è il compromesso migliore.

## Struttura

```
UnitCamS3-ONVIF/
├── platformio.ini          board esp32s3box (stesso modulo della UnitCamS3), PSRAM, LittleFS
├── partitions.csv          tabella partizioni del demo
├── data/config.json        configurazione opzionale da caricare nel filesystem
├── lib/                    AsyncTCP + ESPAsyncWebServer (copiate dal demo)
└── src/
    ├── main.cpp
    ├── app_config.h        porte, path RTSP, timeout, info dispositivo
    ├── hal/                camera, LED, config persistente (da hal del demo)
    ├── net/                Wi-Fi STA + AP di setup, mDNS, NTP
    ├── servers/            server HTTP: pagina web, API, endpoint ONVIF
    ├── apis/camera/        snapshot / MJPEG / controlli sensore (dal demo)
    ├── rtsp/               server RTSP + pacchettizzazione RTP/JPEG
    ├── onvif/              servizi SOAP Device/Media + WS-Discovery
    └── utils/              verifica credenziali (Digest, Basic, WS-Security)
```

## Endpoint

| URL                                   | Descrizione                                   |
|---------------------------------------|-----------------------------------------------|
| `rtsp://<ip>:554/mjpeg/1`             | stream RTSP (qualunque path è accettato)      |
| `http://<ip>/onvif/device_service`    | servizio ONVIF Device                         |
| `http://<ip>/onvif/media_service`     | servizio ONVIF Media                          |
| `http://<ip>/`                        | pagina di configurazione e anteprima          |
| `http://<ip>/api/v1/capture`          | snapshot JPEG                                 |
| `http://<ip>/api/v1/stream`           | stream MJPEG via HTTP                         |
| `http://<ip>/api/v1/status`           | stato sensore (JSON)                          |
| `http://<ip>/api/v1/control?var=&val=`| regolazioni sensore al volo (come nel demo)   |
| `http://<ip>/api/v1/info`             | stato dispositivo e URL (JSON)                |
