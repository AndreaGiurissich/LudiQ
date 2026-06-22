/*
 * cubo_unificato.ino — Cube Tournament Controller (SCHELETRO v0.4)
 * -----------------------------------------------------------------
 * Macchina a stati + menu + RNG + POKEMON (tetto 50') + SCACCHI
 * (classical/rapid/blitz) + NFC (identificazione giocatori)
 * + HOSTLINK via WiFi/WebSocket verso il PC (server.js).
 * Ancora SENZA: gesti IMU (l'hook e' gia' segnato).
 *
 * NOVITA' v0.4:
 *   - WiFi + WebSocket: il cubo si connette a server.js come client
 *     e gli manda gli eventi (hello con cube/table, heartbeat, e
 *     player_tag / match_start / match_end). Riceve anche comandi
 *     dall'host (es. 'beep'). Configura rete e IP nei #define sotto.
 *   - hostSend() ora spedisce su WebSocket (con eco su Serial @HOST).
 *
 * NOVITA' v0.3:
 *   - Il PN532 legge i tag in ST_PLAYERS: primo tap = P1, secondo
 *     tap (tag DIVERSO) = P2. Tessera e portachiavi vengono
 *     riconosciuti per UID dalla tabella KNOWN_TAGS qui sotto.
 *   - Ogni evento per l'host esce su Serial come riga JSON con
 *     prefisso "@HOST ". Quando ci sara' il WiFi cambieremo SOLO
 *     il corpo di hostSend() per spedire su WebSocket.
 *
 * Flusso:  SPLASH -> MENU -> GIOCATORI (NFC!)
 *            Pokemon: -> MATCH (tempo totale max 50 min in alto)
 *            Scacchi: -> SETUP FORMATO -> MATCH (orologio a scalare)
 *          -> FINE MATCH -> MENU
 *          RNG apribile quasi ovunque con ANNULLA tenuto premuto.
 *
 * Comandi:
 *   SX / DX           naviga, cambia selezione, +/- prize
 *   OK                conferma (scacchi: avvia/pausa orologio)
 *   ANNULLA           indietro / esci
 *   ANNULLA (lungo)   apre l'RNG   <- sostituto temporaneo dello shake
 *   'p' da seriale    passa turno / completa la mossa
 *                     <- sostituto temporaneo della rotazione del cubo
 *
 * Driver NFC: mini-driver PN532-I2C scritto INLINE in questo file (la
 *   classe MiniPN532 piu' sotto), condensato dalla libreria Seeed-Studio
 *   /PN532 (licenza BSD: (c) Adafruit, (c) Seeed). Cosi' non serve
 *   installare NIENTE: niente libreria, niente file esterni. In I2C
 *   interroga il bus per sapere quando la risposta e' pronta, percio'
 *   NON serve il pin IRQ (a differenza della Adafruit su ESP32).
 * Libreria WiFi: "WebSockets" di Markus Sattler (Links2004). Questa
 *   SI' dal Library Manager: cerca "WebSockets" e prendi quella di
 *   Links2004. Il cubo e' il CLIENT, server.js sul PC e' il server.
 *
 * IMPORTANTE: i pin del display NON sono in questo sketch! Stanno
 * nello User_Setup.h della libreria TFT_eSPI. Per i numeri grandi
 * serve LOAD_FONT6 attivo (di default lo e').
 *
 * Pin usati qui:
 *   Pulsanti: OK 25 | ANNULLA 26 | SX 27 | DX 14 (verso GND, pullup interno)
 *   LED 13, Buzzer 33
 *   I2C: SDA 21, SCL 22 (PN532 a 0x24, MPU6500 a 0x68)
 *   PN532: bastano SDA/SCL/VCC/GND, NIENTE IRQ (vedi nota libreria sopra)
 */

#include <TFT_eSPI.h>
#include <esp_system.h>
#include <WiFi.h>
#include <WebSocketsClient.h>
#include <Wire.h>
#include "FastIMU.h"          // MPU6500 per i gesti (stesso bus I2C del PN532)
// Il driver NFC e' piu' sotto, scritto inline (classe MiniPN532):
// niente libreria da installare, niente file esterni.

TFT_eSPI tft = TFT_eSPI();

// ----------------------------- Pin -----------------------------
#define PIN_LED   13
#define PIN_BUZZ  33
#define I2C_SDA   21
#define I2C_SCL   22
// La libreria Seeed in I2C fa polling sul bus: niente pin IRQ/RESET
// da collegare. Bastano i 4 fili SDA/SCL/VCC/GND gia' presenti.

const uint8_t BTN_PIN[4]  = { 25, 26, 27, 14 };   // OK, ANNULLA, SX, DX
const char*   BTN_NAME[4] = { "ok", "annulla", "sx", "dx" };

#define DEBOUNCE_MS   30
#define LONGPRESS_MS  800

// ---------------------- WiFi / Host (WebSocket) -----------------
// Il cubo si connette come CLIENT al server.js che gira sul PC.
// METTI QUI i tuoi dati. L'IP e' quello del PC che fa girare
// server.js: trovalo con 'ipconfig' (Windows) o 'ip a' (Linux/Mac),
// e' tipo 192.168.x.x (NON 127.0.0.1!). Cubo e PC sulla STESSA rete.
#define WIFI_SSID    "TUO_SSID"
#define WIFI_PASS    "TUA_PASSWORD"
#define HOST_IP      "192.168.1.100"   // <-- IP del PC con server.js
#define HOST_PORT    8080              // = PORT dentro server.js
#define CUBE_ID      "cubo-01"         // come il cubo si presenta all'host
#define TABLE_NUM    1                 // numero del tavolo di questo cubo
#define HEARTBEAT_MS 5000              // ogni quanto dire "sono vivo" [ms]

// ------------------------ Stati e giochi ------------------------
enum State    { ST_SPLASH, ST_MENU, ST_PLAYERS, ST_CHESS_SETUP,
                ST_MATCH, ST_MATCH_END, ST_RNG };
enum BtnEvent { EV_NONE, EV_OK, EV_CANCEL, EV_LEFT, EV_RIGHT, EV_CANCEL_LONG, EV_PASS, EV_SHAKE };
enum Game     { GAME_POKEMON, GAME_YUGIOH, GAME_SCACCHI, GAME_COUNT };

const char* GAME_NAME[GAME_COUNT] = { "POKEMON TCG", "YU-GI-OH!", "SCACCHI" };
const char* GAME_ID[GAME_COUNT]   = { "pokemon", "yugioh", "scacchi" };  // per il JSON
// Palette: un colore accento per ogni gioco (pokemon, yugioh, scacchi)
const uint16_t GAME_ACCENT[GAME_COUNT] = { TFT_GOLD, TFT_MAGENTA, TFT_CYAN };
const char* STATE_NAME[]          = { "SPLASH", "MENU", "GIOCATORI", "SETUP_SCACCHI",
                                      "MATCH", "FINE_MATCH", "RNG" };

State state       = ST_SPLASH;
State rngReturnTo = ST_MENU;     // dove tornare quando chiudi l'RNG
Game  currentGame = GAME_POKEMON;
int   menuSel     = 0;
unsigned long stateEnteredAt = 0;
bool  redraw      = true;

// Centri X delle due meta' dello schermo (layout simmetrico,
// condiviso da Pokemon, Scacchi e Identificazione). Rotation 1.
const int HALF_CX[2] = { 80, 240 };

// Palette per gioco: ogni gioco ha il suo colore d'accento (header,
// cornici di selezione, evidenziazioni). uiAccent viene impostato in
// drawScreen() a seconda della schermata; fuori dai match resta ciano.
#define COL_POKEMON TFT_YELLOW    // Pokemon: giallo
#define COL_YUGIOH  TFT_PURPLE    // Yu-Gi-Oh: viola (+ dettagli oro)
#define COL_SCACCHI TFT_ORANGE    // Scacchi: ambra
uint16_t uiAccent = TFT_CYAN;

uint16_t accentFor(Game g) {
  switch (g) {
    case GAME_POKEMON: return COL_POKEMON;
    case GAME_YUGIOH:  return COL_YUGIOH;
    case GAME_SCACCHI: return COL_SCACCHI;
    default:           return TFT_CYAN;
  }
}

// ----------------------------- NFC ------------------------------
// Tag noti del kit: alla PRIMA lettura il monitor seriale stampa
// l'UID di ogni tag ([NFC] P1 = ... uid DD:5C:2A:99). Copia i byte
// qui sotto e ricarica: da quel momento il cubo sapra' chi e' la
// tessera e chi il portachiavi. I tag non in tabella funzionano
// comunque (compaiono come "TAG sconosciuto" con il loro UID).
struct KnownTag { const char* name; uint8_t len; uint8_t uid[7]; };

KnownTag KNOWN_TAGS[] = {
  { "TESSERA",     4, { 0x00, 0x00, 0x00, 0x00 } },   // <-- metti il TUO UID
  { "PORTACHIAVI", 4, { 0x11, 0x11, 0x11, 0x11 } },   // <-- metti il TUO UID
};
#define KNOWN_COUNT (sizeof(KNOWN_TAGS) / sizeof(KNOWN_TAGS[0]))

// ===================== Mini-driver PN532 in I2C ======================
// Condensato dalla libreria Seeed-Studio/PN532 (BSD). Fa solo cio' che
// ci serve: init, versione firmware, lettura UID di un tag. In I2C
// interroga il "ready byte" sul bus per sapere quando il PN532 ha la
// risposta pronta -> NESSUN pin IRQ da collegare. I metodi hanno le
// stesse firme della libreria, percio' il resto del codice non cambia.
#define PN532_PREAMBLE      0x00
#define PN532_STARTCODE1    0x00
#define PN532_STARTCODE2    0xFF
#define PN532_POSTAMBLE     0x00
#define PN532_HOSTTOPN532   0xD4
#define PN532_PN532TOHOST   0xD5
#define PN532_I2C_ADDRESS   (0x48 >> 1)        // = 0x24 (lo stesso visto dallo scanner)
#define PN532_ACK_WAIT_TIME 10
#define PN532_INVALID_FRAME -3
#define PN532_NO_SPACE      -4
#define PN532_TIMEOUT       -2
#define PN532_COMMAND_GETFIRMWAREVERSION  0x02
#define PN532_COMMAND_SAMCONFIGURATION    0x14
#define PN532_COMMAND_RFCONFIGURATION     0x32
#define PN532_COMMAND_INLISTPASSIVETARGET 0x4A
#define PN532_MIFARE_ISO14443A            0x00

class MiniPN532 {
public:
  MiniPN532(TwoWire& wire) : _wire(&wire), command(0) {}

  void begin() { delay(500); }   // wakeup: lascia stabilizzare il PN532 (Wire gia' avviato)

  uint32_t getFirmwareVersion() {
    uint8_t buf[12];
    buf[0] = PN532_COMMAND_GETFIRMWAREVERSION;
    if (writeCommand(buf, 1)) return 0;
    if (readResponse(buf, sizeof(buf)) < 0) return 0;
    uint32_t r = buf[0];
    r = (r << 8) | buf[1];
    r = (r << 8) | buf[2];
    r = (r << 8) | buf[3];
    return r;
  }

  bool SAMConfig() {
    uint8_t buf[8];
    buf[0] = PN532_COMMAND_SAMCONFIGURATION;
    buf[1] = 0x01;   // modo normale
    buf[2] = 0x14;   // timeout 50ms * 20 = 1 s
    buf[3] = 0x01;   // flag interno del comando (NON il pin fisico IRQ)
    if (writeCommand(buf, 4)) return false;
    return (0 < readResponse(buf, sizeof(buf)));
  }

  bool setPassiveActivationRetries(uint8_t maxRetries) {
    uint8_t buf[8];
    buf[0] = PN532_COMMAND_RFCONFIGURATION;
    buf[1] = 5;       // item 5 = MaxRetries
    buf[2] = 0xFF;    // MxRtyATR
    buf[3] = 0x01;    // MxRtyPSL
    buf[4] = maxRetries;
    if (writeCommand(buf, 5)) return false;
    return (0 < readResponse(buf, sizeof(buf)));
  }

  // Legge l'UID del primo tag in campo. Ritorna true se trovato.
  bool readPassiveTargetID(uint8_t cardbaudrate, uint8_t* uid, uint8_t* uidLength, uint16_t timeout) {
    uint8_t buf[24];
    buf[0] = PN532_COMMAND_INLISTPASSIVETARGET;
    buf[1] = 1;             // max 1 carta per volta
    buf[2] = cardbaudrate;
    if (writeCommand(buf, 3)) return false;
    if (readResponse(buf, sizeof(buf), timeout) < 0) return false;
    if (buf[0] != 1) return false;          // nessuna carta trovata
    *uidLength = buf[5];
    for (uint8_t i = 0; i < buf[5]; i++) uid[i] = buf[6 + i];
    return true;
  }

private:
  TwoWire* _wire;
  uint8_t  command;

  uint8_t wr(uint8_t d) { return _wire->write(d); }
  uint8_t rd()          { return _wire->read(); }

  int8_t writeCommand(const uint8_t* header, uint8_t hlen,
                      const uint8_t* body = 0, uint8_t blen = 0) {
    command = header[0];
    _wire->beginTransmission(PN532_I2C_ADDRESS);
    wr(PN532_PREAMBLE); wr(PN532_STARTCODE1); wr(PN532_STARTCODE2);
    uint8_t length = hlen + blen + 1;       // TFI + DATA
    wr(length); wr(~length + 1);            // LEN + checksum di LEN
    wr(PN532_HOSTTOPN532);
    uint8_t sum = PN532_HOSTTOPN532;
    for (uint8_t i = 0; i < hlen; i++) { if (!wr(header[i])) return PN532_INVALID_FRAME; sum += header[i]; }
    for (uint8_t i = 0; i < blen; i++) { if (!wr(body[i]))   return PN532_INVALID_FRAME; sum += body[i];  }
    wr(~sum + 1);                           // checksum dei dati
    wr(PN532_POSTAMBLE);
    _wire->endTransmission();
    return readAckFrame();
  }

  int8_t readAckFrame() {
    const uint8_t ACK[6] = { 0, 0, 0xFF, 0, 0xFF, 0 };
    uint8_t b[6];
    uint16_t t = 0;
    do {
      if (_wire->requestFrom((uint8_t)PN532_I2C_ADDRESS, (uint8_t)7)) {
        if (rd() & 1) break;                // primo byte = status: pronto?
      }
      delay(1);
      if (++t > PN532_ACK_WAIT_TIME) return PN532_TIMEOUT;
    } while (1);
    for (uint8_t i = 0; i < 6; i++) b[i] = rd();
    for (uint8_t i = 0; i < 6; i++) if (b[i] != ACK[i]) return PN532_INVALID_FRAME;
    return 0;
  }

  int16_t getResponseLength(uint16_t timeout) {
    const uint8_t NACK[6] = { 0, 0, 0xFF, 0xFF, 0, 0 };
    uint16_t t = 0;
    do {
      if (_wire->requestFrom((uint8_t)PN532_I2C_ADDRESS, (uint8_t)6)) {
        if (rd() & 1) break;
      }
      delay(1);
      if (timeout && ++t > timeout) return -1;
    } while (1);
    if (rd() != 0x00 || rd() != 0x00 || rd() != 0xFF) return PN532_INVALID_FRAME;
    uint8_t length = rd();
    _wire->beginTransmission(PN532_I2C_ADDRESS);   // NACK: richiedi di nuovo la risposta
    for (uint8_t i = 0; i < 6; i++) wr(NACK[i]);
    _wire->endTransmission();
    return length;
  }

  int16_t readResponse(uint8_t buf[], uint8_t len, uint16_t timeout = 1000) {
    int16_t length = getResponseLength(timeout);
    if (length < 0) return length;
    uint16_t t = 0;
    do {
      if (_wire->requestFrom((uint8_t)PN532_I2C_ADDRESS, (uint8_t)(6 + length + 2))) {
        if (rd() & 1) break;
      }
      delay(1);
      if (timeout && ++t > timeout) return -1;
    } while (1);
    if (rd() != 0x00 || rd() != 0x00 || rd() != 0xFF) return PN532_INVALID_FRAME;
    length = rd();
    if ((uint8_t)(length + rd()) != 0) return PN532_INVALID_FRAME;   // checksum di LEN
    uint8_t cmd = command + 1;              // comando di risposta atteso
    if (rd() != PN532_PN532TOHOST || rd() != cmd) return PN532_INVALID_FRAME;
    length -= 2;
    if (length > len) return PN532_NO_SPACE;
    uint8_t sum = PN532_PN532TOHOST + cmd;
    for (uint8_t i = 0; i < length; i++) { buf[i] = rd(); sum += buf[i]; }
    uint8_t checksum = rd();
    if ((uint8_t)(sum + checksum) != 0) return PN532_INVALID_FRAME;
    rd();   // POSTAMBLE
    return length;
  }
};

MiniPN532 nfc(Wire);          // driver inline: niente libreria, niente IRQ
bool nfcOk = false;

// WebSocket verso l'host (server.js). wsConnected = canale aperto.
WebSocketsClient webSocket;
bool          wsConnected   = false;
unsigned long lastHeartbeat = 0;

// ----------------------------- IMU ------------------------------
// Riconoscimento gesti dell'MPU6500 (FastIMU, STESSO bus I2C del PN532):
//   RIBALTAMENTO del cubo (inclina e riporta giu') -> passa turno.
//     Vale come EV_PASS SOLO durante gli SCACCHI in match.
//   SHAKE -> EV_SHAKE: apre l'RNG da un match; dentro l'RNG = lancia.
// Logica e soglie identiche allo sketch di test gia' validato:
//   - una ROTAZIONE tiene |a| ~1 g, uno SHAKE lo butta lontano da 1 g;
//   - il passa turno richiede anche un'INCLINAZIONE >35 gradi rispetto
//     al riposo, cosi' girare il cubo in piano (per leggere lo schermo)
//     NON passa il turno.
MPU6500   imu;
calData   imuCalib = { 0 };
AccelData imuAccel;
GyroData  imuGyro;
bool      imuOk = false;

const float         GES_ACC_QUIET  = 0.08f;   // |a|-1 sotto = accel fermo
const float         GES_GYR_QUIET  = 15.0f;   // |gyro| sotto = fermo (bias incluso)
const float         GES_GYR_PASS   = 45.0f;   // picco |gyro| per "rotazione decisa"
const float         GES_TILT_PASS  = 35.0f;   // inclinazione min [deg] per un RIBALTAMENTO
const float         GES_ACC_SHAKE  = 0.40f;   // |a|-1 sopra = un "colpo" di shake
const int           GES_SHAKE_HITS = 3;       // colpi nel gesto per dire SHAKE
const unsigned long GES_QUIET_MS    = 180;    // quiete per "gesto finito" [ms]
const unsigned long GES_GESTURE_MAX = 2500;   // durata max di un gesto [ms]
const unsigned long GES_COOLDOWN_MS = 600;    // pausa dopo un gesto [ms]
const unsigned long GES_SAMPLE_MS   = 20;     // ~50 letture/sec

bool          gesInGesture = false;
unsigned long gesStart = 0, gesLastActive = 0, gesLastFire = 0, gesLastSample = 0;
float         gesMaxGyr = 0, gesMaxTilt = 0;
int           gesShakeHits = 0;
bool          gesAbovePrev = false;
float         gesRestAx = 0.0f, gesRestAy = -1.0f, gesRestAz = 0.0f;   // gravita' a riposo

void imuInit() {
  // Wire e' gia' avviato da nfcInit() (stesso bus, stessi pin SDA21/SCL22):
  // NON lo richiamo per non disturbare il PN532. Inizializzo solo il sensore.
  int err = imu.init(imuCalib, 0x68);
  Wire.setClock(100000);        // riporto il bus a 100 kHz (lo vuole il PN532)
  if (err != 0) {
    imuOk = false;
    Serial.printf("[IMU] MPU6500 non inizializzato (codice %d): gesti disattivati\n", err);
    return;
  }
  imuOk = true;
  Serial.println("[IMU] ok: gesti attivi (ribalta = passa turno, shake = RNG)");
}

// Campiona l'IMU a ~50 Hz e riconosce i gesti. Ritorna EV_PASS (solo se siamo
// negli scacchi in match), EV_SHAKE, oppure EV_NONE. DA CHIAMARE A OGNI giro
// del loop, cosi' la macchina non perde campioni e tiene il riposo aggiornato.
BtnEvent pollGesture() {
  if (!imuOk) return EV_NONE;
  unsigned long now = millis();
  if (now - gesLastSample < GES_SAMPLE_MS) return EV_NONE;
  gesLastSample = now;

  imu.update();
  imu.getAccel(&imuAccel);
  imu.getGyro(&imuGyro);

  float amag = sqrtf(imuAccel.accelX*imuAccel.accelX +
                     imuAccel.accelY*imuAccel.accelY +
                     imuAccel.accelZ*imuAccel.accelZ);
  float adev = fabsf(amag - 1.0f);
  float gmag = sqrtf(imuGyro.gyroX*imuGyro.gyroX +
                     imuGyro.gyroY*imuGyro.gyroY +
                     imuGyro.gyroZ*imuGyro.gyroZ);
  bool active     = (adev > GES_ACC_QUIET) || (gmag > GES_GYR_QUIET);
  bool aboveShake = (adev > GES_ACC_SHAKE);

  if (!gesInGesture) {
    if (!active) {                       // fermo: aggiorno l'orientamento di riposo
      gesRestAx = imuAccel.accelX;
      gesRestAy = imuAccel.accelY;
      gesRestAz = imuAccel.accelZ;
    } else if (now - gesLastFire >= GES_COOLDOWN_MS) {
      gesInGesture  = true;
      gesStart      = now;
      gesLastActive = now;
      gesMaxGyr     = gmag;
      gesMaxTilt    = 0.0f;
      gesShakeHits  = aboveShake ? 1 : 0;
      gesAbovePrev  = aboveShake;
    }
    return EV_NONE;
  }

  if (gmag > gesMaxGyr) gesMaxGyr = gmag;
  if (aboveShake && !gesAbovePrev) gesShakeHits++;     // conta i colpi (fronti di salita)
  gesAbovePrev = aboveShake;
  if (active) gesLastActive = now;

  float restMag = sqrtf(gesRestAx*gesRestAx + gesRestAy*gesRestAy + gesRestAz*gesRestAz);
  float dot     = imuAccel.accelX*gesRestAx + imuAccel.accelY*gesRestAy + imuAccel.accelZ*gesRestAz;
  float cosA    = dot / (amag * restMag + 1e-6f);
  if (cosA >  1.0f) cosA =  1.0f;
  if (cosA < -1.0f) cosA = -1.0f;
  float tilt = acosf(cosA) * 57.2958f;
  if (tilt > gesMaxTilt) gesMaxTilt = tilt;

  bool ended = (now - gesLastActive > GES_QUIET_MS) || (now - gesStart > GES_GESTURE_MAX);
  if (!ended) return EV_NONE;

  // gesto finito: classifico (lo shake ha la priorita')
  BtnEvent result = EV_NONE;
  if (gesShakeHits >= GES_SHAKE_HITS) {
    result = EV_SHAKE;
    Serial.println("[IMU] SHAKE");
  } else if (gesMaxGyr >= GES_GYR_PASS && gesMaxTilt >= GES_TILT_PASS) {
    if (state == ST_MATCH && currentGame == GAME_SCACCHI) {   // ribaltamento = passa, solo scacchi
      result = EV_PASS;
      Serial.println("[IMU] RIBALTAMENTO -> passa turno");
    } else {
      Serial.println("[IMU] ribaltamento ignorato (non e' un match di scacchi)");
    }
  } else if (gesMaxGyr >= GES_GYR_PASS) {
    Serial.printf("[IMU] giro in piano ignorato (tilt %.0f)\n", gesMaxTilt);
  }
  gesInGesture = false;
  gesLastFire  = now;
  return result;
}

// Identificazione: chi e' P1 e chi e' P2 in questo match
uint8_t playerUid[2][7];
uint8_t playerUidLen[2] = { 0, 0 };
String  playerName[2]   = { "", "" };  // "" = tag non in tabella
int     playersFound    = 0;           // 0, 1 o 2

unsigned long lastNfcPoll = 0;         // throttle del polling
unsigned long lastReadAt  = 0;         // anti-rilettura dello stesso tag
uint8_t       lastUid[7];
uint8_t       lastUidLen  = 0;
unsigned long warnUntil   = 0;         // avviso "tag gia' usato" a tempo

// ----------------------------- RNG ------------------------------
#define RNG_COUNT 8
const char* RNG_NAME[RNG_COUNT] = { "MONETA", "D4", "D6", "D8", "D10", "D12", "D20", "D100" };
const int   RNG_MAX[RNG_COUNT]  = { 2, 4, 6, 8, 10, 12, 20, 100 };
int rngSel    = 0;
int rngResult = -1;              // -1 = non ancora lanciato
int rngRolls  = 0;

// --------------------------- Pokemon ----------------------------
// Contatore prize cards PRESE da ciascun giocatore (0..6). Chi arriva
// a 6 vince. Doppio orologio stile scacchi (conta in SU): scorre il
// tempo di chi e' di turno; 'p' (poi la rotazione) passa il turno.
// TETTO DI TORNEO: tempo P1 + tempo P2 <= 50 minuti. Il totale e'
// mostrato grande in alto; allo scadere gli orologi si congelano,
// triplo beep, e il match prosegue "fuori tempo" (i 3 turni extra
// del regolamento li gestiscono i giocatori; 'p' continua a spostare
// il marcatore del turno ma senza piu' contare il tempo).
#define PKMN_PRIZES_WIN    6
#define PKMN_TIME_LIMIT_MS (50UL * 60UL * 1000UL)              // 50 minuti
#define PKMN_TIME_WARN_MS  (PKMN_TIME_LIMIT_MS - 10UL*60UL*1000UL) // arancio negli ultimi 10

int  pkmnPrize[2]   = { 0, 0 };
int  pkmnActive     = 0;         // giocatore selezionato per i prize (0=P1, 1=P2)
bool pkmnWinPending = false;     // qualcuno e' a 6: attende conferma con OK
int  pkmnWinner     = -1;
bool pkmnTimeUp     = false;     // raggiunti i 50 minuti totali

int           pkmnTurn         = 0;        // di chi e' il turno (orologio attivo)
unsigned long pkmnElapsed[2]   = { 0, 0 }; // tempo accumulato per giocatore [ms]
unsigned long turnStartedAt    = 0;        // inizio del turno corrente [ms]
bool          pkmnClockRunning = false;    // orologio in marcia? (pausa durante l'RNG)

void pokemonReset() {
  pkmnPrize[0] = pkmnPrize[1] = 0;
  pkmnActive     = 0;
  pkmnWinPending = false;
  pkmnWinner     = -1;
  pkmnTimeUp     = false;
  pkmnElapsed[0] = pkmnElapsed[1] = 0;
  pkmnTurn         = 0;
  turnStartedAt    = millis();
  pkmnClockRunning = true;       // l'orologio di P1 parte subito
}

// Tempo totale del match: P1 + P2 + frazione del turno in corso
unsigned long pokemonTotalMs() {
  unsigned long t = pkmnElapsed[0] + pkmnElapsed[1];
  if (pkmnClockRunning) t += millis() - turnStartedAt;
  return t;
}

// Ferma l'orologio accumulando il tempo del turno in corso (RNG/vittoria)
void pokemonPauseClock() {
  if (pkmnClockRunning) {
    pkmnElapsed[pkmnTurn] += millis() - turnStartedAt;
    pkmnClockRunning = false;
  }
}

// Riavvia l'orologio del giocatore di turno (rientro dall'RNG).
// A tempo scaduto NON riparte: il match prosegue fuori tempo.
void pokemonResumeClock() {
  if (!pkmnClockRunning && !pkmnWinPending && !pkmnTimeUp) {
    turnStartedAt    = millis();
    pkmnClockRunning = true;
  }
}

// Passa il turno: accumula il tempo, cambia giocatore, riparte l'orologio.
// Dopo il TEMPO sposta solo il marcatore ">" (utile per i 3 turni extra).
void pokemonPassTurn() {
  if (pkmnWinPending) return;
  if (pkmnClockRunning) pkmnElapsed[pkmnTurn] += millis() - turnStartedAt;
  pkmnTurn = 1 - pkmnTurn;
  if (!pkmnTimeUp) {
    turnStartedAt    = millis();
    pkmnClockRunning = true;
  }
  redraw = true;
  Serial.printf("[PKMN] passa turno -> tocca a P%d\n", pkmnTurn + 1);
}

// Tetto dei 50 minuti: al raggiungimento congela gli orologi e avvisa
void pokemonCheckTimeLimit() {
  if (pkmnTimeUp || pkmnWinPending) return;
  if (pokemonTotalMs() >= PKMN_TIME_LIMIT_MS) {
    pokemonPauseClock();
    pkmnTimeUp = true;
    redraw     = true;
    Serial.println("[PKMN] TEMPO! Raggiunti i 50 minuti (P1+P2)");
    beepBlocking(900, 120);
    beepBlocking(700, 120);
    beepBlocking(500, 220);
  }
}

// ---------------------------- Scacchi ---------------------------
// Doppio orologio a SCALARE con tre formati da torneo:
//   CLASSICAL  90 min per le prime 40 mosse, +30 min quando il
//              giocatore completa la sua 40a mossa, incremento di
//              30 s a mossa fin dalla prima (stile FIDE)
//   RAPID      tempo totale per giocatore a scelta dell'host
//   BLITZ      3 min per giocatore + 2 s di incremento a mossa
// "Passare" ('p' da seriale, poi pulsante/rotazione) = mossa
// completata: il TUO orologio si ferma, prendi l'incremento, parte
// quello dell'avversario. Tempo a zero = bandierina, vince l'altro.
// HOOK HOSTLINK: il formato arrivera' dal PC host via WiFi; la
// schermata di setup locale restera' come fallback.

enum ChessFmt { CHESS_CLASSICAL, CHESS_RAPID, CHESS_BLITZ, CHESS_FMT_COUNT };

struct ChessCfg {
  const char*   name;       // nome a schermo
  const char*   descr;      // riga descrittiva nel setup
  unsigned long baseMs;     // tempo iniziale per giocatore
  unsigned long incMs;      // incremento per mossa completata
  int           bonusMove;  // mossa alla quale scatta il bonus (0 = mai)
  unsigned long bonusMs;    // bonus (classical: +30 min alla mossa 40)
};

const ChessCfg CHESS_CFG[CHESS_FMT_COUNT] = {
  { "CLASSICAL", "90' / 40 mosse +30', inc 30s", 90UL*60000UL, 30000UL, 40, 30UL*60000UL },
  { "RAPID",     "tempo a scelta dell'host",     15UL*60000UL,     0UL,  0,           0UL },
  { "BLITZ",     "3 min + 2s a mossa",            3UL*60000UL,  2000UL,  0,           0UL },
};

ChessFmt chessFmt       = CHESS_BLITZ;
int      chessFmtSel    = 0;       // selezione nella schermata formato
int      chessSetupStep = 0;       // 0 = formato, 1 = minuti (solo rapid)
int      rapidMinutes   = 15;      // scelta host per il rapid (5..60)

unsigned long chessRemain[2] = { 0, 0 };  // tempo RESIDUO per giocatore [ms]
int           chessMoves[2]  = { 0, 0 };  // mosse completate
int           chessTurn      = 0;         // chi muove (0 = P1 = bianco)
unsigned long chessTurnAt    = 0;         // inizio del turno corrente [ms]
bool          chessRunning   = false;     // orologio in marcia?
bool          chessResumeRng = false;     // era in marcia prima dell'RNG?
int           chessFlag      = -1;        // chi ha esaurito il tempo (-1 = nessuno)

void chessReset() {
  unsigned long base = CHESS_CFG[chessFmt].baseMs;
  if (chessFmt == CHESS_RAPID) base = (unsigned long)rapidMinutes * 60000UL;
  chessRemain[0] = chessRemain[1] = base;
  chessMoves[0]  = chessMoves[1]  = 0;
  chessTurn    = 0;
  chessRunning = false;            // parte in pausa: OK avvia l'orologio di P1
  chessFlag    = -1;
  Serial.printf("[CHESS] nuovo match %s, %s per giocatore\n",
                CHESS_CFG[chessFmt].name, fmtTime(base).c_str());
}

// Tempo residuo "vivo" del giocatore p (scala mentre e' di turno)
unsigned long chessLiveRemain(int p) {
  unsigned long r = chessRemain[p];
  if (chessRunning && p == chessTurn) {
    unsigned long spent = millis() - chessTurnAt;
    r = (spent >= r) ? 0 : r - spent;
  }
  return r;
}

void chessPauseClock() {
  if (chessRunning) {
    chessRemain[chessTurn] = chessLiveRemain(chessTurn);
    chessRunning = false;
  }
}

void chessResumeClock() {
  if (!chessRunning && chessFlag < 0) {
    chessTurnAt  = millis();
    chessRunning = true;
  }
}

// Mossa completata: fermo il MIO orologio, prendo incremento (ed
// eventuale bonus alla mossa 40), parte l'orologio dell'avversario
void chessPassTurn() {
  if (!chessRunning || chessFlag >= 0) return;   // si passa solo a orologio attivo
  chessRemain[chessTurn] = chessLiveRemain(chessTurn);
  chessMoves[chessTurn]++;
  chessRemain[chessTurn] += CHESS_CFG[chessFmt].incMs;
  if (CHESS_CFG[chessFmt].bonusMove > 0 &&
      chessMoves[chessTurn] == CHESS_CFG[chessFmt].bonusMove) {
    chessRemain[chessTurn] += CHESS_CFG[chessFmt].bonusMs;
    Serial.printf("[CHESS] P%d completa la mossa %d: +%s\n", chessTurn + 1,
                  CHESS_CFG[chessFmt].bonusMove,
                  fmtTime(CHESS_CFG[chessFmt].bonusMs).c_str());
  }
  Serial.printf("[CHESS] P%d mossa %d, restano %s\n", chessTurn + 1,
                chessMoves[chessTurn], fmtTime(chessRemain[chessTurn]).c_str());
  chessTurn   = 1 - chessTurn;
  chessTurnAt = millis();
  redraw = true;                   // aggiorna marcatore ">" e contamosse
}

// Bandierina: tempo a zero -> il giocatore di turno perde
void chessCheckFlag() {
  if (!chessRunning || chessFlag >= 0) return;
  if (chessLiveRemain(chessTurn) == 0) {
    chessRemain[chessTurn] = 0;
    chessRunning = false;
    chessFlag    = chessTurn;
    redraw = true;
    Serial.printf("[CHESS] bandierina! P%d perde a tempo (mosse %d-%d)\n",
                  chessFlag + 1, chessMoves[0], chessMoves[1]);
    beepBlocking(900, 120);
    beepBlocking(600, 250);
  }
}

// --------------------------- Yu-Gi-Oh! --------------------------
// Life points (partono da 8000). UN solo timer del match in alto
// (conta in su, tetto 50 min) che va in PAUSA mentre modifichi i LP.
// Tre schermate interne (yugiScreen):
//   0 BASE : timer grande + i due totali LP. Bottoni = scegli il giocatore.
//   1 OPS  : LP compatti + griglia TOUCH per variazioni rapide.
//   2 CALC : calcolatrice TOUCH; il risultato si applica ai LP col segno.
// La selezione del giocatore usa i bottoni/seriale; le griglie il touch.
#define YUGI_LP_START      8000
#define YUGI_TIME_LIMIT_MS (50UL * 60UL * 1000UL)
#define YUGI_TIME_WARN_MS  (YUGI_TIME_LIMIT_MS - 10UL*60UL*1000UL)

int  yugiLP[2]      = { YUGI_LP_START, YUGI_LP_START };
int  yugiActive     = 0;        // giocatore selezionato (0/1)
int  yugiScreen     = 0;        // 0=base, 1=ops, 2=calc
int  yugiSign       = 0;        // +1 / -1 / 0 (operazione "armata" nelle OPS)
bool yugiWinPending = false;
int  yugiWinner     = -1;
bool yugiTimeUp     = false;

unsigned long yugiElapsed     = 0;     // tempo match accumulato [ms]
unsigned long yugiStartedAt   = 0;
bool          yugiClockRunning = false;

// Calcolatrice: acc (op) cur, valutazione a catena con + e -
long yugiCalcAcc   = 0;
long yugiCalcCur   = 0;
char yugiCalcOp    = ' ';       // ' ', '+', '-'
bool yugiCalcFresh = true;      // cur "vuoto": la prossima cifra lo riparte

void yugiohReset() {
  yugiLP[0] = yugiLP[1] = YUGI_LP_START;
  yugiActive     = 0;
  yugiScreen     = 0;
  yugiSign       = 0;
  yugiWinPending = false;
  yugiWinner     = -1;
  yugiTimeUp     = false;
  yugiElapsed    = 0;
  yugiStartedAt  = millis();
  yugiClockRunning = true;       // alla base il timer scorre
  yugiCalcAcc = yugiCalcCur = 0;
  yugiCalcOp = ' ';
  yugiCalcFresh = true;
}

unsigned long yugiTotalMs() {
  unsigned long t = yugiElapsed;
  if (yugiClockRunning) t += millis() - yugiStartedAt;
  return t;
}

void yugiPauseClock() {
  if (yugiClockRunning) {
    yugiElapsed += millis() - yugiStartedAt;
    yugiClockRunning = false;
  }
}

// Riparte SOLO nella schermata base (nelle ops/calc resta in pausa)
void yugiResumeClock() {
  if (!yugiClockRunning && !yugiWinPending && !yugiTimeUp && yugiScreen == 0) {
    yugiStartedAt    = millis();
    yugiClockRunning = true;
  }
}

void yugiCheckTimeLimit() {
  if (yugiTimeUp || yugiWinPending) return;
  if (yugiTotalMs() >= YUGI_TIME_LIMIT_MS) {
    yugiPauseClock();
    yugiTimeUp = true;
    redraw     = true;
    Serial.println("[YGO] TEMPO! 50 minuti raggiunti");
    beepBlocking(900, 120); beepBlocking(700, 120); beepBlocking(500, 220);
  }
}

// Applica una variazione ai LP del giocatore attivo (clamp + vittoria a 0)
void yugiApplyDelta(long delta) {
  long v = (long)yugiLP[yugiActive] + delta;
  if (v < 0)      v = 0;
  if (v > 99999)  v = 99999;
  yugiLP[yugiActive] = (int)v;
  Serial.printf("[YGO] P%d LP %+ld -> %d\n", yugiActive + 1, delta, yugiLP[yugiActive]);
  if (yugiLP[yugiActive] == 0) {            // a 0 LP si perde
    yugiWinPending = true;
    yugiWinner     = 1 - yugiActive;
    yugiPauseClock();
    Serial.printf("[YGO] P%d a 0 LP -> vince P%d (attende conferma)\n",
                  yugiActive + 1, yugiWinner + 1);
  }
}

// Valore corrente della calcolatrice a catena (acc op cur)
long yugiCalcChain() {
  if (yugiCalcOp == '+') return yugiCalcAcc + yugiCalcCur;
  if (yugiCalcOp == '-') return yugiCalcAcc - yugiCalcCur;
  return yugiCalcCur;                        // nessun operatore: solo il numero
}

// --------------------------- Pulsanti ---------------------------
bool          btnState[4]     = { true, true, true, true }; // pullup: HIGH = rilasciato
unsigned long btnChangedAt[4] = { 0, 0, 0, 0 };
unsigned long btnDownAt[4]    = { 0, 0, 0, 0 };
bool          btnLongFired[4] = { false, false, false, false };

unsigned long ledOffAt = 0;

// ------------------------- Utility ------------------------------
void beepBlocking(int freqHz, int durMs) {
  long half   = 500000L / freqHz;
  long cycles = (long)freqHz * durMs / 1000L;
  for (long i = 0; i < cycles; i++) {
    digitalWrite(PIN_BUZZ, HIGH); delayMicroseconds(half);
    digitalWrite(PIN_BUZZ, LOW);  delayMicroseconds(half);
  }
}

// Feedback ridondante (principio HCI): LED + beep su ogni azione
void feedbackTick(BtnEvent ev) {
  digitalWrite(PIN_LED, HIGH);
  ledOffAt = millis() + 80;
  switch (ev) {
    case EV_OK:          beepBlocking(1300, 35); break;
    case EV_CANCEL:      beepBlocking(700, 35);  break;
    case EV_CANCEL_LONG: beepBlocking(500, 70);  break;
    case EV_PASS:        beepBlocking(1500, 45); break;   // passa turno / mossa
    case EV_SHAKE:       beepBlocking(1800, 60); break;   // shake -> RNG
    default:             beepBlocking(1000, 15); break;   // tick SX/DX
  }
}

// Formatta millisecondi in M:SS (orologi dei giocatori)
String fmtTime(unsigned long ms) {
  unsigned long s = ms / 1000;
  char buf[12];
  snprintf(buf, sizeof(buf), "%lu:%02lu", s / 60, s % 60);
  return String(buf);
}

void changeState(State s) {
  state          = s;
  stateEnteredAt = millis();
  redraw         = true;
  Serial.printf("[STATO] %s\n", STATE_NAME[s]);
}

// --------------------------- Host link --------------------------
// Tutti gli eventi per il tabellone passano da qui. Ora il transport
// e' il WebSocket verso server.js; teniamo anche l'eco su Serial
// (righe "@HOST ...") come comodo debug quando il cubo e' via USB.
void hostSend(const String& json) {
  Serial.print("@HOST ");
  Serial.println(json);
  if (wsConnected) {
    String payload = json;            // sendTXT() della libreria vuole un
    webSocket.sendTXT(payload);       // String& NON-const: gli passiamo una copia
  }
}

// Saluto iniziale all'host: include cube e table cosi' server.js ci
// registra (lui legge proprio msg.cube e msg.table dall'hello).
void hostSendHello() {
  hostSend(String("{\"type\":\"hello\",\"cube\":\"") + CUBE_ID +
           "\",\"table\":" + TABLE_NUM +
           ",\"fw\":\"v0.4\",\"nfc\":" + (nfcOk ? "true" : "false") + "}");
}

// Eventi del WebSocket: connessione, caduta, comandi dall'host.
void webSocketEvent(WStype_t type, uint8_t* payload, size_t length) {
  switch (type) {
    case WStype_CONNECTED:
      wsConnected = true;
      Serial.println("[WS] connesso all'host");
      hostSendHello();                 // appena connessi, ci presentiamo
      break;
    case WStype_DISCONNECTED:
      wsConnected = false;
      Serial.println("[WS] disconnesso (riprovo da solo)");
      break;
    case WStype_TEXT: {
      // Comandi dall'host: server.js manda {"type":"<comando>"}.
      // (la libreria termina il frame TEXT, quindi lo leggiamo come stringa)
      String t = String((char*)payload);
      Serial.printf("[WS] comando dall'host: %s\n", t.c_str());
      if (t.indexOf("beep") >= 0) {     // es: scrivi 'beep' nella console del server
        beepBlocking(1800, 60);
        beepBlocking(2200, 90);
      }
      break;
    }
    default: break;                     // PING/PONG/ERROR ecc.: ignorati
  }
}

// Connessione WiFi + apertura del WebSocket verso server.js.
// Non blocca all'infinito: se la rete non c'e', si va avanti offline
// (gli eventi restano comunque visibili su Serial come @HOST).
void wifiSetup() {
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  Serial.printf("[WiFi] connessione a \"%s\"", WIFI_SSID);
  unsigned long t0 = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - t0 < 10000) {
    delay(250);
    Serial.print(".");
  }
  if (WiFi.status() == WL_CONNECTED) {
    Serial.printf("\n[WiFi] ok, IP del cubo %s\n", WiFi.localIP().toString().c_str());
    webSocket.begin(HOST_IP, HOST_PORT, "/");   // server.js sul PC
    webSocket.onEvent(webSocketEvent);
    webSocket.setReconnectInterval(5000);       // riprova ogni 5s se cade
    Serial.printf("[WS] provo a connettermi a ws://%s:%d/\n", HOST_IP, HOST_PORT);
  } else {
    Serial.println("\n[WiFi] nessuna rete: continuo offline (eventi solo su @HOST)");
  }
}

// UID in esadecimale: "DD:5C:2A:99" (senza ':' se a 7 byte, per stare
// compatti a schermo con gli NTAG)
String uidToString(const uint8_t* uid, uint8_t len) {
  String s;
  for (uint8_t i = 0; i < len; i++) {
    if (i && len <= 4) s += ":";
    if (uid[i] < 0x10) s += "0";
    s += String(uid[i], HEX);
  }
  s.toUpperCase();
  return s;
}

// JSON del giocatore p: {"uid":"...","name":"TESSERA"} oppure null
String hostPlayerJson(int p) {
  if (playerUidLen[p] == 0) return "null";
  String s = String("{\"uid\":\"") + uidToString(playerUid[p], playerUidLen[p]) + "\",\"name\":";
  if (playerName[p].length()) s += "\"" + playerName[p] + "\"";
  else                        s += "null";
  s += "}";
  return s;
}

// Annuncia l'inizio del match (con formato e tempi se scacchi)
void hostSendMatchStart() {
  String j = String("{\"type\":\"match_start\",\"game\":\"") + GAME_ID[currentGame] + "\"";
  if (currentGame == GAME_SCACCHI) {
    unsigned long base = (chessFmt == CHESS_RAPID)
                         ? (unsigned long)rapidMinutes * 60000UL
                         : CHESS_CFG[chessFmt].baseMs;
    j += String(",\"format\":\"") + CHESS_CFG[chessFmt].name + "\"";
    j += String(",\"base_min\":") + (base / 60000UL);
  }
  j += ",\"p1\":" + hostPlayerJson(0) + ",\"p2\":" + hostPlayerJson(1) + "}";
  hostSend(j);
}

// ------------------- NFC: init e identificazione ----------------
void nfcInit() {
  Wire.begin(I2C_SDA, I2C_SCL);
  nfc.begin();                            // wakeup del PN532 (Wire avviato sopra)
  Wire.setClock(100000);                  // 100 kHz: piu' tollerante col clock stretch
  uint32_t ver = nfc.getFirmwareVersion();
  if (!ver) {
    nfcOk = false;
    Serial.println("[NFC] PN532 NON trovato sul bus I2C (si va avanti senza)");
    Serial.println("[NFC]   controlla switch (I2C: SW1 ON, SW2 OFF) e i 4 fili");
    return;
  }
  Serial.printf("[NFC] PN532 ok, firmware %d.%d\n",
                (int)((ver >> 16) & 0xFF), (int)((ver >> 8) & 0xFF));
  nfc.SAMConfig();
  nfc.setPassiveActivationRetries(0x01);  // risposta rapida se non c'e' un tag
  nfcOk = true;
}

// Cerca l'UID nella tabella dei tag noti (NULL se sconosciuto)
const char* lookupTagName(const uint8_t* uid, uint8_t len) {
  for (unsigned i = 0; i < KNOWN_COUNT; i++) {
    if (KNOWN_TAGS[i].len == len && memcmp(KNOWN_TAGS[i].uid, uid, len) == 0)
      return KNOWN_TAGS[i].name;
  }
  return NULL;
}

// Azzera l'identificazione (entrando in ST_PLAYERS)
void playersReset() {
  playersFound    = 0;
  playerUidLen[0] = playerUidLen[1] = 0;
  playerName[0]   = playerName[1]   = "";
  lastUidLen      = 0;
  lastReadAt      = 0;
  warnUntil       = 0;
}

// Polling NFC durante ST_PLAYERS: primo tag = P1, secondo tag = P2.
// Lo stesso tag non puo' fare entrambi i giocatori. Le riletture
// ravvicinate dello stesso tag (lasciato appoggiato) sono ignorate.
void pollPlayersNfc(unsigned long now) {
  if (!nfcOk || playersFound >= 2) return;
  if (now - lastNfcPoll < 150) return;      // throttle: ~6 tentativi/sec
  lastNfcPoll = now;

  uint8_t uid[7];
  uint8_t len = 0;
  // timeout 50 ms: blocca pochissimo, i pulsanti restano reattivi
  if (!nfc.readPassiveTargetID(PN532_MIFARE_ISO14443A, uid, &len, 50)) return;
  if (len == 0 || len > 7) return;

  // stesso tag tenuto appoggiato: aggiorna il timestamp e ignora
  if (len == lastUidLen && memcmp(uid, lastUid, len) == 0 &&
      now - lastReadAt < 1500) {
    lastReadAt = now;
    return;
  }
  memcpy(lastUid, uid, len);
  lastUidLen = len;
  lastReadAt = now;

  // tag gia' assegnato a P1? (vale per il rientro dello stesso tag)
  if (playersFound == 1 && len == playerUidLen[0] &&
      memcmp(uid, playerUid[0], len) == 0) {
    warnUntil = now + 1500;
    redraw    = true;
    digitalWrite(PIN_LED, HIGH);
    ledOffAt = now + 120;
    beepBlocking(400, 120);                 // beep "errore" basso
    Serial.println("[NFC] questo tag e' gia' P1: serve un tag diverso per P2");
    return;
  }

  // nuovo giocatore identificato
  int p = playersFound;
  memcpy(playerUid[p], uid, len);
  playerUidLen[p] = len;
  const char* known = lookupTagName(uid, len);
  playerName[p] = known ? String(known) : String("");
  playersFound++;
  redraw = true;
  digitalWrite(PIN_LED, HIGH);
  ledOffAt = now + 120;
  beepBlocking(1300, 40);
  beepBlocking(1700, 60);                   // doppio beep ascendente = letto!
  Serial.printf("[NFC] P%d = %s  (uid %s, %d byte)\n", p + 1,
                known ? known : "tag sconosciuto",
                uidToString(uid, len).c_str(), len);
  hostSend(String("{\"type\":\"player_tag\",\"slot\":") + (p + 1) +
           ",\"uid\":\"" + uidToString(uid, len) + "\",\"name\":" +
           (known ? "\"" + String(known) + "\"" : "null") + "}");
}

// Inizializza il gioco scelto e poi entra nel match.
// Gli scacchi passano prima dalla scelta del formato.
void startMatch() {
  switch (currentGame) {
    case GAME_POKEMON: pokemonReset(); break;
    case GAME_SCACCHI:
      chessFmtSel    = 0;
      chessSetupStep = 0;
      changeState(ST_CHESS_SETUP);
      return;                       // match_start partira' dopo il setup
    case GAME_YUGIOH:  yugiohReset(); break;
    default: break;
  }
  hostSendMatchStart();
  changeState(ST_MATCH);
}

// ------------------------ Lettura pulsanti ----------------------
BtnEvent pollButtons() {
  unsigned long now = millis();
  for (int i = 0; i < 4; i++) {
    bool reading = digitalRead(BTN_PIN[i]);
    if (reading != btnState[i] && now - btnChangedAt[i] > DEBOUNCE_MS) {
      btnChangedAt[i] = now;
      btnState[i]     = reading;
      if (reading == LOW) {                    // appena premuto
        btnDownAt[i]    = now;
        btnLongFired[i] = false;
      } else if (!btnLongFired[i]) {           // rilascio = pressione breve
        Serial.printf("[BTN] %s\n", BTN_NAME[i]);
        if (i == 0) return EV_OK;
        if (i == 1) return EV_CANCEL;
        if (i == 2) return EV_LEFT;
        if (i == 3) return EV_RIGHT;
      }
    }
    // Pressione LUNGA (solo ANNULLA): scatta mentre tieni premuto
    if (i == 1 && btnState[i] == LOW && !btnLongFired[i] &&
        now - btnDownAt[i] > LONGPRESS_MS) {
      btnLongFired[i] = true;
      Serial.println("[BTN] annulla (lungo)");
      return EV_CANCEL_LONG;
    }
  }
  return EV_NONE;
}

// ------------------ Lettura comandi da SERIALE ------------------
// Ponte per pilotare il menu senza pulsanti fisici (utile finche'
// mancano i jumper). Convive coi pulsanti veri: nel loop viene
// consultata solo se i pulsanti non hanno prodotto eventi.
//   a = SX   d = DX   o = OK   c = ANNULLA   r = apri RNG
//   p = passa turno / completa la mossa (poi: rotazione del cubo)
BtnEvent pollSerial() {
  if (!Serial.available()) return EV_NONE;
  char c = Serial.read();
  switch (c) {
    case 'a': case 'A': Serial.println("[SER] sx");              return EV_LEFT;
    case 'd': case 'D': Serial.println("[SER] dx");              return EV_RIGHT;
    case 'o': case 'O': Serial.println("[SER] ok");             return EV_OK;
    case 'c': case 'C': Serial.println("[SER] annulla");        return EV_CANCEL;
    case 'r': case 'R': Serial.println("[SER] rng (annulla lungo)"); return EV_CANCEL_LONG;
    case 'p': case 'P': Serial.println("[SER] passa turno");    return EV_PASS;
    case '\r': case '\n': return EV_NONE;   // ignora l'Invio del monitor
    default:
      Serial.printf("[SER] tasto '%c' non mappato (usa a/d/o/c/r/p)\n", c);
      return EV_NONE;
  }
}

// --------------------------- Schermate --------------------------
void drawHeader(const char* title) {
  tft.fillScreen(TFT_BLACK);
  tft.setTextDatum(MC_DATUM);
  tft.setTextSize(1);
  tft.setTextColor(uiAccent, TFT_BLACK);
  tft.drawString(title, 160, 22, 4);
  tft.drawFastHLine(20, 42, 280, TFT_DARKGREY);
}

void drawFooter(const char* hint) {
  tft.setTextDatum(MC_DATUM);
  tft.setTextSize(1);
  tft.setTextColor(TFT_DARKGREY, TFT_BLACK);
  tft.drawString(hint, 160, 226, 2);
}

void drawSplash() {
  tft.fillScreen(TFT_BLACK);
  tft.setTextDatum(MC_DATUM);
  tft.setTextColor(TFT_CYAN, TFT_BLACK);
  tft.setTextSize(2);
  tft.drawString("CUBE", 160, 80, 4);
  tft.drawString("TOURNAMENT", 160, 130, 4);
  tft.setTextSize(1);
  tft.setTextColor(TFT_DARKGREY, TFT_BLACK);
  tft.drawString("controller  v0.4", 160, 180, 2);
}

void drawMenu() {
  drawHeader("SCEGLI IL GIOCO");
  for (int i = 0; i < GAME_COUNT; i++) {
    int y = 78 + i * 48;
    if (i == menuSel) {
      tft.fillRoundRect(30, y - 19, 260, 38, 8, TFT_NAVY);
      tft.drawRoundRect(30, y - 19, 260, 38, 8, TFT_CYAN);
      tft.setTextColor(TFT_WHITE, TFT_NAVY);
    } else {
      tft.setTextColor(TFT_DARKGREY, TFT_BLACK);
    }
    tft.setTextSize(1);
    tft.drawString(GAME_NAME[i], 160, y, 4);
  }
  drawFooter("SX/DX scegli    OK conferma");
}

// Identificazione: layout simmetrico, lo slot in attesa e' evidenziato
void drawPlayers() {
  drawHeader("IDENTIFICAZIONE");
  tft.drawFastVLine(160, 48, 148, TFT_DARKGREY);

  for (int p = 0; p < 2; p++) {
    bool waiting = (p == playersFound) && nfcOk;   // tocca a questo slot

    if (waiting) tft.drawRoundRect(HALF_CX[p] - 72, 52, 144, 144, 8, TFT_CYAN);

    tft.setTextDatum(MC_DATUM);
    tft.setTextSize(1);
    tft.setTextColor(waiting ? TFT_CYAN : TFT_DARKGREY, TFT_BLACK);
    tft.drawString(p == 0 ? "P1" : "P2", HALF_CX[p], 72, 4);

    if (playerUidLen[p]) {                         // gia' identificato
      tft.setTextColor(TFT_GREEN, TFT_BLACK);
      tft.drawString(playerName[p].length() ? playerName[p] : "TAG sconosciuto",
                     HALF_CX[p], 110, 2);
      tft.setTextColor(TFT_DARKGREY, TFT_BLACK);
      tft.drawString(uidToString(playerUid[p], playerUidLen[p]), HALF_CX[p], 130, 2);
      tft.setTextColor(TFT_GREEN, TFT_BLACK);
      tft.drawString("ok!", HALF_CX[p], 160, 2);
    } else if (waiting) {
      tft.setTextColor(TFT_WHITE, TFT_BLACK);
      tft.drawString("appoggia il tag", HALF_CX[p], 110, 2);
      tft.setTextColor(TFT_DARKGREY, TFT_BLACK);
      tft.drawString("(tessera o portachiavi)", HALF_CX[p], 130, 2);
    } else {
      tft.setTextColor(TFT_DARKGREY, TFT_BLACK);
      tft.drawString("- in attesa -", HALF_CX[p], 110, 2);
    }
  }

  // riga di stato sotto le due colonne
  tft.setTextDatum(MC_DATUM);
  if (!nfcOk) {
    tft.setTextColor(TFT_ORANGE, TFT_BLACK);
    tft.drawString("PN532 non trovato: OK per saltare", 160, 206, 2);
  } else if (warnUntil) {
    tft.setTextColor(TFT_ORANGE, TFT_BLACK);
    tft.drawString("Tag gia' usato per P1: serve l'altro!", 160, 206, 2);
  } else if (playersFound == 2) {
    tft.setTextColor(TFT_GREEN, TFT_BLACK);
    tft.drawString("Giocatori pronti!", 160, 206, 2);
  }

  drawFooter(playersFound == 2 ? "OK avvia il match    ANNULLA menu"
                               : "OK salta    ANNULLA menu");
  // HOOK NFC fatto! (v0.3) — restano i gesti IMU e il WiFi
}

void drawMatch() {
  drawHeader(GAME_NAME[currentGame]);
  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  tft.drawString("Logica di gioco in arrivo", 160, 90, 4);
  tft.setTextColor(TFT_DARKGREY, TFT_BLACK);
  tft.drawString("Qui vivranno punteggi e timer", 160, 125, 2);
  tft.setTextColor(TFT_ORANGE, TFT_BLACK);
  tft.drawString("Tieni premuto ANNULLA = RNG", 160, 160, 2);
  drawFooter("OK fine match    ANNULLA menu");
  // HOOK GIOCO: yugioh (tastierino touch LP)
}

// ----------------------- Pokemon: schermata ---------------------
// Ridisegna SOLO il tempo totale in alto (refresh leggero).
// Font 6 = cifre grandi 48px: leggibile dall'altro lato del tavolo.
void drawPkmnTotal() {
  unsigned long t = pokemonTotalMs();
  if (t > PKMN_TIME_LIMIT_MS) t = PKMN_TIME_LIMIT_MS;   // a video mai oltre 50:00
  uint16_t col = TFT_WHITE;
  if      (pkmnTimeUp)             col = TFT_RED;
  else if (t >= PKMN_TIME_WARN_MS) col = TFT_ORANGE;    // ultimi 10 minuti
  tft.fillRect(84, 2, 152, 50, TFT_BLACK);
  tft.setTextDatum(MC_DATUM);
  tft.setTextSize(1);
  tft.setTextColor(col, TFT_BLACK);
  tft.drawString(fmtTime(t), 160, 27, 6);
}

// Ridisegna SOLO l'orologio di un giocatore (niente flicker)
void drawClock(int p) {
  int y = 104;
  tft.fillRect(HALF_CX[p] - 46, y - 15, 92, 30, TFT_BLACK);
  unsigned long ms = pkmnElapsed[p];
  bool running = (pkmnClockRunning && p == pkmnTurn && !pkmnWinPending);
  if (running) ms += millis() - turnStartedAt;
  tft.setTextDatum(MC_DATUM);
  tft.setTextSize(1);
  tft.setTextColor(running ? TFT_GREEN : TFT_DARKGREY, TFT_BLACK);
  tft.drawString(fmtTime(ms), HALF_CX[p], y, 4);
}

void drawPokemon() {
  tft.fillScreen(TFT_BLACK);

  // Tempo di gioco totale, grande in alto (P1 + P2, tetto 50:00)
  drawPkmnTotal();
  tft.setTextDatum(MC_DATUM);
  tft.setTextSize(1);
  tft.setTextColor(TFT_DARKGREY, TFT_BLACK);
  tft.drawString("POKEMON", 40, 27, 2);
  if (pkmnTimeUp) {
    tft.setTextColor(TFT_RED, TFT_BLACK);
    tft.drawString("TEMPO!", 282, 27, 2);
  } else {
    tft.drawString("max 50:00", 282, 27, 2);
  }
  tft.drawFastHLine(20, 54, 280, TFT_DARKGREY);
  tft.drawFastVLine(160, 56, 154, TFT_DARKGREY);   // separatore simmetrico

  for (int p = 0; p < 2; p++) {
    bool active = (p == pkmnActive) && !pkmnWinPending;  // selezionato per i prize
    bool turn   = (p == pkmnTurn) && !pkmnWinPending;    // di turno (orologio)

    // cornice del giocatore attivo (quello che stai modificando)
    if (active) tft.drawRoundRect(HALF_CX[p] - 72, 58, 144, 152, 8, TFT_CYAN);

    // etichetta, con ">" davanti a chi e' di turno
    tft.setTextDatum(MC_DATUM);
    tft.setTextSize(1);
    tft.setTextColor(active ? TFT_CYAN : TFT_DARKGREY, TFT_BLACK);
    String lbl = String(p == 0 ? "P1" : "P2");
    if (turn) lbl = ">" + lbl;
    tft.drawString(lbl, HALF_CX[p], 74, 4);

    // orologio individuale
    drawClock(p);

    // numero dei prize presi
    tft.setTextColor(active ? TFT_WHITE : TFT_LIGHTGREY, TFT_BLACK);
    tft.setTextSize(2);
    tft.drawString(String(pkmnPrize[p]), HALF_CX[p], 152, 4);
    tft.setTextSize(1);

    // 6 pip: i prize presi sono pieni (gialli)
    int pw = 14, gap = 4, total = 6 * pw + 5 * gap;
    int x0 = HALF_CX[p] - total / 2;
    for (int i = 0; i < 6; i++) {
      int x = x0 + i * (pw + gap);
      if (i < pkmnPrize[p]) tft.fillRoundRect(x, 192, pw, pw, 3, TFT_YELLOW);
      else                  tft.drawRoundRect(x, 192, pw, pw, 3, TFT_DARKGREY);
    }
  }

  if (pkmnWinPending) {
    tft.fillRoundRect(40, 90, 240, 54, 10, TFT_DARKGREEN);
    tft.drawRoundRect(40, 90, 240, 54, 10, TFT_GREEN);
    tft.setTextDatum(MC_DATUM);
    tft.setTextColor(TFT_WHITE, TFT_DARKGREEN);
    tft.setTextSize(1);
    tft.drawString(String(pkmnWinner == 0 ? "P1" : "P2") + " VINCE!", 160, 117, 4);
    drawFooter("OK conferma     SX annulla");
  } else {
    drawFooter("OK gioc.   SX/DX prize   p = turno");
  }
}

// ----------------------- Pokemon: input -------------------------
void handlePokemon(BtnEvent ev) {
  // Stato di vittoria: serve conferma esplicita
  if (pkmnWinPending) {
    if (ev == EV_OK) {
      Serial.printf("[PKMN] vittoria confermata: P%d  (tempi %s / %s)\n",
                    pkmnWinner + 1, fmtTime(pkmnElapsed[0]).c_str(),
                    fmtTime(pkmnElapsed[1]).c_str());
      hostSend(String("{\"type\":\"match_end\",\"game\":\"pokemon\",\"winner\":") +
               (pkmnWinner + 1) +
               ",\"prizes\":[" + pkmnPrize[0] + "," + pkmnPrize[1] + "]" +
               ",\"time_ms\":[" + pkmnElapsed[0] + "," + pkmnElapsed[1] + "]" +
               ",\"p1\":" + hostPlayerJson(0) + ",\"p2\":" + hostPlayerJson(1) + "}");
      changeState(ST_MATCH_END);
    } else if (ev == EV_LEFT) {           // mi ero contato un prize di troppo
      if (pkmnPrize[pkmnWinner] > 0) pkmnPrize[pkmnWinner]--;
      pkmnWinPending = false;
      pkmnWinner     = -1;
      pokemonResumeClock();               // riparte solo se non e' gia' TEMPO
      redraw         = true;
    } else if (ev == EV_CANCEL) {
      changeState(ST_MENU);
    }
    return;
  }

  switch (ev) {
    case EV_OK:                                  // seleziona giocatore (toggle)
      pkmnActive = 1 - pkmnActive;
      redraw = true;
      Serial.printf("[PKMN] giocatore attivo: P%d\n", pkmnActive + 1);
      break;

    case EV_PASS:                                // passa turno (rotazione o 'p')
      pokemonPassTurn();
      break;

    case EV_RIGHT:                               // +1 prize al giocatore attivo
      if (pkmnPrize[pkmnActive] < PKMN_PRIZES_WIN) {
        pkmnPrize[pkmnActive]++;
        Serial.printf("[PKMN] P%d prize = %d\n", pkmnActive + 1, pkmnPrize[pkmnActive]);
        if (pkmnPrize[pkmnActive] >= PKMN_PRIZES_WIN) {
          pkmnWinPending = true;
          pkmnWinner     = pkmnActive;
          pokemonPauseClock();                   // congela i tempi alla vittoria
          Serial.printf("[PKMN] P%d a 6 prize -> attende conferma\n", pkmnWinner + 1);
        }
      }
      redraw = true;
      break;

    case EV_LEFT:                                // -1 prize (correzione errori)
      if (pkmnPrize[pkmnActive] > 0) {
        pkmnPrize[pkmnActive]--;
        Serial.printf("[PKMN] P%d prize = %d\n", pkmnActive + 1, pkmnPrize[pkmnActive]);
      }
      redraw = true;
      break;

    case EV_CANCEL:                              // abbandona -> menu
      changeState(ST_MENU);
      break;

    default: break;
  }
}

// ------------------- Scacchi: setup formato ---------------------
void drawChessSetup() {
  if (chessSetupStep == 0) {
    drawHeader("SCACCHI - FORMATO");
    for (int i = 0; i < CHESS_FMT_COUNT; i++) {
      int y = 76 + i * 50;
      if (i == chessFmtSel) {
        tft.fillRoundRect(20, y - 21, 280, 42, 8, TFT_NAVY);
        tft.drawRoundRect(20, y - 21, 280, 42, 8, TFT_CYAN);
        tft.setTextColor(TFT_WHITE, TFT_NAVY);
      } else {
        tft.setTextColor(TFT_DARKGREY, TFT_BLACK);
      }
      tft.setTextDatum(MC_DATUM);
      tft.setTextSize(1);
      tft.drawString(CHESS_CFG[i].name, 160, y - 7, 4);
      tft.setTextColor(i == chessFmtSel ? TFT_CYAN : TFT_DARKGREY,
                       i == chessFmtSel ? TFT_NAVY : TFT_BLACK);
      tft.drawString(CHESS_CFG[i].descr, 160, y + 12, 2);
    }
    drawFooter("SX/DX scegli   OK conferma   ANNULLA menu");
  } else {
    drawHeader("RAPID - TEMPO");
    tft.setTextColor(TFT_WHITE, TFT_BLACK);
    tft.drawString("Minuti per giocatore", 160, 80, 4);
    tft.setTextColor(TFT_YELLOW, TFT_BLACK);
    tft.setTextSize(2);
    tft.drawString(String("<  ") + rapidMinutes + "  >", 160, 135, 4);
    tft.setTextSize(1);
    drawFooter("SX/DX cambia   OK conferma   ANNULLA indietro");
  }
  // HOOK HOSTLINK: il formato del torneo arrivera' dal PC host via WiFi
}

void handleChessSetup(BtnEvent ev) {
  if (chessSetupStep == 0) {                          // scelta del formato
    if (ev == EV_LEFT)  { chessFmtSel = (chessFmtSel + CHESS_FMT_COUNT - 1) % CHESS_FMT_COUNT; redraw = true; }
    if (ev == EV_RIGHT) { chessFmtSel = (chessFmtSel + 1) % CHESS_FMT_COUNT;                   redraw = true; }
    if (ev == EV_OK) {
      chessFmt = (ChessFmt)chessFmtSel;
      if (chessFmt == CHESS_RAPID) {                  // il rapid chiede anche i minuti
        chessSetupStep = 1;
        redraw = true;
      } else {
        chessReset();
        hostSendMatchStart();
        changeState(ST_MATCH);
      }
    }
    if (ev == EV_CANCEL) changeState(ST_MENU);
  } else {                                            // scelta minuti del rapid
    if (ev == EV_LEFT  && rapidMinutes > 5)  { rapidMinutes -= 5; redraw = true; }
    if (ev == EV_RIGHT && rapidMinutes < 60) { rapidMinutes += 5; redraw = true; }
    if (ev == EV_OK)     { chessReset(); hostSendMatchStart(); changeState(ST_MATCH); }
    if (ev == EV_CANCEL) { chessSetupStep = 0; redraw = true; }
  }
}

// --------------------- Scacchi: schermata -----------------------
// Ridisegna SOLO l'orologio di un giocatore (refresh leggero).
// Font 6 grande finche' il tempo sta in 5 caratteri (sotto 100 min),
// altrimenti font 4 (capita solo nel classical dopo il bonus).
void drawChessClock(int p) {
  int y = 108;
  tft.fillRect(HALF_CX[p] - 74, y - 26, 148, 52, TFT_BLACK);
  unsigned long r = chessLiveRemain(p);
  bool mine = (p == chessTurn);
  uint16_t col = TFT_DARKGREY;
  if      (chessFlag == p)           col = TFT_RED;
  else if (mine && chessRunning)     col = (r < 30000UL) ? TFT_RED : TFT_GREEN;
  else if (mine)                     col = TFT_LIGHTGREY;   // di turno ma in pausa
  tft.setTextDatum(MC_DATUM);
  tft.setTextSize(1);
  tft.setTextColor(col, TFT_BLACK);
  tft.drawString(fmtTime(r), HALF_CX[p], y, (r < 6000000UL) ? 6 : 4);
}

void drawChess() {
  String t = String("SCACCHI - ") + CHESS_CFG[chessFmt].name;
  if (chessFmt == CHESS_RAPID) t += " " + String(rapidMinutes) + "'";
  drawHeader(t.c_str());
  tft.drawFastVLine(160, 48, 162, TFT_DARKGREY);

  for (int p = 0; p < 2; p++) {
    bool mine = (p == chessTurn) && chessFlag < 0;

    tft.setTextDatum(MC_DATUM);
    tft.setTextSize(1);
    tft.setTextColor(mine ? TFT_CYAN : TFT_DARKGREY, TFT_BLACK);
    String lbl = String(p == 0 ? "P1" : "P2");
    if (mine) lbl = ">" + lbl;
    tft.drawString(lbl, HALF_CX[p], 64, 4);

    drawChessClock(p);

    tft.setTextColor(TFT_DARKGREY, TFT_BLACK);
    tft.drawString(String("mosse: ") + chessMoves[p], HALF_CX[p], 150, 2);
  }

  if (chessFlag >= 0) {                       // bandierina caduta
    tft.fillRoundRect(40, 168, 240, 40, 10, TFT_MAROON);
    tft.drawRoundRect(40, 168, 240, 40, 10, TFT_RED);
    tft.setTextDatum(MC_DATUM);
    tft.setTextColor(TFT_WHITE, TFT_MAROON);
    tft.drawString(String("TEMPO! VINCE P") + (2 - chessFlag), 160, 188, 4);
    drawFooter("OK conferma    ANNULLA menu");
  } else if (!chessRunning) {
    tft.setTextColor(TFT_ORANGE, TFT_BLACK);
    tft.drawString(chessMoves[0] + chessMoves[1] == 0 ? "OK per iniziare"
                                                      : "PAUSA - OK riprende",
                   160, 182, 2);
    drawFooter("OK avvia   p = mossa   ANNULLA menu");
  } else {
    drawFooter("OK pausa   p = mossa   ANNULLA menu");
  }
  // HOOK GESTI: rotazione del cubo -> chessPassTurn()
}

// ----------------------- Scacchi: input -------------------------
void handleChess(BtnEvent ev) {
  if (chessFlag >= 0) {                        // bandierina: attesa conferma
    if (ev == EV_OK) {
      Serial.printf("[CHESS] vittoria a tempo confermata: P%d\n", 2 - chessFlag);
      hostSend(String("{\"type\":\"match_end\",\"game\":\"scacchi\",\"format\":\"") +
               CHESS_CFG[chessFmt].name + "\",\"winner\":" + (2 - chessFlag) +
               ",\"moves\":[" + chessMoves[0] + "," + chessMoves[1] + "]" +
               ",\"remain_ms\":[" + chessRemain[0] + "," + chessRemain[1] + "]" +
               ",\"p1\":" + hostPlayerJson(0) + ",\"p2\":" + hostPlayerJson(1) + "}");
      changeState(ST_MATCH_END);
    }
    if (ev == EV_CANCEL) changeState(ST_MENU);
    return;
  }

  switch (ev) {
    case EV_OK:                                // avvia / pausa / riprendi
      if (chessRunning) chessPauseClock();
      else              chessResumeClock();
      redraw = true;
      Serial.printf("[CHESS] orologio %s\n", chessRunning ? "avviato" : "in pausa");
      break;

    case EV_PASS:                              // mossa completata ('p', poi rotazione)
      chessPassTurn();
      break;

    case EV_CANCEL:                            // abbandona -> menu
      changeState(ST_MENU);
      break;

    default: break;
  }
}

// ----------------------- Yu-Gi-Oh: disegno ----------------------
// Timer del match (refresh leggero). font 6 = grande, font 4 = compatto.
void drawYugiTime(int x, int y, uint8_t font) {
  unsigned long t = yugiTotalMs();
  if (t > YUGI_TIME_LIMIT_MS) t = YUGI_TIME_LIMIT_MS;
  uint16_t col = TFT_WHITE;
  if      (yugiTimeUp)             col = TFT_RED;
  else if (t >= YUGI_TIME_WARN_MS) col = TFT_ORANGE;
  int w = (font >= 6) ? 150 : 78, h = (font >= 6) ? 46 : 26;
  tft.fillRect(x - w/2, y - h/2, w, h, TFT_BLACK);
  tft.setTextDatum(MC_DATUM); tft.setTextSize(1);
  tft.setTextColor(col, TFT_BLACK);
  tft.drawString(fmtTime(t), x, y, font);
}

// Geometrie delle griglie touch (usate sia per disegnare che per i tap)
void yugiOpsRect(int id, int& x, int& y, int& w, int& h) {
  int c = id % 3, r = id / 3;
  x = 8 + c * 103; y = 50 + r * 61; w = 97; h = 55;
}
void yugiCalcRect(int id, int& x, int& y, int& w, int& h) {
  int c = id % 4, r = id / 4;
  x = 8 + c * 76; y = 66 + r * 42; w = 72; h = 38;
}

// Bottone generico: 'hi' = evidenziato (segno armato), 'go' = stile conferma
void yugiBtn(int x, int y, int w, int h, const char* lab, bool hi, bool go) {
  uint16_t fill = go ? TFT_DARKGREEN : (hi ? 0x300A : TFT_BLACK);   // 0x300A = viola scuro
  uint16_t edge = go ? TFT_GREEN     : (hi ? TFT_GOLD : TFT_DARKGREY);
  uint16_t txt  = go ? TFT_WHITE     : (hi ? TFT_GOLD : TFT_WHITE);
  tft.fillRoundRect(x, y, w, h, 6, fill);
  tft.drawRoundRect(x, y, w, h, 6, edge);
  tft.setTextDatum(MC_DATUM); tft.setTextSize(1);
  tft.setTextColor(txt, fill);
  tft.drawString(lab, x + w/2, y + h/2, 4);
}

// Riga compatta in alto per OPS/CALC: timer a sx, i due LP (attivo evidenziato)
void yugiTopBar() {
  tft.fillRect(0, 0, 320, 45, TFT_BLACK);
  drawYugiTime(52, 20, 4);
  tft.setTextDatum(MC_DATUM); tft.setTextSize(1);
  for (int p = 0; p < 2; p++) {
    bool sel = (p == yugiActive);
    int cx = (p == 0) ? 165 : 265;
    tft.setTextColor(sel ? uiAccent : TFT_DARKGREY, TFT_BLACK);
    tft.drawString(p == 0 ? "P1" : "P2", cx, 10, 2);
    tft.setTextColor(sel ? TFT_WHITE : TFT_LIGHTGREY, TFT_BLACK);
    tft.drawString(String(yugiLP[p]), cx, 28, 4);
  }
  tft.drawFastHLine(0, 45, 320, TFT_DARKGREY);
}

void drawYugiohBase() {
  tft.fillScreen(TFT_BLACK);
  tft.setTextDatum(MC_DATUM); tft.setTextSize(1);
  tft.setTextColor(uiAccent, TFT_BLACK);
  tft.drawString("YU-GI-OH!", 160, 16, 4);
  tft.drawFastHLine(20, 34, 280, TFT_DARKGREY);
  tft.setTextColor(yugiTimeUp ? TFT_RED : TFT_DARKGREY, TFT_BLACK);
  tft.drawString(yugiTimeUp ? "TEMPO!" : "max 50:00", 160, 92, 2);

  drawYugiTime(160, 62, 6);                       // timer match grande

  tft.drawFastVLine(160, 104, 122, TFT_DARKGREY);
  for (int p = 0; p < 2; p++) {
    bool sel = (p == yugiActive) && !yugiWinPending;
    int cx = HALF_CX[p];
    if (sel) tft.drawRoundRect(cx - 74, 108, 148, 116, 8, uiAccent);
    tft.setTextDatum(MC_DATUM); tft.setTextSize(1);
    tft.setTextColor(sel ? uiAccent : TFT_DARKGREY, TFT_BLACK);
    tft.drawString(p == 0 ? "P1" : "P2", cx, 126, 4);
    tft.setTextColor(sel ? TFT_WHITE : TFT_LIGHTGREY, TFT_BLACK);
    tft.drawString(String(yugiLP[p]), cx, 176, 6);
  }

  if (yugiWinPending) {
    tft.fillRoundRect(40, 96, 240, 54, 10, TFT_DARKGREEN);
    tft.drawRoundRect(40, 96, 240, 54, 10, TFT_GREEN);
    tft.setTextDatum(MC_DATUM);
    tft.setTextColor(TFT_WHITE, TFT_DARKGREEN);
    tft.drawString(String(yugiWinner == 0 ? "P1" : "P2") + " VINCE!", 160, 123, 4);
    drawFooter("OK conferma     ANNULLA menu");
  } else {
    drawFooter("SX/DX giocatore   OK modifica LP   ANNULLA menu");
  }
}

void drawYugiohOps() {
  tft.fillScreen(TFT_BLACK);
  yugiTopBar();
  const char* lab[9] = { "-", "CALC", "+", "1000", "50", "100", "/2", "50", "x2" };
  for (int id = 0; id < 9; id++) {
    int x, y, w, h; yugiOpsRect(id, x, y, w, h);
    bool hi = (id == 0 && yugiSign < 0) || (id == 2 && yugiSign > 0);
    yugiBtn(x, y, w, h, lab[id], hi, false);
  }
  drawFooter("+/- poi un valore   CALC apre la calcolatrice   ANNULLA indietro");
}

void drawYugiohCalc() {
  tft.fillScreen(TFT_BLACK);
  yugiTopBar();
  tft.setTextDatum(ML_DATUM); tft.setTextSize(1);
  tft.setTextColor(uiAccent, TFT_BLACK);
  String pre  = String(yugiSign < 0 ? "- " : "+ ") + "su P" + String(yugiActive + 1) + " = ";
  String body = (yugiCalcOp == ' ') ? String(yugiCalcCur)
              : String(yugiCalcAcc) + " " + yugiCalcOp + " " + String(yugiCalcCur);
  tft.drawString(pre + body, 10, 56, 2);
  const char* lab[16] = { "7","8","9","C", "4","5","6","-", "1","2","3","+", "0","00","=","<" };
  for (int id = 0; id < 16; id++) {
    int x, y, w, h; yugiCalcRect(id, x, y, w, h);
    bool isOp = (id == 7 || id == 11);     // - e +
    bool isEq = (id == 14);                // =
    yugiBtn(x, y, w, h, lab[id], isOp, isEq);
  }
  drawFooter("componi il valore, poi =  lo applica ai LP");
}

// ----------------------- Yu-Gi-Oh: input ------------------------
void yugiTapOps(int tx, int ty) {
  for (int id = 0; id < 9; id++) {
    int x, y, w, h; yugiOpsRect(id, x, y, w, h);
    if (tx < x || tx >= x + w || ty < y || ty >= y + h) continue;
    long mult = 0;
    switch (id) {
      case 0: yugiSign = -1; redraw = true; return;                 // -
      case 2: yugiSign = +1; redraw = true; return;                 // +
      case 1:                                                        // CALC (la "mano")
        if (yugiSign != 0) {
          yugiCalcAcc = yugiCalcCur = 0; yugiCalcOp = ' '; yugiCalcFresh = true;
          yugiScreen = 2; redraw = true;
        } else beepBlocking(400, 120);                               // serve prima +/-
        return;
      case 6: yugiApplyDelta(-(yugiLP[yugiActive] / 2)); redraw = true; return;  // /2 dimezza
      case 8: yugiApplyDelta(  yugiLP[yugiActive]);      redraw = true; return;  // x2 raddoppia
      case 3: mult = 1000; break;
      case 4: mult = 50;   break;
      case 5: mult = 100;  break;
      case 7: mult = 50;   break;
    }
    if (yugiSign == 0) { beepBlocking(400, 120); return; }           // valore senza segno armato
    yugiApplyDelta((long)yugiSign * mult);
    redraw = true;
    return;
  }
}

void yugiTapCalc(int tx, int ty) {
  const char keys[16] = { '7','8','9','C', '4','5','6','-', '1','2','3','+', '0','Z','=','<' };
  for (int id = 0; id < 16; id++) {
    int x, y, w, h; yugiCalcRect(id, x, y, w, h);
    if (tx < x || tx >= x + w || ty < y || ty >= y + h) continue;
    char k = keys[id];
    if (k >= '0' && k <= '9') {
      if (yugiCalcFresh) { yugiCalcCur = k - '0'; yugiCalcFresh = false; }
      else if (yugiCalcCur < 10000) yugiCalcCur = yugiCalcCur * 10 + (k - '0');
    } else if (k == 'Z') {                  // tasto "00"
      if (!yugiCalcFresh && yugiCalcCur <= 999) yugiCalcCur *= 100;
      yugiCalcFresh = false;
    } else if (k == '+' || k == '-') {
      yugiCalcAcc = yugiCalcChain();
      yugiCalcOp = k; yugiCalcFresh = true;
    } else if (k == 'C') {
      yugiCalcAcc = yugiCalcCur = 0; yugiCalcOp = ' '; yugiCalcFresh = true;
    } else if (k == '=') {                  // calcola e APPLICA ai LP col segno
      long res = yugiCalcChain();
      if (res < 0) res = 0;
      yugiApplyDelta((long)yugiSign * res);
      yugiScreen = 0; yugiSign = 0;
      yugiResumeClock();
    } else if (k == '<') {                  // indietro alle OPS senza applicare
      yugiScreen = 1;
    }
    redraw = true;
    return;
  }
}

bool touchDown = false;
void yugiohPollTouch() {
#ifdef TOUCH_CS
  uint16_t tx, ty;
  bool down = tft.getTouch(&tx, &ty);
  if (down && !touchDown) {
    touchDown = true;
    digitalWrite(PIN_LED, HIGH); ledOffAt = millis() + 60; beepBlocking(1400, 18);
    if      (yugiScreen == 1) yugiTapOps(tx, ty);
    else if (yugiScreen == 2) yugiTapCalc(tx, ty);
  } else if (!down) {
    touchDown = false;
  }
#endif
}

void handleYugioh(BtnEvent ev) {
  if (yugiWinPending) {                      // vittoria: serve conferma esplicita
    if (ev == EV_OK) {
      hostSend(String("{\"type\":\"match_end\",\"game\":\"yugioh\",\"winner\":") +
               (yugiWinner + 1) + ",\"lp\":[" + yugiLP[0] + "," + yugiLP[1] + "]" +
               ",\"p1\":" + hostPlayerJson(0) + ",\"p2\":" + hostPlayerJson(1) + "}");
      changeState(ST_MATCH_END);
    } else if (ev == EV_CANCEL) changeState(ST_MENU);
    return;
  }
  if (yugiScreen == 0) {                     // BASE: scegli il giocatore coi bottoni
    if (ev == EV_LEFT)  { yugiActive = 0; redraw = true; }
    if (ev == EV_RIGHT) { yugiActive = 1; redraw = true; }
    if (ev == EV_OK)    { yugiScreen = 1; yugiSign = 0; yugiPauseClock(); redraw = true; }
    if (ev == EV_CANCEL) changeState(ST_MENU);
  } else {                                   // OPS/CALC: ANNULLA torna indietro
    if (ev == EV_CANCEL) {
      if (yugiScreen == 2) { yugiScreen = 1; redraw = true; }
      else { yugiScreen = 0; yugiSign = 0; yugiResumeClock(); redraw = true; }
    }
    // le operazioni vere su LP avvengono via touch (yugiohPollTouch)
  }
}

// ------------------------- Fine match ---------------------------
void drawMatchEnd() {
  drawHeader("FINE MATCH");
  tft.setTextColor(TFT_GREEN, TFT_BLACK);
  if (currentGame == GAME_POKEMON && pkmnWinner >= 0) {
    tft.drawString(String(pkmnWinner == 0 ? "P1" : "P2") + " VINCE!", 160, 88, 4);
    tft.setTextColor(TFT_WHITE, TFT_BLACK);
    tft.drawString(String("Prize  ") + pkmnPrize[0] + " - " + pkmnPrize[1], 160, 122, 4);
    tft.setTextColor(TFT_DARKGREY, TFT_BLACK);
    tft.drawString("Tempi  " + fmtTime(pkmnElapsed[0]) + "  /  " + fmtTime(pkmnElapsed[1]),
                   160, 155, 2);
  } else if (currentGame == GAME_SCACCHI && chessFlag >= 0) {
    tft.drawString(String("P") + (2 - chessFlag) + " VINCE A TEMPO", 160, 88, 4);
    tft.setTextColor(TFT_WHITE, TFT_BLACK);
    tft.drawString(String("Mosse  ") + chessMoves[0] + " - " + chessMoves[1], 160, 122, 4);
    tft.setTextColor(TFT_DARKGREY, TFT_BLACK);
    tft.drawString("Residui  " + fmtTime(chessRemain[0]) + "  /  " + fmtTime(chessRemain[1]),
                   160, 155, 2);
  } else {
    tft.drawString("Match concluso!", 160, 95, 4);
  }
  tft.setTextColor(TFT_DARKGREY, TFT_BLACK);
  tft.drawString("(risultato inviato all'host via @HOST)", 160, 185, 2);
  drawFooter("OK torna al menu");
  // HOOK HOSTLINK: con il WiFi gli stessi JSON andranno su WebSocket
}

void drawRng() {
  drawHeader("RNG");
  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  tft.setTextSize(1);
  String sel = String("<  ") + RNG_NAME[rngSel] + "  >";
  tft.drawString(sel, 160, 72, 4);

  if (rngResult < 0) {
    tft.setTextColor(TFT_DARKGREY, TFT_BLACK);
    tft.drawString("premi OK per lanciare", 160, 140, 4);
  } else {
    tft.setTextColor(TFT_YELLOW, TFT_BLACK);
    String res = (rngSel == 0) ? String(rngResult == 1 ? "TESTA" : "CROCE")
                               : String(rngResult);
    tft.setTextSize(rngSel == 0 ? 2 : 3);
    tft.drawString(res, 160, 142, 4);
    tft.setTextSize(1);
    tft.setTextColor(TFT_DARKGREY, TFT_BLACK);
    tft.drawString(String("lanci: ") + rngRolls, 160, 190, 2);
  }
  drawFooter("SX/DX cambia   OK lancia   ANNULLA esci");
}

void drawScreen() {
  uiAccent = accentFor(currentGame);     // palette del gioco corrente (giallo/viola/arancio)
  switch (state) {
    case ST_SPLASH:      drawSplash();     break;
    case ST_MENU:        drawMenu();       break;
    case ST_PLAYERS:     drawPlayers();    break;
    case ST_CHESS_SETUP: drawChessSetup(); break;
    case ST_MATCH:
      if      (currentGame == GAME_POKEMON) drawPokemon();
      else if (currentGame == GAME_SCACCHI) drawChess();
      else {                                          // Yu-Gi-Oh: tre schermate interne
        if      (yugiScreen == 1) drawYugiohOps();
        else if (yugiScreen == 2) drawYugiohCalc();
        else                      drawYugiohBase();
      }
      break;
    case ST_MATCH_END:   drawMatchEnd();   break;
    case ST_RNG:         drawRng();        break;
  }
}

// -------------------------- Setup -------------------------------
void setup() {
  Serial.begin(115200);
  delay(300);
  pinMode(PIN_LED, OUTPUT);
  pinMode(PIN_BUZZ, OUTPUT);
  for (int i = 0; i < 4; i++) pinMode(BTN_PIN[i], INPUT_PULLUP);

  randomSeed(esp_random());   // seme dal generatore hardware: dadi onesti

  nfcInit();                  // PN532 su I2C (se manca, si va avanti senza)
  imuInit();                  // MPU6500 sullo stesso bus I2C (gesti: ribalta/shake)

  tft.init();
  tft.setRotation(1);         // landscape 320x240 (se lo vedi ruotato: 0-3)
#ifdef TOUCH_CS
  uint16_t calData[5] = { 212, 3541, 377, 3389, 1 };  // la tua calibrazione
  tft.setTouch(calData);      // il touch servira' per il tastierino Yu-Gi-Oh
#endif

  drawSplash();               // qualcosa a schermo mentre il WiFi prova a connettersi
  wifiSetup();                // WiFi + WebSocket verso server.js (l'hello parte al connect)

  changeState(ST_SPLASH);
  Serial.println("Scheletro v0.4 pronto.");
  Serial.println("--- Seriale: a=SX  d=DX  o=OK  c=ANNULLA  r=RNG  p=PASSA TURNO/MOSSA ---");
}

// --------------------------- Loop -------------------------------
void loop() {
  unsigned long now = millis();

  webSocket.loop();                    // tiene viva la connessione all'host
  // Battito periodico: dice all'host che il cubo e' vivo + segnale WiFi
  if (wsConnected && now - lastHeartbeat > HEARTBEAT_MS) {
    lastHeartbeat = now;
    hostSend(String("{\"type\":\"heartbeat\",\"rssi\":") + WiFi.RSSI() + "}");
  }

  BtnEvent gestureEv = pollGesture();     // IMU: campiona SEMPRE (anche se non usato qui)
  BtnEvent ev = pollButtons();
  if (ev == EV_NONE) ev = pollSerial();   // pulsanti assenti? pilota da monitor seriale
  if (ev == EV_NONE) ev = gestureEv;      // ne' pulsanti ne' seriale? prendo il gesto IMU
  if (ev != EV_NONE) feedbackTick(ev);

  // RNG apribile quasi ovunque con ANNULLA lungo (sostituto dello shake).
  // I match in corso mettono in pausa i loro orologi.
  // HOOK GESTI: lo shake dell'IMU generera' lo stesso evento.
  if ((ev == EV_CANCEL_LONG || ev == EV_SHAKE) && state != ST_RNG && state != ST_SPLASH) {
    if (state == ST_MATCH && currentGame == GAME_POKEMON) {
      pokemonPauseClock();
    }
    if (state == ST_MATCH && currentGame == GAME_SCACCHI) {
      chessResumeRng = chessRunning;     // ricorda se l'orologio era in marcia
      chessPauseClock();
    }
    if (state == ST_MATCH && currentGame == GAME_YUGIOH) {
      yugiPauseClock();
    }
    rngReturnTo = state;
    rngResult   = -1;
    rngRolls    = 0;
    changeState(ST_RNG);
    ev = EV_NONE;
  }

  switch (state) {

    case ST_SPLASH:
      if (now - stateEnteredAt > 1800 || ev == EV_OK) changeState(ST_MENU);
      break;

    case ST_MENU:
      if (ev == EV_LEFT)  { menuSel = (menuSel + GAME_COUNT - 1) % GAME_COUNT; redraw = true; }
      if (ev == EV_RIGHT) { menuSel = (menuSel + 1) % GAME_COUNT;              redraw = true; }
      if (ev == EV_OK) {
        currentGame = (Game)menuSel;
        playersReset();                  // nuova identificazione per ogni match
        changeState(ST_PLAYERS);
      }
      break;

    case ST_PLAYERS:
      pollPlayersNfc(now);               // primo tap = P1, secondo = P2
      if (warnUntil && now > warnUntil) { warnUntil = 0; redraw = true; }
      if (playersFound == 2 && ev == EV_NONE && warnUntil == 0) {
        // niente: si aspetta l'OK esplicito per avviare (HCI: conferma)
      }
      if (ev == EV_OK)     startMatch(); // con 2 tag = avvia, senza = salta
      if (ev == EV_CANCEL) changeState(ST_MENU);
      break;

    case ST_CHESS_SETUP:
      // HOOK HOSTLINK: con il WiFi il formato arrivera' dal PC host
      handleChessSetup(ev);
      break;

    case ST_MATCH:
      if      (currentGame == GAME_POKEMON) handlePokemon(ev);
      else if (currentGame == GAME_SCACCHI) handleChess(ev);
      else                                  handleYugioh(ev);
      break;

    case ST_MATCH_END:
      if (ev == EV_OK || ev == EV_CANCEL) changeState(ST_MENU);
      break;

    case ST_RNG:
      if (ev == EV_LEFT)  { rngSel = (rngSel + RNG_COUNT - 1) % RNG_COUNT; rngResult = -1; redraw = true; }
      if (ev == EV_RIGHT) { rngSel = (rngSel + 1) % RNG_COUNT;             rngResult = -1; redraw = true; }
      if (ev == EV_OK || ev == EV_SHAKE) {            // OK o un altro shake = lancia
        rngResult = random(1, RNG_MAX[rngSel] + 1);   // 1..max incluso
        rngRolls++;
        redraw = true;
        Serial.printf("[RNG] %s -> %d\n", RNG_NAME[rngSel], rngResult);
      }
      if (ev == EV_CANCEL) {
        changeState(rngReturnTo);
        if (rngReturnTo == ST_MATCH && currentGame == GAME_POKEMON)
          pokemonResumeClock();          // riparte solo se non e' gia' TEMPO
        if (rngReturnTo == ST_MATCH && currentGame == GAME_SCACCHI && chessResumeRng)
          chessResumeClock();            // riparte solo se era in marcia prima
        if (rngReturnTo == ST_MATCH && currentGame == GAME_YUGIOH)
          yugiResumeClock();             // riparte solo se siamo alla schermata base
      }
      break;
  }

  // Touch Yu-Gi-Oh: operazioni rapide sui LP (gli altri giochi usano i pulsanti).
  // Gira fuori dal flusso pulsanti perche' un tap puo' cambiare schermata e LP;
  // i tap impostano 'redraw', percio' va chiamato prima del blocco qui sotto.
  if (state == ST_MATCH && currentGame == GAME_YUGIOH) yugiohPollTouch();

  if (redraw) { drawScreen(); redraw = false; }

  // Tick orologi: ~4 volte/sec, ridisegnando SOLO le zone dei timer
  // (niente flicker da fillScreen). Qui vivono anche i controlli di
  // fine tempo: tetto 50' per Pokemon, bandierina per gli scacchi.
  static unsigned long lastClockTick = 0;
  if (state == ST_MATCH && now - lastClockTick > 250) {
    if (currentGame == GAME_POKEMON && pkmnClockRunning && !pkmnWinPending) {
      lastClockTick = now;
      drawClock(0);
      drawClock(1);
      drawPkmnTotal();
      pokemonCheckTimeLimit();
    } else if (currentGame == GAME_SCACCHI && chessRunning) {
      lastClockTick = now;
      chessCheckFlag();                  // prima il controllo bandierina...
      drawChessClock(0);                 // ...poi il refresh dei due orologi
      drawChessClock(1);
    } else if (currentGame == GAME_YUGIOH && yugiClockRunning && !yugiWinPending) {
      lastClockTick = now;
      yugiCheckTimeLimit();              // tetto 50': ferma il clock e segna TEMPO!
      if (yugiTimeUp) redraw = true;     // appena scaduto: redraw completo (mostra TEMPO!)
      else drawYugiTime(160, 62, 6);     // il clock scorre solo in base: aggiorno li' il tempo
    }
  }

  // Spegnimento LED non bloccante
  if (ledOffAt != 0 && now > ledOffAt) { digitalWrite(PIN_LED, LOW); ledOffAt = 0; }
}
