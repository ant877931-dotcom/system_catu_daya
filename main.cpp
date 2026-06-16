#include <Arduino.h>
#include <WiFi.h>
#include <Firebase_ESP_Client.h>
#include <addons/RTDBHelper.h>
#include <LiquidCrystal_I2C.h>
#include <Wire.h>
#include <PZEM004Tv30.h>
#include <Adafruit_INA219.h>
#include <time.h> 

// ==========================================
// DEKLARASI PIN KONEKSI
// ==========================================
#define PIN_PZEM_RX   17
#define PIN_PZEM_TX   16
#define PIN_ACS758    34 
#define PIN_ACS712    35 
#define PIN_PLN_DET   27 // Sensor Detektor PLN (Active Low)
#define PIN_RELAY     25 
#define PIN_BUZZER    4  

// ==========================================
// 1. DOMAIN (Entitas Data)
// ==========================================
struct AtsState {
    bool isPlnOn;
    bool isCharging;
    bool isOverload;

    float acVoltage;
    float acCurrent;
    float acPower;
    float acEnergy;

    float batteryVoltage;
    float outputDcCurrent; 
    float chargeCurrent;   
    float batterySoc;      
};

// ==========================================
// 2. INTERFACE (Port / Abstraksi)
// ==========================================
class IAcSensor {
public:
    virtual void readSensor() = 0;
    virtual float getPower() = 0;
    virtual float getVoltage() = 0;
    virtual float getCurrent() = 0;
    virtual float getEnergy() = 0;
};

class IDcSensor {
public:
    virtual float readValue() = 0; 
};

class IActuator {
public:
    virtual void turnOn() = 0;
    virtual void turnOff() = 0;
};

class IDigitalInput {
public:
    virtual bool isHigh() = 0;
};

// ==========================================
// 3. INFRASTRUCTURE (Implementasi Driver)
// ==========================================

// --- Driver PZEM004T ---
class PzemDriver : public IAcSensor {
private:
    PZEM004Tv30 pzem;
    float v, i, p, e;
    unsigned long lastPzemRead; 
public:
    PzemDriver(uint8_t rx, uint8_t tx) : pzem(Serial2, rx, tx), v(0), i(0), p(0), e(0), lastPzemRead(0) {}
    
    void readSensor() override {
        if (millis() - lastPzemRead >= 1000) {
            lastPzemRead = millis();
            float tempV = pzem.voltage();
            if (!isnan(tempV)) {
                v = tempV;
                i = pzem.current();
                p = pzem.power();
                e = pzem.energy();
            }
        }
    }
    float getPower() override { return p; }
    float getVoltage() override { return v; }
    float getCurrent() override { return i; }
    float getEnergy() override { return e; }
};

// --- Driver Aktuator ---
class DigitalActuator : public IActuator {
private:
    uint8_t pin;
public:
    DigitalActuator(uint8_t pinNumber) : pin(pinNumber) {
        pinMode(pin, OUTPUT);
        digitalWrite(pin, LOW);
    }
    void turnOn() override { digitalWrite(pin, HIGH); }
    void turnOff() override { digitalWrite(pin, LOW); }
};

// --- Driver Detektor PLN (FIX: ACTIVE LOW + DEBOUNCER) ---
class PlnDetector : public IDigitalInput {
private:
    uint8_t pin;
    unsigned long lastPlnOnTime; // Timer filter Zero-Crossing
public:
    PlnDetector(uint8_t pinNumber) : pin(pinNumber), lastPlnOnTime(0) {
        // [PERBAIKAN]: Gunakan PULLUP untuk sensor Active Low agar stabil di HIGH saat tidak ada PLN
        pinMode(pin, INPUT_PULLUP); 
    }
    
    bool isHigh() override { 
        // [PERBAIKAN]: Logika dibalik. LOW = Ada Listrik (Optocoupler aktif menyambung ke GND)
        if (digitalRead(pin) == LOW) {
            lastPlnOnTime = millis();
            return true;
        } else {
            // Jika HIGH (Tidak ada listrik), tunggu 1 detik untuk memastikan itu bukan sekadar kedipan AC (Zero-Crossing)
            if (millis() - lastPlnOnTime < 1000) {
                return true; 
            }
            return false;
        }
    }
};

// --- Driver ACS Analog ---
class AcsAnalogDriver : public IDcSensor {
private:
    uint8_t pin;
    float offsetVoltage;
    float sensitivity;
public:
    AcsAnalogDriver(uint8_t pinNumber, float offsetV, float sens) 
        : pin(pinNumber), offsetVoltage(offsetV), sensitivity(sens) {}
    
    float readValue() override {
        long sum = 0;
        for (int x = 0; x < 10; x++) sum += analogRead(pin);
        float adcAvg = sum / 10.0f;
        float voltage = (adcAvg * 3.3f) / 4095.0f;
        float current = (voltage - offsetVoltage) / sensitivity;
        return (current < 0) ? 0.0f : current;
    }
};

// --- Driver INA219 ---
class Ina219Driver : public IDcSensor {
private:
    Adafruit_INA219 ina219;
    bool isInit;
public:
    Ina219Driver() : isInit(false) {}
    
    void begin() {
        if (!ina219.begin()) {
            Serial.println("Warning: INA219 tidak terdeteksi!");
            isInit = false;
        } else {
            isInit = true;
        }
    }
    
    float readValue() override {
        if (!isInit) return 0.0f;
        return ina219.getBusVoltage_V() + (ina219.getShuntVoltage_mV() / 1000.0f);
    }
};

// --- Driver LCD 20x4 ULTRA STABLE ---
class UltraStableLcdDriver {
private:
    LiquidCrystal_I2C lcd;
    unsigned long lastSwitchTime;
    unsigned long lastRegRefresh;
    int currentPage;
    char displayBuffer[4][21]; 
    const unsigned long displayInterval = 5000; 
    const unsigned long regRefreshInterval = 500; 

    void safePrintRow(int row, const char* text) {
        lcd.setCursor(0, row);
        for (int i = 0; text[i] != '\0' && i < 20; i++) {
            lcd.print(text[i]);
            delayMicroseconds(1500); 
        }
    }

public:
    UltraStableLcdDriver(uint8_t addr) : lcd(addr, 20, 4), lastSwitchTime(0), lastRegRefresh(0), currentPage(0) {
        for(int r = 0; r < 4; r++) strcpy(displayBuffer[r], "                    ");
    }
    
    void begin() { 
        lcd.init(); 
        lcd.backlight(); 
        lcd.clear();
    }

    void update(const AtsState& state) {
        if (millis() - lastRegRefresh >= regRefreshInterval) {
            lastRegRefresh = millis();
            lcd.display();
            lcd.noCursor();
            lcd.noBlink();
        }

        if (millis() - lastSwitchTime >= displayInterval) {
            lastSwitchTime = millis();
            currentPage = (currentPage + 1) % 3; 
            
            lcd.init(); 
            for(int r = 0; r < 4; r++) strcpy(displayBuffer[r], "                    "); 
        }

        char newRows[4][21];
        
        if (currentPage == 0) {
            snprintf(newRows[0], 21, "PLN    : %-11s", state.isPlnOn ? "ON" : "OFF");
            snprintf(newRows[1], 21, "INV    : %-11s", state.isPlnOn ? "OFF" : "ON");
            snprintf(newRows[2], 21, "CHARGE : %-11s", state.isCharging ? "ON" : "OFF");
            snprintf(newRows[3], 21, "MODE   : %-11s", state.isPlnOn ? "NORMAL" : "BACKUP");
        } 
        else if (currentPage == 1) {
            snprintf(newRows[0], 21, "Volt  : %-5.1f V     ", state.acVoltage);
            snprintf(newRows[1], 21, "Arus  : %-5.2f A     ", state.acCurrent);
            snprintf(newRows[2], 21, "Daya  : %-5.1f W     ", state.acPower);
            snprintf(newRows[3], 21, "Energi: %-5.2f kWh   ", state.acEnergy);
        } 
        else {
            snprintf(newRows[0], 21, "AKI  : %-6.2f V     ", state.batteryVoltage);
            snprintf(newRows[1], 21, "ARUS : %-6.2f A     ", state.outputDcCurrent); 
            snprintf(newRows[2], 21, "SOC  : %-6.0f %%     ", state.batterySoc);
            
            char stat[12];
            if (state.isCharging) snprintf(stat, 12, "CHARGING");
            else snprintf(stat, 12, "OFF");
            snprintf(newRows[3], 21, "STAT : %-13s", stat);
        }

        for (int r = 0; r < 4; r++) {
            if (strcmp(newRows[r], displayBuffer[r]) != 0) {
                safePrintRow(r, newRows[r]);
                strcpy(displayBuffer[r], newRows[r]);
            }
        }
    }
};

// --- Driver Telemetri Firebase ---
class FirebaseTelemetry {
private:
    FirebaseData fbdo;
    FirebaseAuth auth;
    FirebaseConfig config;
    bool isConnected = false;
public:
    void begin() {
        WiFi.begin("TECNO CAMON 40", "414c766532b1");
        Serial.print("Menghubungkan WiFi");
        while (WiFi.status() != WL_CONNECTED) {
            Serial.print(".");
            delay(500);
        }
        Serial.println("\nWiFi Terhubung!");

        configTime(25200, 0, "pool.ntp.org", "time.nist.gov");
        Serial.println("Sinkronisasi Waktu Internet...");
        
        config.api_key = "AIzaSyDNBwjTrdDQh2prE2olTlP1hmQ8779uszo";
        config.database_url = "ats-system-9640b-default-rtdb.firebaseio.com";
        
        Firebase.signUp(&config, &auth, "", "");
        Firebase.begin(&config, &auth);
        Firebase.reconnectWiFi(true);
        isConnected = true;
    }

    void sendState(const AtsState& state) {
        if (!isConnected || !Firebase.ready()) return;

        struct tm timeinfo;
        if (!getLocalTime(&timeinfo)) {
            Serial.println("Gagal membaca waktu NTP");
            return;
        }

        char dateStr[15];
        strftime(dateStr, sizeof(dateStr), "%Y-%m-%d", &timeinfo);
        char hourStr[10];
        strftime(hourStr, sizeof(hourStr), "%H:00", &timeinfo);

        char timestampStr[30];
        snprintf(timestampStr, sizeof(timestampStr), "%s %s", dateStr, hourStr);

        char historyPath[64];
        snprintf(historyPath, sizeof(historyPath), "/SmartATS/History/%s/%s", dateStr, hourStr);

        FirebaseJson json;
        json.set("status_pln", state.isPlnOn ? "ON" : "OFF");
        json.set("status_inverter", state.isPlnOn ? "OFF" : "ON");
        json.set("status_pengisian", state.isCharging ? "ON" : "OFF");
        json.set("mode_sistem", state.isPlnOn ? "NORMAL" : "BACKUP");
        
        json.set("tegangan_ac", state.acVoltage);
        json.set("arus_ac", state.acCurrent);
        json.set("daya_ac", state.acPower);
        json.set("energi_ac", state.acEnergy);
        
        json.set("tegangan_baterai", state.batteryVoltage);
        json.set("arus_beban_baterai", state.outputDcCurrent);
        json.set("persentase_baterai", state.batterySoc);
        
        json.set("status_beban", state.acPower > 450.0f ? "OVERLOAD" : "AMAN");
        json.set("waktu_pembaruan", timestampStr);

        Firebase.RTDB.setJSONAsync(&fbdo, "/SmartATS/Realtime", &json);
        Firebase.RTDB.setJSONAsync(&fbdo, historyPath, &json);
    }
};

// ==========================================
// 4. USE CASE (Logika Utama)
// ==========================================
class AtsController {
private:
    AtsState state;
    IAcSensor* pzemSensor;
    IDcSensor* acsCharge;
    IDcSensor* acsDischarge;
    IDcSensor* ina219Sensor;
    IDigitalInput* plnSensor;
    IActuator* buzzer;
    IActuator* chargeRelay;

public:
    AtsController(IAcSensor* pzem, IDcSensor* acsChg, IDcSensor* acsDis, 
                  IDcSensor* ina, IDigitalInput* pln, IActuator* bz, IActuator* relay) 
        : pzemSensor(pzem), acsCharge(acsChg), acsDischarge(acsDis), 
          ina219Sensor(ina), plnSensor(pln), buzzer(bz), chargeRelay(relay) {
        state = {false, false, false, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f};
    }

    void processLogic() {
        pzemSensor->readSensor();
        state.acPower = pzemSensor->getPower();
        state.acVoltage = pzemSensor->getVoltage();
        state.acCurrent = pzemSensor->getCurrent();
        state.acEnergy = pzemSensor->getEnergy();

        state.isPlnOn = plnSensor->isHigh();
        state.chargeCurrent = acsCharge->readValue();
        state.outputDcCurrent = acsDischarge->readValue();
        state.batteryVoltage = ina219Sensor->readValue();

        state.batterySoc = (state.batteryVoltage - 11.5f) / (13.8f - 11.5f) * 100.0f;
        if (state.batterySoc > 100.0f) state.batterySoc = 100.0f;
        if (state.batterySoc < 0.0f) state.batterySoc = 0.0f;

        if (state.acPower > 450.0f) {
            state.isOverload = true;
            buzzer->turnOn();
        } else {
            state.isOverload = false;
            buzzer->turnOff();
        }

        if (state.isPlnOn) {
            if (state.chargeCurrent > 0.15f) { 
                state.isCharging = true;
                chargeRelay->turnOn();
            } else if (state.chargeCurrent < 0.05f) { 
                state.isCharging = false;
                chargeRelay->turnOff();
            }
        } else {
            state.isCharging = false;
            chargeRelay->turnOff();
        }
    }

    AtsState getCurrentState() const { return state; }
};

// ==========================================
// 5. MAIN LOOP
// ==========================================

PzemDriver pzem(PIN_PZEM_RX, PIN_PZEM_TX);
AcsAnalogDriver acs712Charge(PIN_ACS712, 1.65f, 0.066f); 
AcsAnalogDriver acs758Discharge(PIN_ACS758, 2.50f, 0.010f); 

Ina219Driver ina219;
PlnDetector plnInput(PIN_PLN_DET);
DigitalActuator alarmBuzzer(PIN_BUZZER);
DigitalActuator pilotLampRelay(PIN_RELAY);

UltraStableLcdDriver localDisplay(0x27); 
FirebaseTelemetry cloudTelemetry;

AtsController atsLogic(&pzem, &acs712Charge, &acs758Discharge, &ina219, &plnInput, &alarmBuzzer, &pilotLampRelay);

unsigned long lastSendTime = 0;
const unsigned long sendInterval = 5000; 

void setup() {
    Serial.begin(115200);
    
    Wire.begin(); 
    ina219.begin();
    
    Wire.setClock(10000); 

    localDisplay.begin();
    cloudTelemetry.begin(); 
}

void loop() {
    atsLogic.processLogic();
    AtsState currentState = atsLogic.getCurrentState();

    localDisplay.update(currentState);

    if (millis() - lastSendTime >= sendInterval) {
        lastSendTime = millis();
        cloudTelemetry.sendState(currentState);
    }
    
    delay(20);
}
