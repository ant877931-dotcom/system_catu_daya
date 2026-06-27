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
#define PIN_PLN_DET   27 
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
    
    char loadStatusText[32]; 
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
    PzemDriver(HardwareSerial& serialPort, uint8_t rx, uint8_t tx) : pzem(serialPort, rx, tx), v(0), i(0), p(0), e(0), lastPzemRead(0) {}
    
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

class ActiveLowActuator : public IActuator {
private:
    uint8_t pin;
public:
    ActiveLowActuator(uint8_t pinNumber) : pin(pinNumber) {
        pinMode(pin, OUTPUT);
        digitalWrite(pin, HIGH); 
    }
    void turnOn() override { digitalWrite(pin, LOW); }  
    void turnOff() override { digitalWrite(pin, HIGH); } 
};

// --- Driver Detektor PLN ---
class PlnDetector : public IDigitalInput {
private:
    uint8_t pin;
    unsigned long lastPlnOnTime; 
public:
    PlnDetector(uint8_t pinNumber) : pin(pinNumber), lastPlnOnTime(0) {
        pinMode(pin, INPUT_PULLUP); 
    }
    
    bool isHigh() override { 
        if (digitalRead(pin) == LOW) {
            lastPlnOnTime = millis();
            return true;
        } else {
            if (millis() - lastPlnOnTime < 1000) return true; 
            return false;
        }
    }
};

// --- Driver ACS Analog (Telah Disesuaikan dengan Program Kalibrasi) ---
class AcsAnalogDriver : public IDcSensor {
private:
    uint8_t pin;
    float offsetVoltage;
    float sensitivity;
    float maxCurrent;
    float noiseThreshold;
    float refVoltage;
    float filteredCurrent;
public:
    AcsAnalogDriver(uint8_t pinNumber, float offsetV, float sens, float maxC, float noiseThresh, float refV) 
        : pin(pinNumber), offsetVoltage(offsetV), sensitivity(sens), maxCurrent(maxC), noiseThreshold(noiseThresh), refVoltage(refV), filteredCurrent(0.0f) {
        pinMode(pin, INPUT);
    }
    
    float readValue() override {
        long totalADC = 0;
        int samples = 1000; // Oversampling 1000 kali sesuai program Anda
        
        for (int x = 0; x < samples; x++) {
            totalADC += analogRead(pin);
            delayMicroseconds(50); // Jeda singkat agar ADC stabil
        }
        
        float adcAvg = totalADC / (float)samples;
        float voltageSensor = adcAvg * (refVoltage / 4095.0f);
        
        // Rumus identik dengan program kalibrasi
        float current = (voltageSensor - offsetVoltage) / sensitivity;
        
        if (current < 0.0f) current = 0.0f;
        
        // Filter EMA untuk menghaluskan output akhir
        filteredCurrent = (filteredCurrent * 0.3f) + (current * 0.7f);
        
        if (filteredCurrent < noiseThreshold) {
            filteredCurrent = 0.0f;
        }
        if (filteredCurrent > maxCurrent) {
            filteredCurrent = maxCurrent;
        }
        return filteredCurrent;
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
        return ina219.getBusVoltage_V();
    }
};

// --- Driver LCD 20x4 ---
class UltraStableLcdDriver {
private:
    LiquidCrystal_I2C lcd;
    unsigned long lastSwitchTime;
    int currentPage;
    bool isFirstBoot; 
    char displayBuffer[4][21]; 
    const unsigned long displayInterval = 5000; 

public:
    UltraStableLcdDriver(uint8_t addr) : lcd(addr, 20, 4), lastSwitchTime(0), currentPage(0), isFirstBoot(true) {
        for(int r=0; r<4; r++) strcpy(displayBuffer[r], "                    ");
    }
    
    void begin() { 
        lcd.init(); lcd.backlight(); lcd.clear();
        lcd.setCursor(5, 1); lcd.print("Monitoring");
        lcd.setCursor(0, 2); lcd.print("Power Supply System");
        delay(5000); 
        lcd.clear();
        isFirstBoot = false; 
        lastSwitchTime = millis(); 
    }

    void update(const AtsState& state) {
        if (isFirstBoot) return;

        if (millis() - lastSwitchTime >= displayInterval) {
            lastSwitchTime = millis();
            currentPage = (currentPage + 1) % 4; 
            lcd.clear(); 
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
        else if (currentPage == 2) {
            float tampilArusMasuk = state.isCharging ? state.chargeCurrent : 0.0f;
            snprintf(newRows[0], 21, "TEG. AKI : %-5.1f V  ", state.batteryVoltage);
            snprintf(newRows[1], 21, "ARUS CHG : %-5.2f A  ", tampilArusMasuk); 
            snprintf(newRows[2], 21, "ARUS OUT : %-5.2f A  ", state.outputDcCurrent);
            snprintf(newRows[3], 21, "SOC:%3.0f%% [%s]", state.batterySoc, state.isCharging ? "CHARGING" : "DISCHG  ");
        }
        else {
            for(int i=0; i<4; i++) strcpy(newRows[i], "                    ");
            strcpy(newRows[0], "    STATUS BEBAN    ");
            snprintf(newRows[2], 21, "%-20.20s", state.loadStatusText);
        }

        for (int r=0; r<4; r++) {
            if (strcmp(newRows[r], displayBuffer[r]) != 0) {
                lcd.setCursor(0, r); lcd.print(newRows[r]);
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
            Serial.print("."); delay(500);
        }
        Serial.println("\nWiFi Terhubung!");

        configTime(25200, 0, "pool.ntp.org", "time.nist.gov");
        
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
        if (!getLocalTime(&timeinfo)) return;

        char dateStr[15], timeKey[10], timestampStr[30], historyPath[64];
        strftime(dateStr, sizeof(dateStr), "%Y-%m-%d", &timeinfo);
        
        if (state.isCharging) {
            int currentMinute = timeinfo.tm_min;
            int roundedMinute = (currentMinute / 10) * 10; 
            snprintf(timeKey, sizeof(timeKey), "%02d:%02d", timeinfo.tm_hour, roundedMinute);
        } else {
            snprintf(timeKey, sizeof(timeKey), "%02d:00", timeinfo.tm_hour);
        }
        
        snprintf(timestampStr, sizeof(timestampStr), "%s %s", dateStr, timeKey);
        snprintf(historyPath, sizeof(historyPath), "/SmartATS/History/%s/%s", dateStr, timeKey);

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
        json.set("persentase_baterai", state.batterySoc);
        json.set("status_beban", state.loadStatusText);
        json.set("waktu_pembaruan", timestampStr);
        
        float arusMasuk = state.isCharging ? state.chargeCurrent : 0.0f;
        json.set("arus_pengisian", arusMasuk); 
        json.set("arus_keluar_baterai", state.outputDcCurrent); 

        Firebase.RTDB.setJSONAsync(&fbdo, "/SmartATS/Realtime", &json);
        Firebase.RTDB.setJSONAsync(&fbdo, historyPath, &json);
    }
};

// ==========================================
// 4. USE CASE (Logika Utama + Kompensasi 2 Arah)
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

    unsigned long overloadStartTime;
    bool isOverloadPending;

public:
    AtsController(IAcSensor* pzem, IDcSensor* acsChg, IDcSensor* acsDis, 
                  IDcSensor* ina, IDigitalInput* pln, IActuator* bz, IActuator* relay) 
        : pzemSensor(pzem), acsCharge(acsChg), acsDischarge(acsDis), 
          ina219Sensor(ina), plnSensor(pln), buzzer(bz), chargeRelay(relay),
          overloadStartTime(0), isOverloadPending(false) 
    {
        state.isPlnOn = false;
        state.isCharging = false;
        state.isOverload = false;
        state.acVoltage = 0.0f;
        state.acCurrent = 0.0f;
        state.acPower = 0.0f;
        state.acEnergy = 0.0f;
        state.batteryVoltage = 0.0f;
        state.outputDcCurrent = 0.0f;
        state.chargeCurrent = 0.0f;
        state.batterySoc = 0.0f;
        strcpy(state.loadStatusText, "AMAN");
    }

    void processLogic() {
        pzemSensor->readSensor();
        state.acPower = pzemSensor->getPower();
        state.acVoltage = pzemSensor->getVoltage();
        state.acCurrent = pzemSensor->getCurrent();
        state.acEnergy = pzemSensor->getEnergy();

        state.isPlnOn = plnSensor->isHigh();
        
        if (!state.isPlnOn) {
            state.chargeCurrent = 0.0f;
            state.isCharging = false;
            chargeRelay->turnOff();
            state.outputDcCurrent = acsDischarge->readValue();
        } else {
            state.outputDcCurrent = 0.0f;
            state.chargeCurrent = acsCharge->readValue();
            if (state.chargeCurrent > 0.3f) { 
                state.isCharging = true;
                chargeRelay->turnOn();
            } else { 
                state.isCharging = false;
                state.chargeCurrent = 0.0f; 
                chargeRelay->turnOff();
            }
        }

        // Kompensasi Dua Arah
        float rawBatteryVoltage = ina219Sensor->readValue();
        float rDischarge = 0.015f; 
        float rCharge = 0.120f;    

        if (state.isCharging) {
            state.batteryVoltage = rawBatteryVoltage - (state.chargeCurrent * rCharge);
        } else {
            state.batteryVoltage = rawBatteryVoltage + (state.outputDcCurrent * rDischarge);
        }

        if (state.batteryVoltage > 13.8f) state.batteryVoltage = 13.8f;
        if (state.batteryVoltage < 0.0f) state.batteryVoltage = 0.0f;

        state.batterySoc = ((state.batteryVoltage - 11.5f) / (12.8f - 11.5f)) * 100.0f;
        if (state.batterySoc > 100.0f) state.batterySoc = 100.0f;
        if (state.batterySoc < 0.0f) state.batterySoc = 0.0f;

        // Dynamic Power Derating
        float vMin = 11.8f; 
        float vMax = 12.8f; 
        float maxAllowedPower = 0.0f;
        
        if (!state.isPlnOn) { 
            if (state.batteryVoltage >= vMax) {
                maxAllowedPower = 450.0f;
            } else if (state.batteryVoltage <= vMin) {
                maxAllowedPower = 0.0f;
            } else {
                maxAllowedPower = (state.batteryVoltage - vMin) * 450.0f; 
            }
        } else {
            maxAllowedPower = 450.0f;
        }

        // Debounce 10 Detik
        bool isCurrentlyOverload = false;
        char tempStatusText[32];

        if (!state.isPlnOn && state.batteryVoltage <= vMin) {
            isCurrentlyOverload = true;
            snprintf(tempStatusText, sizeof(tempStatusText), "CRITICAL (AKI LOW)");
        } else if (state.acPower > maxAllowedPower) {
            isCurrentlyOverload = true;
            snprintf(tempStatusText, sizeof(tempStatusText), "OVERLOAD (MAKS %.0fW)", maxAllowedPower);
        } else {
            snprintf(tempStatusText, sizeof(tempStatusText), "AMAN (MAKS %.0fW)", maxAllowedPower);
        }

        if (isCurrentlyOverload) {
            if (!isOverloadPending) {
                overloadStartTime = millis(); 
                isOverloadPending = true;
            }
            
            unsigned long elapsed = millis() - overloadStartTime;
            if (elapsed >= 10000) {
                state.isOverload = true;
                strcpy(state.loadStatusText, tempStatusText);
            } else {
                unsigned long remaining = 10 - (elapsed / 1000);
                snprintf(state.loadStatusText, sizeof(state.loadStatusText), "PENDING (%lus)", remaining);
                state.isOverload = false; 
            }
        } else {
            isOverloadPending = false;
            overloadStartTime = 0;
            state.isOverload = false;
            strcpy(state.loadStatusText, tempStatusText);
        }

        if (state.isOverload) {
            buzzer->turnOn();
        } else {
            buzzer->turnOff();
        }
    }

    AtsState getCurrentState() const { return state; }
};

// ==========================================
// 5. MAIN LOOP SETUP
// ==========================================
HardwareSerial pzemSerial(2);
PzemDriver pzem(pzemSerial, PIN_PZEM_RX, PIN_PZEM_TX);

// INISIALISASI SESUAI DENGAN NILAI KALIBRASI YANG DIBERIKAN
// ACS712 (Charge): Offset 1.65V, Sensitivitas 0.066 V/A
AcsAnalogDriver acs712Charge(PIN_ACS712, 1.65f, 0.066f, 20.0f, 0.3f, 3.3f); 

// ACS758 (Discharge): Offset 1.985V, Sensitivitas 0.01175 V/A (sesuai kode kalibrasi)
AcsAnalogDriver acs758Discharge(PIN_ACS758, 1.985f, 0.01175f, 200.0f, 0.5f, 3.3f); 

Ina219Driver ina219;
PlnDetector plnInput(PIN_PLN_DET);
DigitalActuator alarmBuzzer(PIN_BUZZER);
ActiveLowActuator pilotLampRelay(PIN_RELAY);

UltraStableLcdDriver localDisplay(0x27); 
FirebaseTelemetry cloudTelemetry;

AtsController atsLogic(&pzem, &acs712Charge, &acs758Discharge, &ina219, &plnInput, &alarmBuzzer, &pilotLampRelay);

unsigned long lastSendTime = 0;
const unsigned long sendInterval = 5000; 

void setup() {
    Serial.begin(115200);
    analogReadResolution(12); // Pastikan resolusi ADC adalah 12-bit sesuai kalibrasi
    
    pzemSerial.begin(9600, SERIAL_8N1, PIN_PZEM_RX, PIN_PZEM_TX);
    Wire.begin(); 
    
    localDisplay.begin();
    ina219.begin();
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
    
    delay(10);
}
