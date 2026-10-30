
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <limits.h>
#include <stdint.h>
#include <sys/stat.h>
#include <time.h>
#ifdef _WIN32
#include <direct.h>
#define MB_MKDIR(path) _mkdir(path)
#else
#include <unistd.h>
#define MB_MKDIR(path) mkdir(path, 0755)
#endif
#include <libpq-fe.h>
#include <microhttpd.h>

#define PORT 8080
#define CURRENT_USER_ID "U001"
#define AUDIO_UPLOAD_DIR "C:/Users/Mizpah Annie/MelodyBox/audio"

#define DB_CONNINFO "host=localhost port=5432 dbname=melodybox_db user=postgres"

#define MAX_REQUEST_SIZE 16384

/* =========================================================
   REQUEST DATA
   ========================================================= */


struct RequestData
{
    char *body;
    size_t size;

    struct MHD_PostProcessor *post_processor;
    FILE *upload_file;
    char upload_path[512];
    int upload_started;
    int upload_finished;
    int upload_error;
};


/* =========================================================
   JSON HELPERS
   ========================================================= */

/*
   Minimal JSON string parser.

   Handles:
   - JSON strings
   - escaped quotes
   - escaped backslashes
   - common escaped characters
   - null values

   This parser is intended for the flat JSON object sent
   by the MelodyBox Add Song form.
*/

static void skip_spaces(const char **p)
{
    while (**p && isspace((unsigned char)**p))
        (*p)++;
}

static int hex_value(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static int append_utf8(char *out, size_t *len, unsigned int cp)
{
    if (cp <= 0x7F)
    {
        out[(*len)++] = (char)cp;
    }
    else if (cp <= 0x7FF)
    {
        out[(*len)++] = (char)(0xC0 | (cp >> 6));
        out[(*len)++] = (char)(0x80 | (cp & 0x3F));
    }
    else if (cp <= 0xFFFF)
    {
        if (cp >= 0xD800 && cp <= 0xDFFF)
            return 0;

        out[(*len)++] = (char)(0xE0 | (cp >> 12));
        out[(*len)++] = (char)(0x80 | ((cp >> 6) & 0x3F));
        out[(*len)++] = (char)(0x80 | (cp & 0x3F));
    }
    else if (cp <= 0x10FFFF)
    {
        out[(*len)++] = (char)(0xF0 | (cp >> 18));
        out[(*len)++] = (char)(0x80 | ((cp >> 12) & 0x3F));
        out[(*len)++] = (char)(0x80 | ((cp >> 6) & 0x3F));
        out[(*len)++] = (char)(0x80 | (cp & 0x3F));
    }
    else
    {
        return 0;
    }

    return 1;
}

static char *parse_json_string(const char **p)
{
    if (**p != '"')
        return NULL;

    (*p)++;

    size_t capacity = strlen(*p) + 1;
    char *out = malloc(capacity);

    if (!out)
        return NULL;

    size_t len = 0;

    while (**p && **p != '"')
    {
        unsigned char ch = (unsigned char)**p;
        (*p)++;

        if (ch == '\\')
        {
            char esc = **p;

            if (!esc)
            {
                free(out);
                return NULL;
            }

            (*p)++;

            switch (esc)
            {
                case '"':  out[len++] = '"';  break;
                case '\\': out[len++] = '\\'; break;
                case '/':  out[len++] = '/';  break;
                case 'b':  out[len++] = '\b'; break;
                case 'f':  out[len++] = '\f'; break;
                case 'n':  out[len++] = '\n'; break;
                case 'r':  out[len++] = '\r'; break;
                case 't':  out[len++] = '\t'; break;

                case 'u':
                {
                    unsigned int cp = 0;

                    for (int i = 0; i < 4; i++)
                    {
                        int h = hex_value(**p);

                        if (h < 0)
                        {
                            free(out);
                            return NULL;
                        }

                        cp = cp * 16 + (unsigned int)h;
                        (*p)++;
                    }

                    if (!append_utf8(out, &len, cp))
                    {
                        free(out);
                        return NULL;
                    }

                    break;
                }

                default:
                    free(out);
                    return NULL;
            }
        }
        else
        {
            if (ch < 32)
            {
                free(out);
                return NULL;
            }

            out[len++] = (char)ch;
        }
    }

    if (**p != '"')
    {
        free(out);
        return NULL;
    }

    (*p)++;

    out[len] = '\0';
    return out;
}

/*
   Extract a string property from the JSON object.

   Returns:
   1 = property found and string returned
   0 = property missing
  -1 = malformed property
*/

static int json_get_string(
    const char *json,
    const char *key,
    char **value)
{
    *value = NULL;

    const char *p = json;

    skip_spaces(&p);

    if (*p != '{')
        return -1;

    p++;

    while (*p)
    {
        skip_spaces(&p);

        if (*p == '}')
            return 0;

        char *property = parse_json_string(&p);

        if (!property)
            return -1;

        skip_spaces(&p);

        if (*p != ':')
        {
            free(property);
            return -1;
        }

        p++;
        skip_spaces(&p);

        int matched = strcmp(property, key) == 0;
        free(property);

        if (matched)
        {
            if (*p == '"')
            {
                *value = parse_json_string(&p);
                return *value ? 1 : -1;
            }

            if (strncmp(p, "null", 4) == 0)
            {
                *value = NULL;
                return 1;
            }

            return -1;
        }

        /*
           Skip an unknown property.
           The form currently sends only strings, numbers,
           or null for its properties.
        */

        if (*p == '"')
        {
            char *ignored = parse_json_string(&p);
            free(ignored);
        }
        else
        {
            while (*p && *p != ',' && *p != '}')
                p++;
        }

        skip_spaces(&p);

        if (*p == ',')
        {
            p++;
            continue;
        }

        if (*p == '}')
            return 0;

        return -1;
    }

    return 0;
}

static int json_get_required_string(
    const char *json,
    const char *key,
    char **value)
{
    int result = json_get_string(json, key, value);

    if (result != 1 || *value == NULL || (*value)[0] == '\0')
    {
        free(*value);
        *value = NULL;
        return 0;
    }

    return 1;
}

/* =========================================================
   JSON RESPONSE HELPERS
   ========================================================= */

static char *escape_json(const char *input)
{
    if (!input)
        input = "";

    size_t capacity = strlen(input) * 6 + 1;
    char *output = malloc(capacity);

    if (!output)
        return NULL;

    size_t j = 0;

    for (size_t i = 0; input[i] != '\0'; i++)
    {
        unsigned char ch = (unsigned char)input[i];

        if (ch == '"' || ch == '\\')
        {
            output[j++] = '\\';
            output[j++] = ch;
        }
        else if (ch < 32)
        {
            output[j++] = ' ';
        }
        else
        {
            output[j++] = ch;
        }
    }

    output[j] = '\0';
    return output;
}

/* =========================================================
   HTTP RESPONSE
   ========================================================= */

static enum MHD_Result send_response(
    struct MHD_Connection *connection,
    const char *json,
    unsigned int status_code)
{
    struct MHD_Response *response =
        MHD_create_response_from_buffer(
            strlen(json),
            (void *)json,
            MHD_RESPMEM_MUST_COPY
        );

    if (!response)
        return MHD_NO;

    MHD_add_response_header(
        response,
        "Content-Type",
        "application/json; charset=utf-8"
    );

    MHD_add_response_header(
        response,
        "Access-Control-Allow-Origin",
        "*"
    );

    MHD_add_response_header(
        response,
        "Access-Control-Allow-Methods",
        "GET, POST, DELETE, OPTIONS"
    );

    MHD_add_response_header(
        response,
        "Access-Control-Allow-Headers",
        "Content-Type"
    );

    enum MHD_Result result =
        MHD_queue_response(connection, status_code, response);

    MHD_destroy_response(response);

    return result;
}



/* =========================================================
   DATABASE
   ========================================================= */

static PGconn *open_database(void)
{
    PGconn *conn = PQconnectdb(DB_CONNINFO);

    if (PQstatus(conn) != CONNECTION_OK)
    {
        fprintf(stderr, "Database connection failed: %s\n",
                PQerrorMessage(conn));

        PQfinish(conn);
        return NULL;
    }

    return conn;
}

static int query_ok(PGresult *res)
{
    return res &&
        (PQresultStatus(res) == PGRES_COMMAND_OK ||
         PQresultStatus(res) == PGRES_TUPLES_OK);
}

/* =========================================================
   GET /api/songs
   ========================================================= */

static char *get_all_songs_json(void)
{
    PGconn *conn = open_database();

    if (!conn)
        return NULL;

    const char *query =
        "SELECT songs.song_id, songs.song_title, artists.artist_name, "
        "songs.duration_seconds, songs.file_path "
        "FROM songs "
        "INNER JOIN artists "
        "ON songs.artist_id = artists.artist_id "
        "ORDER BY songs.song_id";

    PGresult *res = PQexec(conn, query);

    if (PQresultStatus(res) != PGRES_TUPLES_OK)
    {
        fprintf(stderr, "Song query failed: %s\n",
                PQerrorMessage(conn));

        PQclear(res);
        PQfinish(conn);
        return NULL;
    }

    size_t capacity = 65536;
    size_t length = 0;

    char *json = malloc(capacity);

    if (!json)
    {
        PQclear(res);
        PQfinish(conn);
        return NULL;
    }

    json[0] = '\0';
    length += snprintf(json + length, capacity - length, "[");

    int rows = PQntuples(res);

    for (int i = 0; i < rows; i++)
    {
        char *song_id = escape_json(PQgetvalue(res, i, 0));
        char *song_title = escape_json(PQgetvalue(res, i, 1));
        char *artist_name = escape_json(PQgetvalue(res, i, 2));
        char *duration = escape_json(PQgetvalue(res, i, 3));
        char *file_path = escape_json(PQgetvalue(res, i, 4));

        if (!song_id || !song_title || !artist_name || !duration || !file_path)
        {
            free(song_id);
            free(song_title);
            free(artist_name);
            free(duration);
            free(file_path);
            free(json);

            PQclear(res);
            PQfinish(conn);
            return NULL;
        }

        int written = snprintf(
            json + length,
            capacity - length,
            "%s{\"song_id\":\"%s\","
            "\"song_title\":\"%s\","
            "\"artist_name\":\"%s\","
            "\"duration_seconds\":%s,"
            "\"file_path\":\"%s\"}",
            i == 0 ? "" : ",",
            song_id,
            song_title,
            artist_name,
            duration,
            file_path
        );

        free(song_id);
        free(song_title);
        free(artist_name);
        free(duration);
        free(file_path);

        if (written < 0 || (size_t)written >= capacity - length)
        {
            free(json);
            PQclear(res);
            PQfinish(conn);
            return NULL;
        }

        length += (size_t)written;
    }

    if (length + 2 > capacity)
    {
        free(json);
        PQclear(res);
        PQfinish(conn);
        return NULL;
    }

    json[length++] = ']';
    json[length] = '\0';

    PQclear(res);
    PQfinish(conn);

    return json;
}

/* =========================================================
   GET /api/playlists
   ========================================================= */

static char *get_playlists_json(void)
{
    PGconn *conn = open_database();

    if (!conn)
        return NULL;

    const char *paramValues[1] = {
        CURRENT_USER_ID
    };

    PGresult *res = PQexecParams(
        conn,
        "SELECT playlist_id, playlist_name, total_songs "
        "FROM user_playlists "
        "WHERE user_id = $1 "
        "ORDER BY playlist_id",
        1,
        NULL,
        paramValues,
        NULL,
        NULL,
        0
    );

    if (PQresultStatus(res) != PGRES_TUPLES_OK)
    {
        fprintf(stderr, "Playlist query failed: %s\n",
                PQerrorMessage(conn));

        PQclear(res);
        PQfinish(conn);
        return NULL;
    }

    size_t capacity = 32768;
    size_t length = 0;

    char *json = malloc(capacity);

    if (!json)
    {
        PQclear(res);
        PQfinish(conn);
        return NULL;
    }

    json[0] = '\0';
    length += snprintf(json + length, capacity - length, "[");

    int rows = PQntuples(res);

    for (int i = 0; i < rows; i++)
    {
        char *playlist_id =
            escape_json(PQgetvalue(res, i, 0));

        char *playlist_name =
            escape_json(PQgetvalue(res, i, 1));

        const char *total_songs =
            PQgetvalue(res, i, 2);

        if (!playlist_id || !playlist_name)
        {
            free(playlist_id);
            free(playlist_name);
            free(json);

            PQclear(res);
            PQfinish(conn);
            return NULL;
        }

        int written = snprintf(
            json + length,
            capacity - length,
            "%s{\"playlist_id\":\"%s\","
            "\"playlist_name\":\"%s\","
            "\"total_songs\":%s}",
            i == 0 ? "" : ",",
            playlist_id,
            playlist_name,
            total_songs
        );

        free(playlist_id);
        free(playlist_name);

        if (written < 0 || (size_t)written >= capacity - length)
        {
            free(json);
            PQclear(res);
            PQfinish(conn);
            return NULL;
        }

        length += (size_t)written;
    }

    if (length + 2 > capacity)
    {
        free(json);
        PQclear(res);
        PQfinish(conn);
        return NULL;
    }

    json[length++] = ']';
    json[length] = '\0';

    PQclear(res);
    PQfinish(conn);

    return json;
}

/* =========================================================
   ID GENERATION
   ========================================================= */

/*
   Call while inside a transaction after taking the
   MelodyBox advisory transaction lock.

   Example:
   artists -> A008
   albums  -> AL009
   songs   -> S021
*/

static int generate_id(
    PGconn *conn,
    const char *table,
    const char *column,
    const char *prefix,
    char *output,
    size_t output_size)
{
    char query[256];

    snprintf(
        query,
        sizeof(query),
        "SELECT COALESCE(MAX("
        "CAST(SUBSTRING(%s FROM %zu) AS INTEGER)"
        "), 0) + 1 "
        "FROM %s "
        "WHERE %s LIKE '%s%%'",
        column,
        strlen(prefix) + 1,
        table,
        column,
        prefix
    );

    PGresult *res = PQexec(conn, query);

    if (PQresultStatus(res) != PGRES_TUPLES_OK ||
        PQntuples(res) != 1)
    {
        PQclear(res);
        return 0;
    }

    int next = atoi(PQgetvalue(res, 0, 0));

    PQclear(res);

    int written = snprintf(
        output,
        output_size,
        "%s%03d",
        prefix,
        next
    );

    return written > 0 && (size_t)written < output_size;
}

/* =========================================================
   POST /api/songs
   ========================================================= */

static enum MHD_Result add_song(
    struct MHD_Connection *connection,
    const char *body)
{
    char *song_title = NULL;
    char *genre_id = NULL;
    char *duration_text = NULL;
    char *release_year_text = NULL;
    char *language = NULL;
    char *file_path = NULL;

    char *artist_name = NULL;
    char *artist_country = NULL;
    char *artist_debut_text = NULL;
    char *artist_bio = NULL;

    char *album_name = NULL;
    char *album_date = NULL;
    char *album_type = NULL;
    char *record_label = NULL;

    if (!json_get_required_string(body, "song_title", &song_title) ||
        !json_get_required_string(body, "genre_id", &genre_id) ||
        !json_get_required_string(body, "duration_seconds", &duration_text) ||
        !json_get_required_string(body, "language", &language) ||
        !json_get_required_string(body, "file_path", &file_path) ||
        !json_get_required_string(body, "artist_name", &artist_name))
    {
        goto invalid_input;
    }

    json_get_string(body, "release_year", &release_year_text);
    json_get_string(body, "artist_country", &artist_country);
    json_get_string(body, "artist_debut_year", &artist_debut_text);
    json_get_string(body, "artist_bio", &artist_bio);

    json_get_string(body, "album_name", &album_name);
    json_get_string(body, "album_release_date", &album_date);
    json_get_string(body, "album_type", &album_type);
    json_get_string(body, "record_label", &record_label);

    int duration = atoi(duration_text);

    if (duration <= 0)
        goto invalid_input;

    if (release_year_text &&
        (atoi(release_year_text) < 1900 ||
         atoi(release_year_text) > 2026))
        goto invalid_input;

    if (artist_debut_text &&
        (atoi(artist_debut_text) < 1900 ||
         atoi(artist_debut_text) > 2026))
        goto invalid_input;

    if (album_name && album_name[0] != '\0' &&
        (!album_date || album_date[0] == '\0'))
    {
        free(song_title);
        free(genre_id);
        free(duration_text);
        free(release_year_text);
        free(language);
        free(file_path);
        free(artist_name);
        free(artist_country);
        free(artist_debut_text);
        free(artist_bio);
        free(album_name);
        free(album_date);
        free(album_type);
        free(record_label);

        return send_response(
            connection,
            "{\"error\":\"Album release date is required when creating an album.\"}",
            MHD_HTTP_BAD_REQUEST
        );
    }

    PGconn *conn = open_database();

    if (!conn)
        goto database_error;

    PGresult *res = PQexec(conn, "BEGIN");

    if (!query_ok(res))
    {
        PQclear(res);
        PQfinish(conn);
        goto database_error;
    }

    PQclear(res);

    /*
       Advisory transaction lock:
       serializes ID generation for MelodyBox inserts.
    */

    res = PQexec(
        conn,
        "SELECT pg_advisory_xact_lock(20261024, 1)"
    );

    if (!query_ok(res))
    {
        PQclear(res);
        PQexec(conn, "ROLLBACK");
        PQfinish(conn);
        goto database_error;
    }

    PQclear(res);

    char artist_id[32];
    char album_id[32];
    char song_id[32];

    int artist_exists = 0;

    const char *artist_params[1] = {
        artist_name
    };

    res = PQexecParams(
        conn,
        "SELECT artist_id "
        "FROM artists "
        "WHERE LOWER(artist_name) = LOWER($1) "
        "LIMIT 1",
        1,
        NULL,
        artist_params,
        NULL,
        NULL,
        0
    );

    if (PQresultStatus(res) != PGRES_TUPLES_OK)
    {
        PQclear(res);
        PQexec(conn, "ROLLBACK");
        PQfinish(conn);
        goto database_error;
    }

    if (PQntuples(res) > 0)
    {
        snprintf(
            artist_id,
            sizeof(artist_id),
            "%s",
            PQgetvalue(res, 0, 0)
        );

        artist_exists = 1;
    }

    PQclear(res);

    if (!artist_exists)
    {
        if (!generate_id(
                conn,
                "artists",
                "artist_id",
                "A",
                artist_id,
                sizeof(artist_id)))
        {
            PQexec(conn, "ROLLBACK");
            PQfinish(conn);
            goto database_error;
        }

        const char *artist_params[5] = {
            artist_id,
            artist_name,
            artist_bio,
            artist_country,
            artist_debut_text
        };

        res = PQexecParams(
            conn,
            "INSERT INTO artists "
            "(artist_id, artist_name, bio, country, debut_year) "
            "VALUES ($1, $2, $3, $4, $5)",
            5,
            NULL,
            artist_params,
            NULL,
            NULL,
            0
        );

        if (!query_ok(res))
        {
            PQclear(res);
            PQexec(conn, "ROLLBACK");
            PQfinish(conn);
            goto database_error;
        }

        PQclear(res);
    }

    /*
       Check genre foreign key.
    */

    const char *genre_params[1] = {
        genre_id
    };

    res = PQexecParams(
        conn,
        "SELECT genre_id FROM genres WHERE genre_id = $1",
        1,
        NULL,
        genre_params,
        NULL,
        NULL,
        0
    );

    if (PQresultStatus(res) != PGRES_TUPLES_OK ||
        PQntuples(res) == 0)
    {
        PQclear(res);
        PQexec(conn, "ROLLBACK");
        PQfinish(conn);

        free(song_title);
        free(genre_id);
        free(duration_text);
        free(release_year_text);
        free(language);
        free(file_path);
        free(artist_name);
        free(artist_country);
        free(artist_debut_text);
        free(artist_bio);
        free(album_name);
        free(album_date);
        free(album_type);
        free(record_label);

        return send_response(
            connection,
            "{\"error\":\"Genre ID does not exist in the genres table.\"}",
            MHD_HTTP_BAD_REQUEST
        );
    }

    PQclear(res);

    /*
       Optional album.
       Only create an album if album_name was submitted.
    */

    const char *album_id_param = NULL;

    if (album_name && album_name[0] != '\0')
    {
        if (!generate_id(
                conn,
                "albums",
                "album_id",
                "AL",
                album_id,
                sizeof(album_id)))
        {
            PQexec(conn, "ROLLBACK");
            PQfinish(conn);
            goto database_error;
        }

        album_id_param = album_id;

        const char *album_params[7] = {
            album_id,
            album_name,
            artist_id,
            album_date,
            album_type,
            record_label,
            NULL
        };

        res = PQexecParams(
            conn,
            "INSERT INTO albums "
            "(album_id, album_name, artist_id, release_date, "
            "album_type, record_label) "
            "VALUES ($1, $2, $3, $4::date, $5, $6)",
            6,
            NULL,
            album_params,
            NULL,
            NULL,
            0
        );

        if (!query_ok(res))
        {
            PQclear(res);
            PQexec(conn, "ROLLBACK");
            PQfinish(conn);
            goto database_error;
        }

        PQclear(res);
    }

    /*
       Generate song ID.
    */

    if (!generate_id(
            conn,
            "songs",
            "song_id",
            "S",
            song_id,
            sizeof(song_id)))
    {
        PQexec(conn, "ROLLBACK");
        PQfinish(conn);
        goto database_error;
    }

    /*
       Insert song.

       NULL album_id is allowed by your schema.
       play_count, likes_count, is_explicit, audio_quality
       use their database defaults.
    */

    const char *song_params[8] = {
        song_id,
        song_title,
        artist_id,
        album_id_param,
        genre_id,
        duration_text,
        release_year_text,
        language
    };

    /*
       file_path is added as the ninth parameter below.
    */

    const char *song_insert_params[9] = {
        song_id,
        song_title,
        artist_id,
        album_id_param,
        genre_id,
        duration_text,
        release_year_text,
        language,
        file_path
    };

    (void)song_params;

    res = PQexecParams(
        conn,
        "INSERT INTO songs "
        "(song_id, song_title, artist_id, album_id, genre_id, "
        "duration_seconds, release_year, language, file_path) "
        "VALUES "
        "($1, $2, $3, $4, $5, $6::integer, $7::integer, $8, $9)",
        9,
        NULL,
        song_insert_params,
        NULL,
        NULL,
        0
    );

    if (!query_ok(res))
    {
        PQclear(res);
        PQexec(conn, "ROLLBACK");
        PQfinish(conn);
        goto database_error;
    }

    PQclear(res);

    res = PQexec(conn, "COMMIT");

    if (!query_ok(res))
    {
        PQclear(res);
        PQexec(conn, "ROLLBACK");
        PQfinish(conn);
        goto database_error;
    }

    PQclear(res);
    PQfinish(conn);

    char response_json[256];

    snprintf(
        response_json,
        sizeof(response_json),
        "{\"message\":\"Song added successfully\","
        "\"song_id\":\"%s\"}",
        song_id
    );

    free(song_title);
    free(genre_id);
    free(duration_text);
    free(release_year_text);
    free(language);
    free(file_path);
    free(artist_name);
    free(artist_country);
    free(artist_debut_text);
    free(artist_bio);
    free(album_name);
    free(album_date);
    free(album_type);
    free(record_label);

    return send_response(
        connection,
        response_json,
        MHD_HTTP_CREATED
    );

invalid_input:

    free(song_title);
    free(genre_id);
    free(duration_text);
    free(release_year_text);
    free(language);
    free(file_path);
    free(artist_name);
    free(artist_country);
    free(artist_debut_text);
    free(artist_bio);
    free(album_name);
    free(album_date);
    free(album_type);
    free(record_label);

    return send_response(
        connection,
        "{\"error\":\"Invalid or missing song, artist, or duration fields.\"}",
        MHD_HTTP_BAD_REQUEST
    );

database_error:

    free(song_title);
    free(genre_id);
    free(duration_text);
    free(release_year_text);
    free(language);
    free(file_path);
    free(artist_name);
    free(artist_country);
    free(artist_debut_text);
    free(artist_bio);
    free(album_name);
    free(album_date);
    free(album_type);
    free(record_label);

    return send_response(
        connection,
        "{\"error\":\"Database operation failed. Check the server console and database constraints.\"}",
        MHD_HTTP_INTERNAL_SERVER_ERROR
    );
}

/* =========================================================
   POST /api/playlists
   ========================================================= */

static enum MHD_Result add_playlist(
    struct MHD_Connection *connection,
    const char *body)
{
    char *playlist_name = NULL;
    char *description = NULL;

    if (!json_get_required_string(body, "playlist_name", &playlist_name))
    {
        return send_response(
            connection,
            "{\"error\":\"Playlist name is required.\"}",
            MHD_HTTP_BAD_REQUEST);
    }

    json_get_string(body, "description", &description);

    PGconn *conn = open_database();
    if (!conn)
    {
        free(playlist_name);
        free(description);
        return send_response(connection,
            "{\"error\":\"Database connection failed.\"}",
            MHD_HTTP_INTERNAL_SERVER_ERROR);
    }

    PGresult *res = PQexec(conn, "BEGIN");
    if (!query_ok(res))
    {
        PQclear(res);
        PQfinish(conn);
        free(playlist_name);
        free(description);
        return send_response(connection,
            "{\"error\":\"Could not begin database transaction.\"}",
            MHD_HTTP_INTERNAL_SERVER_ERROR);
    }
    PQclear(res);

    res = PQexec(conn, "SELECT pg_advisory_xact_lock(20261024, 1)");
    if (!query_ok(res))
    {
        PQclear(res);
        PQexec(conn, "ROLLBACK");
        PQfinish(conn);
        free(playlist_name);
        free(description);
        return send_response(connection,
            "{\"error\":\"Could not lock playlist ID generation.\"}",
            MHD_HTTP_INTERNAL_SERVER_ERROR);
    }
    PQclear(res);

    char playlist_id[32];
    if (!generate_id(conn, "user_playlists", "playlist_id", "PL",
                     playlist_id, sizeof(playlist_id)))
    {
        PQexec(conn, "ROLLBACK");
        PQfinish(conn);
        free(playlist_name);
        free(description);
        return send_response(connection,
            "{\"error\":\"Could not generate playlist ID.\"}",
            MHD_HTTP_INTERNAL_SERVER_ERROR);
    }

    const char *params[4] = {
        playlist_id,
        CURRENT_USER_ID,
        playlist_name,
        (description && description[0] != '\0') ? description : NULL
    };

    res = PQexecParams(conn,
        "INSERT INTO user_playlists "
        "(playlist_id, user_id, playlist_name, description) "
        "VALUES ($1, $2, $3, $4)",
        4, NULL, params, NULL, NULL, 0);

    if (!query_ok(res))
    {
        fprintf(stderr, "Playlist insert failed: %s\\n", PQerrorMessage(conn));
        PQclear(res);
        PQexec(conn, "ROLLBACK");
        PQfinish(conn);
        free(playlist_name);
        free(description);
        return send_response(connection,
            "{\"error\":\"Could not create playlist. Check server console and database constraints.\"}",
            MHD_HTTP_INTERNAL_SERVER_ERROR);
    }
    PQclear(res);

    res = PQexec(conn, "COMMIT");
    if (!query_ok(res))
    {
        PQclear(res);
        PQexec(conn, "ROLLBACK");
        PQfinish(conn);
        free(playlist_name);
        free(description);
        return send_response(connection,
            "{\"error\":\"Could not save playlist.\"}",
            MHD_HTTP_INTERNAL_SERVER_ERROR);
    }
    PQclear(res);
    PQfinish(conn);

    char response_json[256];
    snprintf(response_json, sizeof(response_json),
        "{\"message\":\"Playlist created successfully\","
        "\"playlist_id\":\"%s\"}", playlist_id);

    free(playlist_name);
    free(description);
    return send_response(connection, response_json, MHD_HTTP_CREATED);
}


/* =========================================================
   PLAYLIST SONGS + AUDIO
   ========================================================= */

static char *get_playlist_songs_json(const char *playlist_id)
{
    PGconn *conn = open_database();
    if (!conn) return NULL;

    const char *params[2] = { playlist_id, CURRENT_USER_ID };
    PGresult *res = PQexecParams(conn,
        "SELECT s.song_id, s.song_title, a.artist_name, "
        "s.duration_seconds, s.file_path, ps.song_order "
        "FROM playlist_songs ps "
        "JOIN user_playlists up ON up.playlist_id = ps.playlist_id "
        "JOIN songs s ON s.song_id = ps.song_id "
        "JOIN artists a ON a.artist_id = s.artist_id "
        "WHERE ps.playlist_id = $1 AND up.user_id = $2 "
        "ORDER BY ps.song_order, ps.added_date",
        2, NULL, params, NULL, NULL, 0);

    if (!res || PQresultStatus(res) != PGRES_TUPLES_OK)
    {
        if (res) PQclear(res);
        PQfinish(conn);
        return NULL;
    }

    size_t cap = 65536, len = 0;
    char *json = malloc(cap);
    if (!json) { PQclear(res); PQfinish(conn); return NULL; }
    json[len++] = '['; json[len] = '\0';

    for (int i = 0; i < PQntuples(res); i++)
    {
        char *id = escape_json(PQgetvalue(res, i, 0));
        char *title = escape_json(PQgetvalue(res, i, 1));
        char *artist = escape_json(PQgetvalue(res, i, 2));
        char *duration = escape_json(PQgetvalue(res, i, 3));
        char *path = escape_json(PQgetvalue(res, i, 4));
        char *order = escape_json(PQgetvalue(res, i, 5));
        if (!id || !title || !artist || !duration || !path || !order)
        {
            free(id); free(title); free(artist); free(duration); free(path); free(order);
            free(json); PQclear(res); PQfinish(conn); return NULL;
        }
        int n = snprintf(json + len, cap - len,
            "%s{\"song_id\":\"%s\",\"song_title\":\"%s\","
            "\"artist_name\":\"%s\",\"duration_seconds\":%s,"
            "\"file_path\":\"%s\",\"song_order\":%s}",
            i ? "," : "", id, title, artist, duration, path, order);
        free(id); free(title); free(artist); free(duration); free(path); free(order);
        if (n < 0 || (size_t)n >= cap - len)
        { free(json); PQclear(res); PQfinish(conn); return NULL; }
        len += (size_t)n;
    }
    if (len + 2 > cap) { free(json); PQclear(res); PQfinish(conn); return NULL; }
    json[len++] = ']'; json[len] = '\0';
    PQclear(res); PQfinish(conn);
    return json;
}

static enum MHD_Result add_song_to_playlist(
    struct MHD_Connection *connection,
    const char *playlist_id,
    const char *body)
{
    char *song_id = NULL;

    if (!json_get_required_string(body, "song_id", &song_id))
        return send_response(connection,
            "{\"error\":\"song_id is required.\"}",
            MHD_HTTP_BAD_REQUEST);

    PGconn *conn = open_database();

    if (!conn) {
        fprintf(stderr, "ADD SONG ERROR: Database connection failed.\n");
        free(song_id);
        return send_response(connection,
            "{\"error\":\"Database connection failed.\"}",
            MHD_HTTP_INTERNAL_SERVER_ERROR);
    }

    PGresult *res = PQexec(conn, "BEGIN");

    if (!query_ok(res)) {
        fprintf(stderr, "ADD SONG ERROR at BEGIN: %s\n",
                PQerrorMessage(conn));
        if (res) PQclear(res);
        PQfinish(conn);
        free(song_id);
        return send_response(connection,
            "{\"error\":\"Could not begin transaction.\"}",
            MHD_HTTP_INTERNAL_SERVER_ERROR);
    }
    PQclear(res);
  
    const char *owner_params[2] = { playlist_id, CURRENT_USER_ID };

    /* Step 1: Verify that the playlist belongs to the current user. */
    res = PQexecParams(conn,
        "SELECT playlist_id FROM user_playlists "
        "WHERE playlist_id=$1::varchar "
        "AND user_id=$2::varchar FOR UPDATE",
        2, NULL, owner_params, NULL, NULL, 0);

    fprintf(stderr,
        "PLAYLIST QUERY: status=%d, rows=%d, error=%s\n",
        res ? PQresultStatus(res) : -1,
        res ? PQntuples(res) : -1,
        PQerrorMessage(conn));

    if (!res || PQresultStatus(res) != PGRES_TUPLES_OK ||
        PQntuples(res) != 1) {
        fprintf(stderr,
            "ADD SONG ERROR checking playlist %s: %s\n",
            playlist_id, PQerrorMessage(conn));

        if (res) PQclear(res);
        PQexec(conn, "ROLLBACK");
        PQfinish(conn);
        free(song_id);

        return send_response(connection,
            "{\"error\":\"Playlist not found.\"}",
            MHD_HTTP_NOT_FOUND);
    }
    PQclear(res);

    /* Step 2: Verify that the song exists. */
    const char *song_params[1] = { song_id };

    res = PQexecParams(conn,
        "SELECT song_id FROM songs WHERE song_id=$1::varchar",
        1, NULL, song_params, NULL, NULL, 0);

    if (!res || PQresultStatus(res) != PGRES_TUPLES_OK ||
        PQntuples(res) != 1) {
        fprintf(stderr,
            "ADD SONG ERROR checking song %s: %s\n",
            song_id, PQerrorMessage(conn));

        if (res) PQclear(res);
        PQexec(conn, "ROLLBACK");
        PQfinish(conn);
        free(song_id);

        return send_response(connection,
            "{\"error\":\"Song not found.\"}",
            MHD_HTTP_NOT_FOUND);
    }
    PQclear(res);

    /* Step 3: Insert the song into the playlist. */
    const char *insert_params[2] = { playlist_id, song_id };

    res = PQexecParams(conn,
        "INSERT INTO playlist_songs "
        "(playlist_id, song_id, song_order) "
        "VALUES ($1::varchar, $2::varchar, "
        "(SELECT COALESCE(MAX(song_order), 0) + 1 "
        "FROM playlist_songs WHERE playlist_id=$1::varchar))",
        2, NULL, insert_params, NULL, NULL, 0);

    if (!query_ok(res)) {
        const char *err = res
            ? PQresultErrorField(res, PG_DIAG_SQLSTATE) : NULL;
        int duplicate = err && strcmp(err, "23505") == 0;

        fprintf(stderr,
            "ADD SONG ERROR at INSERT (playlist=%s, song=%s, SQLSTATE=%s): %s\n",
            playlist_id, song_id, err ? err : "unknown",
            PQerrorMessage(conn));

        if (res) PQclear(res);
        PQexec(conn, "ROLLBACK");
        PQfinish(conn);
        free(song_id);

        return send_response(connection,
            duplicate
                ? "{\"error\":\"This song is already in the playlist.\"}"
                : "{\"error\":\"Could not add song to playlist.\"}",
            duplicate ? MHD_HTTP_CONFLICT
                      : MHD_HTTP_INTERNAL_SERVER_ERROR);
    }
    PQclear(res);

    res = PQexecParams(conn,
        "UPDATE user_playlists SET "
        "total_songs=(SELECT COUNT(*) FROM playlist_songs "
        "WHERE playlist_id=$1),"
        "total_duration_minutes=COALESCE(("
        "SELECT SUM(s.duration_seconds)/60 "
        "FROM playlist_songs ps "
        "JOIN songs s ON s.song_id=ps.song_id "
        "WHERE ps.playlist_id=$1),0) "
        "WHERE playlist_id=$1",
        1, NULL, owner_params, NULL, NULL, 0);

    if (!query_ok(res)) {
        fprintf(stderr, "ADD SONG ERROR at UPDATE TOTALS: %s\n",
                PQerrorMessage(conn));
        if (res) PQclear(res);
        PQexec(conn, "ROLLBACK");
        PQfinish(conn);
        free(song_id);
        return send_response(connection,
            "{\"error\":\"Could not update playlist totals.\"}",
            MHD_HTTP_INTERNAL_SERVER_ERROR);
    }
    PQclear(res);

    res = PQexec(conn, "COMMIT");

    if (!query_ok(res)) {
        fprintf(stderr, "ADD SONG ERROR at COMMIT: %s\n",
                PQerrorMessage(conn));
        if (res) PQclear(res);
        PQexec(conn, "ROLLBACK");
        PQfinish(conn);
        free(song_id);
        return send_response(connection,
            "{\"error\":\"Could not commit playlist update.\"}",
            MHD_HTTP_INTERNAL_SERVER_ERROR);
    }

    PQclear(res);
    PQfinish(conn);
    free(song_id);

    return send_response(connection,
        "{\"message\":\"Song added to playlist.\"}",
        MHD_HTTP_CREATED);
}

static enum MHD_Result remove_song_from_playlist(
    struct MHD_Connection *connection,
    const char *playlist_id,
    const char *song_id)
{
    PGconn *conn = open_database();
    if (!conn) return send_response(connection, "{\"error\":\"Database connection failed.\"}", MHD_HTTP_INTERNAL_SERVER_ERROR);
    PGresult *res = PQexec(conn, "BEGIN");
    if (!query_ok(res)) { PQclear(res); PQfinish(conn); return send_response(connection, "{\"error\":\"Could not begin transaction.\"}", MHD_HTTP_INTERNAL_SERVER_ERROR); }
    PQclear(res);

    const char *params[3] = { playlist_id, CURRENT_USER_ID, song_id };
    res = PQexecParams(conn,
        "DELETE FROM playlist_songs ps USING user_playlists up "
        "WHERE ps.playlist_id=$1 AND ps.song_id=$3 AND up.playlist_id=ps.playlist_id AND up.user_id=$2",
        3, NULL, params, NULL, NULL, 0);
    if (!query_ok(res))
    { PQclear(res); PQexec(conn,"ROLLBACK"); PQfinish(conn); return send_response(connection, "{\"error\":\"Could not remove song.\"}", MHD_HTTP_INTERNAL_SERVER_ERROR); }
    int deleted = atoi(PQcmdTuples(res));
    PQclear(res);
    if (!deleted)
    { PQexec(conn,"ROLLBACK"); PQfinish(conn); return send_response(connection, "{\"error\":\"Song or playlist not found.\"}", MHD_HTTP_NOT_FOUND); }

    const char *p[1] = { playlist_id };
    res = PQexecParams(conn,
        "UPDATE user_playlists SET "
        "total_songs=(SELECT COUNT(*) FROM playlist_songs WHERE playlist_id=$1),"
        "total_duration_minutes=COALESCE((SELECT SUM(s.duration_seconds)/60 FROM playlist_songs ps JOIN songs s ON s.song_id=ps.song_id WHERE ps.playlist_id=$1),0) "
        "WHERE playlist_id=$1",
        1, NULL, p, NULL, NULL, 0);
    if (!query_ok(res))
    { PQclear(res); PQexec(conn,"ROLLBACK"); PQfinish(conn); return send_response(connection, "{\"error\":\"Could not update playlist totals.\"}", MHD_HTTP_INTERNAL_SERVER_ERROR); }
    PQclear(res);
    res = PQexec(conn, "COMMIT");
    if (!query_ok(res))
    { PQclear(res); PQexec(conn,"ROLLBACK"); PQfinish(conn); return send_response(connection, "{\"error\":\"Could not commit removal.\"}", MHD_HTTP_INTERNAL_SERVER_ERROR); }
    PQclear(res); PQfinish(conn);
    return send_response(connection, "{\"message\":\"Song removed from playlist.\"}", MHD_HTTP_OK);
}


/* =========================================================
   DELETE /api/playlists/{playlist_id}
   Deletes only the playlist and its membership rows.
   Songs in the main music library are never deleted.
   ========================================================= */
static enum MHD_Result delete_playlist(
    struct MHD_Connection *connection,
    const char *playlist_id)
{
    PGconn *conn = open_database();
    if (!conn)
        return send_response(connection,
            "{\"error\":\"Database connection failed.\"}",
            MHD_HTTP_INTERNAL_SERVER_ERROR);

    PGresult *res = PQexec(conn, "BEGIN");
    if (!query_ok(res)) {
        PQclear(res);
        PQfinish(conn);
        return send_response(connection,
            "{\"error\":\"Could not begin transaction.\"}",
            MHD_HTTP_INTERNAL_SERVER_ERROR);
    }
    PQclear(res);

    const char *params[2] = { playlist_id, CURRENT_USER_ID };
    res = PQexecParams(conn,
        "SELECT playlist_id FROM user_playlists "
        "WHERE playlist_id=$1 AND user_id=$2 FOR UPDATE",
        2, NULL, params, NULL, NULL, 0);
    if (!res || PQresultStatus(res) != PGRES_TUPLES_OK ||
        PQntuples(res) != 1) {
        if (res) PQclear(res);
        PQexec(conn, "ROLLBACK");
        PQfinish(conn);
        return send_response(connection,
            "{\"error\":\"Playlist not found.\"}",
            MHD_HTTP_NOT_FOUND);
    }
    PQclear(res);

    res = PQexecParams(conn,
        "DELETE FROM playlist_songs WHERE playlist_id=$1",
        1, NULL, params, NULL, NULL, 0);
    if (!query_ok(res)) {
        PQclear(res);
        PQexec(conn, "ROLLBACK");
        PQfinish(conn);
        return send_response(connection,
            "{\"error\":\"Could not remove playlist songs.\"}",
            MHD_HTTP_INTERNAL_SERVER_ERROR);
    }
    PQclear(res);

    res = PQexecParams(conn,
        "DELETE FROM user_playlists WHERE playlist_id=$1 AND user_id=$2",
        2, NULL, params, NULL, NULL, 0);
    if (!query_ok(res) || atoi(PQcmdTuples(res)) != 1) {
        if (res) PQclear(res);
        PQexec(conn, "ROLLBACK");
        PQfinish(conn);
        return send_response(connection,
            "{\"error\":\"Could not delete playlist.\"}",
            MHD_HTTP_INTERNAL_SERVER_ERROR);
    }
    PQclear(res);

    res = PQexec(conn, "COMMIT");
    if (!query_ok(res)) {
        PQclear(res);
        PQexec(conn, "ROLLBACK");
        PQfinish(conn);
        return send_response(connection,
            "{\"error\":\"Could not commit playlist deletion.\"}",
            MHD_HTTP_INTERNAL_SERVER_ERROR);
    }
    PQclear(res);
    PQfinish(conn);

    return send_response(connection,
        "{\"message\":\"Playlist deleted successfully.\"}",
        MHD_HTTP_OK);
}


/* =========================================================
   LIKED SONGS
   ========================================================= */

static char *get_liked_songs_json(void)
{
    PGconn *conn = open_database();
    if (!conn) return NULL;

    const char *params[1] = { CURRENT_USER_ID };
    PGresult *res = PQexecParams(conn,
        "SELECT s.song_id, s.song_title, a.artist_name, "
        "s.duration_seconds, s.file_path, ls.liked_at "
        "FROM liked_songs ls "
        "JOIN songs s ON s.song_id = ls.song_id "
        "JOIN artists a ON a.artist_id = s.artist_id "
        "WHERE ls.user_id = $1::varchar "
        "ORDER BY ls.liked_at DESC, s.song_title",
        1, NULL, params, NULL, NULL, 0);

    if (!res || PQresultStatus(res) != PGRES_TUPLES_OK) {
        fprintf(stderr, "Liked songs query failed: %s\n", PQerrorMessage(conn));
        if (res) PQclear(res);
        PQfinish(conn);
        return NULL;
    }

    size_t cap = 65536, len = 0;
    char *json = malloc(cap);
    if (!json) { PQclear(res); PQfinish(conn); return NULL; }
    json[len++] = '['; json[len] = '\0';

    for (int i = 0; i < PQntuples(res); i++) {
        char *id = escape_json(PQgetvalue(res, i, 0));
        char *title = escape_json(PQgetvalue(res, i, 1));
        char *artist = escape_json(PQgetvalue(res, i, 2));
        char *duration = escape_json(PQgetvalue(res, i, 3));
        char *path = escape_json(PQgetvalue(res, i, 4));
        char *liked_at = escape_json(PQgetvalue(res, i, 5));
        if (!id || !title || !artist || !duration || !path || !liked_at) {
            free(id); free(title); free(artist); free(duration); free(path); free(liked_at);
            free(json); PQclear(res); PQfinish(conn); return NULL;
        }
        int n = snprintf(json + len, cap - len,
            "%s{\"song_id\":\"%s\",\"song_title\":\"%s\","
            "\"artist_name\":\"%s\",\"duration_seconds\":%s,"
            "\"file_path\":\"%s\",\"liked_at\":\"%s\"}",
            i ? "," : "", id, title, artist, duration, path, liked_at);
        free(id); free(title); free(artist); free(duration); free(path); free(liked_at);
        if (n < 0 || (size_t)n >= cap - len) {
            free(json); PQclear(res); PQfinish(conn); return NULL;
        }
        len += (size_t)n;
    }

    if (len + 2 > cap) { free(json); PQclear(res); PQfinish(conn); return NULL; }
    json[len++] = ']'; json[len] = '\0';
    PQclear(res); PQfinish(conn);
    return json;
}

static enum MHD_Result like_song(struct MHD_Connection *connection, const char *body)
{
    char *song_id = NULL;
    if (!json_get_required_string(body, "song_id", &song_id))
        return send_response(connection, "{\"error\":\"song_id is required.\"}", MHD_HTTP_BAD_REQUEST);

    PGconn *conn = open_database();
    if (!conn) {
        free(song_id);
        return send_response(connection, "{\"error\":\"Database connection failed.\"}", MHD_HTTP_INTERNAL_SERVER_ERROR);
    }

    const char *params[2] = { CURRENT_USER_ID, song_id };
    PGresult *res = PQexecParams(conn,
        "INSERT INTO liked_songs (user_id, song_id) "
        "VALUES ($1::varchar, $2::varchar) "
        "ON CONFLICT (user_id, song_id) DO NOTHING",
        2, NULL, params, NULL, NULL, 0);

    if (!query_ok(res)) {
        fprintf(stderr, "Like song failed: %s\n", PQerrorMessage(conn));
        if (res) PQclear(res);
        PQfinish(conn); free(song_id);
        return send_response(connection, "{\"error\":\"Could not like song.\"}", MHD_HTTP_INTERNAL_SERVER_ERROR);
    }

    int inserted = atoi(PQcmdTuples(res));
    PQclear(res); PQfinish(conn); free(song_id);
    if (!inserted)
        return send_response(connection, "{\"message\":\"Song is already liked.\"}", MHD_HTTP_OK);
    return send_response(connection, "{\"message\":\"Song liked successfully.\"}", MHD_HTTP_CREATED);
}

static enum MHD_Result unlike_song(struct MHD_Connection *connection, const char *song_id)
{
    PGconn *conn = open_database();
    if (!conn)
        return send_response(connection, "{\"error\":\"Database connection failed.\"}", MHD_HTTP_INTERNAL_SERVER_ERROR);

    const char *params[2] = { CURRENT_USER_ID, song_id };
    PGresult *res = PQexecParams(conn,
        "DELETE FROM liked_songs WHERE user_id=$1::varchar AND song_id=$2::varchar",
        2, NULL, params, NULL, NULL, 0);

    if (!query_ok(res)) {
        fprintf(stderr, "Unlike song failed: %s\n", PQerrorMessage(conn));
        if (res) PQclear(res);
        PQfinish(conn);
        return send_response(connection, "{\"error\":\"Could not unlike song.\"}", MHD_HTTP_INTERNAL_SERVER_ERROR);
    }

    int deleted = atoi(PQcmdTuples(res));
    PQclear(res); PQfinish(conn);
    if (!deleted)
        return send_response(connection, "{\"error\":\"Liked song not found.\"}", MHD_HTTP_NOT_FOUND);
    return send_response(connection, "{\"message\":\"Song unliked successfully.\"}", MHD_HTTP_OK);
}

/* Audio is streamed from the file_path already stored in songs. */
struct AudioFileContext { FILE *file; };

static ssize_t audio_reader(void *cls, uint64_t pos, char *buf, size_t max)
{
    struct AudioFileContext *ctx = cls;
    if (!ctx || !ctx->file) return MHD_CONTENT_READER_END_WITH_ERROR;
    if (pos > (uint64_t)LONG_MAX || fseek(ctx->file, (long)pos, SEEK_SET) != 0)
        return MHD_CONTENT_READER_END_WITH_ERROR;
    size_t n = fread(buf, 1, max, ctx->file);
    if (n == 0 && ferror(ctx->file)) return MHD_CONTENT_READER_END_WITH_ERROR;
    return (ssize_t)n;
}

static void audio_free(void *cls)
{
    struct AudioFileContext *ctx = cls;
    if (ctx) { if (ctx->file) fclose(ctx->file); free(ctx); }
}

static enum MHD_Result stream_song_audio(struct MHD_Connection *connection, const char *song_id)
{
    PGconn *conn = open_database();
    if (!conn) return send_response(connection, "{\"error\":\"Database connection failed.\"}", MHD_HTTP_INTERNAL_SERVER_ERROR);
    const char *params[1] = { song_id };
    PGresult *res = PQexecParams(conn, "SELECT file_path FROM songs WHERE song_id=$1",
        1, NULL, params, NULL, NULL, 0);
    if (!res || PQresultStatus(res) != PGRES_TUPLES_OK || PQntuples(res) != 1)
    {
        if (res) PQclear(res);
        PQfinish(conn);
        return send_response(connection, "{\"error\":\"Song not found.\"}", MHD_HTTP_NOT_FOUND);
    }
    char *path = strdup(PQgetvalue(res, 0, 0));
    PQclear(res); PQfinish(conn);
    if (!path) return send_response(connection, "{\"error\":\"Out of memory.\"}", MHD_HTTP_INTERNAL_SERVER_ERROR);

    struct AudioFileContext *ctx = calloc(1, sizeof(*ctx));
    if (!ctx) { free(path); return send_response(connection, "{\"error\":\"Out of memory.\"}", MHD_HTTP_INTERNAL_SERVER_ERROR); }
    ctx->file = fopen(path, "rb");
    free(path);
    if (!ctx->file) { free(ctx); return send_response(connection, "{\"error\":\"Audio file could not be opened. Check the song file_path on the server.\"}", MHD_HTTP_NOT_FOUND); }

    if (fseek(ctx->file, 0, SEEK_END) != 0)
    { audio_free(ctx); return send_response(connection, "{\"error\":\"Could not read audio file.\"}", MHD_HTTP_INTERNAL_SERVER_ERROR); }
    long size = ftell(ctx->file);
    if (size < 0 || fseek(ctx->file, 0, SEEK_SET) != 0)
    { audio_free(ctx); return send_response(connection, "{\"error\":\"Could not read audio file.\"}", MHD_HTTP_INTERNAL_SERVER_ERROR); }

    struct MHD_Response *response = MHD_create_response_from_callback(
        (uint64_t)size, 64 * 1024, &audio_reader, ctx, &audio_free);
    if (!response) { audio_free(ctx); return send_response(connection, "{\"error\":\"Could not create audio response.\"}", MHD_HTTP_INTERNAL_SERVER_ERROR); }
    MHD_add_response_header(response, "Content-Type", "audio/mpeg");
    MHD_add_response_header(response, "Accept-Ranges", "bytes");
    MHD_add_response_header(response, "Access-Control-Allow-Origin", "*");
    enum MHD_Result result = MHD_queue_response(connection, MHD_HTTP_OK, response);
    MHD_destroy_response(response);
    return result;
}


/* =========================================================
   POST /api/upload-audio (multipart/form-data)
   ========================================================= */

static enum MHD_Result audio_upload_iterator(void *cls,
    enum MHD_ValueKind kind, const char *key, const char *filename,
    const char *content_type, const char *transfer_encoding,
    const char *data, uint64_t off, size_t size)
{
    (void)kind; (void)content_type; (void)transfer_encoding;
    struct RequestData *rd = (struct RequestData *)cls;
    if (!rd || !key || strcmp(key, "audio_file") != 0)
        return MHD_YES;

    if (filename && filename[0] != '\0') {
        if (!rd->upload_started) {
            struct stat st;
            if (stat(AUDIO_UPLOAD_DIR, &st) != 0) {
                if (MB_MKDIR(AUDIO_UPLOAD_DIR) != 0 && stat(AUDIO_UPLOAD_DIR, &st) != 0) {
                    rd->upload_error = 1;
                    return MHD_NO;
                }
            }
            static unsigned long upload_counter = 0;
            unsigned long serial = ++upload_counter;
            int n = snprintf(rd->upload_path, sizeof(rd->upload_path),
                AUDIO_UPLOAD_DIR "/melodybox_%lu_%lu.mp3",
                (unsigned long)time(NULL), serial);
            if (n < 0 || (size_t)n >= sizeof(rd->upload_path)) {
                rd->upload_error = 1;
                return MHD_NO;
            }
            rd->upload_file = fopen(rd->upload_path, "wb");
            if (!rd->upload_file) {
                fprintf(stderr, "Audio upload: cannot create %s\n", rd->upload_path);
                rd->upload_error = 1;
                return MHD_NO;
            }
            rd->upload_started = 1;
        }
        if (size > 0) {
            if (!data || fwrite(data, 1, size, rd->upload_file) != size) {
                rd->upload_error = 1;
                return MHD_NO;
            }
        } else {
            if (rd->upload_file) {
                if (fclose(rd->upload_file) != 0) rd->upload_error = 1;
                rd->upload_file = NULL;
            }
            rd->upload_finished = 1;
        }
    }
    (void)off;
    return rd->upload_error ? MHD_NO : MHD_YES;
}

static enum MHD_Result handle_audio_upload(struct MHD_Connection *connection,
    const char *upload_data, size_t *upload_data_size, void **con_cls)
{
    struct RequestData *rd = (struct RequestData *)*con_cls;
    if (!rd) {
        rd = calloc(1, sizeof(*rd));
        if (!rd) return send_response(connection, "{\"error\":\"Out of memory.\"}", MHD_HTTP_INTERNAL_SERVER_ERROR);
        const char *ct = MHD_lookup_connection_value(connection, MHD_HEADER_KIND, "Content-Type");
        if (!ct || strncmp(ct, "multipart/form-data", 19) != 0) {
            free(rd);
            return send_response(connection, "{\"error\":\"Expected multipart/form-data with an audio_file field.\"}", MHD_HTTP_BAD_REQUEST);
        }
        rd->post_processor = MHD_create_post_processor(connection, 64 * 1024, &audio_upload_iterator, rd);
        if (!rd->post_processor) {
            free(rd);
            return send_response(connection, "{\"error\":\"Could not initialize upload parser.\"}", MHD_HTTP_INTERNAL_SERVER_ERROR);
        }
        *con_cls = rd;
        return MHD_YES;
    }

    if (*upload_data_size > 0) {
        if (MHD_post_process(rd->post_processor, upload_data, *upload_data_size) == MHD_NO)
            rd->upload_error = 1;
        *upload_data_size = 0;
        return MHD_YES;
    }

    if (rd->post_processor) {
        MHD_destroy_post_processor(rd->post_processor);
        rd->post_processor = NULL;
    }
    if (rd->upload_file) {
        if (fclose(rd->upload_file) != 0) rd->upload_error = 1;
        rd->upload_file = NULL;
    }
    if (rd->upload_error || !rd->upload_started || !rd->upload_path[0]) {
        if (rd->upload_path[0]) remove(rd->upload_path);
        free(rd);
        *con_cls = NULL;
        return send_response(connection,
            "{\"error\":\"Audio upload failed. Check that the Audio folder is writable and a file was selected.\"}",
            MHD_HTTP_INTERNAL_SERVER_ERROR);
    }

    char *escaped_path = escape_json(rd->upload_path);
    if (!escaped_path) {
        remove(rd->upload_path);
        free(rd);
        *con_cls = NULL;
        return send_response(connection, "{\"error\":\"Could not prepare uploaded file path.\"}", MHD_HTTP_INTERNAL_SERVER_ERROR);
    }
    size_t cap = strlen(escaped_path) + 64;
    char *json = malloc(cap);
    if (!json) {
        free(escaped_path); remove(rd->upload_path); free(rd); *con_cls = NULL;
        return send_response(connection, "{\"error\":\"Out of memory.\"}", MHD_HTTP_INTERNAL_SERVER_ERROR);
    }
    snprintf(json, cap, "{\"message\":\"Audio uploaded successfully.\",\"file_path\":\"%s\"}", escaped_path);
    free(escaped_path);
    free(rd);
    *con_cls = NULL;
    enum MHD_Result result = send_response(connection, json, MHD_HTTP_CREATED);
    free(json);
    return result;
}

/* =========================================================
   GET /api/recommended/{song_id}
   Recommend other songs sharing the current song's artist
   or genre. Artist matches are ranked first.
   ========================================================= */
static char *get_recommended_songs_json(const char *song_id)
{
    PGconn *conn = open_database();
    if (!conn) return NULL;
    const char *params[1] = { song_id };
    PGresult *res = PQexecParams(conn,
        "SELECT s.song_id, s.song_title, a.artist_name, "
        "s.duration_seconds, s.file_path "
        "FROM songs current "
        "JOIN songs s ON s.song_id <> current.song_id "
        "JOIN artists a ON a.artist_id = s.artist_id "
        "WHERE current.song_id = $1 "
        "AND (s.artist_id = current.artist_id OR s.genre_id = current.genre_id) "
        "ORDER BY CASE WHEN s.artist_id = current.artist_id THEN 0 ELSE 1 END, s.song_title",
        1, NULL, params, NULL, NULL, 0);
    if (!res || PQresultStatus(res) != PGRES_TUPLES_OK) {
        fprintf(stderr, "Recommendations query failed: %s\n", PQerrorMessage(conn));
        if (res) PQclear(res);
        PQfinish(conn);
        return NULL;
    }
    size_t cap = 65536, len = 0;
    char *json = malloc(cap);
    if (!json) { PQclear(res); PQfinish(conn); return NULL; }
    json[len++] = '['; json[len] = '\0';
    for (int i = 0; i < PQntuples(res); i++) {
        char *id = escape_json(PQgetvalue(res, i, 0));
        char *title = escape_json(PQgetvalue(res, i, 1));
        char *artist = escape_json(PQgetvalue(res, i, 2));
        char *duration = escape_json(PQgetvalue(res, i, 3));
        char *path = escape_json(PQgetvalue(res, i, 4));
        if (!id || !title || !artist || !duration || !path) {
            free(id); free(title); free(artist); free(duration); free(path);
            free(json); PQclear(res); PQfinish(conn); return NULL;
        }
        int n = snprintf(json + len, cap - len,
            "%s{\"song_id\":\"%s\",\"song_title\":\"%s\",\"artist_name\":\"%s\",\"duration_seconds\":%s,\"file_path\":\"%s\"}",
            i ? "," : "", id, title, artist, duration, path);
        free(id); free(title); free(artist); free(duration); free(path);
        if (n < 0 || (size_t)n >= cap - len) {
            free(json); PQclear(res); PQfinish(conn); return NULL;
        }
        len += (size_t)n;
    }
    if (len + 2 > cap) { free(json); PQclear(res); PQfinish(conn); return NULL; }
    json[len++] = ']'; json[len] = '\0';
    PQclear(res); PQfinish(conn);
    return json;
}

/* =========================================================
   POST /api/songs/update
   Update the title of an existing song.
   ========================================================= */
static enum MHD_Result update_song_title(struct MHD_Connection *connection, const char *body)
{
    char *song_id = NULL;
    char *song_title = NULL;
    if (!json_get_required_string(body, "song_id", &song_id) ||
        !json_get_required_string(body, "song_title", &song_title)) {
        free(song_id);
        free(song_title);
        return send_response(connection, "{\"error\":\"Song ID and non-empty title are required.\"}", MHD_HTTP_BAD_REQUEST);
    }

    PGconn *conn = open_database();
    if (!conn) {
        free(song_id); free(song_title);
        return send_response(connection, "{\"error\":\"Database connection failed.\"}", MHD_HTTP_INTERNAL_SERVER_ERROR);
    }
    const char *params[2] = { song_title, song_id };
    PGresult *res = PQexecParams(conn,
        "UPDATE songs SET song_title = $1 WHERE song_id = $2",
        2, NULL, params, NULL, NULL, 0);
    free(song_id);
    free(song_title);

    if (!res || PQresultStatus(res) != PGRES_COMMAND_OK) {
        fprintf(stderr, "Update song failed: %s\n", PQerrorMessage(conn));
        if (res) PQclear(res);
        PQfinish(conn);
        return send_response(connection, "{\"error\":\"Could not update song.\"}", MHD_HTTP_INTERNAL_SERVER_ERROR);
    }
    if (atoi(PQcmdTuples(res)) != 1) {
        PQclear(res); PQfinish(conn);
        return send_response(connection, "{\"error\":\"Song not found.\"}", MHD_HTTP_NOT_FOUND);
    }
    PQclear(res);
    PQfinish(conn);
    return send_response(connection, "{\"message\":\"Song title updated.\"}", MHD_HTTP_OK);
}

/* =========================================================
   DELETE /api/songs/{song_id}
   Remove a song from dependent tables and the music library.
   The audio file on disk is intentionally kept.
   ========================================================= */
static enum MHD_Result delete_song(struct MHD_Connection *connection, const char *song_id)
{
    PGconn *conn = open_database();
    if (!conn) return send_response(connection, "{\"error\":\"Database connection failed.\"}", MHD_HTTP_INTERNAL_SERVER_ERROR);
    PGresult *res = PQexec(conn, "BEGIN");
    if (!query_ok(res)) {
        if (res) PQclear(res);
        PQfinish(conn);
        return send_response(connection, "{\"error\":\"Could not start delete transaction.\"}", MHD_HTTP_INTERNAL_SERVER_ERROR);
    }
    PQclear(res);
    const char *params[1] = { song_id };
    const char *queries[] = {
        "DELETE FROM playlist_songs WHERE song_id=$1",
        "DELETE FROM liked_songs WHERE song_id=$1",
        "DELETE FROM song_ratings WHERE song_id=$1",
        "DELETE FROM listening_history WHERE song_id=$1"
    };
    for (size_t i = 0; i < sizeof(queries)/sizeof(queries[0]); i++) {
        res = PQexecParams(conn, queries[i], 1, NULL, params, NULL, NULL, 0);
        if (!query_ok(res)) {
            fprintf(stderr, "Delete song dependency cleanup failed: %s\n", PQerrorMessage(conn));
            PQclear(res);
            PQexec(conn, "ROLLBACK");
            PQfinish(conn);
            return send_response(connection, "{\"error\":\"Could not remove song references. No song was deleted; check dependent-table names in the database.\"}", MHD_HTTP_INTERNAL_SERVER_ERROR);
        }
        PQclear(res);
    }
    res = PQexec(conn,
        "UPDATE user_playlists up SET total_songs = "
        "(SELECT COUNT(*) FROM playlist_songs ps WHERE ps.playlist_id=up.playlist_id)");
    if (!query_ok(res)) {
        if (res) PQclear(res);
        PQexec(conn, "ROLLBACK"); PQfinish(conn);
        return send_response(connection, "{\"error\":\"Could not update playlist counts.\"}", MHD_HTTP_INTERNAL_SERVER_ERROR);
    }
    PQclear(res);
    res = PQexecParams(conn, "DELETE FROM songs WHERE song_id=$1", 1, NULL, params, NULL, NULL, 0);
    if (!query_ok(res) || atoi(PQcmdTuples(res)) != 1) {
        if (res) PQclear(res);
        PQexec(conn, "ROLLBACK"); PQfinish(conn);
        return send_response(connection, "{\"error\":\"Song not found or could not be deleted.\"}", MHD_HTTP_NOT_FOUND);
    }
    PQclear(res);
    res = PQexec(conn, "COMMIT");
    if (!query_ok(res)) {
        if (res) PQclear(res);
        PQexec(conn, "ROLLBACK"); PQfinish(conn);
        return send_response(connection, "{\"error\":\"Could not commit song deletion.\"}", MHD_HTTP_INTERNAL_SERVER_ERROR);
    }
    PQclear(res); PQfinish(conn);
    return send_response(connection, "{\"message\":\"Song deleted. The audio file on disk was kept.\"}", MHD_HTTP_OK);
}

/* =========================================================
   HTTP REQUEST HANDLER
   ========================================================= */

static enum MHD_Result request_handler(
    void *cls,
    struct MHD_Connection *connection,
    const char *url,
    const char *method,
    const char *version,
    const char *upload_data,
    size_t *upload_data_size,
    void **con_cls)
{
    (void)cls;
    (void)version;

    /*
       OPTIONS request.
    */

    if (strcmp(method, "OPTIONS") == 0)
        return send_response(connection, "{}", MHD_HTTP_OK);

    /*
       GET endpoints.
    */

    if (strcmp(method, "GET") == 0)
    {
        if (strcmp(url, "/api/songs") == 0)
        {
            char *json = get_all_songs_json();

            if (!json)
            {
                return send_response(
                    connection,
                    "{\"error\":\"Could not retrieve songs\"}",
                    MHD_HTTP_INTERNAL_SERVER_ERROR
                );
            }

            enum MHD_Result result =
                send_response(connection, json, MHD_HTTP_OK);

            free(json);
            return result;
        }

        if (strncmp(url, "/api/recommended/", 17) == 0)
        {
            const char *id = url + 17;
            if (!*id || strchr(id, '/') != NULL || strlen(id) >= 32)
                return send_response(connection, "{\"error\":\"Invalid song ID.\"}", MHD_HTTP_BAD_REQUEST);
            char *json = get_recommended_songs_json(id);
            if (!json)
                return send_response(connection, "{\"error\":\"Could not retrieve recommendations.\"}", MHD_HTTP_INTERNAL_SERVER_ERROR);
            enum MHD_Result result = send_response(connection, json, MHD_HTTP_OK);
            free(json);
            return result;
        }

        if (strncmp(url, "/api/playlists/", 15) == 0)
        {
            const char *id = url + 15;
            const char *suffix = strstr(id, "/songs");
            if (suffix && strcmp(suffix, "/songs") == 0)
            {
                size_t id_len = (size_t)(suffix - id);
                if (id_len == 0 || id_len >= 32)
                    return send_response(connection, "{\"error\":\"Invalid playlist ID.\"}", MHD_HTTP_BAD_REQUEST);
                char playlist_id[32];
                memcpy(playlist_id, id, id_len);
                playlist_id[id_len] = '\0';
                char *json = get_playlist_songs_json(playlist_id);
                if (!json)
                    return send_response(connection, "{\"error\":\"Could not retrieve playlist songs.\"}", MHD_HTTP_INTERNAL_SERVER_ERROR);
                enum MHD_Result result = send_response(connection, json, MHD_HTTP_OK);
                free(json);
                return result;
            }
        }

        if (strncmp(url, "/api/audio/", 11) == 0)
        {
            const char *id = url + 11;
            if (*id && strchr(id, '/') == NULL && strlen(id) < 32)
                return stream_song_audio(connection, id);
            return send_response(connection, "{\"error\":\"Invalid song ID.\"}", MHD_HTTP_BAD_REQUEST);
        }

        if (strcmp(url, "/api/liked-songs") == 0)
        {
            char *json = get_liked_songs_json();
            if (!json)
                return send_response(connection, "{\"error\":\"Could not retrieve liked songs.\"}", MHD_HTTP_INTERNAL_SERVER_ERROR);
            enum MHD_Result result = send_response(connection, json, MHD_HTTP_OK);
            free(json);
            return result;
        }

        if (strcmp(url, "/api/playlists") == 0)
        {
            char *json = get_playlists_json();

            if (!json)
            {
                return send_response(
                    connection,
                    "{\"error\":\"Could not retrieve playlists\"}",
                    MHD_HTTP_INTERNAL_SERVER_ERROR
                );
            }

            enum MHD_Result result =
                send_response(connection, json, MHD_HTTP_OK);

            free(json);
            return result;
        }

        return send_response(
            connection,
            "{\"error\":\"Endpoint not found\"}",
            MHD_HTTP_NOT_FOUND
        );
    }

    /* Audio upload uses multipart/form-data, not the JSON body handler. */
    if (strcmp(method, "POST") == 0 && strcmp(url, "/api/upload-audio") == 0)
        return handle_audio_upload(connection, upload_data, upload_data_size, con_cls);

    /*
       POST /api/songs and POST /api/playlists
    */

    int post_playlist_song = 0;
    char post_playlist_id[32] = "";
    if (strncmp(url, "/api/playlists/", 15) == 0)
    {
        const char *id = url + 15;
        const char *suffix = strstr(id, "/songs");
        if (suffix && strcmp(suffix, "/songs") == 0)
        {
            size_t n = (size_t)(suffix - id);
            if (n > 0 && n < sizeof(post_playlist_id))
            {
                memcpy(post_playlist_id, id, n);
                post_playlist_id[n] = '\0';
                post_playlist_song = 1;
            }
        }
    }

    if (strcmp(method, "POST") == 0 &&
        (strcmp(url, "/api/songs") == 0 ||
         strcmp(url, "/api/playlists") == 0 ||
         strcmp(url, "/api/liked-songs") == 0 ||
         strcmp(url, "/api/songs/update") == 0 ||
         post_playlist_song))
    {
        struct RequestData *data = *con_cls;

        /*
           First callback:
           allocate request body storage.
        */

        if (data == NULL)
        {
            data = calloc(1, sizeof(struct RequestData));

            if (!data)
            {
                return send_response(
                    connection,
                    "{\"error\":\"Out of memory\"}",
                    MHD_HTTP_INTERNAL_SERVER_ERROR
                );
            }

            data->body = malloc(MAX_REQUEST_SIZE + 1);

            if (!data->body)
            {
                free(data);

                return send_response(
                    connection,
                    "{\"error\":\"Out of memory\"}",
                    MHD_HTTP_INTERNAL_SERVER_ERROR
                );
            }

            data->body[0] = '\0';
            data->size = 0;

            *con_cls = data;

            return MHD_YES;
        }

        /*
           Receive POST body in chunks.
        */

        if (*upload_data_size > 0)
        {
            size_t chunk_size = *upload_data_size;

            if (data->size + chunk_size > MAX_REQUEST_SIZE)
            {
                return send_response(
                    connection,
                    "{\"error\":\"Request body too large\"}",
                    MHD_HTTP_CONTENT_TOO_LARGE
                );
            }

            memcpy(
                data->body + data->size,
                upload_data,
                chunk_size
            );

            data->size += chunk_size;
            data->body[data->size] = '\0';

            *upload_data_size = 0;

            return MHD_YES;
        }

        /*
           All POST body bytes received.
        */

        enum MHD_Result result;
        if (post_playlist_song)
            result = add_song_to_playlist(connection, post_playlist_id, data->body);
        else if (strcmp(url, "/api/liked-songs") == 0)
            result = like_song(connection, data->body);
        else if (strcmp(url, "/api/playlists") == 0)
            result = add_playlist(connection, data->body);
        else if (strcmp(url, "/api/songs/update") == 0)
            result = update_song_title(connection, data->body);
        else
            result = add_song(connection, data->body);

        free(data->body);
        free(data);
        *con_cls = NULL;

        return result;
    }


    if (strcmp(method, "DELETE") == 0 &&
        strncmp(url, "/api/songs/", 11) == 0)
    {
        const char *song_id = url + 11;
        if (*song_id && strchr(song_id, '/') == NULL && strlen(song_id) < 32)
            return delete_song(connection, song_id);
        return send_response(connection, "{\"error\":\"Invalid song ID.\"}", MHD_HTTP_BAD_REQUEST);
    }

    if (strcmp(method, "DELETE") == 0 &&
        strncmp(url, "/api/liked-songs/", 17) == 0)
    {
        const char *song_id = url + 17;
        if (*song_id && strchr(song_id, '/') == NULL && strlen(song_id) < 32)
            return unlike_song(connection, song_id);
        return send_response(connection, "{\"error\":\"Invalid song ID.\"}", MHD_HTTP_BAD_REQUEST);
    }

    if (strcmp(method, "DELETE") == 0 &&
        strncmp(url, "/api/playlists/", 15) == 0)
    {
        const char *id = url + 15;
        const char *suffix = strstr(id, "/songs/");
        if (suffix && strcmp(suffix, "/songs/") != 0)
        {
            size_t id_len = (size_t)(suffix - id);
            const char *song_id = suffix + 7;
            if (id_len > 0 && id_len < 32 && *song_id &&
                strchr(song_id, '/') == NULL && strlen(song_id) < 32)
            {
                char playlist_id[32];
                memcpy(playlist_id, id, id_len);
                playlist_id[id_len] = '\0';
                return remove_song_from_playlist(connection, playlist_id, song_id);
            }
            return send_response(connection, "{\"error\":\"Invalid playlist-song URL.\"}", MHD_HTTP_BAD_REQUEST);
        }

        if (*id && strchr(id, '/') == NULL && strlen(id) < 32)
            return delete_playlist(connection, id);

        return send_response(connection, "{\"error\":\"Invalid playlist URL.\"}", MHD_HTTP_BAD_REQUEST);
    }

    return send_response(
        connection,
        "{\"error\":\"Method not allowed or endpoint not found\"}",
        MHD_HTTP_METHOD_NOT_ALLOWED
    );
}

/* =========================================================
   REQUEST CLEANUP
   ========================================================= */


static void request_completed(
    void *cls,
    struct MHD_Connection *connection,
    void **con_cls,
    enum MHD_RequestTerminationCode toe)
{
    (void)cls;
    (void)connection;
    (void)toe;

    struct RequestData *data = *con_cls;

    if (data)
    {
        if (data->post_processor)
        {
            MHD_destroy_post_processor(data->post_processor);
        }

        if (data->upload_file)
        {
            fclose(data->upload_file);
        }

        free(data->body);
        free(data);
        *con_cls = NULL;
    }
}


/* =========================================================
   MAIN
   ========================================================= */

int main(void)
{
    struct MHD_Daemon *daemon;

    printf("Starting MelodyBox API server...\n");

    daemon = MHD_start_daemon(
        MHD_USE_INTERNAL_POLLING_THREAD,
        PORT,
        NULL,
        NULL,
        &request_handler,
        NULL,
        MHD_OPTION_NOTIFY_COMPLETED,
        &request_completed,
        NULL,
        MHD_OPTION_END
    );

    if (!daemon)
    {
        fprintf(stderr, "Could not start HTTP server.\n");
        return 1;
    }

    printf("MelodyBox API running at http://localhost:%d\n", PORT);
    printf("Songs:     http://localhost:%d/api/songs\n", PORT);
    printf("Playlists: http://localhost:%d/api/playlists\n", PORT);
    printf("POST songs: http://localhost:%d/api/songs\n", PORT);
    printf("POST audio upload: http://localhost:%d/api/upload-audio\n", PORT);
    printf("POST playlists: http://localhost:%d/api/playlists\n", PORT);
    printf("Playlist songs: http://localhost:%d/api/playlists/PL001/songs\n", PORT);
    printf("Audio stream:   http://localhost:%d/api/audio/S001\n", PORT);
    printf("Press Enter to stop the server.\n");

    getchar();

    MHD_stop_daemon(daemon);

    printf("MelodyBox API stopped.\n");

    return 0;
}
