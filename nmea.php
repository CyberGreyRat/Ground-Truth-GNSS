<?php
// PHP-Skript erlauben, bis zu 30 Sekunden zu laufen, 
// da die Verarbeitung eines 5-Sekunden-Blocks dauern kann.
set_time_limit(30);

// ----- 1. Datenbank-Konfiguration (Deine Daten) -----

$servername = "localhost";
$username = "";
$password = ""; // <-- WICHTIG: Passwort eintragen
$dbname = "";



// ----- 2. Verbindung herstellen -----
$conn = new mysqli($servername, $username, $password, $dbname);
if ($conn->connect_error) {
    // Wenn die DB nicht erreichbar ist, brich sofort ab.
    header("HTTP/1.1 500 Internal Server Error");
    die("DB Connection Failed: " . $conn->connect_error);
}
$conn->set_charset("utf8mb4");

// ----- 3. Schritt A: Den rohen Datenblock vom ESP empfangen -----
// Dein ESP sendet 'text/plain', daher lesen wir den "rohen" Body.
$raw_post_body = file_get_contents('php://input');

if (empty(trim($raw_post_body))) {
    die("Leere Anfrage empfangen.");
}

// ----- 4. Schritt B: Den Block in einzelne Zeilen zerlegen -----
// Dein ESP sendet Sätze getrennt mit "\n".
// preg_split ist robuster als explode, falls es \r\n oder nur \n ist.
$lines = preg_split("/\r\n|\n|\r/", $raw_post_body);

$lines_processed = 0;
$sats_updated = 0;

// Bereite die SQL-Statements EINMAL vor (viel schneller als in der Schleife)
$stmt_raw = $conn->prepare("INSERT INTO nmea_logs (sentence) VALUES (?)");
$stmt_check = $conn->prepare("SELECT azimut, elevation, snr FROM satellite_tracks WHERE sat_id = ? ORDER BY id DESC LIMIT 1"); // nach ID sortieren ist oft schneller
$stmt_clean = $conn->prepare("INSERT INTO satellite_tracks (sat_id, elevation, azimut, snr) VALUES (?, ?, ?, ?)");


// ----- 5. Schritt C: Jede einzelne Zeile verarbeiten -----
foreach ($lines as $raw_sentence) {
    
    $raw_sentence = trim($raw_sentence); // Entferne unsichtbare Zeichen
    
    if (empty($raw_sentence) || !str_starts_with($raw_sentence, '$')) {
        continue; // Überspringe leere Zeilen oder ungültige Sätze
    }

    // 1. Rohdaten IMMER speichern
    $stmt_raw->bind_param("s", $raw_sentence);
    $stmt_raw->execute();
    $lines_processed++;

    // 2. Prüfen, ob es ein GSV-Satz ist (Satellites in View)
    // GSV Sätze (GPGSV, GLGSV, GAGSV, GBGSV...) sind die einzigen mit Sat-Positionen
    if (strlen($raw_sentence) > 6 && substr($raw_sentence, 3, 3) === 'GSV') {
        
        // NMEA-Satz von Checksumme befreien und zerlegen
        $sentence_parts = explode('*', $raw_sentence)[0];
        $fields = explode(',', $sentence_parts);
        $field_count = count($fields);

        // Loop durch die Satelliten-Blöcke (4 Felder pro Sat)
        // [4:SatID], [5:Elv], [6:Lev/Azimut], [7:SNR]
        for ($i = 4; $i < $field_count; $i += 4) {
            
            // Sicherstellen, dass der Block vollständig ist (mindestens SatID, Elv, Lev)
            if (isset($fields[$i]) && isset($fields[$i+1]) && isset($fields[$i+2])) {
                
                // Leere Felder überspringen (wichtig, da SNR oft fehlt)
                if (empty(trim($fields[$i])) || empty(trim($fields[$i+1])) || empty(trim($fields[$i+2]))) {
                    continue;
                }

                $sat_id = (int)$fields[$i];
                $elevation = (int)$fields[$i+1]; // Elevation (als Integer)
                $azimut = (int)$fields[$i+2];    // Azimut (als Integer)
                $snr = (!empty(trim($fields[$i+3]))) ? (int)$fields[$i+3] : 0; // SNR (als Integer)

                // 3. Duplikate-Logik: Prüfe letzten Eintrag für DIESEN Satelliten
                $is_different = false;
                
                $stmt_check->bind_param("i", $sat_id);
                $stmt_check->execute();
                $result = $stmt_check->get_result();

                if ($result->num_rows == 0) {
                    $is_different = true; // Satellit ist neu
                } else {
                    $last_entry = $result->fetch_assoc();
                    
                    // Strikter Vergleich (da wir jetzt Integer nutzen)
                    // Nur wenn sich Azimut, Elevation ODER Stärke ändert, speichern.
                    if ($last_entry['azimut'] != $azimut || 
                        $last_entry['elevation'] != $elevation || 
                        $last_entry['snr'] != $snr) {
                        
                        $is_different = true; // Daten haben sich geändert
                    }
                }

                // 4. Bei Bedarf in 'satellite_tracks' speichern
                if ($is_different) {
                    $stmt_clean->bind_param("iiii", $sat_id, $elevation, $azimut, $snr);
                    $stmt_clean->execute();
                    $sats_updated++;
                }
            }
        } // Ende for-Loop (Satelliten im Satz)
    } // Ende if (GSV)
} // Ende foreach (Zeilen im Block)


// ----- 6. Statements schließen und Antwort an ESP -----
$stmt_raw->close();
$stmt_check->close();
$stmt_clean->close();
$conn->close();

// Wichtig: Feedback an den ESP senden, damit du im Serial Monitor siehst, was passiert ist
echo "V2.0 OK: $lines_processed Zeilen geloggt. $sats_updated Sats aktualisiert.";
?>