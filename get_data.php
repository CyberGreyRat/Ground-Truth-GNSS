<?php
// V2.4: Behebt das "Wackel-Problem" (Mikro-Glitches) beim Senden.

// PHP-Header (Fehler ausblenden, JSON-Typ)
ini_set('display_errors', 0);
ini_set('memory_limit', '1024M'); // Mehr Speicher für 1M+ Sätze
set_time_limit(600); // 10 Minuten max.
error_reporting(0);
header('Content-Type: application/json');

// ----- 1. Datenbank-Konfiguration (DEINE SERVER-DATEN) -----
$servername = ""; 
$username = "";
$password = "";
$dbname = "";
// ----- 2. Verbindung herstellen -----
$conn = new mysqli($servername, $username, $password, $dbname);
if ($conn->connect_error) {
    die(json_encode(['error' => 'DB-Verbindung fehlgeschlagen: ' . $conn->connect_error]));
}
$conn->set_charset("utf8mb4");

// ----- 3. Zeit-Parameter abfragen (MIT DOWNSAMPLING-LOGIK) -----
$range = $_GET['range'] ?? '1h'; // Standard 1h
$sql_filter = "";
$use_downsampling = false; // NEUE Variable

switch ($range) {
    case '1h':
        $sql_filter = "WHERE timestamp >= NOW() - INTERVAL 1 HOUR";
        break;
    case '12h':
        $sql_filter = "WHERE timestamp >= NOW() - INTERVAL 12 HOUR";
        break;
    case '24h':
        $sql_filter = "WHERE timestamp >= NOW() - INTERVAL 1 DAY";
        break;
    case '7d':
        $sql_filter = "WHERE timestamp >= NOW() - INTERVAL 7 DAY";
        $use_downsampling = true; // HIER
        break;
    case 'all':
    default:
        $sql_filter = "WHERE 1=1"; // (Sicherer als leerer Filter, falls 'all' genutzt wird)
        $use_downsampling = true; // HIER
        break;
}

// ----- 4. SQL-Abfrage (HOLT ALLE DATEN) -----

if ($use_downsampling) {
    // NEU: Downsampling-Abfrage (MySQL 8.0+ / MariaDB 10.2+)
    // Diese Abfrage erstellt 10-Minuten-"Eimer" (600 Sekunden)
    // und nimmt nur den LETZTEN Datensatz aus jedem Eimer pro Satellit.
    $sql = "
    WITH RankedData AS (
        SELECT 
            timestamp, sat_id, elevation, azimut, snr, lat, lon,
            
            -- Partitioniere nach Satellit UND 10-Minuten-Intervall
            ROW_NUMBER() OVER(
                PARTITION BY sat_id, (UNIX_TIMESTAMP(timestamp) DIV 600) 
                ORDER BY timestamp DESC
            ) as rn
        FROM 
            satellite_tracks
        $sql_filter
    )
    SELECT 
        timestamp, sat_id, elevation, azimut, snr, lat, lon
    FROM 
        RankedData
    WHERE 
        rn = 1 -- Nimm nur den letzten Eintrag (rn=1) pro Eimer
    ORDER BY 
        timestamp ASC"; // WICHTIG: Nach Zeit sortieren!

} else {
    // Standard-Abfrage (wie bisher)
    $sql = "
        SELECT 
            timestamp, sat_id, elevation, azimut, snr, lat, lon 
        FROM 
            satellite_tracks
        $sql_filter
        ORDER BY 
            timestamp ASC";
}

$result = $conn->query($sql);

if (!$result) {
    // Wenn die SQL fehlschlägt (z.B. alte DB-Version), gib SQL-Fehler zurück
    die(json_encode(['error' => 'SQL-Fehler: ' . $conn->error]));
}

// ----- 5. DATEN-GRUPPIERUNG (V2.4 FIX) -----

$data_frames = [];
$current_frame = null;
$current_time_str = null;
$fallback_lat = 50.65; // Saalfeld
$fallback_lon = 11.36;

if ($result->num_rows > 0) {
    while ($row = $result->fetch_assoc()) {

        $timestamp_key = $row['timestamp'];
        // Lese Lat/Lon (oder nutze Fallback, falls RMC fehlte)
        $lat = ($row['lat'] != 0) ? (float)$row['lat'] : $fallback_lat;
        $lon = ($row['lon'] != 0) ? (float)$row['lon'] : $fallback_lon;

        $satellite = [
            'id' => $row['sat_id'],
            'elev' => (int)$row['elevation'],
            'azim' => (int)$row['azimut'],
            'snr' => (int)$row['snr']
        ];

        if ($timestamp_key !== $current_time_str) {
            // Speichere den *vorherigen* Frame (nachdem er final gefiltert wurde)
            if ($current_frame !== null) {
                // Konvertiere das Satelliten-Map zurück in ein Array
                $current_frame['satellites'] = array_values($current_frame['satellites']);
                $data_frames[] = $current_frame;
            }

            // Starte einen *neuen* Frame
            $current_time_str = $timestamp_key;
            $current_frame = [
                'datetime' => $timestamp_key,
                'lat' => $lat,
                'lon' => $lon,
                'satellites' => [] // WICHTIG: Als leeres Array (Map) initialisieren
            ];

            // Aktualisiere Fallbacks für den (seltenen) Fall, 
            // dass der nächste Frame *nur* GSV-Sätze hat.
            $fallback_lat = $lat;
            $fallback_lon = $lon;
        }

        // --- DER V2.4 "WACKEL-FIX" ---
        $current_frame['satellites'][$satellite['id']] = $satellite;
    } // Ende While-Schleife

    // Den allerletzten Frame nicht vergessen!
    if ($current_frame !== null) {
        $current_frame['satellites'] = array_values($current_frame['satellites']);
        $data_frames[] = $current_frame;
    }
}

// ----- 6. Sauberes, GEFRAMTES JSON senden -----
$conn->close();
echo json_encode($data_frames);
