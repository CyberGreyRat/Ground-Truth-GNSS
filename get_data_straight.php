<?php
// V3.0: "Relative Time" Update
// Lädt Daten relativ zum LETZTEN Eintrag in der DB, nicht relativ zur Server-Uhrzeit.

ini_set('display_errors', 0);
ini_set('memory_limit', '1024M'); 
set_time_limit(600); 
error_reporting(0);
header('Content-Type: application/json');

// ----- 1. Datenbank-Konfiguration -----
$servername = "";
$username = "";
$password = "";
$dbname = "";

$conn = new mysqli($servername, $username, $password, $dbname);
if ($conn->connect_error) {
    die(json_encode(['error' => 'DB-Verbindung fehlgeschlagen: ' . $conn->connect_error]));
}
$conn->set_charset("utf8mb4");

// ----- 2. Den LETZTEN Zeitstempel finden -----
// Wir fragen die DB: "Wann war das letzte Signal?"
$result_max = $conn->query("SELECT MAX(timestamp) as max_time FROM satellite_tracks");
$row_max = $result_max->fetch_assoc();
$max_time = $row_max['max_time'];

if (!$max_time) {
    die(json_encode([])); // DB ist leer
}

// ----- 3. Zeit-Parameter abfragen -----
$range = $_GET['range'] ?? '1h';
$sql_filter = "";
$use_downsampling = false;

// Wir nutzen $max_time als Anker statt NOW()
switch ($range) {
    case '1h':
        $sql_filter = "WHERE timestamp >= '$max_time' - INTERVAL 1 HOUR";
        break;
    case '12h':
        $sql_filter = "WHERE timestamp >= '$max_time' - INTERVAL 12 HOUR";
        break;
    case '24h':
        $sql_filter = "WHERE timestamp >= '$max_time' - INTERVAL 1 DAY";
        break;
    case '7d':
        $sql_filter = "WHERE timestamp >= '$max_time' - INTERVAL 7 DAY";
        $use_downsampling = true;
        break;
    case 'all':
    default:
        $sql_filter = "WHERE 1=1"; 
        $use_downsampling = true;
        break;
}

// ----- 4. SQL-Abfrage -----

if ($use_downsampling) {
    // Downsampling (10 Min Eimer)
    $sql = "
    WITH RankedData AS (
        SELECT 
            timestamp, sat_id, elevation, azimut, snr, lat, lon,
            ROW_NUMBER() OVER(
                PARTITION BY sat_id, (UNIX_TIMESTAMP(timestamp) DIV 600) 
                ORDER BY timestamp DESC
            ) as rn
        FROM 
            satellite_tracks
        $sql_filter
    )
    SELECT timestamp, sat_id, elevation, azimut, snr, lat, lon
    FROM RankedData
    WHERE rn = 1
    ORDER BY timestamp ASC";
} else {
    // Alle Daten im Zeitraum
    $sql = "
        SELECT timestamp, sat_id, elevation, azimut, snr, lat, lon 
        FROM satellite_tracks
        $sql_filter
        ORDER BY timestamp ASC";
}

$result = $conn->query($sql);

if (!$result) {
    die(json_encode(['error' => 'SQL-Fehler: ' . $conn->error]));
}

// ----- 5. Daten gruppieren -----

$data_frames = [];
$current_frame = null;
$current_time_str = null;
$fallback_lat = 50.65; 
$fallback_lon = 11.36;

if ($result->num_rows > 0) {
    while ($row = $result->fetch_assoc()) {

        $timestamp_key = $row['timestamp'];
        $lat = ($row['lat'] != 0) ? (float)$row['lat'] : $fallback_lat;
        $lon = ($row['lon'] != 0) ? (float)$row['lon'] : $fallback_lon;

        $satellite = [
            'id' => $row['sat_id'],
            'elev' => (int)$row['elevation'],
            'azim' => (int)$row['azimut'],
            'snr' => (int)$row['snr']
        ];

        if ($timestamp_key !== $current_time_str) {
            if ($current_frame !== null) {
                $current_frame['satellites'] = array_values($current_frame['satellites']);
                $data_frames[] = $current_frame;
            }

            $current_time_str = $timestamp_key;
            $current_frame = [
                'datetime' => $timestamp_key,
                'lat' => $lat,
                'lon' => $lon,
                'satellites' => [] 
            ];

            $fallback_lat = $lat;
            $fallback_lon = $lon;
        }

        $current_frame['satellites'][$satellite['id']] = $satellite;
    }

    if ($current_frame !== null) {
        $current_frame['satellites'] = array_values($current_frame['satellites']);
        $data_frames[] = $current_frame;
    }
}

$conn->close();
echo json_encode($data_frames);
?>