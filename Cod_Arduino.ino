#include <WiFi.h>
#include <Wire.h>
#include "MAX30105.h"
#include "spo2_algorithm.h"
#include "heartRate.h"

// SETĂRI WI-FI 
const char* ssid = "ESP32";
const char* password = "password123";
WiFiServer server(80);
String header;

MAX30105 particleSensor;

// Buffer BPM 
const byte RATE_SIZE = 4;
byte rates[RATE_SIZE];
byte rateSpot = 0;
long lastBeat = 0;
float beatsPerMinute;
int beatAvg = 0;

// Buffer SpO2
#define SPO2_BUFFER_SIZE 50 
uint32_t irBuffer[SPO2_BUFFER_SIZE];
uint32_t redBuffer[SPO2_BUFFER_SIZE];
int32_t spo2Value = 0;
int8_t validSPO2;
int32_t heartRateSPO2;
int8_t validHeartRate;
int bufferIndex = 0;
bool spo2Ready = false;

// Calibrare BPM 
int baselineBPM = 0;
bool isCalibrated = false;
unsigned long calibrationStart = 0;
bool calibrationRunning = false;
long calibrationSum = 0;
int calibrationCount = 0;
const unsigned long CALIBRATION_DURATION = 20000; // 20 secunde

// Pini 
const int pinRosu = 25;
const int pinVerde = 26;
const int pinAlbastru = 27;
const int pinBuzzer = 15;
const int PIN_CAM_RESET = 4;

const int frequency = 2000;
const int resolution = 8;

String cameraIP = "Așteptare...";

//  Date primite de la camera ESP32-S3-CAM (Python) 
int cam_ear_alert = 0;       
int cam_mar_alert = 0;      
int cam_head_alert = 0;     
float cam_ear_value = 0.0;
float cam_mar_value = 0.0;
float cam_head_angle = 0.0;
unsigned long lastCamUpdate = 0; 

int calcScore(bool camFresh) {
  int scor = 0;

  // Scor SpO2
  if (spo2Ready && spo2Value > 0) {
    if      (spo2Value < 90)  scor += 35;
    else if (spo2Value <= 93) scor += 20;
    else if (spo2Value <= 95) scor += 10;
  }

  // Scor BPM bazat pe scădere față de valoarea calibrată
  if (isCalibrated && beatAvg > 0) {
    int drop = baselineBPM - beatAvg;

    if      (drop >= 4  && drop < 8)  scor += 15;  
    else if (drop >= 8  && drop < 12) scor += 30;  
    else if (drop >= 12)              scor += 50;  
  }

  // 3. Scor cameră
  if (camFresh) {
    if (cam_ear_alert  == 1) scor += 25;
    if (cam_head_alert == 1) scor += 20;
    if (cam_mar_alert  == 1) scor += 15;
  }

  return min(scor, 100);
}

void setup() {
  Serial.begin(115200);
  pinMode(pinBuzzer, OUTPUT);

  ledcAttach(pinBuzzer, 2000, 8); 
  ledcWrite(pinBuzzer, 0); 


  Serial2.begin(115200, SERIAL_8N1, 16, 17);
  // Configurează pinul de Reset pentru cameră
  pinMode(PIN_CAM_RESET, OUTPUT);
  digitalWrite(PIN_CAM_RESET, LOW);
  delay(200);
  digitalWrite(PIN_CAM_RESET, HIGH); 

  delay(1000);
  Wire.begin(32, 33); //  SDA=32, SCL=33
  // Pornire Wi-Fi în modul Access Point (AP)
  Serial.print("Setting AP (Access Point)");
  WiFi.softAP(ssid, password);
  IPAddress IP = WiFi.softAPIP();
  Serial.print("AP IP address: ");
  Serial.println(IP);
  server.begin();

  //Inițializare Senzor MAX30100
  if (!particleSensor.begin(Wire, I2C_SPEED_FAST)) {
    Serial.println("Senzorul MAX30100 nu a fost găsit");
  }
  particleSensor.setup();
  particleSensor.setPulseAmplitudeRed(0x3F); 
  particleSensor.setPulseAmplitudeIR(0x3F);
}

void setCuloare(int r, int g, int b) {
    analogWrite(pinRosu, r);
    analogWrite(pinVerde, g);
    analogWrite(pinAlbastru, b);
}

void loop() {

  if (Serial2.available()) {
    String mesaj = Serial2.readStringUntil('\n');
    if (mesaj.startsWith("IP:")) {
      cameraIP = mesaj.substring(3); 
      Serial.print("IP CAMERĂ: ");
      Serial.println(cameraIP);
    }
  }

  long irValue = particleSensor.getIR();
  long redValue = particleSensor.getRed();
  yield();

  if (irValue < 50000) {
    rateSpot = 0;
    beatAvg = 0;
    bufferIndex = 0;
    spo2Value = 0;
    spo2Ready = false;
    calibrationRunning = false;
    setCuloare(0, 0, 0);
    ledcWrite(pinBuzzer, 0);
  } else {
    // CALCUL BPM
    if (checkForBeat(irValue) == true) {
      long delta = millis() - lastBeat;
      lastBeat = millis();
      if (delta > 300 && delta < 1500) {
        beatsPerMinute = 60 / (delta / 1000.0);
        if (beatsPerMinute > 40 && beatsPerMinute < 200) {
          if (beatAvg == 0) for (byte i = 0; i < RATE_SIZE; i++) rates[i] = (byte)beatsPerMinute;
          rates[rateSpot++] = (byte)beatsPerMinute;
          rateSpot %= RATE_SIZE;
          beatAvg = 0;
          for (byte x = 0; x < RATE_SIZE; x++) beatAvg += rates[x];
          beatAvg /= RATE_SIZE;
        }
      }
    }
    // CALIBRARE BPM
    if (beatAvg > 0 && !isCalibrated) {
      if (!calibrationRunning) {
        // Pornim calibrarea când avem primul BPM valid
        calibrationRunning = true;
        calibrationStart = millis();
        calibrationSum = 0;
        calibrationCount = 0;
        Serial.println("Calibrare BPM pornită...");
      }
 
      calibrationSum += beatAvg;
      calibrationCount++;

      if (millis() - calibrationStart >= CALIBRATION_DURATION) {
        baselineBPM = calibrationSum / calibrationCount;
        isCalibrated = true;
        calibrationRunning = false;
        Serial.print("Calibrare finalizată. Baseline BPM: ");
        Serial.println(baselineBPM);
      }
    }

    // COLECTARE ȘI CALCUL SpO2
    if (bufferIndex < SPO2_BUFFER_SIZE) {
      irBuffer[bufferIndex] = (uint32_t)irValue;
      redBuffer[bufferIndex] = (uint32_t)redValue;
      bufferIndex++;
    } else {
      int32_t oldVal = spo2Value;
      maxim_heart_rate_and_oxygen_saturation(irBuffer, SPO2_BUFFER_SIZE, redBuffer, &spo2Value, &validSPO2, &heartRateSPO2, &validHeartRate);
      if (validSPO2 == 1 && spo2Value > 70 && spo2Value <= 100) spo2Ready = true;
      else spo2Value = oldVal;
      bufferIndex = 0;
    }

    // LOGICA ALARME 
    bool camFresh = (millis() - lastCamUpdate) < 5000;
    bool camera = (cam_ear_alert == 1 || cam_mar_alert == 1 || cam_head_alert == 1);

    if (beatAvg > 0 || camera)  {
      unsigned long t = millis();
      int scor = calcScore(camFresh);

    if (scor >= 70) {
       setCuloare(255, 0, 0);
       ledcWrite(pinBuzzer, 220);  // oboseala severa
     } else if (scor >= 45) {
       setCuloare(255, 100, 0); // oboseala moderata
       ledcWrite(pinBuzzer, (t % 1000 < 200) ? 120 : 0);
     } else if (scor >= 20) {
       setCuloare(255, 200, 0); // oboseala usoara
       ledcWrite(pinBuzzer, (t % 2000 < 200) ? 80 : 0);
     } else {
       setCuloare(0, 255, 0); // stare normala
       ledcWrite(pinBuzzer, 0);
     }
   } 
 } 

  // SERVER WEB
  WiFiClient client = server.available();
  if (client) {
    String currentLine = "";
    header = "";

    while (client.connected()) {
      if (client.available()) {
        char c = client.read();
        header += c;

        if (c == '\n') {
          if (currentLine.length() == 0) {
            // RUTA 1: POST /updateCamera (date de la Python)
            if (header.indexOf("POST /updateCamera") >= 0) {
              int contentLength = 0;
              int clIdx = header.indexOf("Content-Length:");
              if (clIdx < 0) clIdx = header.indexOf("content-length:");
              if (clIdx >= 0) {
                int eol = header.indexOf('\n', clIdx);
                String clStr = header.substring(clIdx + 15, eol);
                clStr.trim();
                contentLength = clStr.toInt();
                Serial.println("Content-Length: " + String(contentLength));
              }
              String body = "";
              if (contentLength > 0) {
                unsigned long startT = millis();
                while ((int)body.length() < contentLength && millis() - startT < 2000) {
                  if (client.available()) body += (char)client.read();
                }
              } 
              else {
                delay(50);
                unsigned long startT = millis();
                while (millis() - startT < 300) {
                  while (client.available()) {
                    body += (char)client.read();
                    startT = millis();
                  }
                }
              }

              if (body.length() > 0) {
                cam_ear_alert  = (body.indexOf("\"cam_ear\": 1")  >= 0 || body.indexOf("\"cam_ear\":1")  >= 0) ? 1 : 0;
                cam_mar_alert  = (body.indexOf("\"cam_mar\": 1")  >= 0 || body.indexOf("\"cam_mar\":1")  >= 0) ? 1 : 0;
                cam_head_alert = (body.indexOf("\"cam_head\": 1") >= 0 || body.indexOf("\"cam_head\":1") >= 0) ? 1 : 0;
                
                int idx;
                idx = body.indexOf("\"cam_ear_v\":");
                if (idx >= 0) cam_ear_value = body.substring(body.indexOf(':', idx) + 1).toFloat();
                idx = body.indexOf("\"cam_mar_v\":");
                if (idx >= 0) cam_mar_value = body.substring(body.indexOf(':', idx) + 1).toFloat();
                idx = body.indexOf("\"cam_angle\":");
                if (idx >= 0) cam_head_angle = body.substring(body.indexOf(':', idx) + 1).toFloat();

                lastCamUpdate = millis();

                Serial.println("Body primit: [" + body + "]");
                Serial.println("EAR=" + String(cam_ear_alert) + " MAR=" + String(cam_mar_alert) + " HEAD=" + String(cam_head_alert));

              }

              client.println("HTTP/1.1 200 OK");
              client.println("Content-Type: application/json");
              client.println("Connection: close");
              client.println();
              client.print("{\"status\":\"ok\"}");
              break;
            } 
            // RUTA 2: GET /getData (cerut de browser la 400ms)
            else if (header.indexOf("GET /getData") >= 0) {
              client.println("HTTP/1.1 200 OK");
              client.println("Content-Type: application/json");
              client.println("Connection: close");
              client.println();

              int progress = bufferIndex * 100 / SPO2_BUFFER_SIZE;
              bool camFresh = (millis() - lastCamUpdate) < 5000;
              Serial.println("camFresh=" + String(camFresh) + " lastUpdate=" + String(lastCamUpdate) + " millis=" + String(millis()));
              int scor = (beatAvg > 0) ? calcScore(camFresh) : 0;

              String statusText = "Detectare...";
              String statusColor = "orange";

              if (beatAvg > 0 && !isCalibrated) {
                int calProgress = (int)((millis() - calibrationStart) * 100 / CALIBRATION_DURATION);
                statusText = "Calibrare " + String(min(calProgress, 100)) + "%";
                statusColor = "#3498db";
              } else if (beatAvg > 0) {
                if      (scor >= 70) { statusText = "OBOSEALA SEVERA";   statusColor = "red";     }
                else if (scor >= 45) { statusText = "OBOSEALA MODERATA"; statusColor = "orange";  }
                else if (scor >= 20) { statusText = "OBOSEALA USOARA";   statusColor = "#b5b500"; }
                else                 { statusText = "STARE NORMALA";     statusColor = "green";   }
              }

              client.print(
                "{\"bpm\": " + String(beatAvg) +
                ", \"spo2\": " + String(spo2Ready ? spo2Value : 0) +
                ", \"progress\": " + String(progress) +
                ", \"status\": \"" + statusText + "\"" +
                ", \"color\": \"" + statusColor + "\"" +
                ", \"scor\": " + String(scor) +
                ", \"calibrated\": " + String(isCalibrated ? 1 : 0) +   
                ", \"baseline\": " + String(baselineBPM) +               
                ", \"cam_ear\": " + String(camFresh ? cam_ear_alert : -1) +
                ", \"cam_mar\": " + String(camFresh ? cam_mar_alert : -1) +
                ", \"cam_head\": " + String(camFresh ? cam_head_alert : -1) +
                ", \"cam_ear_v\": " + String(cam_ear_value, 2) +
                ", \"cam_mar_v\": " + String(cam_mar_value, 2) +
                ", \"cam_angle\": " + String(cam_head_angle, 1) +
                ", \"cam_fresh\": " + String(camFresh ? 1 : 0) +
                "}"
              );
              break;
            } 
            // RUTA 3: GET / (pagina HTML)
            else {
              client.println("HTTP/1.1 200 OK");
              client.println("Content-type:text/html");
              client.println();
              client.println("<!DOCTYPE html><html><head><meta charset='UTF-8'>");

              // CSS
              client.println("<style>");
              client.println("body{font-family:'Segoe UI',Tahoma,Geneva,Verdana,sans-serif;background:#f4f7f6;color:#333;margin:0;padding:20px;}");
              client.println(".header-bar{background:white;padding:15px;border-radius:15px;box-shadow:0 5px 15px rgba(0,0,0,0.05);margin-bottom:20px;display:flex;align-items:center;justify-content:center;text-align:center;}");
              client.println(".header-text h3{margin:0;color:#2c3e50;font-size:17px;letter-spacing:0.5px;line-height:1.2;}");
              client.println(".header-text h4{margin:5px 0 0 0;color:#7f8c8d;font-size:15px;font-weight:normal;}");
              client.println(".dashboard-container{display:grid;grid-template-columns:1fr 1fr;gap:20px;max-width:800px;margin:0 auto;}");
              client.println(".status-bar{grid-column:1/-1;background:white;padding:15px;border-radius:12px;box-shadow:0 2px 10px rgba(0,0,0,0.03);display:flex;justify-content:space-between;align-items:center;}");
              client.println(".block{background:white;border-radius:15px;padding:25px;box-shadow:0 10px 25px rgba(0,0,0,0.05);position:relative;}");
              client.println(".block-title{font-size:14px;color:#7f8c8d;text-transform:uppercase;letter-spacing:1px;margin-bottom:15px;}");
              client.println(".val-big{font-size:55px;font-weight:bold;margin:10px 0;}");
              client.println(".bpm-col{color:#e63946;} .spo2-col{color:#2a9d8f;}");
              client.println(".unit{font-size:24px;color:#95a5a6;font-weight:normal;}");
              client.println(".sub-label{font-size:12px;color:#bdc3c7;text-transform:uppercase;}");
              client.println(".cal-badge{font-size:11px;color:#3498db;background:#eaf4fb;border-radius:6px;padding:2px 8px;margin-left:8px;}");
              client.println("</style>");

              // JavaScript
              client.println("<script>setInterval(function() {");
              client.println("  fetch('/getData').then(r => r.json()).then(data => {");
              client.println("    var statText = document.getElementById('statusText');");
              client.println("    var calBadge = document.getElementById('calBadge') || {innerHTML:''};");
              client.println("    if(data.bpm > 0) {");
              client.println("      statText.innerHTML = '&#10003; ' + data.status; statText.style.color = data.color;");
              client.println("      document.getElementById('bpmDisp').innerHTML = data.bpm;");
              client.println("      if(data.spo2 > 0) document.getElementById('spo2Disp').innerHTML = data.spo2 + ' <span class=\"unit\">%</span>';");
              client.println("      else document.getElementById('spo2Disp').innerHTML = '<small>Analiza ' + data.progress + '%</small>';");
              client.println("      if(data.calibrated == 1) calBadge.innerHTML = 'Baseline: ' + data.baseline + ' BPM';");
              client.println("      else calBadge.innerHTML = 'Calibrare...';");
              client.println("      document.getElementById('scorDisp').innerHTML = data.scor;");        
              client.println("      document.getElementById('scorDisp').style.color = data.color;"); 
              client.println("      document.getElementById('scorBar').style.width = data.scor + '%';");       
              client.println("      document.getElementById('scorBar').style.background = data.color;");    
              client.println("    } else {");
              client.println("      statText.innerHTML = 'PUNETI DEGETUL PE SENZOR'; statText.style.color = 'orange';");
              client.println("      document.getElementById('bpmDisp').innerHTML = '--';");
              client.println("      document.getElementById('spo2Disp').innerHTML = '--';");
              client.println("      document.getElementById('scorDisp').innerHTML = '--';");  
              client.println("      document.getElementById('scorBar').style.width = '0%';");           
              client.println("      calBadge.innerHTML = '';");
              client.println("    }");

              client.println("    if(data.cam_fresh == 1) {");
              client.println("      document.getElementById('camEar').innerHTML  = data.cam_ear  == 1 ? \"<span style='color:#e74c3c;'>DA</span>\" : \"<span style='color:#2ecc71;'>NU</span>\";");
              client.println("      document.getElementById('camEar').style.color  = data.cam_ear  == 1 ? 'red' : 'green';");
              client.println("      document.getElementById('camMar').innerHTML  = data.cam_mar  == 1 ? \"<span style='color:#e74c3c;'>DA</span>\" : \"<span style='color:#2ecc71;'>NU</span>\";");
              client.println("      document.getElementById('camMar').style.color  = data.cam_mar  == 1 ? 'red' : 'green';");
              client.println("      document.getElementById('camHead').innerHTML = data.cam_head == 1 ? \"<span style='color:#e74c3c;'>DA</span>\" : \"<span style='color:#2ecc71;'>NU</span>\";");
              client.println("      document.getElementById('camHead').style.color = data.cam_head == 1 ? 'red' : 'green';");
              client.println("    } else {");
              client.println("      var off = '&#9888; Camera offline';");
              client.println("      document.getElementById('camEar').innerHTML  = off; document.getElementById('camEar').style.color  = 'gray';");
              client.println("      document.getElementById('camMar').innerHTML  = off; document.getElementById('camMar').style.color  = 'gray';");
              client.println("      document.getElementById('camHead').innerHTML = off; document.getElementById('camHead').style.color = 'gray';");
              client.println("    }");

              client.println("  });");
              client.println("}, 400);</script></head>");

              // HTML
              client.println("<body>");
              client.println("<div class='header-bar'>");
              client.println("<div class='header-text'><h3>Universitatea Nationala de Stiinta si Tehnologie POLITEHNICA Bucuresti</h3><h4>Facultatea de Antreprenoriat, Ingineria si Managementul Afacerilor</h4></div>");
              client.println("</div>");

              client.println("<div class='dashboard-container'>");

              client.println("<div class='status-bar'>");
              client.println("<span style='color:#bdc3c7;font-size:12px;'>MONITOR OBOSEALĂ</span>");
              client.println("<span id='statusText' class='warn'>Detectare...</span>");
              client.println("</div>");

              client.println("<div class='block' style='grid-column:1/-1;'><div class='block-title'>Scor oboseală</div>");
              client.println("<div style='display:flex;align-items:center;gap:15px;'>");
              client.println("  <div style='flex:1;background:#f0f0f0;border-radius:50px;height:28px;overflow:hidden;'>");
              client.println("    <div id='scorBar' style='height:100%;width:0%;background:green;border-radius:50px;transition:width 0.5s ease, background 0.5s ease;'></div>");
              client.println("  </div>");
              client.println("  <span id='scorDisp' style='font-size:22px;font-weight:bold;color:#8e44ad;min-width:50px;text-align:right;'>0</span>");
              client.println("</div></div>");

              client.println("<div class='block'><div class='block-title'>Frecvență cardiacă</div>");
              client.println("<p id='bpmDisp' class='val-big bpm-col'>--</p><p class='sub-label'>BPM</p></div>");

              client.println("<div class='block'><div class='block-title'>Saturație oxigen</div>");
              client.println("<p id='spo2Disp' class='val-big spo2-col'>--</p><p class='sub-label'>SPO2</p></div>");

              client.println("<div class='block' style='grid-column:1/-1;'>");
              client.println("<div class='block-title'>Status facial</div>");
              client.println("<table style='width:100%; font-size:16px; border-spacing:0 12px; border-collapse:separate;'>");

              client.println("<tr><td style='color:#555;'>Ochi închiși</td><td id='camEar' style='font-weight:bold; text-align:right;'>NU</td></tr>");
              client.println("<tr><td style='color:#555;'>Căscat</td><td id='camMar' style='font-weight:bold; text-align:right;'>NU</td></tr>");
              client.println("<tr><td style='color:#555;'>Înclinare cap</td><td id='camHead' style='font-weight:bold; text-align:right;'>NU</td></tr>");

              client.println("</table></div>");
              client.println("</div></body></html>");
              break;
            } 
          } else {
            currentLine = "";
          }
        } else if (c != '\r') {
          currentLine += c;
        }
      }
    }
    header = "";
    client.stop();
  }
}