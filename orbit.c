/**
 * orbit_processor.c (V5.8 - Aggregator-Version)
 *
 * V5.8 FIX: Behebt den Linker-Fehler "undefined reference to 'parse_csv_time'"
 * indem die fehlende Funktion hinzugefügt wird.
 * Behebt auch die -Wunused-but-set-variable Warnung.
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
    Vector3 world_pos; 
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

// V5.8 FIX: Fehlende Funktion hinzugefügt
time_t parse_csv_time(const char* time_str) {
    struct tm tm = {0};
    // Liest das Format "YYYY-MM-DD HH:MM:SS"
    sscanf(time_str, "%d-%d-%d %d:%d:%d", 
           &tm.tm_year, &tm.tm_mon, &tm.tm_mday, 
           &tm.tm_hour, &tm.tm_min, &tm.tm_sec);
    tm.tm_year -= 1900; // tm_year ist Jahre seit 1900
    tm.tm_mon -= 1;     // tm_mon ist 0-11
    tm.tm_isdst = -1;   // Automatische Sommerzeit-Erkennung
    return mktime(&tm);
}

// Konvertiert Lat/Lon in 3D-Weltkoordinaten
Vector3 getPositionFromLatLon(double lat, double lon, double radius) {
    double latRad = deg2rad(lat);
    double lonRad = deg2rad(-lon);
    Vector3 v;
    v.x = cos(latRad) * cos(lonRad) * radius;
    v.y = sin(latRad) * radius;
    v.z = cos(latRad) * sin(lonRad) * radius;
    return v;
}

// Vektor-Helfer
Vector3 v_add(Vector3 v1, Vector3 v2) {
    v1.x += v2.x; v1.y += v2.y; v1.z += v2.z;
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
    if (mag == 0.0) return v;
    return v_scale(v, 1.0 / mag);
}
Vector3 v_cross(Vector3 v1, Vector3 v2) {
    Vector3 normal;
    normal.x = v1.y * v2.z - v1.z * v2.y;
    normal.y = v1.z * v2.x - v1.x * v2.z;
    normal.z = v1.x * v2.y - v1.y * v2.x;
    return normal;
}
// Quaternion-Rotation
typedef struct { double x, y, z, w; } Quaternion;
Quaternion q_setFromUnitVectors(Vector3 vFrom, Vector3 vTo) {
    double r = v_dot(vFrom, vTo) + 1.0;
    Quaternion q;
    if (r < 0.000001) { 
        q.x = 0.0; q.y = 1.0; q.z = 0.0; q.w = 0.0;
        if (fabs(vFrom.x) > fabs(vFrom.z)) {
            q.x = -vFrom.y; q.y = vFrom.x; q.z = 0.0;
        } else {
            q.x = 0.0; q.y = -vFrom.z; q.z = vFrom.y;
        }
    } else {
        Vector3 v = v_cross(vFrom, vTo);
        q.x = v.x; q.y = v.y; q.z = v.z; q.w = r;
    }
    double mag = sqrt(q.x*q.x + q.y*q.y + q.z*q.z + q.w*q.w);
    q.x /= mag; q.y /= mag; q.z /= mag; q.w /= mag;
    return q;
}
Vector3 v_applyQuaternion(Vector3 v, Quaternion q) {
    double ix = q.w * v.x + q.y * v.z - q.z * v.y;
    double iy = q.w * v.y + q.z * v.x - q.x * v.z;
    double iz = q.w * v.z + q.x * v.y - q.y * v.x;
    double iw = -q.x * v.x - q.y * v.y - q.z * v.z;
    Vector3 v_out;
    v_out.x = ix * q.w + iw * -q.x + iy * -q.z - iz * -q.y;
    v_out.y = iy * q.w + iw * -q.y + iz * -q.x - ix * -q.z;
    v_out.z = iz * q.w + iw * -q.z + ix * -q.y - iy * -q.x;
    return v_out;
}

Vector3 calculateWorldPos(Vector3 userWorldPos, double azim, double elev, double orbit_radius) {
    Vector3 userUpVector = v_normalize(userWorldPos);
    Vector3 yUp = {0.0, 1.0, 0.0};
    Quaternion rotationQuaternion = q_setFromUnitVectors(yUp, userUpVector);

    double azimRad = deg2rad(azim - 90.0);
    double elevRad = deg2rad(elev);
    Vector3 sightLineDirection;
    sightLineDirection.x = cos(elevRad) * cos(azimRad);
    sightLineDirection.y = sin(elevRad);
    sightLineDirection.z = cos(elevRad) * sin(azimRad);

    sightLineDirection = v_applyQuaternion(sightLineDirection, rotationQuaternion);
    sightLineDirection = v_normalize(sightLineDirection);

    double a = 1.0;
    double b = 2.0 * v_dot(userWorldPos, sightLineDirection);
    double c = v_mag_sq(userWorldPos) - (orbit_radius * orbit_radius);
    double discriminant = (b * b) - (4.0 * a * c);

    if (discriminant >= 0) {
        double t = (-b + sqrt(discriminant)) / (2.0 * a);
        return v_add(userWorldPos, v_scale(sightLineDirection, t));
    } else {
        return v_scale(sightLineDirection, orbit_radius);
    }
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
    a->size = a->capacity = 0;
}
int compare_datapoints_by_time(const void *a, const void *b) {
    DataPoint *pa = (DataPoint*)a;
    DataPoint *pb = (DataPoint*)b;
    double diff = difftime(pa->timestamp, pb->timestamp);
    if (diff < 0) return -1;
    if (diff > 0) return 1;
    return 0;
}

void process_satellite_passes(OrbitArray *final_orbits, PointArray *points, const char* sat_id) {
    if (points->size < MIN_PASS_LENGTH) return;
    
    size_t longest_pass_len = 0;
    size_t longest_pass_start_idx = 0;
    size_t current_pass_len = 0;
    size_t current_pass_start_idx = 0;

    for (size_t i = 0; i < points->size - 1; i++) {
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
    current_pass_len++;
    if (current_pass_len > longest_pass_len) {
        longest_pass_len = current_pass_len;
        longest_pass_start_idx = current_pass_start_idx;
    }
    if (longest_pass_len < MIN_PASS_LENGTH) return;

    double echte_hoehe_km = get_nominal_altitude_km(sat_id);
    double orbit_radius = VISUAL_EARTH_RADIUS * ((EARTH_RADIUS_KM + echte_hoehe_km) / EARTH_RADIUS_KM);

    Vector3 v1 = points->points[longest_pass_start_idx].world_pos;
    Vector3 v_far = v1;
    double max_dist_sq = 0;
    // V5.8 FIX: Unbenutzte Variable 'p_far_idx' entfernt
    // size_t p_far_idx = longest_pass_start_idx; 

    for (size_t i = 1; i < longest_pass_len; i++) {
        Vector3 v_current = points->points[longest_pass_start_idx + i].world_pos;
        
        double dist_sq = pow(v_current.x - v1.x, 2) + 
                         pow(v_current.y - v1.y, 2) + 
                         pow(v_current.z - v1.z, 2);
                         
        if (dist_sq > max_dist_sq) {
            max_dist_sq = dist_sq;
            v_far = v_current;
            // p_far_idx = longest_pass_start_idx + i; // War unbenutzt
        }
    }

    if (max_dist_sq < 0.1) return;
    
    Vector3 normal = v_cross(v1, v_far);
    double mag_normal_sq = v_mag_sq(normal);
    if (mag_normal_sq < 0.0001) return; 
    
    normal = v_normalize(normal);

    Vector3 node_direction;
    node_direction.x = -normal.z;
    node_direction.y = 0;
    node_direction.z = normal.x;

    double mag_node_dir_sq = v_mag_sq(node_direction);
    if (mag_node_dir_sq < 0.0001) {
        node_direction = v1; 
    } else {
        node_direction = v_scale(v_normalize(node_direction), orbit_radius);
    }

    Vector3 v_test = node_direction;
    double angle_rad_test = 0.017;
    double cos_a = cos(angle_rad_test);
    double sin_a = sin(angle_rad_test);
    Vector3 k_cross_v = v_cross(normal, v_test);
    v_test = v_add(v_scale(v_test, cos_a), v_scale(k_cross_v, sin_a));

    Vector3 startPoint = node_direction;
    if (v_test.y < 0) {
        startPoint = v_scale(node_direction, -1.0);
    }

    OrbitData orbit;
    strncpy(orbit.sat_id, sat_id, 15);
    orbit.sat_id[15] = '\0';
    orbit.normal = normal;
    orbit.point_on_orbit = startPoint;
    orbit.angular_velocity = 0.0;     
    orbit.start_time = points->points[longest_pass_start_idx].timestamp; 
    orbit.color_hex = getSatColor(sat_id);
    orbit.radius = orbit_radius;
    
    add_orbit(final_orbits, orbit);
}

int main() {
    FILE *f_csv = fopen("satellite_tracks.csv", "r");
    if (f_csv == NULL) {
        fprintf(stderr, "Fehler: 'satellite_tracks.csv' nicht gefunden.\n");
        exit(1);
    }
    
    printf("PASS 1: Lese 'satellite_tracks.csv' in den Speicher...\n");
    
    Vector3 observer_world_pos = getPositionFromLatLon(OBSERVER_LAT, OBSERVER_LON, VISUAL_EARTH_RADIUS);

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
        p.timestamp = parse_csv_time(time_buffer); // V5.8 FIX: Diese Funktion existiert jetzt
        
        double alt_km = get_nominal_altitude_km(sat_id_buffer);
        double r = VISUAL_EARTH_RADIUS * ((EARTH_RADIUS_KM + alt_km) / EARTH_RADIUS_KM);
        p.world_pos = calculateWorldPos(observer_world_pos, p.azim, p.elev, r);
        
        add_point(&sat_aggregator[sat_index].points, p);
    }

    fclose(f_csv);
    printf("...Lesen abgeschlossen. %d einzigartige Satelliten gefunden.\n", total_unique_sats);

    // ========== PASS 2: Verarbeitung ==========
    printf("PASS 2: Verarbeite gesammelte Daten...\n");

    OrbitArray final_orbits;
    init_orbit_array(&final_orbits, total_unique_sats);

    for (int i = 0; i < total_unique_sats; i++) {
        // V5.8 FIX: %zu -> %lu (unsigned long) für bessere Windows-Kompatibilität
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