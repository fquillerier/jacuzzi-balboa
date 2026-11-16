# Jacuzzi Balboa GL/EL – control din telefon

Firmware ESP32 care se conectează la placa de comandă a unui jacuzzi Balboa din seriile GL/EL (GL2000, GL2001, GL8000, EL2000, EL8000…) prin portul de panou principal și oferă o pagină web din care jacuzzi-ul se comandă de pe telefon, fără Home Assistant.

Bazat pe proiectul [netmindz/balboa_GL_ML_spa_control](https://github.com/netmindz/balboa_GL_ML_spa_control) (decodarea protocolului, comunicația RS485), folosit cu acordul autorului.

## Ce face

- Pagină web pentru telefon: temperatura apei, temperatura dorită (+/−), încălzire, jeturi 1 și 2, bule (blower), lumină, mod Standard / Economic / Somn.
- Actualizare în timp real (WebSocket), fără reîncărcarea paginii.
- Configurare WiFi din telefon, fără parole în cod.
- Home Assistant prin MQTT, opțional.
- Actualizare firmware prin WiFi.

## Piese

- ESP32 DevKit (ESP-WROOM-32)
- Modul TTL–RS485 cu control automat al direcției (pinii TXD, RXD, VCC, GND, fără RE/DE)
- Modul step-down setat pe 5 V (dacă ESP-ul se alimentează din jacuzzi) sau alimentator USB 5 V
- Fire dupont mamă

## Legături

Pinii conectorului de pe placă, numerotați ca în poza din [wiki-ul proiectului original](https://github.com/netmindz/balboa_GL_ML_spa_control/wiki/Install-Guide) (1–2 sus, 7–8 jos), pe un port „Main panel” liber:

| Placa jacuzzi | Legat la |
|---|---|
| Pin 1 | RS485 B |
| Pin 3 | RS485 A |
| Pin 5 (panel select) | ESP32 GPIO18 |
| Pin 6 (≈9,7 V) | Step-down IN+ (doar dacă alimentezi din jacuzzi) |
| Pin 7 sau 8 (GND) | ESP32 GND și step-down IN− |
| Pin 2, 4 | Neconectați |

| Modul RS485 | ESP32 |
|---|---|
| RXD | GPIO19 |
| TXD | GPIO23 |
| VCC | 3V3 |
| GND | GND |

Step-down OUT+ → pinul 5V al ESP32, OUT− → GND. Verifică cu multimetrul că ieșirea e 5 V înainte de a lega ESP-ul. Nu alimenta ESP-ul simultan din step-down și din USB.

Întrerupe alimentarea jacuzzi-ului din tablou înainte de orice legătură.

## Instalare firmware

### Varianta simplă (din browser)

1. Descarcă `jacuzzi-full-install.bin` de la ultima versiune din [Releases](../../releases) (sau din tabul Actions → ultimul build → artefactul `jacuzzi-firmware`).
2. Conectează ESP32 la calculator prin USB și deschide în Chrome sau Edge: https://espressif.github.io/esptool-js/
3. Apasă **Connect**, alege portul ESP32.
4. La **Flash Address** scrie `0x0`, alege fișierul `jacuzzi-full-install.bin`, apasă **Program**.

### Varianta pentru dezvoltare (PlatformIO)

```
cd sensor
pio run -e esp32dev -t upload
pio device monitor
```

Pagina web se editează în `sensor/web/index.html`. Fișierul `sensor/src/webui.h` se regenerează automat la fiecare build.

## Prima pornire

1. Alimentează ESP-ul. Pe telefon apare rețeaua WiFi **Jacuzzi-Setup**. Conectează-te la ea.
2. Se deschide pagina de configurare (dacă nu, intră pe `http://192.168.4.1`). Alege rețeaua de acasă și scrie parola. Câmpurile MQTT rămân goale dacă nu folosești Home Assistant.
3. ESP-ul repornește și intră în rețeaua de acasă.
4. Pagina jacuzzi-ului: **http://jacuzzi.local** (sau adresa IP a ESP-ului din router).

Dacă rețeaua salvată nu e disponibilă la pornire, hotspot-ul Jacuzzi-Setup stă deschis 5 minute, apoi ESP-ul repornește și încearcă din nou.

## Folosire

- **Temperatura dorită:** apasă + sau −; după 1,5 secunde fără apăsări, valoarea se trimite. Dacă temperatura setată apare „Necunoscută”, o apăsare o citește din jacuzzi.
- **Jeturi, bule, lumină:** fiecare apăsare e echivalentă cu butonul de pe panou.
- **Setări și informații** (jos în pagină): versiune firmware, actualizare firmware, schimbarea rețelei WiFi.

## Actualizare firmware prin WiFi

Din pagină: **Setări și informații → Actualizare firmware** (sau `http://jacuzzi.local:8080/update`), apoi încarci `jacuzzi-update.bin`.

## API (pentru integrări)

| Adresă | Ce face |
|---|---|
| `GET /api/status` | Starea în JSON |
| `GET /api/cmd?a=up\|down\|light\|jet1\|jet2\|aux\|mode\|time` | O apăsare de buton |
| `GET /api/target?t=37.5` | Setează temperatura (26–40 °C, pas 0,5) |
| `GET /api/mode?m=0\|1\|2` | Standard / Economic / Somn |
| `POST /api/wifireset` | Șterge rețeaua WiFi și repornește în modul de configurare |
| WebSocket port 81 | Starea în timp real |

## Limitări cunoscute

- Comenzile sunt apăsări de buton simulate. Starea afișată e cea raportată de placa jacuzzi-ului, deci se actualizează după ce placa confirmă.
- Codurile pentru jeturi provin din proiectul original și sunt marcate acolo ca neverificate pe toate modelele.
- Starea bulelor (blower) nu e citită din mesajele plăcii; butonul doar comută.
- Temperatura setată se cunoaște doar după ce placa intră o dată în meniul de setare a temperaturii.
