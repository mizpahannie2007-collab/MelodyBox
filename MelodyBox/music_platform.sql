-- Drop existing tables if they exist
DROP TABLE IF EXISTS playlist_songs CASCADE;
DROP TABLE IF EXISTS user_playlists CASCADE;
DROP TABLE IF EXISTS listening_history CASCADE;
DROP TABLE IF EXISTS song_ratings CASCADE;
DROP TABLE IF EXISTS songs CASCADE;
DROP TABLE IF EXISTS albums CASCADE;
DROP TABLE IF EXISTS artists CASCADE;
DROP TABLE IF EXISTS genres CASCADE;
DROP TABLE IF EXISTS users CASCADE;

-- ============================================================================
-- TABLE 1: USERS TABLE
-- ============================================================================
CREATE TABLE users (
    user_id VARCHAR(10) PRIMARY KEY,
    username VARCHAR(50) NOT NULL UNIQUE,
    email VARCHAR(100) NOT NULL UNIQUE,
    password VARCHAR(255) NOT NULL,
    full_name VARCHAR(100) NOT NULL,
    date_of_birth DATE,
    country VARCHAR(50) DEFAULT 'India',
    registration_date DATE DEFAULT CURRENT_DATE,
    last_login TIMESTAMP,
    is_active CHAR(1) DEFAULT 'Y' CHECK (is_active IN ('Y', 'N')),
    profile_pic_path VARCHAR(255)
);

-- ============================================================================
-- TABLE 2: GENRES TABLE
-- ============================================================================
CREATE TABLE genres (
    genre_id VARCHAR(10) PRIMARY KEY,
    genre_name VARCHAR(50) NOT NULL UNIQUE,
    description VARCHAR(200),
    created_date DATE DEFAULT CURRENT_DATE
);

-- ============================================================================
-- TABLE 3: ARTISTS TABLE
-- ============================================================================
CREATE TABLE artists (
    artist_id VARCHAR(10) PRIMARY KEY,
    artist_name VARCHAR(100) NOT NULL,
    bio TEXT,
    country VARCHAR(50),
    debut_year INT CHECK (debut_year >= 1900 AND debut_year <= 2026),
    total_albums INT DEFAULT 0,
    followers_count INT DEFAULT 0,
    is_verified CHAR(1) DEFAULT 'N' CHECK (is_verified IN ('Y', 'N'))
);

-- ============================================================================
-- TABLE 4: ALBUMS TABLE (FOREIGN KEY: artist_id references artists)
-- ============================================================================
CREATE TABLE albums (
    album_id VARCHAR(10) PRIMARY KEY,
    album_name VARCHAR(100) NOT NULL,
    artist_id VARCHAR(10) NOT NULL,
    release_date DATE NOT NULL,
    total_tracks INT DEFAULT 0,
    album_type VARCHAR(20) CHECK (album_type IN ('Single', 'Album', 'EP', 'Compilation')),
    record_label VARCHAR(100),
    FOREIGN KEY (artist_id) REFERENCES artists(artist_id)
        ON DELETE CASCADE
        ON UPDATE CASCADE
);

-- ============================================================================
-- TABLE 5: SONGS TABLE (FOREIGN KEY: album_id, artist_id, genre_id)
-- ============================================================================
CREATE TABLE songs (
    song_id VARCHAR(10) PRIMARY KEY,
    song_title VARCHAR(150) NOT NULL,
    artist_id VARCHAR(10) NOT NULL,
    album_id VARCHAR(10),
    genre_id VARCHAR(10) NOT NULL,
    duration_seconds INT NOT NULL CHECK (duration_seconds > 0),
    release_year INT CHECK (release_year >= 1900 AND release_year <= 2026),
    language VARCHAR(30) NOT NULL,
    file_path VARCHAR(255) NOT NULL,
    file_size_mb NUMERIC(8,2) CHECK (file_size_mb > 0),
    play_count INT DEFAULT 0,
    likes_count INT DEFAULT 0,
    is_explicit CHAR(1) DEFAULT 'N' CHECK (is_explicit IN ('Y', 'N')),
    audio_quality VARCHAR(20) DEFAULT '320kbps',
    FOREIGN KEY (artist_id) REFERENCES artists(artist_id)
        ON DELETE CASCADE
        ON UPDATE CASCADE,
    FOREIGN KEY (album_id) REFERENCES albums(album_id)
        ON DELETE SET NULL
        ON UPDATE CASCADE,
    FOREIGN KEY (genre_id) REFERENCES genres(genre_id)
        ON DELETE RESTRICT
        ON UPDATE CASCADE
);

-- ============================================================================
-- TABLE 6: USER_PLAYLISTS TABLE (FOREIGN KEY: user_id references users)
-- ============================================================================
CREATE TABLE user_playlists (
    playlist_id VARCHAR(10) PRIMARY KEY,
    user_id VARCHAR(10) NOT NULL,
    playlist_name VARCHAR(100) NOT NULL,
    description VARCHAR(300),
    is_public CHAR(1) DEFAULT 'Y' CHECK (is_public IN ('Y', 'N')),
    created_date DATE DEFAULT CURRENT_DATE,
    total_songs INT DEFAULT 0,
    total_duration_minutes INT DEFAULT 0,
    cover_path VARCHAR(255),
    FOREIGN KEY (user_id) REFERENCES users(user_id)
        ON DELETE CASCADE
        ON UPDATE CASCADE
);

-- ============================================================================
-- TABLE 7: PLAYLIST_SONGS TABLE
-- ============================================================================
CREATE TABLE playlist_songs (
    playlist_id VARCHAR(10),
    song_id VARCHAR(10),
    added_date TIMESTAMP DEFAULT CURRENT_TIMESTAMP,
    song_order INT NOT NULL,
    PRIMARY KEY (playlist_id, song_id),
    FOREIGN KEY (playlist_id) REFERENCES user_playlists(playlist_id)
        ON DELETE CASCADE
        ON UPDATE CASCADE,
    FOREIGN KEY (song_id) REFERENCES songs(song_id)
        ON DELETE CASCADE
        ON UPDATE CASCADE
);

-- ============================================================================
-- TABLE 8: LISTENING_HISTORY TABLE
-- ============================================================================
CREATE TABLE listening_history (
    history_id VARCHAR(10) PRIMARY KEY,
    user_id VARCHAR(10) NOT NULL,
    song_id VARCHAR(10) NOT NULL,
    played_at TIMESTAMP DEFAULT CURRENT_TIMESTAMP,
    duration_played_seconds INT DEFAULT 0,
    completed CHAR(1) DEFAULT 'N' CHECK (completed IN ('Y', 'N')),
    device_type VARCHAR(30),
    FOREIGN KEY (user_id) REFERENCES users(user_id)
        ON DELETE CASCADE
        ON UPDATE CASCADE,
    FOREIGN KEY (song_id) REFERENCES songs(song_id)
        ON DELETE CASCADE
        ON UPDATE CASCADE
);

-- ============================================================================
-- TABLE 9: SONG_RATINGS TABLE
-- ============================================================================
CREATE TABLE song_ratings (
    rating_id VARCHAR(10) PRIMARY KEY,
    user_id VARCHAR(10) NOT NULL,
    song_id VARCHAR(10) NOT NULL,
    rating INT NOT NULL CHECK (rating >= 1 AND rating <= 5),
    review_text TEXT,
    rating_date DATE DEFAULT CURRENT_DATE,
    is_liked CHAR(1) DEFAULT 'N' CHECK (is_liked IN ('Y', 'N')),
    FOREIGN KEY (user_id) REFERENCES users(user_id)
        ON DELETE CASCADE
        ON UPDATE CASCADE,
    FOREIGN KEY (song_id) REFERENCES songs(song_id)
        ON DELETE CASCADE
        ON UPDATE CASCADE,
    UNIQUE(user_id, song_id) -- One rating per user per song
);

-- ============================================================================
-- INSERT SAMPLE DATA
-- ============================================================================

-- Insert Users
INSERT INTO users (user_id, username, email, password, full_name, date_of_birth, country, registration_date, last_login, is_active) VALUES
('U001', 'music_lover', 'harsh@email.com', 'pass123', 'Harsh Kumar', '2000-05-15', 'India', '2024-01-10', NOW(), 'Y'),
('U002', 'melody_fan', 'priya@email.com', 'pass456', 'Priya Sharma', '1998-08-22', 'India', '2024-03-20', NOW(), 'Y'),
('U003', 'rock_star', 'rohit@email.com', 'pass789', 'Rohit Singh', '2001-12-30', 'India', '2024-06-15', NOW(), 'Y');

-- Insert Genres
INSERT INTO genres VALUES
('G001', 'Hindi Pop', 'Modern Hindi pop music with catchy beats', CURRENT_DATE),
('G002', 'Hindi Classical', 'Traditional Indian classical music', CURRENT_DATE),
('G003', 'English Pop', 'Contemporary English pop hits', CURRENT_DATE),
('G004', 'Rock', 'Classic and modern rock music', CURRENT_DATE);

-- ============================================================================
-- USEFUL VIEWS FOR MP3 PLAYER APPLICATION
-- ============================================================================

-- View: Popular Songs
CREATE VIEW vw_popular_songs AS
SELECT
    s.song_id,
    s.song_title,
    a.artist_name,
    g.genre_name,
    s.play_count,
    s.likes_count,
    s.duration_seconds
FROM songs s
JOIN artists a ON s.artist_id = a.artist_id
JOIN genres g ON s.genre_id = g.genre_id
ORDER BY s.play_count DESC;

-- View: User's Favorite Songs
CREATE VIEW vw_user_favorites AS
SELECT
    u.username,
    s.song_title,
    a.artist_name,
    sr.rating,
    sr.rating_date
FROM song_ratings sr
JOIN users u ON sr.user_id = u.user_id
JOIN songs s ON sr.song_id = s.song_id
JOIN artists a ON s.artist_id = a.artist_id
WHERE sr.rating = 5;

-- View: Playlist Details
CREATE VIEW vw_playlist_details AS
SELECT
    up.playlist_name,
    u.username AS created_by,
    s.song_title,
    a.artist_name,
    ps.added_date,
    ps.song_order
FROM user_playlists up
JOIN users u ON up.user_id = u.user_id
JOIN playlist_songs ps ON up.playlist_id = ps.playlist_id
JOIN songs s ON ps.song_id = s.song_id
JOIN artists a ON s.artist_id = a.artist_id
ORDER BY up.playlist_id, ps.song_order;

-- ============================================================================
-- INDEXES
-- ============================================================================
CREATE INDEX idx_songs_genre ON songs(genre_id);
CREATE INDEX idx_songs_artist ON songs(artist_id);
CREATE INDEX idx_songs_album ON songs(album_id);
CREATE INDEX idx_listening_history_user ON listening_history(user_id);
CREATE INDEX idx_listening_history_song ON listening_history(song_id);
CREATE INDEX idx_playlist_songs_playlist ON playlist_songs(playlist_id);
CREATE INDEX idx_song_ratings_user ON song_ratings(user_id);
CREATE INDEX idx_song_ratings_song ON song_ratings(song_id);

-- ============================================================================
-- STORED PROCEDURE: Add song to playlist
-- ============================================================================
CREATE OR REPLACE FUNCTION add_song_to_playlist(
    p_playlist_id VARCHAR,
    p_song_id VARCHAR
) RETURNS VOID AS $$
BEGIN
    INSERT INTO playlist_songs (playlist_id, song_id, song_order)
    VALUES (
        p_playlist_id,
        p_song_id,
        (SELECT COALESCE(MAX(song_order), 0) + 1
         FROM playlist_songs
         WHERE playlist_id = p_playlist_id)
    );

    UPDATE user_playlists
    SET total_songs = total_songs + 1
    WHERE playlist_id = p_playlist_id;
END;
$$ LANGUAGE plpgsql;

-- ============================================================================
-- TRIGGER: Update play count when song is played
-- ============================================================================
CREATE OR REPLACE FUNCTION update_play_count()
RETURNS TRIGGER AS $$
BEGIN
    IF NEW.completed = 'Y' THEN
        UPDATE songs
        SET play_count = play_count + 1
        WHERE song_id = NEW.song_id;
    END IF;
    RETURN NEW;
END;
$$ LANGUAGE plpgsql;

CREATE TRIGGER trg_update_play_count
AFTER INSERT ON listening_history
FOR EACH ROW
EXECUTE FUNCTION update_play_count();

-- ============================================================================
-- DATABASE SUMMARY
-- ============================================================================
SELECT 'Database created successfully!' AS Status;
SELECT COUNT(*) AS Total_Users FROM users;
SELECT COUNT(*) AS Total_Genres FROM genres;
SELECT COUNT(*) AS Total_Artists FROM artists;
SELECT COUNT(*) AS Total_Albums FROM albums;
SELECT COUNT(*) AS Total_Songs FROM songs;
SELECT COUNT(*) AS Total_Playlists FROM user_playlists;

-- Display Songs by Genre
SELECT g.genre_name, COUNT(s.song_id) AS song_count
FROM genres g
LEFT JOIN songs s ON g.genre_id = s.genre_id
GROUP BY g.genre_name
ORDER BY g.genre_name;
