/**
 * orbit_processor.c (V5.12 - Aggregator-Version)
 *
 * V5.12 FIX: Behebt den fundamentalen Mathematik-Fehler in 'calculateWorldPos'.
 * Das alte Skript hat die Koordinatensysteme (Lokal, ECEF, ECI) vermischt,
 * was zu einer falschen, flachen (aequatorialen) Bahnebene fuehrte.
 *
 * Die neue Logik (V5.12) implementiert die korrekte Umrechnung:
 * 1. (Az, El) -> Topozentrisch (ENU - East, North, Up)
 * 2. (ENU)     -> Geozentrisch (ECEF - Earth-Centered, Earth-Fixed)
 * 3. (ECEF)    -> Inertiell (ECI - Earth-Centered Inertial)
 *
 * Erst *danach* wird die 3-Punkt-Bahnebenenberechnung durchgefuehrt.
 *
 * Kompilieren (oder 'make' nutzen):
 * gcc -Wall -O2 -o orbit_processor orbit_processor.c -lm
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <math.h>

// ========== KONFIGURATION ==========
#define VISUAL_EARTH_RADIUS 5.0
#define EARTH_RADIUS_KM 6371.0
#define PI 3.1415926535

// Hardcodierte Beobachter-Position (Saalfeld)
const double OBSERVER_LAT = 50.64893550;
const double OBSERVER_LON = 11.36386983;

#define MAX_LINE_LENGTH 256
#define MAX_PASS_GAP_SECONDS 300 // (5 Minuten)
#define MIN_PASS_LENGTH 10       // Mindestens 10 Punkte

#define MAX_UNIQUE_SATELLITES 500 

// ========== DATENSTRUKTUREN ==========

typedef struct {
    double x, y, z;
} Vector3;

typedef struct {
    double azim;
    double elev;
    time_t timestamp;
    Vector3 world_pos; // Berechnete 3D-Position (jetzt im ECI-Frame)
} DataPoint;

typedef struct {
    char sat_id[16];
    Vector3 normal;
    Vector3 point_on_orbit;
    double angular_velocity;
    time_t start_time;
    int color_hex;
    double radius;
} OrbitData;

typedef struct {
    DataPoint *points;
    size_t size;
    size_t capacity;
} PointArray;

typedef struct {
    char sat_id[16];
    PointArray points;
} SatelliteData;

typedef struct {
    OrbitData *orbits;
    size_t size;
    size_t capacity;
} OrbitArray;


// ========== HILFSFUNKTIONEN ==========

double get_nominal_altitude_km(const char* sat_id) {
    switch (sat_id[0]) {
        case 'G': return 20200.0;
        case 'R': return 19100.0;
        case 'E': return 23222.0;
        case 'C': return 21528.0;
        default:  return 20200.0;
    }
}
int getSatColor(const char* sat_id) {
    switch (sat_id[0]) {
        case 'G': return 0x00ff00;
        case 'R': return 0xff0000;
        case 'E': return 0xffff00;
        case 'C': return 0x0000ff;
        default:  return 0xffffff;
    }
}
double deg2rad(double deg) { return deg * PI / 180.0; }

time_t parse_csv_time(const char* time_str) {
    struct tm tm = {0};
    sscanf(time_str, "%d-%d-%d %d:%d:%d", 
           &tm.tm_year, &tm.tm_mon, &tm.tm_mday, 
           &tm.tm_hour, &tm.tm_min, &tm.tm_sec);
    tm.tm_year -= 1900;
    tm.tm_mon -= 1;
    tm.tm_isdst = -1; // Lass mktime die Sommerzeit bestimmen
    return mktime(&tm);
}

// Konvertiert Lat/Lon in ECEF 3D-Weltkoordinaten
Vector3 getPositionFromLatLon_ECEF(double lat, double lon, double radius) {
    double latRad = deg2rad(lat);
    double lonRad = deg2rad(lon); // ECEF nutzt positives Lon
    Vector3 v;
    v.x = radius * cos(latRad) * cos(lonRad);
    v.y = radius * cos(latRad) * sin(lonRad);
    v.z = radius * sin(latRad);
    return v;
}

// Vektor-Helfer
Vector3 v_add(Vector3 v1, Vector3 v2) {
    v1.x += v2.x; v1.y += v2.y; v1.z += v2.z;
    return v1;
}
Vector3 v_sub(Vector3 v1, Vector3 v2) {
    v1.x -= v2.x; v1.y -= v2.y; v1.z -= v2.z;
    return v1;
}
Vector3 v_scale(Vector3 v, double s) {
    v.x *= s; v.y *= s; v.z *= s;
    return v;
}
double v_dot(Vector3 v1, Vector3 v2) {
    return v1.x * v2.x + v1.y * v2.y + v1.z * v2.z;
}
double v_mag_sq(Vector3 v) { return v.x * v.x + v.y * v.y + v.z * v.z; }
double v_mag(Vector3 v) { return sqrt(v_mag_sq(v)); }
Vector3 v_normalize(Vector3 v) {
    double mag = v_mag(v);
    if (mag < 1e-9) return v;
    return v_scale(v, 1.0 / mag);
}
Vector3 v_cross(Vector3 v1, Vector3 v2) {
    Vector3 normal;
    normal.x = v1.y * v2.z - v1.z * v2.y;
    normal.y = v1.z * v2.x - v1.x * v2.z;
    normal.z = v1.x * v2.y - v1.y * v2.x;
    return normal;
}

// V5.12 NEU: Rotiert einen Vektor um die Z-Achse (für ECEF->ECI)
Vector3 v_apply_z_rotation(Vector3 v, double angle) {
    double cos_a = cos(angle);
    double sin_a = sin(angle);
    Vector3 v_out;
    v_out.x = v.x * cos_a - v.y * sin_a;
    v_out.y = v.x * sin_a + v.y * cos_a;
    v_out.z = v.z;
    return v_out;
}

// V5.12 NEU: Berechnet die Erdrotation (GMST-Annäherung)
double getEarthRotationAngle(time_t timestamp) {
    // Wandle Unix-Zeit in Julianisches Datum um
    double JD = (double)timestamp / 86400.0 + 2440587.5;
    double d = JD - 2451545.0;
    
    // Berechne GMST (Greenwich Mean Sidereal Time) - Annäherung
    double GMST = 18.697374558 + 24.0611018903 * d + 0.000021 * d * d;
    GMST = fmod(GMST, 24.0);
    if (GMST < 0) GMST += 24.0;
    
    // Wandle Stunden in Radiant um
    return deg2rad(GMST * 15.0); // 360 Grad / 24 Stunden = 15 Grad/Stunde
}


/**
 * V5.12: Berechnet die 3D-Weltposition (ECI)
 * Korrekte Umwandlung von Az/El/Timestamp -> ECI
 */
Vector3 calculateWorldPos_ECI(
    double obs_lat_deg, 
    double obs_lon_deg, 
    double obs_alt_km, 
    time_t timestamp, 
    double azim_deg, 
    double elev_deg, 
    double sat_radius_km) 
{
    // --- Schritt 1: Beobachterposition in ECEF ---
    // (Earth-Centered, Earth-Fixed - Rotiert mit der Erde)
    double obs_lat_rad = deg2rad(obs_lat_deg);
    double obs_lon_rad = deg2rad(obs_lon_deg);
    
    // Position des Beobachters auf dem Ellipsoid (WGS84-Annäherung)
    // (Für unsere Zwecke reicht eine Kugel)
    double obs_radius_km = EARTH_RADIUS_KM + obs_alt_km;
    Vector3 obs_ecef = getPositionFromLatLon_ECEF(obs_lat_deg, obs_lon_deg, obs_radius_km);

    // --- Schritt 2: Az/El in ENU (East-North-Up) Vektor umwandeln ---
    // (Lokales Horizont-System des Beobachters)
    double az_rad = deg2rad(azim_deg);
    double el_rad = deg2rad(elev_deg);

    Vector3 v_enu;
    v_enu.x = cos(el_rad) * sin(az_rad); // East
    v_enu.y = cos(el_rad) * cos(az_rad); // North
    v_enu.z = sin(el_rad);               // Up

    // --- Schritt 3: ENU-Vektor in ECEF-Vektor umwandeln ---
    // Wir rotieren den ENU-Vektor, um ihn an der ECEF-Position des Beobachters auszurichten
    double sinLat = sin(obs_lat_rad);
    double cosLat = cos(obs_lat_rad);
    double sinLon = sin(obs_lon_rad);
    double cosLon = cos(obs_lon_rad);

    Vector3 v_ecef;
    v_ecef.x = -sinLon * v_enu.x + -sinLat * cosLon * v_enu.y + cosLat * cosLon * v_enu.z;
    v_ecef.y =  cosLon * v_enu.x + -sinLat * sinLon * v_enu.y + cosLat * sinLon * v_enu.z;
    v_ecef.z =  0      * v_enu.x +  cosLat          * v_enu.y + sinLat          * v_enu.z;

    // --- Schritt 4: ECEF-Sichtlinie zur Satellitenposition (Ray-Sphere Intersection) ---
    // (Dieser Vektor v_ecef ist die *Richtung* vom Beobachter zum Satelliten)
    Vector3 sightLineDirection = v_normalize(v_ecef);
    
    double a = 1.0;
    double b = 2.0 * v_dot(obs_ecef, sightLineDirection);
    double c = v_mag_sq(obs_ecef) - (sat_radius_km * sat_radius_km);
    double discriminant = (b * b) - (4.0 * a * c);

    Vector3 sat_pos_ecef;
    if (discriminant >= 0) {
        double t = (-b + sqrt(discriminant)) / (2.0 * a);
        sat_pos_ecef = v_add(obs_ecef, v_scale(sightLineDirection, t));
    } else {
        // Sollte nicht passieren, wenn elev > 0
        sat_pos_ecef = v_add(obs_ecef, v_scale(sightLineDirection, sat_radius_km - obs_radius_km));
    }

    // --- Schritt 5: ECEF-Position in ECI (Inertial) umwandeln ---
    // (ECI - Rotiert *nicht* mit der Erde. Hier liegt die Bahn fest.)
    // Wir drehen die ECEF-Position um die Z-Achse (Polachse) zurück,
    // basierend auf der Erdrotation zum gegebenen Zeitpunkt.
    
    double earth_rotation_angle = getEarthRotationAngle(timestamp);
    
    // Rotiere *entgegen* der Erdrotation (negativer Winkel)
    Vector3 sat_pos_eci = v_apply_z_rotation(sat_pos_ecef, -earth_rotation_angle);

    return sat_pos_eci;
}


// Array-Helfer
void init_point_array(PointArray *a, size_t initialCapacity) {
    a->points = malloc(initialCapacity * sizeof(DataPoint));
    if (a->points == NULL) { fprintf(stderr, "Fehler: malloc für PointArray fehlgeschlagen.\n"); exit(1); }
    a->size = 0;
    a->capacity = initialCapacity;
}
void add_point(PointArray *a, DataPoint point) {
    if (a->size == a->capacity) {
        a->capacity *= 2;
        a->points = realloc(a->points, a->capacity * sizeof(DataPoint));
        if (a->points == NULL) { fprintf(stderr, "Fehler: realloc für PointArray fehlgeschlagen.\n"); exit(1); }
    }
    a->points[a->size++] = point;
}
void free_point_array(PointArray *a) {
    if(a->points) free(a->points);
    a->points = NULL;
    a->size = a->capacity = 0;
}
void init_orbit_array(OrbitArray *a, size_t initialCapacity) {
    a->orbits = malloc(initialCapacity * sizeof(OrbitData));
    if (a->orbits == NULL) { fprintf(stderr, "Fehler: malloc für OrbitArray fehlgeschlagen.\n"); exit(1); }
    a->size = 0;
    a->capacity = initialCapacity;
}
void add_orbit(OrbitArray *a, OrbitData orbit) {
    if (a->size == a->capacity) {
        a->capacity *= 2;
        a->orbits = realloc(a->orbits, a->capacity * sizeof(OrbitData)); 
        if (a->orbits == NULL) { fprintf(stderr, "Fehler: realloc für OrbitArray fehlgeschlagen.\n"); exit(1); }
    }
    a->orbits[a->size++] = orbit;
}
void free_orbit_array(OrbitArray *a) {
    free(a->orbits);
    a->orbits = NULL;
    a->size = 0;
    a->capacity = 0;
}
int compare_datapoints_by_time(const void *a, const void *b) {
    DataPoint *pa = (DataPoint*)a;
    DataPoint *pb = (DataPoint*)b;
    double diff = difftime(pa->timestamp, pb->timestamp);
    if (diff < 0) return -1;
    if (diff > 0) return 1;
    return 0;
}

/**
 * V5.11: 3-PUNKT-LOGIK
 * (Diese Funktion ist jetzt korrekt, da 'world_pos' im ECI-Frame ist)
 */
void process_satellite_passes(OrbitArray *final_orbits, PointArray *points, const char* sat_id) {
    if (points->size < MIN_PASS_LENGTH) return;
    
    // 1. Finde längsten Pass
    size_t longest_pass_len = 0;
    size_t longest_pass_start_idx = 0;
    size_t current_pass_len = 0;
    size_t current_pass_start_idx = 0;

    for (size_t i = 0; i < points->size - 1; i++) {
        // Wir verwenden nur Punkte über dem Horizont
        if (points->points[i].elev <= 0) {
            if (current_pass_len > longest_pass_len) {
                longest_pass_len = current_pass_len;
                longest_pass_start_idx = current_pass_start_idx;
            }
            current_pass_len = 0;
            continue;
        }

        if (current_pass_len == 0) {
            current_pass_start_idx = i;
        }
        current_pass_len++;
        
        double time_diff = difftime(points->points[i+1].timestamp, points->points[i].timestamp);
        if (time_diff > MAX_PASS_GAP_SECONDS) {
            if (current_pass_len > longest_pass_len) {
                longest_pass_len = current_pass_len;
                longest_pass_start_idx = current_pass_start_idx;
            }
            current_pass_len = 0;
        }
    }
    // Letzten Pass prüfen
    current_pass_len++; // (der letzte Punkt)
    if (current_pass_len > longest_pass_len) {
        longest_pass_len = current_pass_len;
        longest_pass_start_idx = current_pass_start_idx;
    }
    if (longest_pass_len < MIN_PASS_LENGTH) return;

    // 2. Hole die 3 Punkte für die Ebenenberechnung
    Vector3 p1 = points->points[longest_pass_start_idx].world_pos;
    Vector3 p_mid = points->points[longest_pass_start_idx + (longest_pass_len / 2)].world_pos;
    Vector3 p_end = points->points[longest_pass_start_idx + longest_pass_len - 1].world_pos;
    
    // Skaliere sie auf visuelle Einheiten
    double echte_hoehe_km = get_nominal_altitude_km(sat_id);
    double visual_orbit_radius = VISUAL_EARTH_RADIUS * ((EARTH_RADIUS_KM + echte_hoehe_km) / EARTH_RADIUS_KM);
    
    // (Stelle sicher, dass die Punkte auf der visuellen Kugel liegen,
    // falls die ECI-Berechnung leicht abweicht)
    p1 = v_scale(v_normalize(p1), visual_orbit_radius);
    p_mid = v_scale(v_normalize(p_mid), visual_orbit_radius);
    p_end = v_scale(v_normalize(p_end), visual_orbit_radius);


    // 3. Erzeuge zwei Vektoren, die *auf* der Ebene liegen
    Vector3 vec_A = v_sub(p_mid, p1);
    Vector3 vec_B = v_sub(p_end, p1);

    // 4. Berechne den Normalenvektor (die Neigung) als Kreuzprodukt
    Vector3 normal = v_cross(vec_A, vec_B);
    double mag_normal_sq = v_mag_sq(normal);
    
    if (mag_normal_sq < 0.0001) {
        printf("WARNUNG: Instabile Bahn für %s, wird übersprungen.\n", sat_id);
        return; 
    }
    
    normal = v_normalize(normal);

    // 5. Berechne den Startpunkt (aufsteigender Knoten / Äquator)
    // (ECI-Koordinaten: Z ist die Polachse, X/Y ist die Äquatorebene)
    Vector3 node_direction;
    node_direction.x = -normal.y;
    node_direction.y = normal.x;
    node_direction.z = 0; // Per Definition auf der Äquatorebene

    double mag_node_dir_sq = v_mag_sq(node_direction);
    if (mag_node_dir_sq < 0.0001) {
        // Fallback: Wenn Bahn fast flach ist (normal=(0,0,1)),
        // nutze einfach (1,0,0) als Startrichtung.
        node_direction.x = 1.0; node_direction.y = 0.0; node_direction.z = 0.0;
    } else {
        node_direction = v_normalize(node_direction);
    }
    
    node_direction = v_scale(node_direction, visual_orbit_radius);

    // Prüfe, ob wir den aufsteigenden oder absteigenden Knoten haben
    Vector3 v_test = node_direction;
    double angle_rad_test = 0.017; // Kleiner Winkel zum "anstupsen"
    double cos_a = cos(angle_rad_test);
    double sin_a = sin(angle_rad_test);
    Vector3 k_cross_v = v_cross(normal, v_test);
    v_test = v_add(v_scale(v_test, cos_a), v_scale(k_cross_v, sin_a));

    Vector3 startPoint = node_direction;
    if (v_test.z < 0) { // Wenn der nächste Punkt nach Süden geht (neg. Z)
        startPoint = v_scale(node_direction, -1.0); // Nimm den gegenüberliegenden (aufsteigenden)
    }

    // 6. Orbit-Struktur füllen
    OrbitData orbit;
    strncpy(orbit.sat_id, sat_id, 15);
    orbit.sat_id[15] = '\0';
    orbit.normal = normal;
    orbit.point_on_orbit = startPoint;
    orbit.angular_velocity = 0.0; // JS berechnet dies     
    orbit.start_time = points->points[longest_pass_start_idx].timestamp; 
    orbit.color_hex = getSatColor(sat_id);
    orbit.radius = visual_orbit_radius;
    
    add_orbit(final_orbits, orbit);
}


// ========== HAUPTFUNKTION (main) ==========
int main() {
    FILE *f_csv = fopen("satellite_tracks.csv", "r");
    if (f_csv == NULL) {
        fprintf(stderr, "Fehler: 'satellite_tracks.csv' nicht gefunden.\n");
        exit(1);
    }
    
    printf("PASS 1: Lese 'satellite_tracks.csv' in den Speicher...\n");
    
    // V5.12: Beobachter-Position (Kugel-Annäherung)
    // (Wir brauchen hier nicht die volle ECEF-Position, da calculateWorldPos
    // die Lat/Lon direkt verwendet)
    double observer_altitude_km = 0.3; // Annahme für Saalfeld (ca. 300m)

    static SatelliteData sat_aggregator[MAX_UNIQUE_SATELLITES]; 
    int total_unique_sats = 0;
    memset(sat_aggregator, 0, sizeof(sat_aggregator));

    char line[MAX_LINE_LENGTH];
    long line_count = 0;

    while (fgets(line, sizeof(line), f_csv)) {
        line_count++;
        if (line_count % 500000 == 0) {
            printf("... %ld Zeilen gelesen\n", line_count);
        }

        char sat_id_buffer[16];
        char time_buffer[32];
        char azim_str[16];
        char elev_str[16];

        memset(sat_id_buffer, 0, sizeof(sat_id_buffer));
        memset(time_buffer, 0, sizeof(time_buffer));
        memset(azim_str, 0, sizeof(azim_str));
        memset(elev_str, 0, sizeof(elev_str));

        int items = sscanf(line, 
            "\"%*[^\"]\","    // 1. "id" (verwerfen)
            "\"%31[^\"]\","   // 2. "timestamp" (speichern)
            "\"%15[^\"]\","   // 3. "sat_id" (speichern)
            "\"%15[^\"]\","   // 4. "azim" (speichern)
            "\"%15[^\"]\","   // 5. "elev" (speichern)
            "\"%*[^\"]\","    // 6. "snr" (verwerfen)
            "\"%*[^\"]\","    // 7. "lat" (verwerfen)
            "\"%*[^\"]\"",    // 8. "lon" (verwerfen)
            time_buffer, sat_id_buffer, azim_str, elev_str);
        
        if (items < 4) continue; 

        int sat_index = -1;
        for (int i = 0; i < total_unique_sats; i++) {
            if (strcmp(sat_aggregator[i].sat_id, sat_id_buffer) == 0) {
                sat_index = i;
                break;
            }
        }

        if (sat_index == -1) {
            if (total_unique_sats >= MAX_UNIQUE_SATELLITES) {
                if(total_unique_sats == MAX_UNIQUE_SATELLITES) {
                     fprintf(stderr, "Warnung: MAX_UNIQUE_SATELLITES (%d) erreicht. Ignoriere neue Satelliten.\n", MAX_UNIQUE_SATELLITES);
                     total_unique_sats++; 
                }
                continue; 
            }
            sat_index = total_unique_sats;
            snprintf(sat_aggregator[sat_index].sat_id, 16, "%s", sat_id_buffer);
            init_point_array(&sat_aggregator[sat_index].points, 10000); 
            total_unique_sats++;
        }
         if (sat_index >= MAX_UNIQUE_SATELLITES) continue;

        DataPoint p;
        p.azim = atof(azim_str);
        p.elev = atof(elev_str);
        p.timestamp = parse_csv_time(time_buffer);
        
        // V5.12 FIX: Berechne die 3D-Position des Satelliten im ECI-Frame
        double alt_km = get_nominal_altitude_km(sat_id_buffer);
        p.world_pos = calculateWorldPos_ECI(
            OBSERVER_LAT, 
            OBSERVER_LON, 
            observer_altitude_km, 
            p.timestamp, 
            p.azim, 
            p.elev, 
            EARTH_RADIUS_KM + alt_km
        );
        
        add_point(&sat_aggregator[sat_index].points, p);
    }

    fclose(f_csv);
    printf("...Lesen abgeschlossen. %d einzigartige Satelliten gefunden.\n", total_unique_sats);

    // ========== PASS 2: Verarbeitung ==========
    printf("PASS 2: Verarbeite gesammelte Daten...\n");

    OrbitArray final_orbits;
    init_orbit_array(&final_orbits, total_unique_sats);

    for (int i = 0; i < total_unique_sats; i++) {
        printf("...sortiere %s (%lu Punkte)...\n", sat_aggregator[i].sat_id, (unsigned long)sat_aggregator[i].points.size);
        if (sat_aggregator[i].points.size > 0) {
             qsort(sat_aggregator[i].points.points, sat_aggregator[i].points.size, sizeof(DataPoint), compare_datapoints_by_time);
        }
       
        printf("...verarbeite %s (%lu Punkte)\n", sat_aggregator[i].sat_id, (unsigned long)sat_aggregator[i].points.size);
        process_satellite_passes(&final_orbits, &sat_aggregator[i].points, sat_aggregator[i].sat_id);
    }

    printf("...Verarbeitung abgeschlossen. %lu Orbits berechnet.\n", (unsigned long)final_orbits.size);


    // 9. Ergebnisse in orbits.json schreiben
    FILE *f_json = fopen("orbits.json", "w");
    if (f_json == NULL) {
        fprintf(stderr, "Fehler beim Öffnen von orbits.json zum Schreiben!\n");
        exit(1);
    }

    fprintf(f_json, "{\n");
    
    // available_sats
    fprintf(f_json, "  \"available_sats\": [\n    ");
    for(int i = 0; i < total_unique_sats; i++) {
        fprintf(f_json, "\"%s\"%s\n    ", sat_aggregator[i].sat_id, (i == total_unique_sats - 1) ? "" : ",");
    }
    fprintf(f_json, "],\n");

    // orbits
    fprintf(f_json, "  \"orbits\": [\n");
    for (size_t i = 0; i < final_orbits.size; i++) {
        OrbitData o = final_orbits.orbits[i];
        fprintf(f_json, "    {\n");
        fprintf(f_json, "      \"sat_id\": \"%s\",\n", o.sat_id);
        fprintf(f_json, "      \"radius\": %f,\n", o.radius);
        fprintf(f_json, "      \"color\": %d,\n", o.color_hex);
        // V5.12 ECI-Koordinaten: Z ist die Polachse
        fprintf(f_json, "      \"startPoint\": {\"x\": %f, \"y\": %f, \"z\": %f},\n", o.point_on_orbit.x, o.point_on_orbit.y, o.point_on_orbit.z);
        fprintf(f_json, "      \"normal\": {\"x\": %f, \"y\": %f, \"z\": %f},\n", o.normal.x, o.normal.y, o.normal.z);
        fprintf(f_json, "      \"angular_velocity\": %f,\n", o.angular_velocity);
        fprintf(f_json, "      \"start_time\": %ld\n", (long)o.start_time);
        fprintf(f_json, "    }%s\n", (i == final_orbits.size - 1) ? "" : ",");
    }
    fprintf(f_json, "  ]\n");
    fprintf(f_json, "}\n");

    fclose(f_json);

    // Speicher freigeben
    for (int i = 0; i < total_unique_sats; i++) {
        free_point_array(&sat_aggregator[i].points);
    }
    free_orbit_array(&final_orbits);

    printf("orbits.json erfolgreich geschrieben!\n");
    return 0;
}