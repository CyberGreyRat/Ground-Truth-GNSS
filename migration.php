<?php
// Dieses Skript liest alle alten Daten aus nmea_logs
// und füllt die saubere satellite_tracks Tabelle.
// EINMALIG ausführen! (Kann lange dauern)
set_time_limit(300); // 5 Minuten
ini_set('display_errors', 1);
error_reporting(E_ALL);
echo "Starte Migration...<br>";

// --- 1. Datenbank-Konfiguration (LOKAL) ---
$servername = "localhost";
$username = "";
$password = "";
$dbname = "";

// --- NMEA Hilfsfunktion (Kopiert) ---
function get_system_prefix($sentence) {
    if (strlen($sentence) < 3) return 'U';
    $t1 = $sentence[1];
    $t2 = $sentence[2];
    if ($t1 == 'G' && $t2 == 'P') return 'G';
    if ($t1 == 'G' && $t2 == 'L') return 'R';
    if ($t1 == 'G' && $t2 == 'A') return 'E';
    if ($t1 == 'G' && $t2 == 'B') return 'C';
    return 'U';
}

// ----- 2. Verbindung herstellen -----
$conn = new mysqli($servername, $username, $password, $dbname);
if ($conn->connect_error) {
    die("DB Connection Failed: " . $conn->connect_error);
}
$conn->set_charset("utf8mb4");
echo "DB Verbunden...<br>";

// --- Lösche alte Daten, um sauber zu starten (optional) ---
$conn->query("TRUNCATE TABLE satellite_tracks");
echo "Alte 'satellite_tracks' geleert.<br>";

// ----- 3. Alle relevanten Rohdaten holen -----
$sql = "SELECT server_timestamp, sentence FROM nmea_logs WHERE sentence LIKE '%GSV%' ORDER BY id ASC";
$result_raw = $conn->query($sql);
if (!$result_raw) {
    die("Fehler beim Lesen von nmea_logs: " . $conn->error);
}
echo $result_raw->num_rows . " GSV-Sätze gefunden. Beginne Verarbeitung...<br>";

$sats_updated = 0;
$sats_rejected_glitch = 0;
$lines_processed = 0;

// ----- 4. SQL-Statements vorbereiten -----
// (Wir brauchen hier KEINEN Duplikats-Check, da wir die volle Historie wollen)
// (Doch, wir brauchen ihn, sonst wird die Tabelle riesig!)
$stmt_check = $conn->prepare("SELECT azimut, elevation, snr FROM satellite_tracks WHERE sat_id = ? ORDER BY id DESC LIMIT 1");
$stmt_clean = $conn->prepare("INSERT INTO satellite_tracks (sat_id, elevation, azimut, snr, `timestamp`) VALUES (?, ?, ?, ?, ?)");

$last_percent = 0;

// ----- 5. Jede Zeile verarbeiten -----
while ($row = $result_raw->fetch_assoc()) {
    $lines_processed++;
    $raw_sentence = trim($row['sentence']);
    $db_timestamp = $row['server_timestamp']; // Wichtig: Nimm alten Zeitstempel!

    // Fortschritt anzeigen (jewegeils bei 10%)
    $percent = floor(($lines_processed / $result_raw->num_rows) * 100);
    if ($percent > $last_percent + 9) {
        echo "Fortschritt: $percent% ($lines_processed Sätze verarbeitet)<br>";
        $last_percent = $percent;
        flush(); // Ausgabe an Browser senden
    }

    if (empty($raw_sentence) || !str_starts_with($raw_sentence, '$')) {
        continue;
    }

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

                if ($elevation < 0 || $elevation > 90 || $azimut < 0 || $azimut > 360) {
                    $sats_rejected_glitch++;
                    continue; 
                }

                $unique_sat_id = $id_prefix . $sat_id_num;

                $is_different = false;
                $stmt_check->bind_param("s", $unique_sat_id);
                $stmt_check->execute();
                $result_check = $stmt_check->get_result();

                if ($result_check->num_rows == 0) {
                    $is_different = true; 
                } else {
                    $last_entry = $result_check->fetch_assoc();
                    if ($last_entry['azimut'] != $azimut || 
                        $last_entry['elevation'] != $elevation || 
                        $last_entry['snr'] != $snr) {
                        $is_different = true;
                    }
                }

                if ($is_different) {
                    // WICHTIG: "siiis" -> sat_id, elev, azim, snr, timestamp
                    $stmt_clean->bind_param("siiis", $unique_sat_id, $elevation, $azimut, $snr, $db_timestamp);
                    $stmt_clean->execute();
                    $sats_updated++;
                }
            }
        }
    }
}

$stmt_check->close();
$stmt_clean->close();
$conn->close();
echo "<hr>Migration Abgeschlossen!<br>";
echo "Verarbeitete Zeilen: $lines_processed<br>";
echo "Glitches (ungültige Daten) verworfen: $sats_rejected_glitch<br>";
echo "Saubere Datensätze in 'satellite_tracks' geschrieben: $sats_updated<br>";
?>