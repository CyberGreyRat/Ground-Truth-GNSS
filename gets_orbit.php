<?php
header("Content-Type: application/json; charset=utf-8");

// ======================================
// CSV EINLESEN
// ======================================
$csvFile = "satellite_tracks.csv";

if (!file_exists($csvFile)) {
    echo json_encode(["error" => "CSV nicht gefunden"]);
    exit;
}

$fh = fopen($csvFile, "r");
if (!$fh) {
    echo json_encode(["error" => "CSV konnte nicht geöffnet werden"]);
    exit;
}

// ======================================
// HILFSFUNKTIONEN
// ======================================

// Wandelt LAT/LON Grad → kartesische Vector3 um
function geoToVector($lat_deg, $lon_deg, $earthRadius = 6371.0)
{
    // PHP interne deg2rad() benutzen
    $lat = deg2rad($lat_deg);
    $lon = deg2rad($lon_deg);

    // Y = Up-Achse
    $x = $earthRadius * cos($lat) * cos($lon);
    $y = $earthRadius * sin($lat);
    $z = $earthRadius * cos($lat) * sin($lon);

    return [$x, $y, $z];
}

// berechnet Länge eines 3D-Vektors
function vecLength($v)
{
    return sqrt($v[0] * $v[0] + $v[1] * $v[1] + $v[2] * $v[2]);
}

// differenz zweier 3D-Vektoren
function vecSub($a, $b)
{
    return [
        $a[0] - $b[0],
        $a[1] - $b[1],
        $a[2] - $b[2]
    ];
}

// ======================================
// CSV VERARBEITEN
// ======================================

$groups = [];     // gruppiert nach SAT-ID (G5, G7, C23, etc.)
$line = 0;

while (($row = fgetcsv($fh)) !== false) {
    $line++;

    // falsche oder leere Zeilen überspringen
    if (count($row) < 8) continue;

    // CSV-Felder extrahieren
    // ["1","2025-11-14 11:01:45","G5","243","57","32","50.6489","11.3638"]

    $satId = trim($row[2]);
    $lat   = floatval(str_replace(",", ".", $row[6]));
    $lon   = floatval(str_replace(",", ".", $row[7]));
    $time  = $row[1];

    if ($satId === "") continue;

    if (!isset($groups[$satId])) {
        $groups[$satId] = [];
    }

    $groups[$satId][] = [
        "time" => $time,
        "lat"  => $lat,
        "lon"  => $lon
    ];
}

fclose($fh);

// ======================================
// ORBITS BERECHNEN
// ======================================

$orbits = [];

foreach ($groups as $satId => $points) {

    $count = count($points);

    if ($count < 2) {
        continue; // keine Bahn berechenbar
    }

    // Startpunkt
    $p0 = $points[0];
    $vec0 = geoToVector($p0["lat"], $p0["lon"]);

    // Endpunkt
    $p1 = $points[$count - 1];
    $vec1 = geoToVector($p1["lat"], $p1["lon"]);

    // Bahnvektor
    $diff = vecSub($vec1, $vec0);
    $dist = vecLength($diff);

    $orbits[] = [
        "satellite" => $satId,
        "points"    => $count,
        "start"     => $p0,
        "end"       => $p1,
        "distance"  => $dist,
    ];
}

// ======================================
// JSON SPEICHERN
// ======================================

file_put_contents("orbits.json", json_encode($orbits, JSON_PRETTY_PRINT));

echo json_encode([
    "status" => "ok",
    "orbits" => count($orbits),
    "message" => "orbits.json wurde erzeugt"
]);

?>
