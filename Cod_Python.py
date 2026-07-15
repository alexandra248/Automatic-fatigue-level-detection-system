import cv2
import mediapipe as mp
from mediapipe.tasks import python
from mediapipe.tasks.python import vision
import urllib.request, os
import numpy as np
from collections import deque 
import requests 
import time
import threading 
import csv 
from datetime import datetime

ESP32_DASHBOARD_IP = "http://192.168.4.1"  
SAVE_INTERVAL = 1.0        # Intervalul de salvare în secunde
last_save_time = 0.0      
PROCESS_EVERY_N = 2        # Procesează 1 din 2 frame-uri cu MediaPipe
frame_count = 0

MODEL_PATH = "face_landmarker.task"
MODEL_URL = "https://storage.googleapis.com/mediapipe-models/face_landmarker/face_landmarker/float16/1/face_landmarker.task"

if not os.path.exists(MODEL_PATH):
    print("Se descarca modelul FaceLandmarker...")
    urllib.request.urlretrieve(MODEL_URL, MODEL_PATH)
    print("Model descarcat!")

#Indici pentru ochi 
LEFT_EYE = [33, 160, 158, 133, 153, 144]
RIGHT_EYE = [263, 387, 385, 362, 380, 373]

#Indici pentru gură
MOUTH = [61, 291, 82, 87, 312, 317]

excel_lock = threading.Lock()

#Functii pentru a calcula distanta dintre ochi
def euclidean_distance(p1, p2):
    return np.sqrt((p1[0] - p2[0]) ** 2 + (p1[1] - p2[1]) ** 2)

def EAR(eye_landmarks, w, h):
    #Convertire punctele in coordonate pixel
    p=[(int(lm.x * w), int(lm.y * h)) for lm in eye_landmarks]
    #Calcul distanta verticala
    A = euclidean_distance(p[1], p[5])  # p2-p6
    B = euclidean_distance(p[2], p[4])  # p3-p5
    #Calcul distanta orizontala
    C = euclidean_distance(p[0], p[3])  # p1-p4
    #Calcul EAR
    ear = (A + B) / (2.0 * C)
    return ear

def MAR(mouth_landmarks, w, h):
    #Convertire punctele in coordonate pixel
    p=[(int(lm.x * w), int(lm.y * h)) for lm in mouth_landmarks]
    #Calcul distanta verticala
    A = euclidean_distance(p[2], p[3])  # p3-p4 stanga
    B = euclidean_distance(p[4], p[5])  # p5-p6 centru
    #Calcul distanta orizontala
    C = euclidean_distance(p[0], p[1])  # p1-p2 latime gura
    #Calcul MAR
    mar = (A + B) / (2.0 * C)
    return mar

def HEAD_POSE(face, w, h): 
    # Indici pentru punctele de referinta
    nose_tip = (int(face[1].x * w), int(face[1].y * h))  # varful nasului
    chin = (int(face[152].x * w), int(face[152].y * h))  # barbia
    left_eye_corner = (int(face[33].x * w), int(face[33].y * h))  # coltul stang al ochiului
    right_eye_corner = (int(face[263].x * w), int(face[263].y * h))  # coltul drept al ochiului

    # Calcul unghiuri de rotatie pe baza acestor puncte
    horizontal_angle = np.arctan2(right_eye_corner[1] - left_eye_corner[1], right_eye_corner[0] - left_eye_corner[0]) * 180 / np.pi
    vertical_angle = np.arctan2(chin[1] - nose_tip[1], chin[0] - nose_tip[0]) * 180 / np.pi

    return horizontal_angle, vertical_angle

# Inițializare fișier Excel 
CSV_FILE = "monitorizare_oboseala.csv"

if not os.path.exists(CSV_FILE):
    with open(CSV_FILE, mode='w', newline='', encoding='utf-8-sig') as f:
        writer = csv.writer(f, delimiter=';')
        headers = ["Timestamp", "BPM", "SpO2 (%)", "EAR", "MAR", "Unghi cap",
                   "Ochi închiși", "Căscat", "Înclinare cap", "Scor oboseală", "Latenta EAR (ms)", "Latenta MAR (ms)", "Latenta înclinare cap (ms)"]
        writer.writerow(headers)
    print(f"Fișier CSV creat: {CSV_FILE}")

def save_to_csv(bpm, spo2, ear, mar, angle, ear_alert, mar_alert, head_alert, score,
                lat_ear, lat_mar, lat_head):
    with excel_lock:
        try:
            with open(CSV_FILE, mode='a', newline='', encoding='utf-8-sig') as f:
                writer = csv.writer(f, delimiter=';') 
                row = [
                    datetime.now().strftime("%Y-%m-%d %H:%M:%S"),
                    bpm,
                    spo2,
                    round(ear, 3),
                    round(mar, 3),
                    round(angle, 1),
                    "DA" if ear_alert else "NU",
                    "DA" if mar_alert else "NU",
                    "DA" if head_alert else "NU",
                    score,
                    int(lat_ear), int(lat_mar), int(lat_head)
                ]
                writer.writerow(row)
        except Exception as e:
            print(f"Eroare scriere CSV: {e}")

def fetch_and_save(ear, mar, angle, ear_alert, mar_alert, head_alert, score):
    bpm_esp = 0
    spo2_esp = 0
    
    try:
        payload = {
            "cam_ear": 1 if ear_alert else 0,
            "cam_mar": 1 if mar_alert else 0,
            "cam_head": 1 if head_alert else 0,
            "cam_ear_v": float(ear),
            "cam_mar_v": float(mar),
            "cam_angle": float(angle)
        }
        requests.post(f"{ESP32_DASHBOARD_IP}/updateCamera", json=payload, timeout=1.0)
    except Exception as e:
        print(f"Eroare trimitere camera catre ESP32: {e}")

    try:
        response = requests.get(f"{ESP32_DASHBOARD_IP}/getData", timeout=1.0)
        if response.status_code == 200:
            data = response.json()
            bpm_esp = data.get("bpm", 0)
            spo2_esp = data.get("spo2", 0)
    except Exception as e:
        print(f"Eroare colectare date senzor de la ESP32: {e}")

    save_to_csv(
        bpm=bpm_esp, 
        spo2=spo2_esp, 
        ear=ear, 
        mar=mar, 
        angle=angle, 
        ear_alert=ear_alert, 
        mar_alert=mar_alert, 
        head_alert=head_alert, 
        score=score,
        lat_ear=last_ear_latency_ms,    
        lat_mar=last_mar_latency_ms,    
        lat_head=last_head_latency_ms 
    )

# Configurare detector MediaPipe Face Landmarker
base_options = python.BaseOptions(model_asset_path=MODEL_PATH)
options = vision.FaceLandmarkerOptions(
    base_options=base_options,
    num_faces=1,
    min_face_detection_confidence=0.5,
    min_face_presence_confidence=0.5,
    min_tracking_confidence=0.5,
)
detector = vision.FaceLandmarker.create_from_options(options)

# Praguri 
EAR_PRAG = 0.2
MAR_PRAG = 0.5 
HEAD_POSE_PRAG = 20  

#Initializare PERCLOS
PERCLOS_WINDOW = 20  # numarul de frame-uri 
perclos_values = deque(maxlen=PERCLOS_WINDOW)  
PERCLOS_PRAG = 0.4  
perclos_mar      = deque(maxlen=PERCLOS_WINDOW)
PERCLOS_MAR_PRAG = 0.4 

# Valori curente 
avg_ear          = 0.0
avg_mar          = 0.0
horizontal_angle = 0.0
perclos_procent  = 0.0
perclos_mar_procent = 0.0
left_eye_landmarks  = []
right_eye_landmarks = []
mouth_landmarks     = []

# Latenta 
ear_alert_start = None
ear_latency   = False
mar_alert_start = None
mar_latency  = False
head_alert_start = None
head_latency   = False

last_ear_latency_ms  = 0
last_mar_latency_ms  = 0
last_head_latency_ms = 0

# Camera ESP-S3-CAM
cap = cv2.VideoCapture(f"http://192.168.4.2:81/stream")
cap.set(cv2.CAP_PROP_BUFFERSIZE, 1) 

while True:
    ret, frame = cap.read()
    if not ret:
        print("Nu se poate citi frame-ul. Verifică conexiunea la cameră.")
        time.sleep(0.1)
        continue

    frame = cv2.flip(frame, 1)
    h, w, _ = frame.shape
    frame_count += 1   

    # Conversie pentru MediaPipe
    if frame_count % PROCESS_EVERY_N == 0:
        rgb_frame = cv2.cvtColor(frame, cv2.COLOR_BGR2RGB)
        mp_image  = mp.Image(image_format=mp.ImageFormat.SRGB, data=rgb_frame)
        results   = detector.detect(mp_image)

        if results.face_landmarks:
            for face in results.face_landmarks:
                left_eye_landmarks  = [face[i] for i in LEFT_EYE]
                right_eye_landmarks = [face[i] for i in RIGHT_EYE]
                mouth_landmarks     = [face[i] for i in MOUTH]

                left_ear  = EAR(left_eye_landmarks, w, h)
                right_ear = EAR(right_eye_landmarks, w, h)
                avg_ear   = (left_ear + right_ear) / 2.0
                avg_mar   = MAR(mouth_landmarks, w, h)
                horizontal_angle, _ = HEAD_POSE(face, w, h)

                perclos_values.append(1 if avg_ear < EAR_PRAG else 0)
                perclos_procent = sum(perclos_values) / len(perclos_values)

                perclos_mar.append(1 if avg_mar > MAR_PRAG else 0)
                perclos_mar_procent = sum(perclos_mar) / len(perclos_mar)

                # Latenta EAR 
                if avg_ear < EAR_PRAG and ear_alert_start is None:
                    if perclos_procent <= PERCLOS_PRAG: 
                        ear_alert_start = time.time()
                        ear_latency   = False
                if perclos_procent > PERCLOS_PRAG and ear_alert_start and not ear_latency:
                    latency_ms = (time.time() - ear_alert_start) * 1000
                    print(f"[LATENTA] Alerta EAR: {latency_ms:.0f} ms")
                    ear_latency = True
                    last_ear_latency_ms = latency_ms
                if avg_ear >= EAR_PRAG:
                    ear_alert_start = None
                    ear_latency   = False
                
                # Latenta MAR 
                if avg_mar > MAR_PRAG and mar_alert_start is None:
                    if perclos_mar_procent <= PERCLOS_MAR_PRAG: 
                        mar_alert_start = time.time()
                        mar_latency   = False
                if perclos_mar_procent > PERCLOS_MAR_PRAG and mar_alert_start and not mar_latency:
                    latency_ms = (time.time() - mar_alert_start) * 1000
                    print(f"[LATENTA] Alerta MAR: {latency_ms:.0f} ms")
                    mar_latency = True
                    last_mar_latency_ms = latency_ms
                if avg_mar <= MAR_PRAG:
                    mar_alert_start = None
                    mar_latency   = False
                
                # Latenta Head Pose
                if abs(horizontal_angle) > HEAD_POSE_PRAG and head_alert_start is None:
                    head_alert_start = time.time()
                    head_latency   = False
                if abs(horizontal_angle) > HEAD_POSE_PRAG and head_alert_start and not head_latency:
                    if (time.time() - head_alert_start) > 0.2:
                        latency_ms = (time.time() - head_alert_start) * 1000
                        print(f"[LATENTA] Alerta Head Pose: {latency_ms:.0f} ms")
                        head_latency = True
                        last_head_latency_ms = latency_ms
                if abs(horizontal_angle) <= HEAD_POSE_PRAG:
                    head_alert_start = None
                    head_latency   = False
        else:
                left_eye_landmarks  = []
                right_eye_landmarks = []
                mouth_landmarks     = []

    status_color = (0, 0, 255) if perclos_procent > PERCLOS_PRAG else (0, 255, 0)
    mar_color    = (0, 0, 255) if perclos_mar_procent > PERCLOS_MAR_PRAG else (0, 255, 0)
    tilt_color   = (0, 0, 255) if abs(horizontal_angle) > HEAD_POSE_PRAG else (0, 255, 0)

    if left_eye_landmarks and right_eye_landmarks and mouth_landmarks:
        # Desenare ochi stâng 
        eye_pts_left = [(int(lm.x * w), int(lm.y * h)) for lm in left_eye_landmarks]
        for pt in eye_pts_left:
            cv2.circle(frame, pt, 1, status_color, -1)

        # Desenare ochi drept 
        eye_pts_right = [(int(lm.x * w), int(lm.y * h)) for lm in right_eye_landmarks]
        for pt in eye_pts_right:
            cv2.circle(frame, pt, 1, status_color, -1)

        # Desenare gură 
        mouth_pts = [(int(lm.x * w), int(lm.y * h)) for lm in mouth_landmarks]
        for pt in mouth_pts:
            cv2.circle(frame, pt, 1, mar_color, -1)

    if perclos_procent > PERCLOS_PRAG:
        cv2.putText(frame, "ALERTA: Oboseala detectata!", (10, 30),
                    cv2.FONT_HERSHEY_SIMPLEX, 0.7, (0, 0, 255), 2)

    cv2.putText(frame, f"EAR: {avg_ear:.2f}", (10, 60),
                cv2.FONT_HERSHEY_SIMPLEX, 0.7, (255, 255, 255), 2)

    if perclos_mar_procent > PERCLOS_MAR_PRAG:
        cv2.putText(frame, "ALERTA: Cascat detectat!", (10, 90),
                    cv2.FONT_HERSHEY_SIMPLEX, 0.7, (0, 0, 255), 2)

    cv2.putText(frame, f"MAR: {avg_mar:.2f}", (10, 120),
                cv2.FONT_HERSHEY_SIMPLEX, 0.7, (255, 255, 255), 2)

    if abs(horizontal_angle) > HEAD_POSE_PRAG:
        cv2.putText(frame, "ALERTA: Inclinare cap detectata!", (10, 150),
                    cv2.FONT_HERSHEY_SIMPLEX, 0.7, (0, 0, 255), 2)

    cv2.putText(frame, f"Head Tilt: {horizontal_angle:.1f}", (10, 180),
                cv2.FONT_HERSHEY_SIMPLEX, 0.7, (255, 255, 255), 2)
    
 # Salvare în Excel 
    current_time = time.time()
    if current_time - last_save_time > SAVE_INTERVAL:
        last_save_time = current_time

        ear_alert = perclos_procent > PERCLOS_PRAG
        mar_alert = perclos_mar_procent > PERCLOS_MAR_PRAG
        head_alert = abs(horizontal_angle) > HEAD_POSE_PRAG

        scor = 0
        if ear_alert:  scor += 25
        if mar_alert:  scor += 15
        if head_alert: scor += 20

        threading.Thread(
            target=fetch_and_save, 
            args=(avg_ear, avg_mar, horizontal_angle, ear_alert, mar_alert, head_alert, scor), 
            daemon=True
        ).start()

    cv2.imshow("Status facial", frame)

    if cv2.waitKey(1) & 0xFF == 27:  
        break

cap.release()
cv2.destroyAllWindows()
