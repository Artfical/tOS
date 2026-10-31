#include "common.h"
#include <sqlite3.h>
#include <unistd.h>
static int one_int(sqlite3 *db, const char *sql, long long *out)
{
    sqlite3_stmt *st;
    if (sqlite3_prepare_v2(db, sql, -1, &st, 0) != SQLITE_OK) { printf("  prepare failed: %s\n", sqlite3_errmsg(db)); return 0; }
    int ok = sqlite3_step(st) == SQLITE_ROW;
    if (ok) *out = sqlite3_column_int64(st, 0);
    sqlite3_finalize(st);
    return ok;
}
static int exec(sqlite3 *db, const char *sql)
{
    char *err = 0;
    int r = sqlite3_exec(db, sql, 0, 0, &err);
    if (r != SQLITE_OK) { printf("  %s: %s\n", sql, err); sqlite3_free(err); }
    return r == SQLITE_OK;
}
int main(void)
{
    sqlite3 *db; long long v;
    unlink("/tmp/test.db"); unlink("/tmp/test.db-journal");
    CHECK(sqlite3_open("/tmp/test.db", &db) == SQLITE_OK, "open a file database");
    CHECK(exec(db, "CREATE TABLE users(id INTEGER PRIMARY KEY, name TEXT, age INT);"
                   "CREATE TABLE orders(id INTEGER PRIMARY KEY, user_id INT, amount REAL);"), "create tables");
    CHECK(exec(db, "BEGIN"), "begin");
    sqlite3_stmt *iu, *io;
    sqlite3_prepare_v2(db, "INSERT INTO users(name, age) VALUES(?, ?)", -1, &iu, 0);
    sqlite3_prepare_v2(db, "INSERT INTO orders(user_id, amount) VALUES(?, ?)", -1, &io, 0);
    for (int i = 0; i < 5000; i++) {
        char name[32]; sprintf(name, "user%04d", i);
        sqlite3_bind_text(iu, 1, name, -1, SQLITE_TRANSIENT); sqlite3_bind_int(iu, 2, 18 + i % 60);
        sqlite3_step(iu); sqlite3_reset(iu);
        for (int k = 0; k < 4; k++) { sqlite3_bind_int(io, 1, i + 1); sqlite3_bind_double(io, 2, (double)(i % 97) + k * 0.5); sqlite3_step(io); sqlite3_reset(io); }
    }
    sqlite3_finalize(iu); sqlite3_finalize(io);
    CHECK(exec(db, "COMMIT;CREATE INDEX o_user ON orders(user_id);"), "commit and index");
    CHECK(one_int(db, "SELECT count(*) FROM orders", &v) && v == 20000, "20000 rows");
    CHECK(one_int(db, "SELECT count(*) FROM users u JOIN orders o ON o.user_id = u.id WHERE u.age = 30", &v) && v == 4 * 84, "a join");
    CHECK(one_int(db, "SELECT count(*) FROM (SELECT user_id, sum(amount) s FROM orders GROUP BY user_id HAVING s > 100)", &v) && v > 0, "group by and having");
    CHECK(one_int(db, "SELECT max(rn) FROM (SELECT row_number() OVER (PARTITION BY age ORDER BY id) rn FROM users)", &v) && v == 84, "a window function");
    CHECK(one_int(db, "SELECT json_extract('{\"a\":{\"b\":[10,20,30]}}', '$.a.b[1]')", &v) && v == 20, "JSON");
    CHECK(exec(db, "CREATE VIRTUAL TABLE docs USING fts5(title, body);"
                   "INSERT INTO docs VALUES('kernel','the tOS kernel boots'),('compiler','gcc compiles C and C++'),('shell','ash is the shell');"), "FTS5 table");
    CHECK(one_int(db, "SELECT count(*) FROM docs WHERE docs MATCH 'compiles OR boots'", &v) && v == 2, "full text search");
    CHECK(exec(db, "BEGIN;DELETE FROM orders WHERE user_id < 100;ROLLBACK;"), "a rollback");
    CHECK(one_int(db, "SELECT count(*) FROM orders", &v) && v == 20000, "the rollback undid the delete");
    CHECK(one_int(db, "SELECT count(*) FROM pragma_integrity_check WHERE integrity_check = 'ok'", &v) && v == 1, "the integrity check says ok");
    sqlite3_close(db);
    CHECK(sqlite3_open("/tmp/test.db", &db) == SQLITE_OK && one_int(db, "SELECT count(*) FROM users", &v) && v == 5000, "the data is still there after reopening");
    CHECK(exec(db, "INSERT INTO users(id, name, age) VALUES(1, 'dup', 1)") == 0, "a duplicate key is refused");
    sqlite3_close(db);
    printf("sqlite %s\n", sqlite3_libversion());
    DONE("sqlite");
}
