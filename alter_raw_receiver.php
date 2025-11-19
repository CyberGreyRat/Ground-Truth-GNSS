<?php

$servername = ".hosting-data.io"; // z.B. db123456789.hosting-data.io
$username = "";
$password = "$$";
$dbname = "";

header('Content-Type: text/plain');

// 1. Datenbankverbindung herstellen
$conn = new mysqli($servername, $username, $password, $dbname);
if ($conn->connect_error) {
    http_response_code(500);
    die("DB-Verbindung fehlgeschlagen: " . $conn->connect_error);
}

// 2. HTTP POST-Rohdaten lesen (der ESP32 sendet einen Text-Block)
if ($_SERVER['REQUEST_METHOD'] == 'POST') {
    
    // Lies den gesamten Body der Anfrage (alle NMEA-Sätze auf einmal)
    $data_block = file_get_contents('php://input');

    if (empty($data_block)) {
        http_response_code(400);
        die("Fehler: Leere Daten empfangen.");
    }

    // 3. Bereite EINEN SQL-Befehl vor (sehr effizient)
    $sql = "INSERT INTO nmea_logs (sentence) VALUES (?)";
    $stmt = $conn->prepare($sql);
    $stmt->bind_param("s", $nmea_sentence);

    // 4. Teile den Block in einzelne Sätze (Zeilen) auf
    $sentences = explode("\n", $data_block);
    $import_count = 0;

    foreach ($sentences as $sentence) {
        // Ignoriere leere Zeilen (entstehen oft am Ende)
        if (trim($sentence) == '' || !str_starts_with($sentence, '$')) {
            continue;
        }
        
        $nmea_sentence = trim($sentence);
        if ($stmt->execute()) {
            $import_count++;
        }
    }

    $stmt->close();
    echo "OK: $import_count Sätze erfolgreich in raw_satellite_db gespeichert.";

} else {
    http_response_code(405); // Method Not Allowed
    echo "Fehler: Nur POST-Anfragen sind erlaubt.";
}

$conn->close();
?>