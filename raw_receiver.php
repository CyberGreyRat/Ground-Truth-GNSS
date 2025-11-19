<?php
/**
 * NMEA Smart Receiver V2.5 (Parse-on-Write)
 *
 * - Empfängt NMEA-Rohdaten-Blöcke vom ESP32 (V2.x ESP-Code).
 * - Parst RMC/GGA für die Position.
 * - Parst GSV für Satelliten.
 * - Wendet alle Filter an (Glitches, Geister-IDs, Duplikate).
 * - Speichert NUR die sauberen, geänderten Daten in 'satellite_tracks'.
 * - Speichert KEINE Rohdaten in 'nmea_logs' (um Platz zu sparen).
 */

set_time_limit(30);

// --- 1. Datenbank-Konfiguration (DEINE SERVER-DATEN) ---
$servername = ""; // z.B. db123456789.hosting-data.io
$username = "";
$password = "";
$dbname = "";

// --- NMEA Hilfsfunktionen (Kopiert von V2.2) ---
function get_system_prefix($sentence) {
    if (strlen($sentence) < 3) return 'U';
    $t1 = $sentence[1]; $t2 = $sentence[2];
    if ($t1 == 'G' && $t2 == 'P') return 'G'; // GPS
    if ($t1 == 'G' && $t2 == 'L') return 'R'; // GLONASS
    if ($t1 == 'G' && $t2 == 'A') return 'E'; // Galileo
    if ($t1 == 'G' && $t2 == 'B') return 'C'; // BeiDou
    return 'U';
}
function parseNMEACoord($coord, $dir) {
    $data = (float)$coord;
    if ($data == 0) return 0;
    $deg = floor($data / 100); 
    $min = $data - ($deg * 100); 
    $decimal = $deg + ($min / 60); 
    return ($dir == 'S' || $dir == 'W') ? -$decimal : $decimal;
}
// --- Ende Hilfsfunktionen ---

// ----- 2. Verbindung herstellen -----
$conn = new mysqli($servername, $username, $password, $dbname);
if ($conn->connect_error) {
    header("HTTP/1.1 500 Internal Server Error");
    die("DB Connection Failed: " . $conn->connect_error);
}
$conn->set_charset("utf8mb4");

// ----- 3. Datenblock vom ESP empfangen -----
$raw_post_body = file_get_contents('php://input');
if (empty(trim($raw_post_body))) {
    die("Leere Anfrage empfangen.");
}

// ----- 4. Block in Zeilen zerlegen -----
$lines = preg_split("/\r\n|\n|\r/", $raw_post_body);
if (empty($lines)) {
    die("Keine Zeilen im Block gefunden.");
}

$lines_processed = 0;
$sats_updated = 0;
$sats_rejected_glitch = 0;
$current_lat = 0.0;
$current_lon = 0.0;

// ----- 5. SQL-Statements vorbereiten -----
// $stmt_raw = $conn->prepare("INSERT INTO nmea_logs (sentence) VALUES (?)"); // <-- ENTFERNT (wie gewünscht)
$stmt_check = $conn->prepare("SELECT azimut, elevation, snr FROM satellite_tracks WHERE sat_id = ? ORDER BY id DESC LIMIT 1");
$stmt_clean = $conn->prepare("INSERT INTO satellite_tracks (sat_id, elevation, azimut, snr, `timestamp`, lat, lon) VALUES (?, ?, ?, ?, NOW(), ?, ?)");


// ----- 6. LOGIK V2.5: Zwei Durchläufe -----

// --- DURCHLAUF 1: Finde die Position (Lat/Lon) ---
foreach (array_reverse($lines) as $raw_sentence) {
    $raw_sentence = trim($raw_sentence);
    if (empty($raw_sentence) || !str_starts_with($raw_sentence, '$')) continue;
    
    $parts = explode(',', $raw_sentence);
    if (count($parts) < 6) continue; // Satz zu kurz
    
    $type = substr($parts[0], 3, 3);
    
    // GNRMC oder GPRMC (A = Active)
    if ($type == 'RMC' && isset($parts[2]) && $parts[2] == 'A') {
        if (isset($parts[3], $parts[4], $parts[5], $parts[6]) && !empty($parts[3]) && !empty($parts[5])) {
            $current_lat = parseNMEACoord($parts[3], $parts[4]);
            $current_lon = parseNMEACoord($parts[5], $parts[6]);
            break; // Wir haben die letzte Position, Schleife stoppen
        }
    }
    // GNGGA oder GPGGA (Fix Quality > 0)
    if ($type == 'GGA' && isset($parts[6]) && (int)$parts[6] > 0) {
         if (isset($parts[2], $parts[3], $parts[4], $parts[5]) && !empty($parts[2]) && !empty($parts[4])) {
            $current_lat = parseNMEACoord($parts[2], $parts[3]);
            $current_lon = parseNMEACoord($parts[4], $parts[5]);
            break; // Wir haben die letzte Position, Schleife stoppen
        }
    }
}

// --- DURCHLAUF 2: Parse Satelliten und speichere NUR saubere Daten ---
foreach ($lines as $raw_sentence) {
    $raw_sentence = trim($raw_sentence);
    if (empty($raw_sentence) || !str_starts_with($raw_sentence, '$')) {
        continue;
    }

    // 1. Rohdaten speichern (ENTFERNT)
    // $stmt_raw->bind_param("s", $raw_sentence);
    // $stmt_raw->execute();
    $lines_processed++;

    // 2. Prüfen, ob es ein GSV-Satz ist
    if (strlen($raw_sentence) > 6 && substr($raw_sentence, 3, 3) === 'GSV') {
        
        $sentence_parts = explode('*', $raw_sentence)[0];
        $fields = explode(',', $sentence_parts);
        $field_count = count($fields);
        $id_prefix = get_system_prefix($raw_sentence);

        for ($i = 4; $i < $field_count; $i += 4) {
            if (isset($fields[$i]) && isset($fields[$i+1]) && isset($fields[$i+2])) {
                
                if (empty(trim($fields[$i])) || empty(trim($fields[$i+1])) || empty(trim($fields[$i+2]))) {
                    continue;
                }
                $sat_id_num = (int)$fields[$i];
                $elevation = (int)$fields[$i+1];
                $azimut = (int)$fields[$i+2];
                $snr = (!empty(trim($fields[$i+3]))) ? (int)$fields[$i+3] : 0;

                // 3. VALIDIERUNGS-FILTER
                if ($elevation < 0 || $elevation > 90 || $azimut < 0 || $azimut > 360) {
                    $sats_rejected_glitch++;
                    continue; 
                }

                // 4. UNIQUE ID
                $unique_sat_id = $id_prefix . $sat_id_num; // z.B. "G08"

                // 5. Duplikate-Logik
                $is_different = false;
                $stmt_check->bind_param("s", $unique_sat_id);
                $stmt_check->execute();
                $result = $stmt_check->get_result();
                if ($result->num_rows == 0) {
                    $is_different = true; 
                } else {
                    $last_entry = $result->fetch_assoc();
                    if ($last_entry['azimut'] != $azimut || $last_entry['elevation'] != $elevation || $last_entry['snr'] != $snr) {
                        $is_different = true;
                    }
                }

                // 6. Bei Bedarf in 'satellite_tracks' speichern
                if ($is_different) {
                    // "siiidd" -> sat_id, elev, azim, snr, lat, lon
                    $stmt_clean->bind_param("siiidd", $unique_sat_id, $elevation, $azimut, $snr, $current_lat, $current_lon);
                    $stmt_clean->execute();
                    $sats_updated++;
                }
            }
        } // Ende for-Loop
    } // Ende if (GSV)
} // Ende foreach

// ----- 7. Schließen und Antwort an ESP -----
// $stmt_raw->close(); // <-- ENTFERNT
$stmt_check->close();
$stmt_clean->close();
$conn->close();

echo "V2.5 OK: $lines_processed Zeilen geparst. $sats_updated Sats aktualisiert. Pos: $current_lat, $current_lon. $sats_rejected_glitch Glitches verworfen.";
?>