/*
 * NODO 1 - Transmisor (sensores + actuadores + LoRa)
 * Proyecto IoT: protección térmica del ganado ante heladas
 *
 * Lee:
 *   - 2 x DS18B20 (temperatura de suelo/superficie) en bus OneWire, GPIO 4
 *   - BME280 (temperatura, humedad, presión) por I2C, GPIO 21/22
 *   - BMP280 opcional (temperatura, presión) por I2C, mismo bus
 * Controla (con histéresis):
 *   - Calefacción: láminas 12 V vía MOSFET en GPIO 25
 *   - Cortina: motorreductor DC vía TB6612FNG + finales de carrera
 * Envía un paquete de texto por LoRa 433 MHz cada INTERVALO_ENVIO_MS.
 *
 * El loop NO se bloquea: la cortina se vigila todo el tiempo (finales de
 * carrera y tiempo máximo de motor) aunque se esté leyendo o transmitiendo.
 *
 * Librerías (Gestor de librerías de Arduino IDE):
 *   OneWire, DallasTemperature, Adafruit BME280, Adafruit BMP280,
 *   Adafruit Unified Sensor, LoRa (Sandeep Mistry)
 * Placa: "ESP32 Dev Module"
 */

#include <SPI.h>
#include <Wire.h>
#include <LoRa.h>
#include <OneWire.h>
#include <DallasTemperature.h>
#include <Adafruit_Sensor.h>
#include <Adafruit_BME280.h>
#include <Adafruit_BMP280.h>

// =================================================================
//  AJUSTES RÁPIDOS (umbrales en °C, se pueden cambiar)
// =================================================================
// 1 = los actuadores se mueven de verdad; 0 = solo se calcula y se reporta
#define ACTUADORES_HABILITADOS 1

// Calefacción: se enciende al bajar a CALEF_ON_C y se apaga al subir a CALEF_OFF_C
#define CALEF_ON_C    3.0
#define CALEF_OFF_C   7.0
// Cortina: se despliega al bajar a CORT_BAJA_C y se recoge al subir a CORT_SUBE_C
#define CORT_BAJA_C   3.0
#define CORT_SUBE_C   8.0

// Motor de la cortina
#define VELOCIDAD_MOTOR       200      // 0-255 (PWM)
#define TIEMPO_MAX_MOTOR_MS   20000UL  // si no llega al final de carrera, se para y marca error
#define TIEMPO_RECOGER_MS     10000UL  // sin 2.º final de carrera: cuánto tiempo gira para recoger (mídelo)
#define INVERTIR_MOTOR        0        // pon 1 si la cortina se mueve al revés

// Finales de carrera (contacto NA a GND + pull-up externo 10k a 3V3)
#define USAR_FC_RETRAIDA      0        // 1 si ya instalaste el 2.º final de carrera (GPIO 35)
// =================================================================

// ---------------- Pines ----------------
// LoRa Ra-01 SX1278 (SPI por defecto del ESP32: SCK 18, MISO 19, MOSI 23)
#define LORA_SCK   18
#define LORA_MISO  19
#define LORA_MOSI  23
#define LORA_CS    27
#define LORA_RST   14
#define LORA_DIO0  26

#define ONEWIRE_PIN 4     // DATA de los dos DS18B20 + pull-up 10k a 3V3
#define I2C_SDA     21
#define I2C_SCL     22

// Actuadores
#define PIN_MOSFET_CALEF 25
#define PIN_TB_PWMA      32
#define PIN_TB_AIN1      16
#define PIN_TB_AIN2      17
#define PIN_TB_STBY      33
#define PIN_FC_DESPLEGADA 34   // solo entrada, sin pull-up interno
#define PIN_FC_RETRAIDA   35   // solo entrada, sin pull-up interno

// ---------------- Radio LoRa (igual en ambos nodos) ----------------
#define LORA_FRECUENCIA  433E6
#define LORA_SF          7
#define LORA_BW          125E3
#define LORA_CR          5       // 4/5
#define LORA_SYNC_WORD   0x12
#define LORA_TX_DBM      14

// ---------------- Aplicación ----------------
#define NODE_ID              1
#define INTERVALO_ENVIO_MS   10000UL
#define ESPERA_DS18_MS       800UL     // conversión DS18B20 a 12 bits

// Bits del campo ERR (0 = todo bien)
#define ERR_T1     0x01
#define ERR_T2     0x02
#define ERR_BME    0x04
#define ERR_BMP    0x08
#define ERR_MOTOR  0x10   // la cortina no llegó al final de carrera a tiempo

// Estado físico de la cortina (campo CEST)
enum EstadoCortina { RETRAIDA = 0, DESPLEGADA = 1, DESPLEGANDO = 2, RETRAYENDO = 3, DESCONOCIDA = 4, FALLA = 5 };
const char *nombreCortina[] = {"RETRAIDA", "DESPLEGADA", "DESPLEGANDO", "RETRAYENDO", "DESCONOCIDA", "FALLA"};

OneWire oneWire(ONEWIRE_PIN);
DallasTemperature ds18(&oneWire);
DeviceAddress dirT1, dirT2;
bool hayT1 = false, hayT2 = false;

Adafruit_BME280 bme;
Adafruit_BMP280 bmp;
bool hayBME = false, hayBMP = false;
uint8_t dirBME = 0, dirBMP = 0;

uint32_t secuencia = 0;
bool calefSolicitada = false;
bool cortinaSolicitada = false;      // true = desplegar
EstadoCortina cortina = DESCONOCIDA;
unsigned long inicioMotor = 0;
bool falloMotor = false;

unsigned long ultimoEnvio = 0;
unsigned long inicioConversion = 0;
bool esperandoDS18 = false;
bool hayDecision = false;            // no mover nada hasta la primera lectura

// ---------------------------------------------------------------
void imprimirDireccion(const DeviceAddress d) {
  for (uint8_t i = 0; i < 8; i++) {
    if (d[i] < 16) Serial.print('0');
    Serial.print(d[i], HEX);
  }
}

void escanearI2C() {
  Serial.println(F("Escaneando bus I2C..."));
  uint8_t n = 0;
  for (uint8_t a = 1; a < 127; a++) {
    Wire.beginTransmission(a);
    if (Wire.endTransmission() == 0) {
      Serial.printf("  Dispositivo en 0x%02X\n", a);
      n++;
    }
  }
  if (n == 0) Serial.println(F("  Ninguno: revisa SDA=21, SCL=22, 3V3 y GND"));
}

void iniciarSensores() {
  ds18.begin();
  int n = ds18.getDeviceCount();
  Serial.printf("DS18B20 detectados: %d (se esperan 2)\n", n);
  hayT1 = ds18.getAddress(dirT1, 0);
  hayT2 = ds18.getAddress(dirT2, 1);
  if (hayT1) { Serial.print(F("  T1 = ")); imprimirDireccion(dirT1); Serial.println(); }
  if (hayT2) { Serial.print(F("  T2 = ")); imprimirDireccion(dirT2); Serial.println(); }
  ds18.setResolution(12);
  ds18.setWaitForConversion(false);   // no bloquear el loop 750 ms

  Wire.begin(I2C_SDA, I2C_SCL);
  escanearI2C();

  const uint8_t direcciones[2] = {0x76, 0x77};
  for (uint8_t i = 0; i < 2 && !hayBME; i++) {
    if (bme.begin(direcciones[i], &Wire)) { hayBME = true; dirBME = direcciones[i]; }
  }
  for (uint8_t i = 0; i < 2 && !hayBMP; i++) {
    if (direcciones[i] == dirBME) continue;
    if (bmp.begin(direcciones[i], BMP280_CHIPID)) { hayBMP = true; dirBMP = direcciones[i]; }
  }
  if (hayBME) Serial.printf("BME280 OK en 0x%02X\n", dirBME);
  else        Serial.println(F("BME280 NO encontrado"));
  if (hayBMP) Serial.printf("BMP280 OK en 0x%02X\n", dirBMP);
  else        Serial.println(F("BMP280 no encontrado (opcional)"));
  if (!hayBME && hayBMP)
    Serial.println(F("  Aviso: si solo aparece un BMP280, tu módulo 'GY-BME280' podría ser en realidad un BMP280 (sin humedad)."));
}

void iniciarLoRa() {
  SPI.begin(LORA_SCK, LORA_MISO, LORA_MOSI, LORA_CS);
  LoRa.setPins(LORA_CS, LORA_RST, LORA_DIO0);
  while (!LoRa.begin(LORA_FRECUENCIA)) {
    Serial.println(F("LoRa NO inicia: revisa cableado SPI, NSS=27, RST=14, 3V3. Reintento en 2 s"));
    delay(2000);
  }
  LoRa.setSpreadingFactor(LORA_SF);
  LoRa.setSignalBandwidth(LORA_BW);
  LoRa.setCodingRate4(LORA_CR);
  LoRa.setSyncWord(LORA_SYNC_WORD);
  LoRa.enableCrc();
  LoRa.setTxPower(LORA_TX_DBM);
  Serial.println(F("LoRa OK a 433 MHz"));
}

// ---------------- Cortina (TB6612FNG) ----------------
// Final de carrera presionado = LOW (contacto a GND). Se lee 2 veces para filtrar ruido.
bool fcPresionado(int pin) {
  if (digitalRead(pin) != LOW) return false;
  delayMicroseconds(200);
  return digitalRead(pin) == LOW;
}
bool fcDesplegada() { return fcPresionado(PIN_FC_DESPLEGADA); }
bool fcRetraida()   { return USAR_FC_RETRAIDA && fcPresionado(PIN_FC_RETRAIDA); }

void motorParar() {
  analogWrite(PIN_TB_PWMA, 0);
  digitalWrite(PIN_TB_AIN1, LOW);
  digitalWrite(PIN_TB_AIN2, LOW);
  digitalWrite(PIN_TB_STBY, LOW);
}

void motorMover(bool desplegar) {
  bool sentido = desplegar ^ (INVERTIR_MOTOR != 0);
  digitalWrite(PIN_TB_STBY, HIGH);
  digitalWrite(PIN_TB_AIN1, sentido ? HIGH : LOW);
  digitalWrite(PIN_TB_AIN2, sentido ? LOW : HIGH);
  analogWrite(PIN_TB_PWMA, VELOCIDAD_MOTOR);
}

void iniciarActuadores() {
  pinMode(PIN_MOSFET_CALEF, OUTPUT); digitalWrite(PIN_MOSFET_CALEF, LOW);
  pinMode(PIN_TB_PWMA, OUTPUT);
  pinMode(PIN_TB_AIN1, OUTPUT);
  pinMode(PIN_TB_AIN2, OUTPUT);
  pinMode(PIN_TB_STBY, OUTPUT);
  motorParar();
  pinMode(PIN_FC_DESPLEGADA, INPUT);   // requiere pull-up externo 10k a 3V3
  pinMode(PIN_FC_RETRAIDA, INPUT);

  if (fcDesplegada())     cortina = DESPLEGADA;
  else if (fcRetraida())  cortina = RETRAIDA;
  else if (USAR_FC_RETRAIDA) cortina = DESCONOCIDA;
  else                    cortina = RETRAIDA;   // sin 2.º final de carrera: se asume recogida
  Serial.printf("Cortina al arrancar: %s\n", nombreCortina[cortina]);
#if !ACTUADORES_HABILITADOS
  Serial.println(F("ACTUADORES DESHABILITADOS: solo se calcula y reporta"));
#endif
}

// Se llama en cada vuelta del loop: arranca, vigila y detiene el motor.
void controlarCortina() {
#if ACTUADORES_HABILITADOS
  if (!hayDecision) return;
  bool moviendo = (cortina == DESPLEGANDO || cortina == RETRAYENDO);

  if (moviendo) {
    bool llego = (cortina == DESPLEGANDO) ? fcDesplegada() : fcRetraida();
    unsigned long limite = (cortina == RETRAYENDO && !USAR_FC_RETRAIDA) ? TIEMPO_RECOGER_MS : TIEMPO_MAX_MOTOR_MS;
    bool tiempoAgotado = millis() - inicioMotor > limite;
    // Si cambia la orden a mitad de camino, se para y en la siguiente vuelta invierte
    bool ordenCambio = (cortina == DESPLEGANDO) != cortinaSolicitada;

    if (llego) {
      motorParar();
      cortina = (cortina == DESPLEGANDO) ? DESPLEGADA : RETRAIDA;
      falloMotor = false;
      Serial.printf("Cortina: %s (final de carrera)\n", nombreCortina[cortina]);
    } else if (tiempoAgotado) {
      motorParar();
#if !USAR_FC_RETRAIDA
      // Sin 2.º final de carrera, la retracción termina por tiempo (no es error)
      if (cortina == RETRAYENDO) {
        cortina = RETRAIDA;
        Serial.println(F("Cortina: RETRAIDA (por tiempo)"));
        return;
      }
#endif
      cortina = FALLA;
      falloMotor = true;
      Serial.println(F("Cortina: FALLA, el motor no llegó al final de carrera a tiempo"));
    } else if (ordenCambio) {
      motorParar();
      cortina = DESCONOCIDA;
      delay(300);   // pequeña pausa antes de invertir el giro
    }
    return;
  }

  // Detenida: ¿hay que moverla?
  if (cortina == FALLA) return;   // no reintentar solo: revisar el mecanismo y reiniciar
  if (cortinaSolicitada && cortina != DESPLEGADA) {
    if (fcDesplegada()) { cortina = DESPLEGADA; return; }
    motorMover(true);
    cortina = DESPLEGANDO;
    inicioMotor = millis();
    Serial.println(F("Cortina: desplegando..."));
  } else if (!cortinaSolicitada && cortina != RETRAIDA) {
    if (fcRetraida()) { cortina = RETRAIDA; return; }
    motorMover(false);
    cortina = RETRAYENDO;
    inicioMotor = millis();
    Serial.println(F("Cortina: recogiendo..."));
  }
#endif
}

void controlarCalefaccion() {
#if ACTUADORES_HABILITADOS
  digitalWrite(PIN_MOSFET_CALEF, calefSolicitada ? HIGH : LOW);
#else
  digitalWrite(PIN_MOSFET_CALEF, LOW);
#endif
}

// ---------------- Lecturas y envío ----------------
float leerDS(bool presente, const DeviceAddress dir) {
  if (!presente) return NAN;
  float t = ds18.getTempC(dir);
  if (t == DEVICE_DISCONNECTED_C || t == 85.0f || t < -55.0f || t > 125.0f) return NAN;
  return t;
}

void agregar(String &s, const char *clave, float v, uint8_t decimales) {
  s += ',';
  s += clave;
  s += '=';
  if (isnan(v)) s += "NA";
  else          s += String(v, (unsigned int)decimales);
}

void leerDecidirYEnviar() {
  float t1 = leerDS(hayT1, dirT1);
  float t2 = leerDS(hayT2, dirT2);

  float ta = NAN, h = NAN, p = NAN;
  if (hayBME) {
    ta = bme.readTemperature();
    h  = bme.readHumidity();
    p  = bme.readPressure() / 100.0f;
  }
  float tb = NAN, pb = NAN;
  if (hayBMP) {
    tb = bmp.readTemperature();
    pb = bmp.readPressure() / 100.0f;
  }

  uint8_t err = 0;
  if (isnan(t1)) err |= ERR_T1;
  if (isnan(t2)) err |= ERR_T2;
  if (isnan(ta) || isnan(h) || isnan(p)) err |= ERR_BME;
  if (hayBMP && (isnan(tb) || isnan(pb))) err |= ERR_BMP;
  if (falloMotor) err |= ERR_MOTOR;

  // --- Lógica con histéresis. Referencia: BME280; si falla, BMP280 ---
  float tRef = !isnan(ta) ? ta : tb;
  if (!isnan(tRef)) {
    if (!calefSolicitada && tRef <= CALEF_ON_C)  calefSolicitada = true;
    if ( calefSolicitada && tRef >= CALEF_OFF_C) calefSolicitada = false;
    if (!cortinaSolicitada && tRef <= CORT_BAJA_C) cortinaSolicitada = true;
    if ( cortinaSolicitada && tRef >= CORT_SUBE_C) cortinaSolicitada = false;
  } else {
    // Sin temperatura válida: calefacción apagada por seguridad, la cortina se queda como está
    calefSolicitada = false;
  }
  hayDecision = true;
  controlarCalefaccion();

  // ID=1,SEQ=12,T1=..,T2=..,TA=..,H=..,P=..,TB=..,PB=..,CALEFACCION=1,CORTINA=1,CEST=1,ERR=0
  String pkt = "ID=" + String(NODE_ID) + ",SEQ=" + String(secuencia);
  agregar(pkt, "T1", t1, 2);
  agregar(pkt, "T2", t2, 2);
  agregar(pkt, "TA", ta, 2);
  agregar(pkt, "H",  h,  1);
  agregar(pkt, "P",  p,  1);
  agregar(pkt, "TB", tb, 2);
  agregar(pkt, "PB", pb, 1);
  pkt += ",CALEFACCION=" + String(calefSolicitada ? 1 : 0);
  pkt += ",CORTINA="     + String(cortinaSolicitada ? 1 : 0);
  pkt += ",CEST="        + String((int)cortina);
  pkt += ",ERR="         + String(err);

  LoRa.beginPacket();          // espera si aún se está transmitiendo el anterior
  LoRa.print(pkt);
  LoRa.endPacket(true);        // asíncrono: no bloquea el control de la cortina

  Serial.printf("[TX] %s | cortina %s\n", pkt.c_str(), nombreCortina[cortina]);
  secuencia++;
}

// ---------------------------------------------------------------
void setup() {
  Serial.begin(115200);
  delay(500);
  Serial.println(F("\n=== NODO 1 - Transmisor ==="));

  iniciarActuadores();   // primero: todo apagado
  iniciarSensores();
  iniciarLoRa();
}

void loop() {
  controlarCortina();    // siempre, para no pasarse del final de carrera

  unsigned long ahora = millis();
  if (!esperandoDS18 && (ultimoEnvio == 0 || ahora - ultimoEnvio >= INTERVALO_ENVIO_MS)) {
    ultimoEnvio = ahora;
    ds18.requestTemperatures();   // no bloquea
    inicioConversion = ahora;
    esperandoDS18 = true;
  }
  if (esperandoDS18 && ahora - inicioConversion >= ESPERA_DS18_MS) {
    esperandoDS18 = false;
    leerDecidirYEnviar();
  }
}
