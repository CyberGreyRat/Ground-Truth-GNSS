<?php
// WICHTIG: Fehler unterdrücken, damit sie nicht das JSON zerstören
ini_set('display_errors', 0);
error_reporting(0);

header('Content-Type: application/json');

// --- Konfiguration ---
// --- ÄNDERUNG 1: Für lokales Testen (z.B. XAMPP/MAMP) ---
$servername = ""; 
$username = "";        
$password = "";           
$dbname = "";  
// --- ENDE ÄNDERUNG 1 ---


// --- NMEA Hilfsfunktionen (Unverändert) ---
function get_system_name($sentence) {
    if (strlen($sentence) < 3) return "Unknown";
    $t1 = $sentence[1];
    $t2 = $sentence[2];
    if ($t1 == 'G' && $t2 == 'P') return "GPS (USA)";
    if ($t1 == 'G' && $t2 == 'L') return "GLONASS (Russisch)";
    if ($t1 == 'G' && $t2 == 'A') return "Galileo (Europaeisch)";
    if ($t1 == 'G' && $t2 == 'B') return "BeiDou (Chinesisch)";
    if ($t1 == 'G' && $t2 == 'Q') return "QZSS (Japanisch)";
    return "Unknown";
}

function parseNMEACoord($coord) {
    $is_neg = (strpos($coord, 'S') !== false || strpos($coord, 'W') !== false);
    $coord = preg_replace("/[NSEW]/", "", $coord);
    $data = (float)$coord;
    if ($data == 0) return 0;
    
    $deg = floor($data / 100); 
    $min = $data - ($deg * 100); 
    $decimal = $deg + ($min / 60); 
    return $is_neg ? -$decimal : $decimal;
}

// -------------------------------------------------
// --- Haupt-Logik: ELT (Transform) ---
// -------------------------------------------------

$conn = new mysqli($servername, $username, $password, $dbname);
if ($conn->connect_error) {
    die(json_encode(['error' => 'DB-Verbindung fehlgeschlagen: ' . $conn->connect_error]));
}

// 1. Hole alle Rohdaten (GSV für Satelliten, RMC für Position)
// --- ÄNDERUNG 2: Limit für lokales Testen reduziert ---
$sql = "
    (SELECT server_timestamp, sentence 
     FROM nmea_logs
     WHERE (
          sentence LIKE '$GPGSV%' OR 
          sentence LIKE '$GLGSV%' OR 
          sentence LIKE '$GAGSV%' OR 
          sentence LIKE '$GBGSV%' OR 
          sentence LIKE '$GQGSV%'
     )
     ORDER BY id DESC
     LIMIT 1000) 
    UNION
    (SELECT server_timestamp, sentence 
     FROM nmea_logs
     WHERE (
          sentence LIKE '$GNRMC%,A,%' OR 
          sentence LIKE '$GPRMC%,A,%'
     )
     ORDER BY id DESC
     LIMIT 100)
    ORDER BY server_timestamp ASC";
// --- ENDE ÄNDERUNG 2 ---

$result = $conn->query($sql);

if (!$result) {
      die(json_encode(['error' => 'SQL-Fehler: ' . $conn->error]));
}

$output_frames = [];
$current_frame = null;
$current_time_str = null;
$current_lat = 50.65; // Fallback Saalfeld
$current_lon = 11.36;

if ($result->num_rows > 0) {
    while($row = $result->fetch_assoc()) {
        $sentence = trim($row['sentence']);
        if (empty($sentence)) continue; // Überspringe leere Zeilen

        $db_time = $row['server_timestamp'];
        $parts = explode(",", $sentence);
        
        // KORRIGIERTER Check: Stelle sicher, dass $parts[0] existiert
        if (!isset($parts[0]) || strlen($parts[0]) < 6) {
            continue; // Überspringe ungültigen Satz
        }
        $type = substr($parts[0], 3, 3); // Hole Typ (GSV, RMC)

        // 2. Frames nach Zeitstempel gruppieren
        $time_key = date('Y-m-d H:i:s', strtotime($db_time));

        if ($time_key !== $current_time_str) {
            if ($current_frame !== null) {
                if (count($current_frame['satellites']) > 0) {
                     $output_frames[] = $current_frame;
                }
            }
            $current_time_str = $time_key;
            $current_frame = [
                'datetime' => $time_key,
                'lat' => $current_lat, 
                'lon' => $current_lon,
                'satellites' => []
            ];
        }

        // 3. Verarbeite den Satz-Typ (ROBUST GEGEN FEHLER)
        if ($type == 'GSV') {
            // --- Es ist ein Satelliten-Satz ---
            $system = get_system_name($sentence);
            $field_count = count($parts);

            // --- ÄNDERUNG 3: System-Präfix für Unique ID (Der "Wackel-Fix") ---
            $id_prefix = 'U'; // U = Unknown
            if (str_starts_with($system, "GPS")) $id_prefix = 'G';
            if (str_starts_with($system, "GLONASS")) $id_prefix = 'R'; // R = Russian
            if (str_starts_with($system, "Galileo")) $id_prefix = 'E'; // E = European
            if (str_starts_with($system, "BeiDou")) $id_prefix = 'C'; // C = Chinese
            // --- ENDE ÄNDERUNG 3 (Teil 1) ---
            
            // Gehe 4 Felder auf einmal durch
            for ($i = 4; $i < $field_count; $i += 4) {
                // KORRIGIERTER Check: Stelle sicher, dass die Felder existieren
                if (isset($parts[$i], $parts[$i+1], $parts[$i+2])) {
                    
                    $id_num = $parts[$i]; // Das ist die "nackte" ID, z.B. "08"
                    $elev = $parts[$i+1];
                    $azim = $parts[$i+2];
                    
                    // SNR ist komplizierter wegen der Checksumme
                    $snr_field = $parts[$i+3] ?? ''; // Nimm das 4. Feld, oder leer
                    $checksum_pos = strpos($snr_field, '*');
                    if ($checksum_pos !== false) {
                        $snr = substr($snr_field, 0, $checksum_pos); // Schneide Checksumme ab
                    } else {
                        $snr = $snr_field;
                    }
                    
                    // Dein Validierungs-Block (den du schon hattest, super!)
                    if (!empty($id_num) && $elev !== '' && $azim !== '') {
                        
                        $elev_float = (float)$elev;
                        $azim_float = (float)$azim;

                        // Filter 1: Physikalische Glitches (z.B. 405°)
                        if ($elev_float >= 0 && $elev_float <= 90 && $azim_float >= 0 && $azim_float <= 360) {
                            
                            // --- ÄNDERUNG 3: Erstelle die Unique ID (Der "Wackel-Fix") ---
                            $unique_id = $id_prefix . $id_num; // z.B. "G08" oder "E08"
                            
                            $current_frame['satellites'][] = [
                                'system' => $system,
                                'id' => $unique_id, // <-- HIER IST DIE WICHTIGSTE ÄNDERUNG
                                'elev' => $elev_float,
                                'azim' => $azim_float,
                                'snr' => (empty($snr) || $snr == 'N/A') ? 'N/A' : (int)$snr
                            ];
                            // --- ENDE ÄNDERUNG 3 (Teil 2) ---
                        }
                        // Ungültige Daten (z.B. elev: 166) werden ignoriert
                    }
                }
            }
        } 
        else if ($type == 'RMC' && isset($parts[2]) && $parts[2] == 'A') {
            // --- Es ist ein Positions-Satz (und er ist 'A'ctive) ---
            // (Unverändert)
            if (isset($parts[3], $parts[4], $parts[5], $parts[6])) {
                if (!empty($parts[3]) && !empty($parts[5])) {
                    $current_lat = parseNMEACoord($parts[3] . $parts[4]);
                    $current_lon = parseNMEACoord($parts[5] . $parts[6]);
                    
                    if ($current_frame) {
                        $current_frame['lat'] = $current_lat;
                        $current_frame['lon'] = $current_lon;
                    }
                }
            }
        }
    }
    // Den allerletzten Frame nicht vergessen
    if ($current_frame !== null && count($current_frame['satellites']) > 0) {
        $output_frames[] = $current_frame;
    }
}

$conn->close();

// 4. Sauberes JSON an das Frontend senden
echo json_encode($output_frames);
?>