/*
  OBD LOGGER - ESP32-C3 (ESP32-2424S012) - con salvataggio su flash
  -----------------------------------------------------------------
  In auto: alimenta l'ESP32 con un caricatore USB. Appena si collega al Wi-Fi
  del lettore crea un nuovo file /log_NNN.txt e ci scrive tutto il log.

  A casa: collega al PC, apri il monitor seriale e usa i comandi:
    help      elenco comandi
    ls        elenca i file di log
    cat N     stampa il file log_N (es. "cat 3")
    rmall     cancella tutti i log
    stop      ferma i tentativi di connessione (monitor pulito)
    start     riprende i tentativi
    p         pausa/riprendi il log continuo
  Qualsiasi altra riga viene inviata al lettore OBD (se connesso).
  Il comando 04 (cancellazione errori) e' bloccato di proposito.
*/

#include <Arduino.h>
#include <WiFi.h>
#include <LittleFS.h>
#include <vector>

// ======================= CONFIGURAZIONE =======================
const char*    WIFI_SSID = "V-LINK";          // SSID esatto del lettore
const char*    WIFI_PASS = "";                // "" se la rete e' aperta
const char*    OBD_IP    = "192.168.0.10";    // "" = usa automaticamente il gateway
const uint16_t OBD_PORTS[] = {35000};         // porte provate in ordine

const uint32_t CMD_TIMEOUT_MS  = 3000;        // timeout comandi normali
const uint32_t INIT_TIMEOUT_MS = 12000;       // primo contatto con l'auto (K-line e' lenta)
const uint32_t LOG_PAUSE_MS    = 0;           // pausa tra un ciclo di log e l'altro
const size_t   MIN_FREE_BYTES  = 32 * 1024;   // sotto questa soglia smette di scrivere su file
// ==============================================================

// ---------- Uscita doppia: seriale + file ----------
class TeeLog : public Print {
public:
  File f;
  bool fileOk = false;
  size_t write(uint8_t c) override {
    Serial.write(c);
    if (fileOk) f.write(c);
    return 1;
  }
  size_t write(const uint8_t* buf, size_t n) override {
    Serial.write(buf, n);
    if (fileOk) f.write(buf, n);
    return n;
  }
};
TeeLog LOG;

WiFiClient obd;
IPAddress  obdIp;
uint16_t   obdPort = 0;
bool supported[256] = {false};
bool paused = false;
bool stopped = false;
bool lastTimeout = false;
bool fsOk = false;
String logPath;

void handleSerial();  // dichiarazione anticipata

// ---------- File di log ----------
void openLogFile() {
  if (LOG.fileOk || !fsOk) return;
  if (LittleFS.totalBytes() - LittleFS.usedBytes() < MIN_FREE_BYTES) {
    Serial.println("[MEMORIA PIENA: log solo su seriale. Usa 'rmall' a casa]");
    return;
  }
  char path[20];
  int n = 1;
  do { snprintf(path, sizeof path, "/log_%03d.txt", n++); } while (LittleFS.exists(path) && n < 1000);
  LOG.f = LittleFS.open(path, "w");
  LOG.fileOk = (bool)LOG.f;
  logPath = path;
  Serial.printf("[Log su file: %s]\n", LOG.fileOk ? path : "ERRORE apertura");
}

void flushLog() {
  if (!LOG.fileOk) return;
  LOG.f.flush();
  if (LittleFS.totalBytes() - LittleFS.usedBytes() < MIN_FREE_BYTES) {
    LOG.println("\n[MEMORIA PIENA: file chiuso, continuo solo su seriale]");
    LOG.f.close();
    LOG.fileOk = false;
  }
}

// ---------- Invio comando e lettura fino al prompt '>' ----------
String sendCmd(const String& cmd, uint32_t timeoutMs = CMD_TIMEOUT_MS, bool echo = true) {
  while (obd.available()) obd.read();          // svuota buffer
  if (echo) { LOG.print("> "); LOG.println(cmd); }
  obd.print(cmd);
  obd.print('\r');

  String resp;
  bool gotPrompt = false;
  uint32_t t0 = millis();
  while (millis() - t0 < timeoutMs && !gotPrompt) {
    while (obd.available()) {
      char c = (char)obd.read();
      if (c == '>') { gotPrompt = true; break; }
      if (c == '\r') c = '\n';
      if (c != 0) resp += c;
    }
    if (!gotPrompt) delay(2);
  }
  uint32_t elapsed = millis() - t0;
  lastTimeout = !gotPrompt;
  resp.trim();

  if (echo) {
    int start = 0;
    while (start < (int)resp.length()) {
      int nl = resp.indexOf('\n', start);
      if (nl < 0) nl = resp.length();
      String line = resp.substring(start, nl);
      line.trim();
      if (line.length()) { LOG.print("< "); LOG.println(line); }
      start = nl + 1;
    }
    if (lastTimeout) LOG.printf("  [TIMEOUT dopo %lu ms]\n", elapsed);
    else             LOG.printf("  [%lu ms]\n", elapsed);
  }
  return resp;
}

// ---------- Estrae i byte dati dopo l'intestazione (es. "410C") ----------
int extractBytes(const String& resp, const String& header, uint8_t* out, int maxLen) {
  int start = 0;
  while (start <= (int)resp.length()) {
    int nl = resp.indexOf('\n', start);
    if (nl < 0) nl = resp.length();
    String line = resp.substring(start, nl);
    start = nl + 1;
    line.replace(" ", "");
    line.trim();
    line.toUpperCase();

    bool hex = line.length() > 0;
    for (size_t i = 0; i < line.length(); i++)
      if (!isxdigit((unsigned char)line[i])) { hex = false; break; }
    if (!hex || !line.startsWith(header)) continue;

    int n = 0;
    for (size_t i = header.length(); i + 1 < line.length() && n < maxLen; i += 2)
      out[n++] = (uint8_t)strtoul(line.substring(i, i + 2).c_str(), nullptr, 16);
    return n;
  }
  return -1;  // nessuna riga valida
}

// ---------- Nomi PID standard ----------
const char* pidName(uint8_t pid) {
  switch (pid) {
    case 0x01: return "Stato MIL/DTC";
    case 0x03: return "Stato sist. carburante";
    case 0x04: return "Carico motore";
    case 0x05: return "Temp. refrigerante";
    case 0x06: return "STFT banco 1";
    case 0x07: return "LTFT banco 1";
    case 0x08: return "STFT banco 2";
    case 0x09: return "LTFT banco 2";
    case 0x0A: return "Press. carburante";
    case 0x0B: return "Press. collettore";
    case 0x0C: return "RPM";
    case 0x0D: return "Velocita'";
    case 0x0E: return "Anticipo";
    case 0x0F: return "Temp. aria aspirata";
    case 0x10: return "Flusso aria MAF";
    case 0x11: return "Farfalla";
    case 0x13: return "Sonde O2 presenti";
    case 0x14: case 0x15: case 0x16: case 0x17:
    case 0x18: case 0x19: case 0x1A: case 0x1B: return "Sonda O2";
    case 0x1C: return "Standard OBD";
    case 0x1D: return "Sonde O2 presenti (2)";
    case 0x1F: return "Tempo da avvio";
    case 0x21: return "Km con MIL accesa";
    case 0x2F: return "Livello carburante";
    case 0x31: return "Km da cancell. DTC";
    case 0x33: return "Press. barometrica";
    case 0x42: return "Tensione centralina";
    case 0x43: return "Carico assoluto";
    case 0x44: return "Lambda comandata";
    case 0x45: return "Farfalla relativa";
    case 0x46: return "Temp. ambiente";
    case 0x5C: return "Temp. olio";
    default:   return "(sconosciuto)";
  }
}

// ---------- Decodifica PID standard ----------
String decodePid(uint8_t pid, const uint8_t* d, int n) {
  if (n < 1) return "(vuoto)";
  float A = d[0], B = (n > 1) ? d[1] : 0;
  char s[48];
  switch (pid) {
    case 0x01: snprintf(s, sizeof s, "MIL=%s DTC=%d", (d[0] & 0x80) ? "ON" : "OFF", d[0] & 0x7F); break;
    case 0x04: case 0x11: case 0x2F: case 0x45:
               snprintf(s, sizeof s, "%.1f %%", A * 100 / 255); break;
    case 0x05: case 0x0F: case 0x46: case 0x5C:
               snprintf(s, sizeof s, "%.0f C", A - 40); break;
    case 0x06: case 0x07: case 0x08: case 0x09:
               snprintf(s, sizeof s, "%.1f %%", (A - 128) * 100 / 128); break;
    case 0x0A: snprintf(s, sizeof s, "%.0f kPa", A * 3); break;
    case 0x0B: case 0x33: snprintf(s, sizeof s, "%.0f kPa", A); break;
    case 0x0C: snprintf(s, sizeof s, "%.0f rpm", (A * 256 + B) / 4); break;
    case 0x0D: snprintf(s, sizeof s, "%.0f km/h", A); break;
    case 0x0E: snprintf(s, sizeof s, "%.1f gradi", A / 2 - 64); break;
    case 0x10: snprintf(s, sizeof s, "%.2f g/s", (A * 256 + B) / 100); break;
    case 0x14: case 0x15: case 0x16: case 0x17:
    case 0x18: case 0x19: case 0x1A: case 0x1B:
      if (B == 0xFF) snprintf(s, sizeof s, "%.3f V", A / 200);
      else           snprintf(s, sizeof s, "%.3f V trim %.1f %%", A / 200, (B - 128) * 100 / 128);
      break;
    case 0x1C: snprintf(s, sizeof s, "tipo %d", d[0]); break;
    case 0x1F: case 0x21: case 0x31:
               snprintf(s, sizeof s, "%.0f", A * 256 + B); break;
    case 0x42: snprintf(s, sizeof s, "%.2f V", (A * 256 + B) / 1000); break;
    case 0x43: snprintf(s, sizeof s, "%.1f %%", (A * 256 + B) * 100 / 255); break;
    case 0x44: snprintf(s, sizeof s, "%.3f", (A * 256 + B) * 2 / 65536); break;
    default:   snprintf(s, sizeof s, "-"); break;
  }
  return String(s);
}

// ---------- Wi-Fi ----------
void scanWifi() {
  Serial.println("\n=== RETI WI-FI VISIBILI ===");
  int n = WiFi.scanNetworks();
  if (n <= 0) Serial.println("  nessuna rete trovata");
  for (int i = 0; i < n; i++) {
    Serial.printf("  %-32s RSSI %4d dBm  ch %2d  %s\n",
                  WiFi.SSID(i).c_str(), WiFi.RSSI(i), WiFi.channel(i),
                  WiFi.encryptionType(i) == WIFI_AUTH_OPEN ? "APERTA" : "protetta");
  }
  WiFi.scanDelete();
}

bool connectWifi() {
  Serial.printf("\n=== CONNESSIONE WI-FI a \"%s\" ===\n", WIFI_SSID);
  WiFi.disconnect();
  delay(100);
  if (strlen(WIFI_PASS)) WiFi.begin(WIFI_SSID, WIFI_PASS);
  else                   WiFi.begin(WIFI_SSID);

  uint32_t t0 = millis(), lastPrint = 0;
  while (WiFi.status() != WL_CONNECTED && millis() - t0 < 20000) {
    handleSerial();
    if (stopped) return false;
    if (millis() - lastPrint >= 1000) {
      lastPrint = millis();
      Serial.printf("  attendo... stato=%d\n", WiFi.status());
    }
    delay(10);
  }
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("WI-FI FALLITO (lettore spento o fuori portata?)");
    return false;
  }

  openLogFile();  // il file nasce solo quando siamo davvero sulla rete del lettore
  LOG.printf("\n########## SESSIONE OBD  (uptime %lu ms) ##########\n", millis());
  LOG.println("WiFi connesso");
  LOG.print("  IP ESP32 : "); LOG.println(WiFi.localIP());
  LOG.print("  Gateway  : "); LOG.println(WiFi.gatewayIP());
  LOG.print("  Subnet   : "); LOG.println(WiFi.subnetMask());
  LOG.printf("  RSSI     : %d dBm\n", WiFi.RSSI());
  return true;
}

// ---------- TCP: prova le porte finche' trova un prompt ELM ----------
bool connectObd() {
  if (strlen(OBD_IP)) obdIp.fromString(OBD_IP);
  else                obdIp = WiFi.gatewayIP();
  LOG.printf("\n=== CONNESSIONE TCP a %s ===\n", obdIp.toString().c_str());

  for (uint16_t port : OBD_PORTS) {
    LOG.printf("Porta %u ... ", port);
    if (!obd.connect(obdIp, port, 3000)) {
      LOG.println("nessuna connessione");
      continue;
    }
    LOG.println("TCP connesso, provo ATI");
    sendCmd("ATI", 3000);
    if (!lastTimeout) {
      obdPort = port;
      LOG.printf(">>> Lettore trovato su %s:%u\n", obdIp.toString().c_str(), port);
      return true;
    }
    LOG.println("  nessun prompt '>': su questa porta non sembra un ELM327");
    obd.stop();
  }
  LOG.println("NESSUNA PORTA VALIDA");
  return false;
}

// ---------- Identificazione e init ----------
void initElm() {
  LOG.println("\n=== IDENTIFICAZIONE E INIT ===");
  sendCmd("ATZ", 5000);
  delay(500);
  sendCmd("ATE0");
  sendCmd("ATL0");
  sendCmd("ATS0");
  sendCmd("ATH0");
  sendCmd("ATI");
  sendCmd("AT@1");
  sendCmd("ATRV");
  sendCmd("ATSP0");
  LOG.println("-- Primo contatto con la centralina (puo' richiedere diversi secondi) --");
  sendCmd("0100", INIT_TIMEOUT_MS);
  sendCmd("ATDP");
  sendCmd("ATDPN");
  LOG.println("-- Centraline che rispondono (header attivi) --");
  sendCmd("ATH1");
  sendCmd("0100");
  sendCmd("ATH0");
}

// ---------- Scoperta PID supportati ----------
void discoverPids() {
  LOG.println("\n=== PID SUPPORTATI (modo 01) ===");
  memset(supported, 0, sizeof supported);
  for (int base = 0x00; base <= 0xC0; base += 0x20) {
    char cmd[5], hdr[5];
    sprintf(cmd, "01%02X", base);
    sprintf(hdr, "41%02X", base);
    String r = sendCmd(cmd, base == 0 ? INIT_TIMEOUT_MS : CMD_TIMEOUT_MS);
    uint8_t b[4];
    if (extractBytes(r, hdr, b, 4) < 4) { LOG.println("  nessuna risposta valida, stop"); break; }
    uint32_t mask = ((uint32_t)b[0] << 24) | ((uint32_t)b[1] << 16) | ((uint32_t)b[2] << 8) | b[3];
    for (int i = 0; i < 32; i++)
      if (mask & (1UL << (31 - i))) supported[base + i + 1] = true;
    if (!(mask & 1)) break;  // nessun blocco successivo
  }
  LOG.print("Supportati:");
  int count = 0;
  for (int p = 1; p < 256; p++)
    if (supported[p]) { LOG.printf(" %02X", p); count++; }
  LOG.printf("\nTotale: %d\n", count);
  for (int p = 1; p < 256; p++)
    if (supported[p] && (p % 0x20) != 0) LOG.printf("  %02X = %s\n", p, pidName(p));
}

// ---------- Info veicolo e codici errore (solo lettura) ----------
void readExtraInfo() {
  LOG.println("\n=== INFO VEICOLO (modo 09) ===");
  sendCmd("0900");
  sendCmd("0902", 5000);
  LOG.println("\n=== CODICI ERRORE (sola lettura) ===");
  sendCmd("0101");
  sendCmd("03", 5000);   // errori memorizzati
  sendCmd("07", 5000);   // errori in attesa
}

// ---------- Comandi locali (gestione file) - output solo su seriale ----------
void printHelp() {
  Serial.println("\nComandi: help | ls | cat N | rmall | stop | start | p | <comando OBD>");
}

bool handleLocal(const String& line) {
  if (line == "help") { printHelp(); return true; }

  if (line == "ls") {
    if (!fsOk) { Serial.println("[filesystem non disponibile]"); return true; }
    if (LOG.fileOk) LOG.f.flush();
    Serial.println("\n=== FILE DI LOG ===");
    File root = LittleFS.open("/");
    File f = root.openNextFile();
    int count = 0;
    while (f) {
      Serial.printf("  %-18s %8u byte%s\n", f.name(), (unsigned)f.size(),
                    (logPath == String("/") + f.name() && LOG.fileOk) ? "  (in scrittura)" : "");
      count++;
      f = root.openNextFile();
    }
    if (!count) Serial.println("  nessun file");
    Serial.printf("Usati %u / %u byte\n", (unsigned)LittleFS.usedBytes(), (unsigned)LittleFS.totalBytes());
    return true;
  }

  if (line.startsWith("cat ")) {
    int n = line.substring(4).toInt();
    char path[20];
    snprintf(path, sizeof path, "/log_%03d.txt", n);
    if (LOG.fileOk) LOG.f.flush();
    File f = LittleFS.open(path, "r");
    if (!f) { Serial.printf("[%s non trovato]\n", path); return true; }
    Serial.setTxTimeoutMs(500);   // durante il dump non perdere caratteri
    Serial.printf("\n===== INIZIO %s (%u byte) =====\n", path, (unsigned)f.size());
    uint8_t buf[256];
    int len;
    while ((len = f.read(buf, sizeof buf)) > 0) Serial.write(buf, len);
    f.close();
    Serial.printf("\n===== FINE %s =====\n", path);
    Serial.setTxTimeoutMs(0);
    return true;
  }

  if (line == "rmall") {
    if (LOG.fileOk) { LOG.f.close(); LOG.fileOk = false; }
    std::vector<String> names;
    File root = LittleFS.open("/");
    File f = root.openNextFile();
    while (f) { names.push_back(String("/") + f.name()); f = root.openNextFile(); }
    root.close();
    for (auto& p : names) LittleFS.remove(p);
    Serial.printf("[Cancellati %u file]\n", (unsigned)names.size());
    return true;
  }

  if (line == "stop") {
    stopped = true;
    obd.stop();
    WiFi.disconnect();
    Serial.println("[FERMO: nessuna riconnessione. 'start' per ripartire]");
    return true;
  }

  if (line == "start") {
    stopped = false;
    Serial.println("[RIPARTO]");
    return true;
  }
  return false;
}

// ---------- Comandi dalla Serial Monitor ----------
void handleSerial() {
  if (!Serial.available()) return;
  String line = Serial.readStringUntil('\n');
  line.trim();
  if (!line.length()) return;
  if (handleLocal(line)) return;
  if (line.equalsIgnoreCase("p")) {
    paused = !paused;
    LOG.println(paused ? "\n[PAUSA - scrivi comandi, 'p' per riprendere]" : "\n[RIPRESO]");
    return;
  }
  if (line.startsWith("04")) {
    Serial.println("[BLOCCATO: 04 cancella gli errori della centralina]");
    return;
  }
  if (!obd.connected()) {
    Serial.println("[OBD non connesso]");
    return;
  }
  sendCmd(line, INIT_TIMEOUT_MS);
}

// ---------- Un ciclo di log su tutti i PID supportati ----------
void logCycle() {
  static uint32_t cycle = 0;
  cycle++;
  uint32_t t0 = millis();
  LOG.printf("\n--- CICLO %lu  t=%lu ms ---\n", cycle, t0);

  for (int pid = 1; pid < 256; pid++) {
    if (!supported[pid] || (pid % 0x20) == 0) continue;
    handleSerial();
    if (paused || stopped || !obd.connected()) return;

    char cmd[5], hdr[5];
    sprintf(cmd, "01%02X", pid);
    sprintf(hdr, "41%02X", pid);
    String r = sendCmd(cmd, CMD_TIMEOUT_MS, false);
    uint8_t b[8];
    int n = extractBytes(r, hdr, b, 8);

    LOG.printf("%02X %-24s ", pid, pidName(pid));
    if (n < 0) {
      r.replace('\n', ' ');
      LOG.printf("ERR: %s%s\n", r.c_str(), lastTimeout ? " [TIMEOUT]" : "");
      continue;
    }
    LOG.printf("%-26s raw:", decodePid(pid, b, n).c_str());
    for (int i = 0; i < n; i++) LOG.printf(" %02X", b[i]);
    LOG.println();
  }
  LOG.printf("--- fine ciclo %lu: %lu ms ---\n", cycle, millis() - t0);
  flushLog();
}

// ---------- Sessione completa ----------
bool startSession() {
  if (WiFi.status() != WL_CONNECTED && !connectWifi()) return false;
  if (!connectObd()) { flushLog(); return false; }
  initElm();
  discoverPids();
  readExtraInfo();
  LOG.println("\n=== INIZIO LOG CONTINUO ===");
  flushLog();
  return true;
}

void waitWithSerial(uint32_t ms) {
  uint32_t t = millis();
  while (millis() - t < ms && !stopped) { handleSerial(); delay(10); }
}

void setup() {
  Serial.begin(115200);
  Serial.setTxTimeoutMs(0);   // senza PC collegato la seriale non deve rallentare nulla
  uint32_t t = millis();
  while (!Serial && millis() - t < 3000) delay(10);
  delay(500);
  Serial.println("\n\n########## OBD LOGGER ESP32-C3 (log su flash) ##########");

  fsOk = LittleFS.begin(true);  // al primo avvio formatta la partizione
  if (fsOk) Serial.printf("Filesystem OK: usati %u / %u byte\n",
                          (unsigned)LittleFS.usedBytes(), (unsigned)LittleFS.totalBytes());
  else      Serial.println("ERRORE filesystem: log solo su seriale");
  printHelp();

  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);
  scanWifi();
}

void loop() {
  if (stopped) { handleSerial(); delay(10); return; }

  if (WiFi.status() != WL_CONNECTED || !obd.connected()) {
    obd.stop();
    Serial.println("\n[Connessione assente: avvio sessione]");
    if (!startSession()) {
      Serial.println("Riprovo tra 5 s...");
      waitWithSerial(5000);
    }
    return;
  }
  handleSerial();
  if (!paused) {
    logCycle();
    if (LOG_PAUSE_MS) delay(LOG_PAUSE_MS);
  } else {
    delay(10);
  }
}