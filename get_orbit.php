<?php
error_reporting(E_ALL);
ini_set('display_errors', 1);
ini_set('memory_limit', '512M'); // Erhöht Speicher für große DB
set_time_limit(300); // 5 Minuten Timeout

// ========== DATENBANK-KONFIGURATION ==========
$servername = "";
$username = "";
$password = "";
$dbname = "";

$conn = new mysqli($servername, $username, $password, $dbname);
if ($conn->connect_error) {
    die("Verbindung fehlgeschlagen: " . $conn->connect_error);
}

// Hilfsfunktion zur Konvertierung von Lat/Lon in einen 3D-Vektor (unverändert)
function toVector3($lat, $lon, $radius) {
    $earthRadius = 6371; // Echter Erdradius in km
    $orbitAltitude = 20200; // Echte GPS-Orbithöhe in km
    $totalRadius = $earthRadius + $orbitAltitude;

    $latRad = deg2rad($lat);
    $lonRad = deg2rad(-$lon);
    
    $x = cos($latRad) * cos($lonRad);
    $y = sin($latRad);
    $z = cos($latRad) * sin($lonRad);

    return ['x' => $x * $radius, 'y' => $y * $radius, 'z' => $z * $radius];
}

$sql_sat_ids = "SELECT DISTINCT sat_id FROM satellite_tracks ORDER BY sat_id ASC";
$result_sat_ids = $conn->query($sql_sat_ids);
if (!$result_sat_ids) {
    die("Fehler bei sat_ids-Query: " . $conn->error);
}
$orbits_data = [];

while($row_sat_id = $result_sat_ids->fetch_assoc()) {
    $sat_id = $row_sat_id['sat_id'];
    
    // Prepared Statement für Sicherheit
    $sql_points = "SELECT lat, lon, timestamp FROM satellite_tracks WHERE sat_id = ? ORDER BY timestamp ASC";
    $stmt = $conn->prepare($sql_points);
    if (!$stmt) {
        die("Prepare-Fehler: " . $conn->error);
    }
    $stmt->bind_param("s", $sat_id);
    $stmt->execute();
    $result_points = $stmt->get_result();

    if ($result_points->num_rows < 10) {
        // Debug: Logge in Datei statt echo, um Output sauber zu halten
        file_put_contents('debug.log', "Überspringe $sat_id - zu wenige Punkte (" . $result_points->num_rows . ")\n", FILE_APPEND);
        continue;
    }

    $points = [];
    while($row = $result_points->fetch_assoc()) { 
        $points[] = $row; 
    }
    $stmt->close();

    $longest_pass = [];
    $current_pass = [];
    if (count($points) > 1) {
        for ($i = 0; $i < count($points) - 1; $i++) {
            $current_pass[] = $points[$i];
            $time1 = new DateTime($points[$i]['timestamp']);
            $time2 = new DateTime($points[$i+1]['timestamp']);
            $interval = $time1->diff($time2);
            if ($interval->i > 5 || $interval->h > 0 || $interval->d > 0) {
                if (count($current_pass) > count($longest_pass)) { $longest_pass = $current_pass; }
                $current_pass = [];
            }
        }
    }
    $current_pass[] = end($points);
    if (count($current_pass) > count($longest_pass)) { $longest_pass = $current_pass; }
    if (count($longest_pass) < 10) {
        file_put_contents('debug.log', "Überspringe $sat_id - längster Pass zu kurz (" . count($longest_pass) . " Punkte)\n", FILE_APPEND);
        continue;
    }

    $point1_data = $longest_pass[0];
    $point2_data = $longest_pass[count($longest_pass) - 1];
    
    $visueller_erdradius = 5;
    $orbit_radius = $visueller_erdradius * ( (6371 + 20200) / 6371 );

    $v1 = toVector3($point1_data['lat'], $point1_data['lon'], $orbit_radius);
    $v2 = toVector3($point2_data['lat'], $point2_data['lon'], $orbit_radius);

    $normal = ['x' => $v1['y'] * $v2['z'] - $v1['z'] * $v2['y'], 'y' => $v1['z'] * $v2['x'] - $v1['x'] * $v2['z'], 'z' => $v1['x'] * $v2['y'] - $v1['y'] * $v2['x']];
    $dot_product = $v1['x'] * $v2['x'] + $v1['y'] * $v2['y'] + $v1['z'] * $v2['z'];
    $angle_rad = acos(max(-1.0, min(1.0, $dot_product / ($orbit_radius * $orbit_radius))));
    
    $time1 = new DateTime($point1_data['timestamp']);
    $time2 = new DateTime($point2_data['timestamp']);
    $time_diff_sec = abs($time2->getTimestamp() - $time1->getTimestamp());
    
    if ($time_diff_sec < 60) {
        file_put_contents('debug.log', "Überspringe $sat_id - Pass zu kurz ($time_diff_sec Sekunden)\n", FILE_APPEND);
        continue;
    }
    $angular_velocity = $angle_rad / $time_diff_sec;

    $orbits_data[] = [ 'id' => $sat_id, 'normal' => $normal, 'point_on_orbit' => $v1, 'angular_velocity' => $angular_velocity, 'start_time' => $time1->getTimestamp() ];
}

$conn->close();
header('Content-Type: application/json');
echo json_encode($orbits_data);
?>