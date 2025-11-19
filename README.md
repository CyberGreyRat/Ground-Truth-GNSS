# 🛰️ Ground-Truth-GNSS
### Realitäts-Check für Satellitendaten: Messung vs. Theorie

![Status](https://img.shields.io/badge/Status-Active_Development-green) ![Hardware](https://img.shields.io/badge/Hardware-Heltec_Wireless_Tracker-orange) ![Tech](https://img.shields.io/badge/Visualisierung-Three.js-white) ![License](https://img.shields.io/badge/License-MIT-blue)

> **"Sind offizielle Orbitdaten Realität? Ich glaube nicht!"**

Dieses Projekt ist ein **investigatives Analyse-Tool**, entwickelt um die "Ground Truth" unserer orbitalen Infrastruktur zu hinterfragen. Es vergleicht die theoretischen Bahndaten (TLE/Kepler-Elemente) von offiziellen Quellen wie NORAD/Celestrak mit **echten, lokal empfangenen Messdaten** eines Heltec Wireless Trackers.

---

## 🧐 Die Mission: Diskrepanzen aufdecken

Wir verlassen uns blind auf GPS, GLONASS und Galileo. Aber stimmen die veröffentlichten Bahndaten wirklich mit der Position überein, an der mein Empfänger das Signal sieht?

Dieses Projekt visualisiert genau diese Lücke:
1.  **Links:** Die "offizielle Wahrheit" (berechnet aus Live-Kepler-Daten von Celestrak).
2.  **Rechts:** Die "gemessene Realität" (basierend auf Signalstärken und Azimut/Elevation des lokalen Sensors).
3.  **Unten:** Ein Dashboard, das Abweichungen in Echtzeit markiert.

## 📸 Visualisierung

### 1. Mission Control: Der direkte Vergleich
Der Herzschlag des Projekts. Ein 3D-Splitscreen, der Theorie und Praxis gegenüberstellt.
<img width="100%" alt="Splitscreen Orbit Visualisierung" src="https://github.com/user-attachments/assets/f3f92e47-ac4b-42b2-af13-ff3a5af7dccb" />

### 2. Ground-Tracks: Der Blick von oben
Die Bodenprojektion der empfangenen Signale. Wo war der Satellit wirklich, als er "Hallo" sagte?
<img width="100%" alt="Ground Tracker Map" src="https://github.com/user-attachments/assets/217f2fc0-184d-4937-9043-28967918d222" />

---

## 🚀 Features

* **Multi-Constellation Tracking:** Unterstützt GPS (USA), GLONASS (Russland), Galileo (EU) und BeiDou (China).
* **Live-Decoding:** Verarbeitet NMEA-Datenströme direkt vom ESP32 (Heltec V3).
* **Physik-Engine:** Berechnet Orbit-Mechanik live im Browser basierend auf aktuellen Ephemeriden (Kepler-Gesetze).
* **Data-Matching:** Identifiziert Satelliten anhand ihrer PRN und vergleicht berechnete Distanz mit gemessener Signalqualität.
* **Interaktiv:** Zoom, Pan, Rotate und Filterung einzelner Konstellationen.

## 🛠️ Tech Stack

Dieses Projekt verbindet Embedded Systems mit moderner Web-Technologie.

| Bereich | Technologie | Beschreibung |
| :--- | :--- | :--- |
| **Hardware** | **Heltec Wireless Tracker** | ESP32-S3 + LoRa + GNSS Modul für Rohdaten-Erfassung. |
| **Frontend** | **Three.js** (WebGL) | Für das physikalische Rendering der Orbits und der Erdkugel. |
| **Core** | **JavaScript (ES6)** | Kepler-Berechnungen, NMEA-Parser und UI-Logik. |
| **Daten** | **JSON & NMEA 0183** | Verarbeitung von Celestrak TLEs und Sensor-Streams. |

## 🔬 Wie es funktioniert

1.  **Datenerfassung:** Der Heltec Tracker empfängt rohe Satellitensignale und speichert Signalstärke (SNR), Azimut, Elevation und PRN-Nummern.
2.  **Referenz-Abruf:** Das System lädt die aktuellsten TLE-Daten (Two-Line Elements) von Celestrak.
3.  **Simulation:** Die Website berechnet für jeden Satelliten die theoretische Position zur exakten Uhrzeit der Messung.
4.  **Vergleich:** Das Dashboard zeigt, ob ein Satellit laut Theorie dort sein sollte, wo er gemessen wurde – oder ob er "driftet".

## 💻 Installation & Start

Du willst die Daten selbst sehen?

1.  **Repository klonen:**
    ```bash
    git clone [https://github.com/CyberGreyRat/Ground-Truth-GNSS.git](https://github.com/CyberGreyRat/Ground-Truth-GNSS.git)
    cd Ground-Truth-GNSS
    ```

2.  **Lokal starten:**
    Da das Projekt CORS-Richtlinien für lokale JSON-Dateien umgeht, nutze einen einfachen Server (z.B. Live Server in VS Code oder Python):
    ```bash
    # Python Beispiel
    python -m http.server
    ```

3.  **Öffnen:**
    Navigiere im Browser zu `http://localhost:8000/splitscreen.html`.

---

## 👨‍💻 Über den Entwickler

Ich bin angehender **Fachinformatiker für Anwendungsentwicklung** mit einem klaren Ziel: **Aerospace**.
Mich fasziniert die Schnittstelle zwischen Low-Level Hardware (C/C++) und komplexer Datenvisualisierung. Dieses Projekt entstand aus dem Wunsch, nicht einfach Daten zu konsumieren, sondern sie zu verifizieren.

---
*Hinweis: Dieses Projekt dient Bildungszwecken. Abweichungen können durch atmosphärische Störungen, Hardware-Latenzen oder veraltete TLE-Daten entstehen.*
