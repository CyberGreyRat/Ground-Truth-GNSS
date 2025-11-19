<?php

$servername = "localhost";
$username = "root";
$password = ""; // <-- WICHTIG: Passwort eintragen
$dbname = "raw_satellite_db";

// ----- 2. Verbindung herstellen -----
$conn = new mysqli($servername, $username, $password, $dbname);
if ($conn->connect_error) {
    header("HTTP/1.1 500 Internal Server Error");
    die("Verbindung fehlgeschlagen: " . $conn->connect_error);
}
$conn->set_charset("utf8mb4");

// ----- 3. API-Logik -----
$response_data = [];
header('Content-Type: application/json');

// WIR NUTZEN DIE NEUEN SPALTENNAMEN aus `satellite_tracks`
if (isset($_GET['get']) && $_GET['get'] == 'sat_ids') {
    // ---- Anfrage A: Liefere alle einzigartigen Satelliten-IDs ----
    $sql = "SELECT DISTINCT sat_id FROM satellite_tracks ORDER BY sat_id ASC";
    $result = $conn->query($sql);
    while ($row = $result->fetch_assoc()) {
        $response_data[] = $row['sat_id'];
    }

} elseif (isset($_GET['sat_id'])) {
    // ---- Anfrage B: Liefere die gesamte Historie für EINEN Satelliten ----
    $sat_id = (int)$_GET['sat_id'];
    
    $stmt = $conn->prepare("SELECT `timestamp`, `sat_id`, `elevation`, `azimut`, `snr` 
                           FROM satellite_tracks 
                           WHERE sat_id = ? 
                           ORDER BY timestamp ASC");
    $stmt->bind_param("i", $sat_id);
    $stmt->execute();
    $result = $stmt->get_result();
    
    while ($row = $result->fetch_assoc()) {
        $response_data[] = $row;
    }
    $stmt->close();

} elseif (isset($_GET['get']) && $_GET['get'] == 'live') {
    // ---- NEUE ANFRAGE C: Liefere die letzte Position von ALLEN Satelliten ----
    // Diese Abfrage nutzt den Index (idx_sat_history) optimal
    // Wir holen die Zeile mit der höchsten ID für jeden Satelliten.
    $sql = "SELECT t1.`timestamp`, t1.`sat_id`, t1.`elevation`, t1.`azimut`, t1.`snr`
            FROM satellite_tracks t1
            INNER JOIN (
                SELECT sat_id, MAX(id) AS max_id
                FROM satellite_tracks
                GROUP BY sat_id
            ) t2 ON t1.sat_id = t2.sat_id AND t1.id = t2.max_id";
    
    $result = $conn->query($sql);
    while ($row = $result->fetch_assoc()) {
        $response_data[] = $row;
    }

} else {
    // ---- Anfrage D (Standard/Fallback): Liefere den LETZTEN Datensatz ----
    $sql = "SELECT `timestamp`, `sat_id`, `elevation`, `azimut`, `snr` 
            FROM satellite_tracks 
            ORDER BY id DESC 
            LIMIT 1";
    $result = $conn->query($sql);
    $response_data = $result->fetch_assoc();
}

// ----- 4. Antwort als JSON senden -----
echo json_encode($response_data);

// ----- 5. Verbindung schließen -----
$conn->close();
?>