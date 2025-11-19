/**
 * NMEA Offline Parser (V2.1)
 *
 * Liest eine rohe NMEA-Log-CSV-Datei (nmea_logs.csv) und erstellt eine
 * saubere, gefilterte CSV (parsed_data.csv), die für die 3D-Visualisierung
 * optimiert ist.
 *
 * Implementiert:
 * 1. Positions-Tracking (RMC/GGA)
 * 2. Glitch-Filter (Validierung)
 * 3. Geister-Filter (Unique IDs, z.B. G08 vs E08)
 * 4. Duplikats-Filter (speichert nur Änderungen)
 *
 * Kompilieren:
 * gcc parser.c -o parser -Wall -O2
 *
 * Ausführen:
 * ./parser [input_file.csv] [output_file.csv]
 * z.B.:
 * ./parser nmea_logs.csv parsed_data.csv
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_LINE_LEN 512
#define MAX_SAT_ID_LEN 10
#define SAT_CACHE_SIZE 256 // Max. Satelliten-ID-Nummer

// ----- 1. Hilfsstrukturen -----

/**
 * Speichert den letzten bekannten Zustand eines Satelliten.
 * Wird verwendet, um Duplikate zu filtern.
 */
typedef struct {
    char sat_id[MAX_SAT_ID_LEN]; // z.B. "G08"
    int elevation;
    int azimut;
    int snr;
} SatState;

/**
 * Ein einfacher Cache (Hash-Map-Array) für Satelliten-Zustände.
 * Wir verwenden ein Array für die System-Präfixe ('G', 'R', 'E', 'C')
 * und ein Array für die ID-Nummer (0-255).
 */
SatState* sat_cache[5][SAT_CACHE_SIZE] = {NULL}; // [System][ID]

// ----- 2. Hilfsfunktionen (NMEA-Logik) -----

/**
 * Gibt den Index für das System-Array zurück.
 */
int get_system_index(char prefix) {
    switch (prefix) {
        case 'G': return 0; // GPS
        case 'R': return 1; // GLONASS
        case 'E': return 2; // Galileo
        case 'C': return 3; // BeiDou
        default:  return 4; // Unknown
    }
}

/**
 * Gibt das 1-Buchstaben-Präfix für einen NMEA-Satz zurück.
 */
char get_system_prefix(const char* talker) {
    if (strncmp(talker, "$GP", 3) == 0) return 'G';
    if (strncmp(talker, "$GL", 3) == 0) return 'R';
    if (strncmp(talker, "$GA", 3) == 0) return 'E';
    if (strncmp(talker, "$GB", 3) == 0) return 'C';
    return 'U';
}

/**
 * Wandelt NMEA-Koordinaten (ddmm.mmmm) in Dezimalgrad um.
 */
double parse_nmea_coord(const char* coord_str, char dir) {
    if (coord_str == NULL || *coord_str == '\0') return 0.0;
    double coord = atof(coord_str);
    double deg = floor(coord / 100.0);
    double min = coord - (deg * 100.0);
    double decimal = deg + (min / 60.0);
    if (dir == 'S' || dir == 'W') {
        decimal = -decimal;
    }
    return decimal;
}

/**
 * Findet oder erstellt einen Satelliten-Status im Cache.
 */
SatState* get_or_create_sat_state(char prefix, int id_num) {
    if (id_num < 0 || id_num >= SAT_CACHE_SIZE) return NULL;
    
    int sys_idx = get_system_index(prefix);
    
    if (sat_cache[sys_idx][id_num] == NULL) {
        // Satellit ist neu, erstelle Eintrag
        sat_cache[sys_idx][id_num] = (SatState*)malloc(sizeof(SatState));
        if (sat_cache[sys_idx][id_num] == NULL) {
            fprintf(stderr, "Speicherfehler!\n");
            exit(1);
        }
        // Initialisiere mit unmöglichen Werten
        snprintf(sat_cache[sys_idx][id_num]->sat_id, MAX_SAT_ID_LEN, "%c%02d", prefix, id_num);
        sat_cache[sys_idx][id_num]->elevation = -1;
        sat_cache[sys_idx][id_num]->azimut = -1;
        sat_cache[sys_idx][id_num]->snr = -1;
    }
    return sat_cache[sys_idx][id_num];
}

/**
 * Gibt den gesamten Speicher des Caches frei.
 */
void free_sat_cache() {
    for (int i = 0; i < 5; i++) {
        for (int j = 0; j < SAT_CACHE_SIZE; j++) {
            if (sat_cache[i][j] != NULL) {
                free(sat_cache[i][j]);
            }
        }
    }
}

// ----- 3. Haupt-Parsing-Funktion -----

void process_line(char* line, FILE* outfile, double* current_lat, double* current_lon) {
    char timestamp[32];
    char sentence[MAX_LINE_LEN];
    char* token;

    // 1. CSV-Zeile parsen (Format: "id","timestamp","sentence")
    // Wir müssen die Anführungszeichen manuell entfernen.
    
    // Finde ID (überspringen)
    token = strtok(line, ",");
    if (token == NULL) return; 
    
    // Finde Timestamp
    token = strtok(NULL, ",");
    if (token == NULL) return;
    strncpy(timestamp, token + 1, 19); // +1 um " zu überspringen, 19 = Länge von YYYY-MM-DD HH:MM:SS
    timestamp[19] = '\0';
    
    // Finde Sentence
    token = strtok(NULL, "\""); // Finde das Zitat-Zeichen
    if (token == NULL) return;
    strncpy(sentence, token, MAX_LINE_LEN - 1);
    sentence[MAX_LINE_LEN - 1] = '\0';

    // Checksumme entfernen
    char* checksum_ptr = strrchr(sentence, '*');
    if (checksum_ptr) {
        *checksum_ptr = '\0';
    }

    // 2. NMEA-Satz-Typ bestimmen
    char* parts[128];
    int part_count = 0;
    char* nmea_token = strtok(sentence, ",");
    while (nmea_token != NULL && part_count < 128) {
        parts[part_count++] = nmea_token;
        nmea_token = strtok(NULL, ",");
    }

    if (part_count == 0) return; // Leerer Satz

    char* talker = parts[0];
    char type[4];
    if (strlen(talker) >= 6) {
        strncpy(type, talker + 3, 3);
        type[3] = '\0';
    } else {
        return; // Zu kurz
    }
    
    // 3. Satz verarbeiten
    
    // --- A: Positions-Update (RMC/GGA) ---
    if (strcmp(type, "RMC") == 0 && part_count > 6 && strcmp(parts[2], "A") == 0) {
        // $GPRMC,time,A,lat,N,lon,E,...
        *current_lat = parse_nmea_coord(parts[3], *parts[4]);
        *current_lon = parse_nmea_coord(parts[5], *parts[6]);
    } 
    else if (strcmp(type, "GGA") == 0 && part_count > 6 && atoi(parts[6]) > 0) {
        // $GPGGA,time,lat,N,lon,E,fix,...
        *current_lat = parse_nmea_coord(parts[2], *parts[3]);
        *current_lon = parse_nmea_coord(parts[4], *parts[5]);
    }
    
    // --- B: Satelliten-Update (GSV) ---
    else if (strcmp(type, "GSV") == 0) {
        char prefix = get_system_prefix(talker);
        
        // Loop durch die 4er-Blöcke von Satelliten
        for (int i = 4; i < part_count; i += 4) {
            if (i + 2 >= part_count) break; // Block ist unvollständig
            
            if (parts[i][0] == '\0' || parts[i+1][0] == '\0' || parts[i+2][0] == '\0') {
                continue; // Leere ID, Elev oder Azim
            }

            int id_num = atoi(parts[i]);
            int elev = atoi(parts[i+1]);
            int azim = atoi(parts[i+2]);
            int snr = (i + 3 < part_count && parts[i+3][0] != '\0') ? atoi(parts[i+3]) : 0;

            // 4. Filter anwenden
            
            // Filter 1: Glitches (Physikalische Unmöglichkeiten)
            if (elev < 0 || elev > 90 || azim < 0 || azim > 360) {
                // Glitch gefunden, ignoriere diesen Satelliten
                continue;
            }

            // Filter 2: Geister (Hole/Erstelle Unique ID)
            SatState* sat = get_or_create_sat_state(prefix, id_num);
            if (sat == NULL) continue; // ID-Nummer außerhalb des Cache-Bereichs

            // Filter 3: Duplikate (Vergleiche mit Cache)
            if (sat->elevation != elev || sat->azimut != azim || sat->snr != snr) {
                
                // Es ist eine NEUE Position, speichere sie
                sat->elevation = elev;
                sat->azimut = azim;
                sat->snr = snr;
                
                // 5. In neue CSV-Datei schreiben
                // Format: timestamp,sat_id,elevation,azimut,snr,lat,lon
                fprintf(outfile, "\"%s\",\"%s\",%d,%d,%d,%.8f,%.8f\n",
                    timestamp,
                    sat->sat_id,
                    sat->elevation,
                    sat->azimut,
                    sat->snr,
                    *current_lat,
                    *current_lon
                );
            }
        }
    }
}

// ----- 4. Main-Funktion -----

int main(int argc, char *argv[]) {
    if (argc != 3) {
        fprintf(stderr, "Benutzung: %s dbs14895752.csv satelliten_daten.csv\n", argv[0]);
        return 1;
    }

    FILE *infile = fopen(argv[1], "r");
    if (!infile) {
        perror("Fehler beim Öffnen der Eingabe-Datei");
        return 1;
    }

    FILE *outfile = fopen(argv[2], "w");
    if (!outfile) {
        perror("Fehler beim Erstellen der Ausgabe-Datei");
        fclose(infile);
        return 1;
    }

    // Header für die neue, saubere CSV-Datei schreiben
    fprintf(outfile, "timestamp,sat_id,elevation,azimut,snr,lat,lon\n");

    char line_buffer[MAX_LINE_LEN];
    double current_lat = 50.65; // Fallback Saalfeld
    double current_lon = 11.36;
    long line_count = 0;

    printf("Starte Offline-Verarbeitung...\n");

    // Erste Zeile (Header) der Eingabe-Datei überspringen
    if (fgets(line_buffer, MAX_LINE_LEN, infile) == NULL) {
        fprintf(stderr, "Eingabe-Datei ist leer.\n");
    }

    // Alle Zeilen verarbeiten
    while (fgets(line_buffer, MAX_LINE_LEN, infile)) {
        // fgets behält das \n, wir brauchen einen beschreibbaren Puffer
        char current_line[MAX_LINE_LEN];
        strncpy(current_line, line_buffer, MAX_LINE_LEN);
        
        process_line(current_line, outfile, &current_lat, &current_lon);
        line_count++;

        if (line_count % 100000 == 0) {
            printf("... %ld Zeilen verarbeitet.\n", line_count);
        }
    }

    printf("Verarbeitung abgeschlossen. %ld Zeilen gelesen.\n", line_count);

    // Aufräumen
    fclose(infile);
    fclose(outfile);
    free_sat_cache();

    printf("Saubere Daten gespeichert in: %s\n", argv[2]);
    return 0;
}