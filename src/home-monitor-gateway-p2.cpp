/* 
 * Project Home Monitor Gateway P2
 * Author: Cameron Haegle
 * Date: 10/24/2026
 * For comprehensive documentation and examples, please visit:
 * https://docs.particle.io/firmware/best-practices/firmware-template/
 */

#include "Particle.h"
#include "Adafruit_SH110X.h"

SYSTEM_MODE(SEMI_AUTOMATIC);
// Device OS 6.4.1 enables system threading by default.

// ============================================================
// Home Monitoring Network - Particle P2 BLE Gateway
//
// Hardware:
//   Particle P2
//   Adafruit SH1107 128x64 OLED FeatherWing
//
// Device OS: 6.4.1
//
// Sensor: HM-Garage-ENV
//
// OLED wiring:
//   SDA -> D0
//   SCL -> D1
//   VCC -> 3V3
//   GND -> GND
// ============================================================


// ============================================================
// Configuration
// ============================================================

constexpr uint8_t OLED_ADDRESS = 0x3C;

constexpr uint32_t BLE_RETRY_INTERVAL_MS = 3000;
constexpr uint32_t OLED_REFRESH_INTERVAL_MS = 1000;
constexpr uint32_t NETWORK_STATUS_INTERVAL_MS = 10000;

// Measurements become stale after 60 seconds.
// Stale measurements do not cause a BLE disconnection.
constexpr uint32_t SENSOR_STALE_TIMEOUT_MS = 60000;

const char SENSOR_NAME[] = "HM-Garage-ENV";

const BleUuid SENSOR_SERVICE_UUID(
    "8b7e0001-6a2d-4c91-9e31-4f2a8c6d1000"
);

const BleUuid SENSOR_CHARACTERISTIC_UUID(
    "8b7e0002-6a2d-4c91-9e31-4f2a8c6d1000"
);

// Door peripheral uses a distinct service and characteristic.
const BleUuid DOOR_SERVICE_UUID("8b7e0011-6a2d-4c91-9e31-4f2a8c6d1000");
const BleUuid DOOR_CHARACTERISTIC_UUID("8b7e0012-6a2d-4c91-9e31-4f2a8c6d1000");


// ============================================================
// OLED
// ============================================================

// Native SH1107 orientation: 64x128.
// Rotation 1 produces 128x64 landscape.

Adafruit_SH1107 display(64, 128, &Wire, -1);

bool oledReady = false;


// ============================================================
// BLE
// ============================================================

BlePeerDevice sensorPeer;
BleCharacteristic sensorCharacteristic;
BlePeerDevice doorPeer;
BleCharacteristic doorCharacteristic;
bool doorConnected = false;
bool doorDataValid = false;
bool doorOpen = false;
uint32_t doorSequence = 0;
uint32_t lastDoorUpdate = 0;

bool sensorConnected = false;
bool sensorDataValid = false;

uint32_t lastScanTime = 0;
uint32_t lastOLEDRefresh = 0;
uint32_t lastNetworkStatusLog = 0;
uint32_t lastSensorUpdate = 0;


// ============================================================
// Sensor Data
// ============================================================

float sensorTemperature = 0.0f;
float sensorHumidity = 0.0f;

uint32_t sensorSequence = 0;

// Publish only changed readings, plus a 15-minute heartbeat.
// Keep the most recent measurement during cloud outages.
constexpr uint32_t CLOUD_PUBLISH_RETRY_MS = 5000;
constexpr uint32_t CLOUD_HEARTBEAT_MS = 15UL * 60UL * 1000UL;

int16_t latestTemperatureRaw = 0;
uint16_t latestHumidityRaw = 0;
int16_t publishedTemperatureRaw = 0;
uint16_t publishedHumidityRaw = 0;
bool havePublishedMeasurement = false;
bool cloudPublishPending = false;
bool wasCloudConnected = false;
uint32_t lastCloudPublishAttempt = 0;
uint32_t lastCloudPublishTime = 0;

void publishPendingMeasurement()
{
    const bool cloudConnected = Particle.connected();
    if (!cloudConnected) {
        wasCloudConnected = false;
        return;
    }

    // On reconnect, synchronize the latest fresh reading once.
    if (!wasCloudConnected) {
        wasCloudConnected = true;
        if (sensorDataValid) {
            cloudPublishPending = true;
        }
    }

    if (!sensorDataValid ||
        millis() - lastSensorUpdate > SENSOR_STALE_TIMEOUT_MS) {
        return; // Never publish an old reading as a current heartbeat.
    }

    const uint32_t now = millis();
    const bool changed = !havePublishedMeasurement ||
        latestTemperatureRaw != publishedTemperatureRaw ||
        latestHumidityRaw != publishedHumidityRaw;
    const bool heartbeatDue = havePublishedMeasurement &&
        now - lastCloudPublishTime >= CLOUD_HEARTBEAT_MS;

    if (!cloudPublishPending && !changed && !heartbeatDue) {
        return;
    }
    if (lastCloudPublishAttempt != 0 &&
        now - lastCloudPublishAttempt < CLOUD_PUBLISH_RETRY_MS) {
        return;
    }
    lastCloudPublishAttempt = now;

    char payload[160];
    snprintf(payload, sizeof(payload),
        "{\"node\":\"HM-Garage-ENV\",\"temperature_c\":%.2f,"
        "\"humidity\":%.2f,\"sequence\":%lu}",
        sensorTemperature, sensorHumidity,
        static_cast<unsigned long>(sensorSequence));

    // Particle.publish acceptance does not guarantee downstream delivery.
    bool accepted = Particle.publish("hm/garage/env", payload, PRIVATE);
    if (accepted) {
        publishedTemperatureRaw = latestTemperatureRaw;
        publishedHumidityRaw = latestHumidityRaw;
        havePublishedMeasurement = true;
        cloudPublishPending = false;
        lastCloudPublishTime = now;
        Serial.printf("[Cloud] Published: %s\n", payload);
    }
    else {
        Serial.println("[Cloud] Publish not accepted; will retry");
    }
}



// ============================================================
// BLE Notification Buffer
// ============================================================

// BLE callbacks can execute asynchronously.
//
// A mutex protects the payload shared between the callback
// and the main application loop.

os_mutex_t payloadMutex = nullptr;

uint8_t pendingPayload[8] = {0};

bool pendingPayloadAvailable = false;
uint8_t pendingDoorPayload[8] = {0};
bool pendingDoorPayloadAvailable = false;


// ============================================================
// Function Prototypes
// ============================================================

void initializeOLED();
void updateOLED();

void initializeBLE();
void scanForSensor();
bool connectToSensor(const BleScanResult& result);
void disconnectSensor();
bool connectToDoor(const BleScanResult& result);
void doorNotificationHandler(const uint8_t* data, size_t len,
                             const BlePeerDevice& peer, void* context);
void processDoorPayload(const uint8_t* data, size_t length);

void sensorNotificationHandler(
    const uint8_t* data,
    size_t len,
    const BlePeerDevice& peer,
    void* context
);

void processSensorPayload(
    const uint8_t* data,
    size_t length
);


// ============================================================
// OLED Initialization
// ============================================================

void initializeOLED()
{
    Serial.println("Initializing SH1107 OLED...");

    Wire.begin();
    Wire.setSpeed(CLOCK_SPEED_100KHZ);

    oledReady = display.begin(OLED_ADDRESS, true);

    if (!oledReady) {
        Serial.println("ERROR: OLED initialization failed");
        return;
    }

    display.setRotation(1);
    display.clearDisplay();

    display.setTextSize(1);
    display.setTextColor(SH110X_WHITE);

    display.setCursor(0, 0);
    display.println("HOME MONITOR");

    display.drawLine(
        0,
        12,
        display.width() - 1,
        12,
        SH110X_WHITE
    );

    display.setCursor(0, 24);
    display.println("Particle P2 Gateway");

    display.setCursor(0, 42);
    display.println("Starting BLE...");

    display.display();

    Serial.printf(
        "OLED initialized: %dx%d\n",
        display.width(),
        display.height()
    );
}


// ============================================================
// OLED Update
// ============================================================

void updateOLED()
{
    if (!oledReady) {
        return;
    }

    display.clearDisplay();

    display.setTextSize(1);
    display.setTextColor(SH110X_WHITE);

    // --------------------------------------------------------
    // Header
    // --------------------------------------------------------

    display.setCursor(0, 0);
    display.print("HOME MONITOR");

    // W = Wi-Fi ready; C = Particle Cloud connected.
    display.setCursor(98, 0);
    display.print(WiFi.ready() ? "W" : "-");
    display.print(Particle.connected() ? "C" : "-");

    display.drawLine(
        0,
        12,
        127,
        12,
        SH110X_WHITE
    );

    // --------------------------------------------------------
    // BLE connection and data status
    // --------------------------------------------------------

    display.setCursor(0, 16);
    display.print("Garage: ");

    if (!sensorConnected) {
        display.println("Offline");
    }
    else if (!sensorDataValid) {
        display.println("Waiting");
    }
    else if (
        millis() - lastSensorUpdate >
        SENSOR_STALE_TIMEOUT_MS
    ) {
        display.println("Stale");
    }
    else {
        display.println("Connected");
    }

    // --------------------------------------------------------
    // Sensor measurements
    // --------------------------------------------------------

    if (sensorDataValid) {

        display.setCursor(0, 29);
        display.print("Temp: ");
        display.print(sensorTemperature, 2);
        display.println(" C");

        display.setCursor(0, 41);
        display.print("RH:   ");
        display.print(sensorHumidity, 2);
        display.println(" %");

        display.setCursor(0, 54);
        display.print("Seq: ");
        display.println(sensorSequence);
    }
    else {

        display.setCursor(0, 32);
        display.println("Waiting for data...");
    }

    display.display();
}


// ============================================================
// BLE Notification Callback
// ============================================================

void sensorNotificationHandler(
    const uint8_t* data,
    size_t len,
    const BlePeerDevice& peer,
    void* context
)
{
    if (data == nullptr || len != 8) {
        return;
    }

    if (payloadMutex == nullptr) {
        return;
    }

    // Copy received data into the shared buffer.
    // Do not perform OLED operations inside this callback.

    os_mutex_lock(payloadMutex);

    memcpy(pendingPayload, data, sizeof(pendingPayload));

    pendingPayloadAvailable = true;

    os_mutex_unlock(payloadMutex);
}


// Door notifications share the existing mutex but use a separate buffer.
void doorNotificationHandler(const uint8_t* data, size_t len,
                             const BlePeerDevice& peer, void* context)
{
    if (data == nullptr || len != 8 || payloadMutex == nullptr) return;
    os_mutex_lock(payloadMutex);
    memcpy(pendingDoorPayload, data, 8);
    pendingDoorPayloadAvailable = true;
    os_mutex_unlock(payloadMutex);
}

void processDoorPayload(const uint8_t* data, size_t length)
{
    if (data == nullptr || length != 8 || data[0] > 1) return;
    doorOpen = data[0] == 1;
    doorSequence = static_cast<uint32_t>(data[4]) |
        (static_cast<uint32_t>(data[5]) << 8) |
        (static_cast<uint32_t>(data[6]) << 16) |
        (static_cast<uint32_t>(data[7]) << 24);
    doorDataValid = true;
    lastDoorUpdate = millis();
    Serial.printf("[Garage DOOR] %s | Seq: %lu\n",
                  doorOpen ? "OPEN" : "CLOSED",
                  static_cast<unsigned long>(doorSequence));
}

// ============================================================
// Decode Sensor Payload
// ============================================================

void processSensorPayload(
    const uint8_t* data,
    size_t length
)
{
    if (data == nullptr || length != 8) {
        return;
    }

    // Temperature: signed 16-bit, little-endian.

    uint16_t temperatureBits =
        static_cast<uint16_t>(data[0]) |
        (static_cast<uint16_t>(data[1]) << 8);

    int16_t temperatureRaw =
        static_cast<int16_t>(temperatureBits);

    // Humidity: unsigned 16-bit, little-endian.

    uint16_t humidityRaw =
        static_cast<uint16_t>(data[2]) |
        (static_cast<uint16_t>(data[3]) << 8);

    // Sequence: unsigned 32-bit, little-endian.

    uint32_t sequence =
        static_cast<uint32_t>(data[4]) |
        (static_cast<uint32_t>(data[5]) << 8) |
        (static_cast<uint32_t>(data[6]) << 16) |
        (static_cast<uint32_t>(data[7]) << 24);

    sensorTemperature = temperatureRaw / 100.0f;
    sensorHumidity = humidityRaw / 100.0f;
    sensorSequence = sequence;
    latestTemperatureRaw = temperatureRaw;
    latestHumidityRaw = humidityRaw;
    // The sequence counter alone must not trigger a cloud event.
    cloudPublishPending = !havePublishedMeasurement ||
        latestTemperatureRaw != publishedTemperatureRaw ||
        latestHumidityRaw != publishedHumidityRaw;

    sensorDataValid = true;
    lastSensorUpdate = millis();

    Serial.printf(
        "[Garage ENV] Temp: %.2f C | RH: %.2f%% | Seq: %lu\n",
        sensorTemperature,
        sensorHumidity,
        static_cast<unsigned long>(sensorSequence)
    );
}


// ============================================================
// BLE Initialization
// ============================================================

void initializeBLE()
{
    Serial.println("Initializing BLE...");

    BLE.on();

    Serial.println("BLE initialized");
}


// ============================================================
// BLE Scanning
// ============================================================

void scanForSensor()
{
    Serial.println();
    Serial.println("========== BLE SCAN ==========");

    constexpr size_t MAX_SCAN_RESULTS = 20;

    BleScanResult results[MAX_SCAN_RESULTS];

    int count = BLE.scan(
        results,
        MAX_SCAN_RESULTS
    );

    Serial.printf(
        "BLE devices found: %d\n",
        count
    );

    for (int i = 0; i < count; i++) {

        const BleScanResult& result = results[i];

        String deviceName =
            result.advertisingData().deviceName();

        String deviceAddress =
            result.address().toString();

        Serial.printf(
            "Device %d:\n"
            "  Name: %s\n"
            "  Address: %s\n"
            "  RSSI: %d dBm\n",
            i + 1,
            deviceName.length()
                ? deviceName.c_str()
                : "(none)",
            deviceAddress.c_str(),
            result.rssi()
        );

        // ----------------------------------------------------
        // Identify sensor using its advertised service UUID.
        //
        // The sensor advertises its service UUID in the
        // primary advertising packet and its name in the
        // scan-response packet.
        // ----------------------------------------------------

        BleUuid advertisedService;

        bool serviceFound =
            result.advertisingData().serviceUUID(
                &advertisedService,
                1
            ) > 0;

        if (
            serviceFound &&
            advertisedService == SENSOR_SERVICE_UUID
        ) {

            Serial.println();
            Serial.println(
                "HOME MONITORING SENSOR FOUND"
            );

            Serial.printf(
                "Address: %s\n",
                deviceAddress.c_str()
            );

            if (!sensorConnected && connectToSensor(result)) {
                // Continue searching if door is still missing.
                if (doorConnected) return;
            }
        }
        if (serviceFound && advertisedService == DOOR_SERVICE_UUID && !doorConnected) {
            if (connectToDoor(result) && sensorConnected) return;
        }
    }

    if (!sensorConnected || !doorConnected) {
        Serial.println("One or more Home Monitoring nodes not found");
    }
}


// ============================================================
// Connect to Sensor
// ============================================================

bool connectToSensor(const BleScanResult& result)
{
    Serial.println("Connecting to sensor...");

    sensorPeer = BLE.connect(result.address());

    if (!sensorPeer.connected()) {

        Serial.println("BLE connection failed");

        sensorConnected = false;

        return false;
    }

    Serial.println("BLE connected");

    // --------------------------------------------------------
    // Discover characteristics
    // --------------------------------------------------------

    BleCharacteristic characteristics[8];

    int count = sensorPeer.discoverAllCharacteristics(
        characteristics,
        8
    );

    bool characteristicFound = false;

    for (int i = 0; i < count; i++) {

        if (
            characteristics[i].UUID() ==
            SENSOR_CHARACTERISTIC_UUID
        ) {

            sensorCharacteristic = characteristics[i];

            characteristicFound = true;

            break;
        }
    }

    if (!characteristicFound) {

        Serial.println(
            "ERROR: Sensor characteristic not found"
        );

        sensorPeer.disconnect();

        sensorConnected = false;

        return false;
    }

    Serial.println(
        "Sensor characteristic discovered"
    );

    // --------------------------------------------------------
    // Register notification callback
    // --------------------------------------------------------

    sensorCharacteristic.onDataReceived(
        sensorNotificationHandler,
        nullptr
    );

    // --------------------------------------------------------
    // Subscribe to notifications
    // --------------------------------------------------------

    int resultCode =
        sensorCharacteristic.subscribe(true);

    if (resultCode != SYSTEM_ERROR_NONE) {

        Serial.printf(
            "ERROR: BLE subscribe failed: %d\n",
            resultCode
        );

        sensorPeer.disconnect();

        sensorConnected = false;

        return false;
    }

    sensorConnected = true;

    // Reset the data freshness state for this connection.
    // Previously received measurements remain available
    // for display until new measurements arrive.

    sensorDataValid = false;

    Serial.println(
        "Subscribed to sensor notifications"
    );

    updateOLED();

    return true;
}


// Establish an independent GATT connection to the door node.
bool connectToDoor(const BleScanResult& result)
{
    Serial.println("[Garage DOOR] Connecting...");
    doorPeer = BLE.connect(result.address());
    if (!doorPeer.connected()) {
        Serial.println("[Garage DOOR] Connection failed");
        return false;
    }

    BleCharacteristic characteristics[8];
    int count = doorPeer.discoverAllCharacteristics(characteristics, 8);
    bool found = false;
    for (int i = 0; i < count; ++i) {
        if (characteristics[i].UUID() == DOOR_CHARACTERISTIC_UUID) {
            doorCharacteristic = characteristics[i];
            found = true;
            break;
        }
    }
    if (!found) {
        Serial.println("[Garage DOOR] Characteristic missing");
        doorPeer.disconnect();
        return false;
    }
    doorCharacteristic.onDataReceived(doorNotificationHandler, nullptr);
    int err = doorCharacteristic.subscribe(true);
    if (err != SYSTEM_ERROR_NONE) {
        Serial.printf("[Garage DOOR] Subscribe failed: %d\n", err);
        doorPeer.disconnect();
        return false;
    }
    doorConnected = true;
    doorDataValid = false; // No notification yet; state is UNKNOWN.
    Serial.println("[Garage DOOR] Connected and subscribed");
    return true;
}

// ============================================================
// Disconnect Handling
// ============================================================

void disconnectSensor()
{
    if (sensorPeer.connected()) {
        sensorPeer.disconnect();
    }

    sensorConnected = false;

    Serial.println("Sensor disconnected");

    updateOLED();
}


// ============================================================
// Setup
// ============================================================

void setup()
{
    Serial.begin(115200);

    waitFor(
        Serial.isConnected,
        10000
    );

    Serial.println();
    Serial.println("================================");
    Serial.println("HOME MONITORING NETWORK");
    Serial.println("Particle P2 BLE Gateway");
    Serial.println("Device OS 6.4.1");
    Serial.println("================================");

    // --------------------------------------------------------
    // Create mutex for BLE notification buffer
    // --------------------------------------------------------

    int mutexResult =
        os_mutex_create(&payloadMutex);

    if (mutexResult != 0) {

        Serial.printf(
            "ERROR: Mutex creation failed: %d\n",
            mutexResult
        );
    }

    // --------------------------------------------------------
    // Initialize hardware and BLE
    // --------------------------------------------------------

    initializeOLED();

    initializeBLE();

    // Request Wi-Fi and Particle Cloud connectivity after local
    // monitoring hardware is initialized. Do not wait for cloud.
    Serial.println("Starting Particle Cloud connection...");
    Particle.connect();

    updateOLED();

    // Allow immediate scanning on the first loop.

    lastScanTime =
        millis() - BLE_RETRY_INTERVAL_MS;

    Serial.println(
        "Gateway initialization complete"
    );
}


// ============================================================
// Main Loop
// ============================================================

void loop()
{
    // Device OS handles cloud reconnection in semi-automatic mode.
    // Report connectivity periodically without blocking BLE processing.
    if (millis() - lastNetworkStatusLog >= NETWORK_STATUS_INTERVAL_MS) {
        lastNetworkStatusLog = millis();
        Serial.printf("[Network] Wi-Fi: %s | Cloud: %s\n",
            WiFi.ready() ? "Ready" : "Offline",
            Particle.connected() ? "Connected" : "Offline");
    }

    // --------------------------------------------------------
    // Process pending BLE notification
    // --------------------------------------------------------

    if (payloadMutex != nullptr) {

        uint8_t payload[8];

        bool havePayload = false;

        os_mutex_lock(payloadMutex);

        if (pendingPayloadAvailable) {

            memcpy(
                payload,
                pendingPayload,
                sizeof(payload)
            );

            pendingPayloadAvailable = false;

            havePayload = true;
        }

        os_mutex_unlock(payloadMutex);

        if (havePayload) {

            processSensorPayload(
                payload,
                sizeof(payload)
            );
        }
    }

    if (payloadMutex != nullptr) {
        uint8_t doorPayload[8];
        bool haveDoorPayload = false;
        os_mutex_lock(payloadMutex);
        if (pendingDoorPayloadAvailable) {
            memcpy(doorPayload, pendingDoorPayload, sizeof(doorPayload));
            pendingDoorPayloadAvailable = false;
            haveDoorPayload = true;
        }
        os_mutex_unlock(payloadMutex);
        if (haveDoorPayload && doorConnected) {
            processDoorPayload(doorPayload, sizeof(doorPayload));
        }
    }

    // Publish the latest new measurement when cloud is available.
    publishPendingMeasurement();

    // --------------------------------------------------------
    // Monitor actual BLE connection
    //
    // IMPORTANT:
    // Do not disconnect merely because measurements
    // have become stale.
    // --------------------------------------------------------

    if (sensorConnected) {

        if (!sensorPeer.connected()) {

            Serial.println(
                "BLE connection lost"
            );

            sensorConnected = false;
        }
    }

    if (doorConnected && !doorPeer.connected()) {
        Serial.println("[Garage DOOR] BLE connection lost");
        doorConnected = false;
        doorDataValid = false;
    }

    // --------------------------------------------------------
    // Reconnect when disconnected
    // --------------------------------------------------------

    if (!sensorConnected || !doorConnected) {

        if (
            millis() - lastScanTime >=
            BLE_RETRY_INTERVAL_MS
        ) {

            scanForSensor();

            // Record the time after scanning completes.

            lastScanTime = millis();
        }
    }

    // --------------------------------------------------------
    // Refresh OLED
    // --------------------------------------------------------

    if (
        millis() - lastOLEDRefresh >=
        OLED_REFRESH_INTERVAL_MS
    ) {

        lastOLEDRefresh = millis();

        updateOLED();
    }
}