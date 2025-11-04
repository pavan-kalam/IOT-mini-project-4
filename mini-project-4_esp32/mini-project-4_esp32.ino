#include <WiFi.h>                    // ESP32 WiFi library for network connectivity
#include <HTTPClient.h>              // HTTP client for sending data to cloud server
#include <ArduinoJson.h>             // JSON processing library for data formatting
#include <DHT.h>                     // DHT sensor library for temperature/humidity reading
#include <WebServer.h>               // HTTP server library for RESTful API endpoints
#include <time.h>                    // timestamp generation
#include <mbedtls/aes.h>             // AES encryption functions from mbedTLS library
#include <mbedtls/base64.h>          // Base64 encoding for secure data transmission

// To setup the pin and sensor
#define DHT_PIN 4
#define LED_PIN 2
#define SENSOR_TYPE DHT11

// To setup the default threshold ranges for both temperature and humidity
float low_threshold = 15.0;
float temp_high_threshold = 30.0;
float low_humdy_threshold = 30.0;
float high_humdy_threshold = 70.0;

// To setup the system controls
bool monitoring = false;
bool calibrated = false;
unsigned long last_reading = 0;
unsigned long last_upload = 0;

// To setup the LED control
int led_indication = 0;  // 0: off, 1: on, 2: blink
unsigned long last_LED_blink = 0;
bool light_state = false;

// Calibration variables -- to improve accuracy of DHT11 sensor
float tmp_corec = 0.0;
float humd_corec = 0.0;
float past_temp_average = 25.0;
float past_humd_average = 50.0;
float adaptive_tmp_tolerance = 2.0;  // Minimum difference needed to apply correction
float adaptive_humd_tolerance = 5.0;
const int CALIB_SAMPLE_READINGS = 10;  // Number of readings to take during calibration
const unsigned long TRAD_TIME_GAP = 5000; // Read sensor every 5 seconds (as per requirements)

// Below is to initialize the DHT sensor and functions
DHT dht(DHT_PIN, SENSOR_TYPE);

// WebServer on port 80 for RESTful API
WebServer server(80);

// To setup the variables for user input to enter wifi credentials and server info for data upload
String ssid;
String wifiPassword;
String serverUrl;
String teamNumber;

// To setup the configurable settings (also this can be updated via /config endpoint)
bool encryptEnabled = true;  // Encryption flag
long uploadInterval = 10000; // Upload every 10 seconds (as per requirements)

// Global flag to control monitoring state and data upload (declared above)

// Latest sensor readings (for /sensor endpoint)
float lastTemperature = 0.0;
float lastHumidity = 0.0;
String lastTimestamp = "";

// NTP settings for Unix timestamp (this is to get the current time from the internet)
const char* ntpServer = "pool.ntp.org";
long gmtOffset_sec = 0;   // UTC for Unix time
int daylightOffset_sec = 0;

// 16-byte encryption key for AES-128-CBC (this is to encrypt the data before sending to the server)
// This must match the encryption key in the server.py file exactly
const unsigned char encryptionKey[16] = {
    'M', 'y', 'S', 'e', 'c', 'r', 'e', 't', 'K', 'e', 'y', '1', '2', '3', '4', '5'
};

// Below is the function to wait for user input from serial monitor with timeout
void waitForSerialInput(String &input, String fieldName) {
  unsigned long timeout = millis() + 30000; // 30 second timeout
  input = "";   //to clear input string
  
  // Main input collection loop with timeout check, and also to check for data and to remove white spaces.
  // Even to validate non-empty input and it confirms once the input is received.
  while (millis() < timeout) {
    if (Serial.available() > 0) {
      input = Serial.readStringUntil('\n'); // This is to read the input from the serial monitor
      input.trim();
      if (input.length() > 0) {
        Serial.println("Received: " + input); // This is to print the received input to the serial monitor
        return;
      }
    }
    delay(100);
    if ((millis() % 5000) < 100) { // Print '.' every 5 seconds
      Serial.print(".");
    }
  }
  
  // Timeout - restart setup process 
  // This is to restart the setup process if the input is not received within 30 seconds
  Serial.println("\nTimeout! No input received for " + fieldName + ".");
  Serial.println("Restarting setup process...");
  Serial.println("Please restart the device and provide input within 30 seconds.");
  ESP.restart(); // Restart the ESP32 to begin setup again
}

// Below is the setup function to initialize all system components and collect user wifi configuration details
void setup() {
  // Start serial communication at 115200 baud rate
  Serial.begin(115200);
  delay(5000); // Wait for serial to initialize
  Serial.println("ESP32 Mini-Project #4 Starting...");
  Serial.println("Waiting for serial input...");

  // To scan for available wifi networks first
  Serial.println("Scanning for available networks...");
  int n = WiFi.scanNetworks();    // Scan for available WiFi networks
  Serial.println("Found " + String(n) + " networks:");
  // Display all discovered networks with RSSI signal strength
  for (int i = 0; i < n; i++) {
    Serial.println(String(i+1) + ": " + WiFi.SSID(i) + " (RSSI: " + WiFi.RSSI(i) + ")");
  }
  
  // Input by user to select the network by number or enter SSID manually
  Serial.println("\nSelect WiFi network:");
  Serial.println("Option 1: Enter network number (1-" + String(n) + ")");
  Serial.println("Option 2: Enter full SSID manually");
  Serial.print("Your choice: ");
  
  String choice = "";   // Variable to store user's network selection
  unsigned long timeout = millis() + 30000;   // 30-second timeout for network selection
  
  // Loop to collect network selection with timeout
  while (millis() < timeout) {
    if (Serial.available() > 0) {
      choice = Serial.readStringUntil('\n');
      choice.trim(); // This is to remove white spaces from the input
      if (choice.length() > 0) {
        Serial.println("Received: " + choice);
        break;
      }
    }
    delay(100);
  }
  
  // To identify if user entered number or SSID manually
  bool isNumber = true;
  for (int i = 0; i < choice.length(); i++) {
    if (!isDigit(choice.charAt(i))) {
      isNumber = false; // This is to set the isNumber flag to false if the input is not a number
      break;
    }
  }
  
  // This logic is to process the network selection based on the input type
  if (isNumber && choice.toInt() >= 1 && choice.toInt() <= n) {
    // if the user selected the network by number
    int networkIndex = choice.toInt() - 1;
    ssid = WiFi.SSID(networkIndex);      // Get SSID from scanned networks
    Serial.println("Selected network: " + ssid);
  } else {
    // if the user entered the SSID manually
    ssid = choice;
    Serial.println("Using SSID: " + ssid);
  }

  // Input by user to get the wifi password, server URL, and team number
  Serial.print("Enter WiFi Password: ");
  waitForSerialInput(wifiPassword, "WiFi Password");

  Serial.print("Enter Server URL (e.g., http://192.168.1.100:8888/api/sensor-data): ");
  waitForSerialInput(serverUrl, "Server URL");

  Serial.print("Enter Team Number: ");
  waitForSerialInput(teamNumber, "Team Number");
  
  // Connect to selected WiFi network with timeout
  WiFi.begin(ssid.c_str(), wifiPassword.c_str());
  Serial.print("Connecting to WiFi...");
  
  int attempts = 0; // To count the number of attempts to connect to the WiFi network
  while (WiFi.status() != WL_CONNECTED && attempts < 30) { // This is to connect to the WiFi network with a timeout of 30 seconds
    delay(1000);
    Serial.print(".");
    attempts++; // To increment the number of attempts
    
    // Display connection status every 10 attempts
    if (attempts % 10 == 0) {
      Serial.println("\nWiFi Status: " + String(WiFi.status()));
    }
  }
  
  // To handle the connection results
  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("\nConnected to WiFi");
    Serial.print("IP Address: ");
    Serial.println(WiFi.localIP());
  } else {
    Serial.println("\nWiFi connection failed!");
    Serial.println("Final status: " + String(WiFi.status()));
    Serial.println("Status codes: 0=IDLE, 1=NO_SSID, 3=CONNECTED, 4=FAILED, 6=DISCONNECTED");
    Serial.println("Status 5 = Connection timeout/authentication failure");
    Serial.println("Continuing with setup...");
  }

  // Below is to initialize NTP for timestamp
  configTime(gmtOffset_sec, daylightOffset_sec, ntpServer);
  Serial.println("NTP time synchronized");

  // Below is to initialize the DHT11 sensor communication
  dht.begin();
  pinMode(LED_PIN, OUTPUT);   //led pin as output
  digitalWrite(LED_PIN, LOW); 
  
  // Below is to setup the RESTful API endpoints
  setupRestApi();

  // To show the menu on startup
  showMenu();
  Serial.println("Environment monitor is ready. Please Enter 'm' for options.");

  Serial.println("Setup complete. Starting loop..."); // This is to print the setup complete message to the serial monitor
}

// Main loop function to handle serial input, web server, and periodic tasks
void loop() {
  // Check for serial input from user
  if (Serial.available() > 0) {
    handleCommands();
  }
  
  // To handle the web server requests
  server.handleClient();

  // If monitoring is on, read sensor data every 5 seconds (as per requirements)
  if (monitoring && (millis() - last_reading >= TRAD_TIME_GAP)) {
    readAndProcessSensor();
    last_reading = millis();
  }
  
  // To manage the LED blinking based on blink mode
  manageLED();

  // Upload data every 10 seconds (only when monitoring and WiFi connected)
  if (monitoring && WiFi.status() == WL_CONNECTED && (millis() - last_upload >= uploadInterval)) {
    uploadData(); // Upload the latest sensor reading to server
    last_upload = millis();
  }

  delay(100);  // Small delay to avoid overloading the ESP32
}

// Below is the function to handle user commands from serial input
void handleCommands() {
  String input = Serial.readStringUntil('\n');
  input.trim(); //To remove white spaces

  if (input.length() == 0) return; // Ignore empty input
  
  char cmd = input.charAt(0);
  String value = (input.length() > 2) ? input.substring(2) : ""; // This is to get the value from the input
  
  // This logic is to process the commands based on the input
  switch (cmd) {
    case 's':
      monitoring = true;
      Serial.println("Monitoring temperature & humidity started.");
      break;
    case 't':
      monitoring = false;
      led_indication = 0; // Turn off LED indication
      digitalWrite(LED_PIN, LOW);
      Serial.println("Monitoring stopped.");
      break;
    case 'c':
      calibrateSensor();
      break;
    case 'h':
      if (value.toFloat() > 0) {
        high_humdy_threshold = value.toFloat();
        Serial.println("Humidity high threshold: " + String(high_humdy_threshold) + "%");
      } else {
        Serial.println("Invalid humidity high threshold.");
      }
      break;
    case 'L':
      if (value.toFloat() > 0 && value.toFloat() < high_humdy_threshold) {
        low_humdy_threshold = value.toFloat();
        Serial.println("Humidity low threshold: " + String(low_humdy_threshold) + "%");
      } else {
        Serial.println("Invalid humidity low threshold.");
      }
      break;
    case 'l':
      if (value.toFloat() > 0 && value.toFloat() < temp_high_threshold) {
        low_threshold = value.toFloat();
        Serial.println("Temperature low threshold: " + String(low_threshold) + "°C");
      } else {
        Serial.println("Invalid temperature low threshold.");
      }
      break;
    case 'u':
      if (value.toFloat() > low_threshold) {
        temp_high_threshold = value.toFloat();
        Serial.println("Temperature high threshold: " + String(temp_high_threshold) + "°C");
      } else {
        Serial.println("Invalid temperature high threshold.");
      }
      break;
    case 'e':  // Toggle encryption command
      encryptEnabled = !encryptEnabled;
      Serial.println("Encryption " + String(encryptEnabled ? "enabled" : "disabled"));
      break;
    case 'w':    // Change WiFi settings command
      changeWiFiSettings();
      break;
    case 'i':    // Show current info command
      showCurrentInfo();
      break;
    case 'n':     // Set team number command
      if (value.length() > 0) {
        teamNumber = value;
        Serial.println("Team number set to: " + teamNumber);
      } else {
        Serial.println("Usage: n <number> - Set team number");
        Serial.println("Example: n 9");
      }
      break;
    case 'm':
      showMenu();
      break;
    default:
      Serial.println("Unknown command. Enter 'm' for menu.");
  }
}

// Below is the function to display the menu options
void showMenu() {
  Serial.println("\n--- Menu ---");
  Serial.println("s - Start monitoring");
  Serial.println("t - Stop monitoring");
  Serial.println("c - Calibrate sensor");
  Serial.println("h <value> - Humidity high threshold");
  Serial.println("L <value> - Humidity low threshold");
  Serial.println("l <value> - Temperature low threshold");
  Serial.println("u <value> - Temperature high threshold");
  Serial.println("e - Toggle encryption");
  Serial.println("w - Change WiFi settings");
  Serial.println("i - Show current info");
  Serial.println("n <number> - Set team number");
  Serial.println("m - Show menu");
  Serial.println("Thresholds - Temp: " + String(low_threshold) + "-" + String(temp_high_threshold) + "°C, Hum: " + String(low_humdy_threshold) + "-" + String(high_humdy_threshold) + "%");
  Serial.println("Calibrated: " + String(calibrated ? "Yes" : "No"));
  Serial.println("Monitoring: " + String(monitoring ? "Active" : "Stopped"));
  Serial.println("WiFi: " + String(WiFi.status() == WL_CONNECTED ? "Connected" : "Disconnected"));
  Serial.println("Encryption: " + String(encryptEnabled ? "Enabled" : "Disabled"));
  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("RESTful API: http://" + WiFi.localIP().toString());
  }
  Serial.println("---\n");
}

// Below is the function to read sensor data, apply calibration, and control LED based on thresholds
void readAndProcessSensor() {
  float raw_temp = dht.readTemperature();
  float raw_hum = dht.readHumidity();
  
  // Logic to validate sensor readings
  if (isnan(raw_temp) || isnan(raw_hum)) {
    Serial.println("Raw Temp: " + String(raw_temp) + "°C, Raw Hum: " + String(raw_hum) + "%");
    Serial.println("Sensor read failed!");
    return;
  }
  
  // Logic to apply calibration if available
  float temp = calibrated ? raw_temp + tmp_corec : raw_temp;
  float hum = calibrated ? raw_hum + humd_corec : raw_hum;
  
  // Clamp humidity to valid range (0-100%) to prevent negative values
  if (hum < 0.0) hum = 0.0;
  if (hum > 100.0) hum = 100.0;
  
  Serial.println("Temp: " + String(temp) + "°C, Hum: " + String(hum) + "%");
  controlLED(temp, hum); // Control LED based on thresholds
  
  // To store the latest readings for API endpoint access
  lastTemperature = temp;
  lastHumidity = hum;
}

// Below is the function to control the LED based on thresholds
void controlLED(float temp, float hum) {
  int new_mode = 0; // Default to Off
  if (temp > temp_high_threshold || hum > high_humdy_threshold) {
    new_mode = 1;  // On
  } else if (temp < low_threshold || hum < low_humdy_threshold) {
    new_mode = 2;  // Blink
  } else {
    new_mode = 0;  // Off
  }
  
  // Logic to change the LED mode if it has changed
  if (new_mode != led_indication) {
    led_indication = new_mode;
    if (led_indication == 0) {
      Serial.println("LED will turn OFF (Temp " + String(low_threshold) + "°C-" + String(temp_high_threshold) + "°C and Hum " + String(low_humdy_threshold) + "%-" + String(high_humdy_threshold) + "%)");
      digitalWrite(LED_PIN, LOW);
    } else if (led_indication == 1) {
      Serial.println("LED will turn ON (Temp > " + String(temp_high_threshold) + "°C or Hum > " + String(high_humdy_threshold) + "%)");
      digitalWrite(LED_PIN, HIGH);
    } else {  // Blink start
      Serial.println("LED will blink (Temp < " + String(low_threshold) + "°C or Hum < " + String(low_humdy_threshold) + "%)");
      last_LED_blink = millis();
      light_state = true;
      digitalWrite(LED_PIN, HIGH);
    }
  }
}

// Below is the function to manage the LED blinking
void manageLED() {
  if (led_indication != 2) return;
  
  // Blink every 500ms
  if (millis() - last_LED_blink > 500) {
    last_LED_blink = millis();
    light_state = !light_state;
    digitalWrite(LED_PIN, light_state ? HIGH : LOW);
  }
}

// Below is the function to calibrate the sensor readings based on historical data
void calibrateSensor() {
  Serial.println("Calibrating... Collecting " + String(CALIB_SAMPLE_READINGS) + " readings.");
  
  // Arrays to save readings
  float temp_readings[CALIB_SAMPLE_READINGS];
  float hum_readings[CALIB_SAMPLE_READINGS];
  int valid_count = 0;
  
  // Collect readings
  for (int i = 0; i < CALIB_SAMPLE_READINGS; i++) {
    float t = dht.readTemperature();
    float h = dht.readHumidity();
    if (!isnan(t) && !isnan(h)) { // Only store valid readings
      temp_readings[valid_count] = t;
      hum_readings[valid_count] = h;
      valid_count++;
    }
    delay(2400); //
  }
  
  // Logic to verify enough valid readings
  if (valid_count < 3) {
    Serial.println("Too few valid sensor readings obtained..");
    return;
  }
  
  // below is the logic to calculate mean and standard deviation
  float tmp_total = 0, humd_total = 0;
  for (int i = 0; i < valid_count; i++) {
    tmp_total += temp_readings[i];
    humd_total += hum_readings[i];
  }
  float tmp_avg = tmp_total / valid_count;
  float humd_avg = humd_total / valid_count;
  
  // below is the logic to calculate standard deviation for outlier detection
  float temp_var = 0, hum_var = 0;
  for (int i = 0; i < valid_count; i++) {
    temp_var += pow(temp_readings[i] - tmp_avg, 2);
    hum_var += pow(hum_readings[i] - humd_avg, 2);
  }
  float temp_std = sqrt(temp_var / valid_count);
  float hum_std = sqrt(hum_var / valid_count);
  
  // below is the logic to remove outliers beyond 2 standard deviations
  tmp_total = 0; humd_total = 0; int clean_count = 0;
  for (int i = 0; i < valid_count; i++) {
    if (abs(temp_readings[i] - tmp_avg) <= 2 * temp_std &&
        abs(hum_readings[i] - humd_avg) <= 2 * hum_std) {
      tmp_total += temp_readings[i];
      humd_total += hum_readings[i];
      clean_count++;
    }
  }

  // below is the logic to get final averages after outlier removal
  float recent_temp_avg = tmp_total / clean_count;
  float recent_hum_avg = humd_total / clean_count;
  
  Serial.println("Recent avg - Temp: " + String(recent_temp_avg) + "°C, Hum: " + String(recent_hum_avg) + "%");
  Serial.println("Historical avg - Temp: " + String(past_temp_average) + "°C, Hum: " + String(past_humd_average) + "%");
  
  // To calculate deviation from historical averages
  float temp_offset = recent_temp_avg - past_temp_average;
  float hum_offset = recent_hum_avg - past_humd_average;
  
  // Apply corrections if deviation exceeds threshold
  if (abs(temp_offset) > adaptive_tmp_tolerance) {
    tmp_corec += temp_offset;
    Serial.println("Temp correction: " + String(temp_offset) + "°C");
  }
  if (abs(hum_offset) > adaptive_humd_tolerance) {
    humd_corec += hum_offset;
    Serial.println("Hum correction: " + String(hum_offset) + "%");
  }
  
  past_temp_average = past_temp_average * 0.7 + recent_temp_avg * 0.3;
  past_humd_average = past_humd_average * 0.7 + recent_hum_avg * 0.3;
  
  adaptive_tmp_tolerance *= (1 + temp_std / 10.0);
  adaptive_humd_tolerance *= (1 + hum_std / 10.0);
  
  // To limit the maximum thresholds to prevent excessive tolerance
  if (adaptive_tmp_tolerance > 5.0) adaptive_tmp_tolerance = 5.0;
  if (adaptive_humd_tolerance > 10.0) adaptive_humd_tolerance = 10.0;
  
  calibrated = true; // To set the calibrated flag to true
  Serial.println("Calibration complete. Corrections - Temp: " + String(tmp_corec) + ", Hum: " + String(humd_corec));
  Serial.println("Adaptive thresholds - Temp: " + String(adaptive_tmp_tolerance) + "°C, Hum: " + String(adaptive_humd_tolerance) + "%");
}

// Below is the function to dynamically change WiFi network without restarting device
// This function will trigger when the user wants to change the WiFi network without restarting the device
void changeWiFiSettings() {
  Serial.println("\n=== CHANGE WiFi SETTINGS ===");
  
  // To disconnect from the current WiFi connection
  WiFi.disconnect();
  Serial.println("Disconnected from current WiFi network.");
  
  // Again to scan for available networks which is same as mentioned in initial setup
  Serial.println("Scanning for available networks...");
  int n = WiFi.scanNetworks();
  Serial.println("Found " + String(n) + " networks:");
  for (int i = 0; i < n; i++) {
    Serial.println(String(i+1) + ": " + WiFi.SSID(i) + " (RSSI: " + WiFi.RSSI(i) + ")");
  }
  
  // To collect new network selection from the user
  Serial.println("\nSelect WiFi network:");
  Serial.println("Option 1: Enter network number (1-" + String(n) + ")");
  Serial.println("Option 2: Enter full SSID manually");
  Serial.print("Your choice: ");
  
  String choice = ""; // To store the user's network selection
  unsigned long timeout = millis() + 30000;
  while (millis() < timeout) { // This is to collect the network selection from the user with a timeout of 30 seconds
    if (Serial.available() > 0) {
      choice = Serial.readStringUntil('\n');
      choice.trim(); // This is to remove white spaces from the input
      if (choice.length() > 0) {
        Serial.println("Received: " + choice); // This is to print the received input to the serial monitor
        break;
      }
    }
    delay(100);
  }
  
  // To determine if the user entered number or SSID and this is same as mentioned in initial setup
  bool isNumber = true;
  for (int i = 0; i < choice.length(); i++) {
    if (!isDigit(choice.charAt(i))) {
      isNumber = false;
      break;
    }
  }
  
  // This logic is to process the network selection based on the input type
  if (isNumber && choice.toInt() >= 1 && choice.toInt() <= n) {
    // User selected by number
    int networkIndex = choice.toInt() - 1;
    ssid = WiFi.SSID(networkIndex);
    Serial.println("Selected network: " + ssid);
  } else {
    // User entered SSID manually
    ssid = choice;
    Serial.println("Using SSID: " + ssid);
  }

  // Get new password
  Serial.print("Enter WiFi Password: ");
  waitForSerialInput(wifiPassword, "WiFi Password");

  // Get new server URL
  Serial.print("Enter Server URL (e.g., http://192.168.1.100:8888/api/sensor-data): ");
  waitForSerialInput(serverUrl, "Server URL");

  // Attempt to connect to new network
  Serial.println("Connecting to new WiFi network...");
  WiFi.begin(ssid.c_str(), wifiPassword.c_str());
  
  int attempts = 0;
  while (WiFi.status() != WL_CONNECTED && attempts < 30) {
    delay(1000);
    Serial.print(".");
    attempts++;
  }
  
  // Below is to display the new connection details
  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("\nConnected to new WiFi network!");
    Serial.print("IP Address: ");
    Serial.println(WiFi.localIP());
    Serial.println("WiFi settings updated successfully.");
  } else {
    Serial.println("\nFailed to connect to new WiFi network!");
    Serial.println("Final status: " + String(WiFi.status()));
    Serial.println("Status codes: 0=IDLE, 1=NO_SSID, 3=CONNECTED, 4=FAILED, 6=DISCONNECTED");
    Serial.println("Status 5 = Connection timeout/authentication failure");
  }
  
  Serial.println("===============================\n");
}

// Below is the function to show the current system status and configuration
void showCurrentInfo() {
  Serial.println("\n=== CURRENT SYSTEM INFO ===");
  Serial.println("WiFi SSID: " + ssid);
  Serial.println("WiFi Status: " + String(WiFi.status() == WL_CONNECTED ? "Connected" : "Disconnected"));
  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("IP Address: " + WiFi.localIP().toString());
    Serial.println("RESTful API: http://" + WiFi.localIP().toString());
  }
  Serial.println("Server URL: " + serverUrl);
  Serial.println("Team Number: " + teamNumber);
  Serial.println("Monitoring: " + String(monitoring ? "Active" : "Stopped"));
  Serial.println("Encryption: " + String(encryptEnabled ? "Enabled" : "Disabled"));
  Serial.println("Upload Interval: " + String(uploadInterval/1000) + " seconds");
  Serial.println("===========================\n");
}

// Below is the function to upload the latest sensor data to server (uses last stored readings)
void uploadData() {
  // To use the latest sensor readings (already read every 5 seconds by readAndProcessSensor())
  float temperature = lastTemperature;
  float humidity = lastHumidity;

  // Logic to check if the readings are valid
  if (temperature == 0.0 && humidity == 0.0) {
    Serial.println("No sensor data available yet!");
    return;
  }

  // To display the sensor readings in the serial monitor
  Serial.println("=== SENSOR READING & UPLOAD ===");
  Serial.println("Temperature: " + String(temperature, 1) + "°C");
  Serial.println("Humidity: " + String(humidity, 1) + "%");
  Serial.println("Timestamp: " + lastTimestamp);
  Serial.println("WiFi Status: " + String(WiFi.status() == WL_CONNECTED ? "Connected" : "Disconnected"));
  Serial.println("Encryption: " + String(encryptEnabled ? "Enabled" : "Disabled"));
  Serial.println("===============================");

  // Logic to check if the WiFi connection is established for upload
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("WiFi not connected. Data displayed above, skipping upload.");
    return;
  }

  // To get the Unix timestamp
  time_t now;
  time(&now);
  lastTimestamp = String(now);

  // To create the JSON object
  StaticJsonDocument<200> doc;      // Create JSON document with 200-byte capacity
  doc["temperature"] = String(temperature, 1);
  doc["humidity"] = String(humidity, 1);
  doc["timestamp"] = lastTimestamp;
  doc["team_number"] = teamNumber;

  String jsonStr;       // String to store serialized JSON
  serializeJson(doc, jsonStr);

  String encryptedData = "";      // Variable for encrypted data
  if (encryptEnabled) {
    // Pad JSON string to multiple of 16 bytes for AES-CBC compatibility
    size_t len = jsonStr.length();
    size_t padLen = ((len / 16) + 1) * 16;  // Round up to next 16-byte boundary

    unsigned char padded[padLen]; // To store the padded JSON string
    memcpy(padded, jsonStr.c_str(), len); // To copy the JSON string to the padded array
    
    // PKCS7 padding to make the JSON string a multiple of 16 bytes
    unsigned char padValue = padLen - len;
    for (size_t i = len; i < padLen; i++) {
      padded[i] = padValue; // To set the padding value to the padded array
    }

    // To generate random IV (first 16 bytes of ciphertext)
    unsigned char iv[16];
    unsigned char iv_copy[16];  // Store a copy of the IV before encryption
    for (int i = 0; i < 16; i++) {
      iv[i] = random(256); // To generate a random IV
    }
    
    // Save a copy of the IV before encryption (mbedtls modifies iv during encryption)
    memcpy(iv_copy, iv, 16); // To copy the IV to the iv_copy array

    // Encrypt with AES-128-CBC
    unsigned char ciphertext[padLen];
    mbedtls_aes_context aes; // To initialize the AES context
    mbedtls_aes_init(&aes);
    mbedtls_aes_setkey_enc(&aes, encryptionKey, 128); // To set the encryption key
    mbedtls_aes_crypt_cbc(&aes, MBEDTLS_AES_ENCRYPT, padLen, iv, padded, ciphertext);
    mbedtls_aes_free(&aes); // To free the AES context

    // Combine IV + ciphertext (use iv_copy instead of iv)
    unsigned char combined[16 + padLen];
    memcpy(combined, iv_copy, 16);  // Use iv_copy to ensure we have the original IV
    memcpy(combined + 16, ciphertext, padLen);

    // To encode combined data as Base64 for HTTP transmission
    size_t base64Len;
    unsigned char base64Out[ ((16 + padLen) * 4) / 3 + 4 ];
    mbedtls_base64_encode(base64Out, sizeof(base64Out), &base64Len, combined, 16 + padLen);
    base64Out[base64Len] = 0;  // Null terminate
    encryptedData = String((char*)base64Out); // To convert the combined data to a string
  }

  // This is to prepare HTTP POST and send data to server (initialize with server URL)
  HTTPClient http;
  http.begin(serverUrl); // To initialize the HTTP client with the server URL
  http.addHeader("Content-Type", "application/x-www-form-urlencoded");

  // Logic to validate team number for data integrity
  if (teamNumber.length() == 0) {
    Serial.println("ERROR: Invalid team number: '" + teamNumber + "'");
    Serial.println("Please use 'w' command to set proper team number");
    http.end();
    return;
  }

  // This is to build encoded data string for HTTP POST
  String postData = "team_number=" + teamNumber +
                    "&temperature=" + String(temperature, 1) +
                    "&humidity=" + String(humidity, 1) +
                    "&timestamp=" + lastTimestamp +
                    "&is_encrypted=" + (encryptEnabled ? "true" : "false");
  if (encryptEnabled) {
    postData += "&encrypted_data=" + encryptedData; // To add the encrypted data to the POST data
  }

  // To print the data that is being sent to the server
  Serial.println("Uploading data:");
  Serial.println("Team Number: " + teamNumber);
  Serial.println("Server URL: " + serverUrl);
  Serial.println("POST Data: " + postData);

  // To send POST request and get response code
  int httpCode = http.POST(postData);
  if (httpCode > 0) {
    String response = http.getString(); // To get the response from the server
    Serial.println("Server response: " + response);
  } else {
    Serial.println("HTTP POST failed, error: " + String(http.errorToString(httpCode).c_str()));
  }
  http.end();
}

// Below is the function to configure all RESTful API endpoints for remote system control
void setupRestApi() {
  // GET /health
  server.on("/health", HTTP_GET, []() {
    String json = "{\"ok\": true, \"uptime_s\": " + String(millis() / 1000) + "}"; // To create the JSON object
    server.send(200, "application/json", json);
  });

  // GET /sensor - This is to return encrypted JSON if encryption enabled, plain JSON otherwise
  server.on("/sensor", HTTP_GET, []() {
    StaticJsonDocument<200> doc;
    doc["temperature"] = String(lastTemperature, 1);
    doc["humidity"] = String(lastHumidity, 1);
    doc["timestamp"] = lastTimestamp;
    doc["team_number"] = teamNumber;
    String jsonStr;
    serializeJson(doc, jsonStr);
    
    // This is to return encrypted or plain data based on encryption setting
    if (encryptEnabled) {
      // Encrypt and return Base64 encoded data
      size_t len = jsonStr.length();
      size_t padLen = ((len / 16) + 1) * 16;  // Round up to next 16-byte boundary

      unsigned char padded[padLen]; 
      memcpy(padded, jsonStr.c_str(), len); // To copy the JSON string to the padded array
      
      // PKCS7 padding to make the JSON string a multiple of 16 bytes
      unsigned char padValue = padLen - len;
      for (size_t i = len; i < padLen; i++) {
        padded[i] = padValue; // To set the padding value to the padded array
      }

      // To generate random IV
      unsigned char iv[16];
      unsigned char iv_copy[16];  // Store a copy of the IV before encryption
      for (int i = 0; i < 16; i++) {
        iv[i] = random(256);
      }
      
      // Save a copy of the IV before encryption (mbedtls modifies iv during encryption)
      memcpy(iv_copy, iv, 16);

      unsigned char ciphertext[padLen];
      mbedtls_aes_context aes;
      mbedtls_aes_init(&aes);
      mbedtls_aes_setkey_enc(&aes, encryptionKey, 128);
      mbedtls_aes_crypt_cbc(&aes, MBEDTLS_AES_ENCRYPT, padLen, iv, padded, ciphertext);
      mbedtls_aes_free(&aes);

      // Combine IV + ciphertext (use iv_copy instead of iv)
      unsigned char combined[16 + padLen];
      memcpy(combined, iv_copy, 16);  // Use iv_copy to ensure we have the original IV
      memcpy(combined + 16, ciphertext, padLen);

      size_t base64Len;
      unsigned char base64Out[ ((16 + padLen) * 4) / 3 + 4 ];
      mbedtls_base64_encode(base64Out, sizeof(base64Out), &base64Len, combined, 16 + padLen);
      base64Out[base64Len] = 0;
      server.send(200, "text/plain", String((char*)base64Out));
    } else {
      // This is to return plain JSON
      server.send(200, "application/json", jsonStr);
    }
  });

  // GET /config - This is to retrieve the current configuration
  server.on("/config", HTTP_GET, []() {
    StaticJsonDocument<200> responseDoc;
    responseDoc["upload_interval"] = uploadInterval;
    responseDoc["encryption_enabled"] = encryptEnabled;
    responseDoc["team_number"] = teamNumber;
    responseDoc["server_url"] = serverUrl;
    responseDoc["monitoring"] = monitoring;
    String response;
    serializeJson(responseDoc, response);
    server.send(200, "application/json", response);
  });

  // POST /config - This is to update the configuration settings
  server.on("/config", HTTP_POST, []() {
    if (server.hasArg("plain")) {
      String body = server.arg("plain");
      StaticJsonDocument<200> doc;
      DeserializationError error = deserializeJson(doc, body);
      // Process only valid JSON input
      if (!error) {
        if (doc.containsKey("upload_interval")) {
          uploadInterval = doc["upload_interval"].as<long>();
        }
        if (doc.containsKey("encryption_enabled")) {
          encryptEnabled = doc["encryption_enabled"].as<bool>();
        }
        if (doc.containsKey("team_number")) {
          teamNumber = doc["team_number"].as<String>();
        }
        if (doc.containsKey("server_url")) {
          serverUrl = doc["server_url"].as<String>();
        }
      }
    }
    // To respond with updated config
    StaticJsonDocument<200> responseDoc;
    responseDoc["upload_interval"] = uploadInterval;
    responseDoc["encryption_enabled"] = encryptEnabled;
    responseDoc["team_number"] = teamNumber;
    responseDoc["server_url"] = serverUrl;
    responseDoc["monitoring"] = monitoring;
    String response;
    serializeJson(responseDoc, response);
    server.send(200, "application/json", response);
  });

  // POST /push-now - This is to immediately push the current sensor data to cloud
  server.on("/push-now", HTTP_POST, []() {
    uploadData();
    server.send(200, "text/plain", "Data pushed to cloud");
  });

  // POST /start - Start monitoring
  // example: curl -X POST [http://esp32-ip]/start
  server.on("/start", HTTP_POST, []() {
    monitoring = true;
    StaticJsonDocument<200> responseDoc;
    responseDoc["status"] = "success";
    responseDoc["message"] = "Monitoring started";
    String response;
    serializeJson(responseDoc, response);
    server.send(200, "application/json", response);
  });

  // POST /stop - Stop monitoring
  server.on("/stop", HTTP_POST, []() {
    monitoring = false;
    StaticJsonDocument<200> responseDoc;
    responseDoc["status"] = "success";
    responseDoc["message"] = "Monitoring stopped";
    String response;
    serializeJson(responseDoc, response);
    server.send(200, "application/json", response);
  });

  // GET /status - Get current system status
  server.on("/status", HTTP_GET, []() {
    StaticJsonDocument<200> responseDoc;
    responseDoc["monitoring"] = monitoring;
    responseDoc["wifi_connected"] = (WiFi.status() == WL_CONNECTED);
    responseDoc["ip_address"] = WiFi.localIP().toString();
    responseDoc["encryption_enabled"] = encryptEnabled;
    responseDoc["team_number"] = teamNumber;
    responseDoc["upload_interval"] = uploadInterval;
    String response;
    serializeJson(responseDoc, response);
    server.send(200, "application/json", response);
  });

  // POST /toggle-encryption - Toggle encryption setting
  server.on("/toggle-encryption", HTTP_POST, []() {
    encryptEnabled = !encryptEnabled;
    StaticJsonDocument<200> responseDoc;
    responseDoc["status"] = "success";
    responseDoc["encryption_enabled"] = encryptEnabled;
    responseDoc["message"] = "Encryption toggled";
    String response;
    serializeJson(responseDoc, response);
    server.send(200, "application/json", response);
  });

  // To start the server
  server.begin();
  Serial.println("RESTful API server started"); // This is to print the server started message to the serial monitor
}
