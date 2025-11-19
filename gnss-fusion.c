/**
 * update_orbits.c (V2 - Multi-GNSS)
 *
 * Liest 'orbits.json' (deine Berechnung) und 'gnss.json' (Offizielle TLE-Daten).
 * Matcht Satelliten (Gxx -> PRN xx, Exx -> GALILEO xx, Cxx -> PRN Cxx)
 * und aktualisiert den 'radius' in orbits.json mit der exakten Physik.
 *
 * Kompilieren: gcc -o update_orbits update_orbits.c -lm
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <ctype.h>

#define MU 398600.4418 // Gravitationskonstante Erde (km^3/s^2)
#define EARTH_RADIUS 6371.0
#define VISUAL_SCALE 5.0 

// Hilfsfunktion: Datei in Speicher lesen
char* read_file(const char* filename) {
    FILE *f = fopen(filename, "r");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long length = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *buffer = malloc(length + 1);
    if (buffer) {
        fread(buffer, 1, length, f);
        buffer[length] = '\0';
    }
    fclose(f);
    return buffer;
}

// Findet einen numerischen Wert im JSON (z.B. "MEAN_MOTION": 2.005...)
double find_json_value(const char* json_obj, const char* key) {
    char search[64];
    sprintf(search, "\"%s\":", key);
    char *pos = strstr(json_obj, search);
    if (!pos) return -1.0;
    
    pos += strlen(search);
    while (*pos == ' ' || *pos == '"' || *pos == ':') pos++;
    return atof(pos);
}

/**
 * Versucht, einen Satelliten in der gnss.json zu finden.
 * Strategie: Wir suchen nach spezifischen Strings im "OBJECT_NAME".
 */
char* find_tle_entry(const char* tle_json, const char* sat_id) {
    char search_pattern[64] = {0};
    int id_num = 0;
    
    // Extrahiere Nummer (z.B. "G13" -> 13)
    // Wir überspringen den ersten Buchstaben und lesen die Zahl
    if (isdigit(sat_id[1])) {
        id_num = atoi(sat_id + 1);
    } else {
        return NULL; // Unbekanntes Format
    }

    // Baue Suchmuster basierend auf System
    if (sat_id[0] == 'G') {
        // GPS: "GPS ... (PRN 13)" -> Suche nach "PRN 13)"
        sprintf(search_pattern, "PRN %d)", id_num);
    } 
    else if (sat_id[0] == 'E') {
        // Galileo: "GSAT... (GALILEO 12)" -> Suche nach "GALILEO %d)"
        sprintf(search_pattern, "GALILEO %d)", id_num);
    }
    else if (sat_id[0] == 'C') {
        // BeiDou: "BEIDOU... (PRN C12)" -> Suche nach "PRN C%d)" 
        // (Achtung: BeiDou TLEs haben oft "PRN Cxx")
        sprintf(search_pattern, "PRN C%d)", id_num);
    }
    else if (sat_id[0] == 'R') {
        // GLONASS ist schwer (NMEA ID != Slot/PRN im Namen).
        // NMEA 65-96 sind Slots. TLE Namen haben oft nur COSMOS Nummern.
        // Wir überspringen GLONASS beim exakten Matching vorerst,
        // oder wir nutzen einen generischen GLONASS Radius, wenn wir "GLONASS" finden.
        return NULL; 
    }
    else {
        return NULL;
    }

    // Suche im JSON
    char *pos = strstr(tle_json, search_pattern);
    if (!pos) return NULL;

    // Gehe zurück zum Anfang des Objekts "{"
    while (pos > tle_json && *pos != '{') pos--;
    return pos;
}

int main() {
    printf("Lade Dateien...\n");
    char *orbits_json = read_file("orbits.json");
    char *gnss_json = read_file("gnss.json"); // NEU: gnss.json

    if (!orbits_json || !gnss_json) {
        fprintf(stderr, "Fehler: orbits.json oder gnss.json fehlen.\n");
        return 1;
    }

    FILE *f_out = fopen("orbits_updated.json", "w");
    if (!f_out) return 1;

    char *cursor = orbits_json;
    char *sat_start = strstr(cursor, "\"sat_id\":");
    
    // Kopiere Header
    if (sat_start) {
        char *obj_start = sat_start;
        while (*obj_start != '{') obj_start--;
        fwrite(cursor, 1, obj_start - cursor, f_out);
        cursor = obj_start;
    }

    int updated_count = 0;

    while (sat_start) {
        // 1. Lese ID
        char sat_id[16];
        sscanf(sat_start, "\"sat_id\": \"%15[^\"]\"", sat_id);
        
        // 2. Finde Objekt-Ende
        char *obj_end = strchr(cursor, '}');
        if (!obj_end) break;
        
        // 3. Suche TLE Match in gnss.json
        char *tle_match = find_tle_entry(gnss_json, sat_id);

        double new_radius = -1.0;
        
        if (tle_match) {
            // Radius berechnen (Kepler 3)
            double mean_motion = find_json_value(tle_match, "MEAN_MOTION");
            if (mean_motion > 0) {
                double n_rad = mean_motion * 2 * 3.1415926535 / 86400.0;
                double a_km = cbrt(MU / (n_rad * n_rad));
                
                // Umrechnen auf deine visuelle Skala
                new_radius = VISUAL_SCALE * (a_km / EARTH_RADIUS);
                updated_count++;
                printf("Update %s: %.2f (TLE: %.0f km)\n", sat_id, new_radius, a_km);
            }
        } else if (sat_id[0] == 'R') {
            // Fallback für GLONASS (Fixer Radius ~25500 km semi-major axis)
            // Da Matching schwer ist, nehmen wir den Standardwert für alle 'R'
            double a_km = 25508.0; 
            new_radius = VISUAL_SCALE * (a_km / EARTH_RADIUS);
            // printf("Update %s (GLONASS Default): %.2f\n", sat_id, new_radius);
        }

        // 4. Schreibe Update
        char *radius_pos = strstr(cursor, "\"radius\":");
        if (radius_pos && radius_pos < obj_end && new_radius > 0) {
            fwrite(cursor, 1, radius_pos - cursor, f_out);
            fprintf(f_out, "\"radius\": %f", new_radius);
            
            // Überspringe alten Wert
            char *val_end = radius_pos + 9; 
            while (*val_end != ',' && *val_end != '}' && *val_end != '\n') val_end++;
            cursor = val_end;
        } else {
            fwrite(cursor, 1, (obj_end - cursor) + 1, f_out);
            cursor = obj_end + 1;
        }

        sat_start = strstr(cursor, "\"sat_id\":");
        if (!sat_start) fprintf(f_out, "%s", cursor);
    }

    fclose(f_out);
    free(orbits_json);
    free(gnss_json);
    printf("Fertig! %d Satelliten aktualisiert. Neue Datei: orbits_updated.json\n", updated_count);
    return 0;
}