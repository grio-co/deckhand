// Encrypted source libraries: the SQLCipher the importer links must write and read SQLCipher 4 databases
// (default parameters), reject a wrong key, and the import must read an encrypted fixture end to end.
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>

#include "check.hpp"
#include "deckhand/import/import.hpp"
#include "deckhand/library/db.hpp"
#include "deckhand/library/library.hpp"

using namespace deckhand;
namespace fs = std::filesystem;

static std::string pragma_text(Db& db, const char* sql) {
  Stmt s(db, sql);
  return s.step() ? s.text(0) : std::string();
}

int main() {
#if !defined(DECKHAND_WITH_SQLCIPHER)
  CHECK(!import_v6_available());
  std::puts("note: built without SQLCipher; encrypted import test skipped");
  return 0;
#else
  CHECK(import_v6_available());
  const fs::path dir = fs::temp_directory_path() / "deckhand_test_import_cipher";
  fs::remove_all(dir);
  fs::create_directories(dir);
  const std::string src = (dir / "source.db").string();
  const std::string key = "test-key-not-a-real-one";

  {  // Create an encrypted database with SQLCipher 4 defaults and a minimal source-shaped schema.
    Db db;
    db.open(src);
    db.exec("PRAGMA key = '" + key + "';");
    CHECK(!pragma_text(db, "PRAGMA cipher_version").empty());
    // SQLCipher 4 defaults: PBKDF2-HMAC-SHA512 with 256000 iterations, HMAC-SHA512 pages, 4096-byte pages.
    CHECK(pragma_text(db, "PRAGMA cipher_default_kdf_iter") == "256000");
    CHECK(pragma_text(db, "PRAGMA cipher_default_page_size") == "4096");
    CHECK(pragma_text(db, "PRAGMA cipher_default_hmac_algorithm") == "HMAC_SHA512");
    CHECK(pragma_text(db, "PRAGMA cipher_default_kdf_algorithm") == "PBKDF2_HMAC_SHA512");
    db.exec(
        "CREATE TABLE djmdContent(ID TEXT, FolderPath TEXT, Title TEXT, ArtistID TEXT, AlbumID TEXT, GenreID TEXT,"
        " KeyID TEXT, BPM INTEGER, Length INTEGER, Rating INTEGER, ReleaseYear INTEGER, BitRate INTEGER,"
        " SampleRate INTEGER, Commnt TEXT, AnalysisDataPath TEXT, rb_local_deleted INTEGER);"
        "CREATE TABLE djmdArtist(ID TEXT, Name TEXT); CREATE TABLE djmdAlbum(ID TEXT, Name TEXT);"
        "CREATE TABLE djmdGenre(ID TEXT, Name TEXT); CREATE TABLE djmdKey(ID TEXT, ScaleName TEXT);"
        "CREATE TABLE djmdCue(ContentID TEXT, Kind INTEGER, InMsec REAL, OutMsec REAL, Comment TEXT,"
        " rb_local_deleted INTEGER);"
        "CREATE TABLE djmdPlaylist(ID TEXT, Name TEXT, Attribute INTEGER, ParentID TEXT, Seq INTEGER,"
        " rb_local_deleted INTEGER);"
        "CREATE TABLE djmdSongPlaylist(PlaylistID TEXT, ContentID TEXT, TrackNo INTEGER, rb_local_deleted INTEGER);"
        "INSERT INTO djmdArtist VALUES('a1','Cipher Artist');"
        "INSERT INTO djmdContent VALUES('c1','/music/cipher.mp3','Cipher Title','a1','','','',12800,200,3,2020,320,"
        "44100,'','',0);"
        "INSERT INTO djmdCue VALUES('c1',1,1500.0,-1.0,'start',0);");
  }

  {  // The file is not readable as plain SQLite.
    std::ifstream f(src, std::ios::binary);
    char head[16] = {};
    f.read(head, sizeof head);
    CHECK(std::string(head, 15) != "SQLite format 3");
  }

  {  // Reopen directly: the right key reads, the wrong key and no key fail.
    Db good;
    good.open(src);
    good.exec("PRAGMA key = '" + key + "';");
    CHECK(pragma_text(good, "SELECT count(*) FROM djmdContent") == "1");

    Db bad;
    bad.open(src);
    bad.exec("PRAGMA key = 'wrong-key';");
    bool failed = false;
    try {
      pragma_text(bad, "SELECT count(*) FROM djmdContent");
    } catch (const std::exception&) {
      failed = true;
    }
    CHECK(failed);

    Db none;
    none.open(src);
    failed = false;
    try {
      pragma_text(none, "SELECT count(*) FROM djmdContent");
    } catch (const std::exception&) {
      failed = true;
    }
    CHECK(failed);
  }

  {  // The importer: right key imports, wrong key reports it.
    Library lib((dir / "library.db").string());
    ImportOptions o;
    o.source_db = src;
    o.key = key;
    o.scratch_dir = (dir / "scratch").string();
    const ImportStats st = import_library_v6(lib, o);
    CHECK(st.tracks_added == 1);
    CHECK(st.cues == 1);
    CHECK(lib.track_id_for_path("/music/cipher.mp3").has_value());

    o.key = "wrong-key";
    bool failed = false;
    try {
      import_library_v6(lib, o);
    } catch (const std::runtime_error&) {
      failed = true;
    }
    CHECK(failed);
  }

  fs::remove_all(dir);
  std::puts("import cipher tests passed");
  return 0;
#endif
}
