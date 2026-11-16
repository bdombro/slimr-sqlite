#define SLIMR_TEST_MAIN
#include "sqlite.hpp"
#include "test.hpp"

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <limits>
#include <stdexcept>
#include <thread>
#include <variant>

using namespace slimr;

TEST_SUITE("SQLite Database") {
    TEST_CASE("Basic CRUD, statement cache, and FTS5") {
        Database db(":memory:");
        db.exec("CREATE TABLE items (id INTEGER PRIMARY KEY, title TEXT, score REAL);");

        // Insert
        auto r1 = db.run("INSERT INTO items (title, score) VALUES (?, ?);", {std::string("first"), 10.5});
        CHECK(r1.changes == 1);
        CHECK(r1.lastInsertRowid == 1);

        auto r2 = db.run("INSERT INTO items (title, score) VALUES (?, ?);", {std::string("second"), 20.0});
        CHECK(r2.changes == 1);
        CHECK(r2.lastInsertRowid == 2);

        // Get single
        auto row = db.get("SELECT * FROM items WHERE id = ?;", {int64_t(1)});
        CHECK(row.has_value());
        CHECK((*row)["title"].get<std::string>() == "first");
        CHECK((*row)["score"].get<double>() == 10.5);

        // All rows
        auto rows = db.all("SELECT * FROM items ORDER BY id;");
        CHECK(rows.size() == 2);
        CHECK(rows[1]["title"].get<std::string>() == "second");

        // Pluck and pluckAll
        auto title = db.pluck<std::string>("SELECT title FROM items WHERE id = ?;", {int64_t(2)});
        CHECK(title.has_value());
        CHECK(*title == "second");

        auto allScores = db.pluckAll<double>("SELECT score FROM items ORDER BY id;");
        CHECK(allScores.size() == 2);
        CHECK(allScores[0] == 10.5);
        CHECK(allScores[1] == 20.0);

        // FTS5 check
        db.exec("CREATE VIRTUAL TABLE docs USING fts5(content);");
        db.run("INSERT INTO docs (content) VALUES (?);", {std::string("apple banana orange")});
        db.run("INSERT INTO docs (content) VALUES (?);", {std::string("banana kiwi peach")});

        auto matchCount = db.pluck<int64_t>("SELECT count(*) FROM docs WHERE docs MATCH ?;", {std::string("banana")});
        CHECK(matchCount.value_or(0) == 2);
    }

    TEST_CASE("Named parameters binding") {
        Database db(":memory:");
        db.exec("CREATE TABLE users (id INTEGER PRIMARY KEY, name TEXT, email TEXT);");

        // Bare name, colon, dollar, at prefix
        std::map<std::string, SqlValue> params1{
            {"name", std::string("Alice")},
            {"email", std::string("alice@example.com")}};
        db.run("INSERT INTO users (name, email) VALUES (:name, :email);", params1);

        std::map<std::string, SqlValue> params2{
            {":name", std::string("Bob")},
            {"email", std::string("bob@example.com")}};
        db.run("INSERT INTO users (name, email) VALUES ($name, @email);", params2);

        auto count = db.pluck<int64_t>("SELECT count(*) FROM users;");
        CHECK(count.value_or(0) == 2);

        auto bob = db.get("SELECT name FROM users WHERE email = :email;", std::map<std::string, SqlValue>{{"email", std::string("bob@example.com")}});
        CHECK(bob.has_value());
        CHECK((*bob)["name"].get<std::string>() == "Bob");
    }

    TEST_CASE("Migrations: vector (1..N)") {
        Database db(":memory:", SqliteOptions{
                                    .migrations = std::vector<std::string>{
                                        "CREATE TABLE v1 (x INT);",
                                        "CREATE TABLE v2 (y TEXT);"}});

        int64_t ver = db.pluck<int64_t>("PRAGMA user_version;").value_or(0);
        CHECK(ver == 2);

        db.run("INSERT INTO v1 VALUES (10);");
        db.run("INSERT INTO v2 VALUES ('ok');");
        CHECK(db.pluck<int64_t>("SELECT x FROM v1;").value_or(0) == 10);
        CHECK(db.pluck<std::string>("SELECT y FROM v2;").value_or("") == "ok");
    }

    TEST_CASE("Migrations: squashed map and stale version check") {
        // Squashed migration starting at version 10
        Database db(":memory:", SqliteOptions{
                                    .migrations = std::map<int, std::string>{
                                        {10, "CREATE TABLE baseline (id INT);"}}});
        CHECK(db.pluck<int64_t>("PRAGMA user_version;").value_or(0) == 10);

        // Stale database test: user_version is 5, but min migration is 10 -> throws!
        testing::TempDirGuard staleDir("stale_db_");
        std::string stalePath = (staleDir.path / "stale.db").string();
        {
            Database staleDb(stalePath);
            staleDb.exec("PRAGMA user_version = 5;");
        }
        CHECK_THROWS_AS((Database(stalePath, SqliteOptions{
                                                 .migrations = std::map<int, std::string>{
                                                     {10, "CREATE TABLE baseline (id INT);"}}})),
                        std::runtime_error);
    }

    TEST_CASE("Transactions and nested Savepoints") {
        Database db(":memory:");
        db.exec("CREATE TABLE logs (msg TEXT);");

        // Successful transaction
        db.transaction([&](Tx &tx) {
            tx.run("INSERT INTO logs VALUES ('tx1');");
        });
        CHECK(db.pluck<int64_t>("SELECT count(*) FROM logs;").value_or(0) == 1);

        // Rollback transaction on throw
        try {
            db.transaction([&](Tx &tx) {
                tx.run("INSERT INTO logs VALUES ('tx2');");
                throw std::runtime_error("abort");
            });
        } catch (...) {
        }
        CHECK(db.pluck<int64_t>("SELECT count(*) FROM logs;").value_or(0) == 1);

        // Nested savepoint commit and partial rollback
        db.transaction([&](Tx &outer) {
            outer.run("INSERT INTO logs VALUES ('outer');");

            // Successful inner savepoint
            outer.transaction([&](Tx &inner) {
                inner.run("INSERT INTO logs VALUES ('inner1');");
            });

            // Failed inner savepoint
            try {
                outer.transaction([&](Tx &inner2) {
                    inner2.run("INSERT INTO logs VALUES ('inner2');");
                    throw std::runtime_error("inner abort");
                });
            } catch (...) {
            }
        });

        auto msgs = db.pluckAll<std::string>("SELECT msg FROM logs ORDER BY rowid;");
        CHECK(msgs.size() == 3);
        CHECK(msgs[0] == "tx1");
        CHECK(msgs[1] == "outer");
        CHECK(msgs[2] == "inner1");
    }

    TEST_CASE("Custom scalar function") {
        Database db(":memory:");
        db.registerFunction("add_ten", [](const std::vector<SqlValue> &args) -> SqlValue {
            if (args.empty())
                return int64_t(0);
            int64_t val = std::get<int64_t>(args[0]);
            return val + 10;
        });

        auto res = db.pluck<int64_t>("SELECT add_ten(32);");
        CHECK(res.value_or(0) == 42);
    }

    TEST_CASE("iterateLimited with columns, limit, and readOnlyConnection") {
        testing::TempDirGuard dirGuard("sqlite_test_");
        std::string dbPath = (dirGuard.path / "test.db").string();

        {
            Database db(dbPath);
            db.exec("CREATE TABLE records (id INT, val TEXT);");
            for (int i = 1; i <= 5; ++i) {
                db.run("INSERT INTO records VALUES (?, ?);", {int64_t(i), "val" + std::to_string(i)});
            }
        }

        Database db(dbPath);
        // Test limit and truncation
        auto iterRes = db.iterateLimited("SELECT * FROM records ORDER BY id;", IterateOptions{
                                                                                   .limit = 3,
                                                                                   .readOnlyConnection = true,
                                                                                   .setupSql = "PRAGMA query_only = ON;"});

        CHECK(iterRes.columns.size() == 2);
        CHECK(iterRes.columns[0] == "id");
        CHECK(iterRes.columns[1] == "val");
        CHECK(iterRes.rows.size() == 3);
        CHECK(iterRes.truncated == true);
    }

    TEST_CASE("Database online backup") {
        testing::TempDirGuard dirGuard("sqlite_backup_");
        std::string srcPath = (dirGuard.path / "src.db").string();
        std::string dstPath = (dirGuard.path / "dst.db").string();

        Database src(srcPath);
        src.exec("CREATE TABLE data (content TEXT);");
        src.run("INSERT INTO data VALUES ('backup this payload');");

        src.backup(dstPath);

        Database dst(dstPath);
        auto payload = dst.pluck<std::string>("SELECT content FROM data;");
        CHECK(payload.has_value());
        CHECK(*payload == "backup this payload");
    }

    TEST_CASE("Verified WAL/NORMAL/FK/busy_timeout pragmas on writable and readOnly DBs") {
        testing::TempDirGuard dirGuard("sqlite_pragmas_");
        std::string dbPath = (dirGuard.path / "pragmas.db").string();

        {
            Database db(dbPath);
            CHECK(db.pluck<std::string>("PRAGMA journal_mode;").value_or("") == "wal");
            CHECK(db.pluck<int64_t>("PRAGMA synchronous;").value_or(-1) == 1);
            CHECK(db.pluck<int64_t>("PRAGMA foreign_keys;").value_or(0) == 1);
            CHECK(db.pluck<int64_t>("PRAGMA temp_store;").value_or(0) == 2);
            CHECK(db.pluck<int64_t>("PRAGMA busy_timeout;").value_or(0) == 5000);
            db.exec("CREATE TABLE t (id INTEGER PRIMARY KEY);");
        }

        {
            Database ro(dbPath, SqliteOptions{.readOnly = true});
            CHECK(ro.pluck<int64_t>("PRAGMA busy_timeout;").value_or(0) == 5000);
            CHECK(ro.pluck<int64_t>("PRAGMA foreign_keys;").value_or(0) == 1);
        }

        std::string missingPath = (dirGuard.path / "does_not_exist.db").string();
        CHECK_THROWS_AS((Database(missingPath, SqliteOptions{.readOnly = true})), std::runtime_error);
    }

    TEST_CASE("get() and pluck() step only the first row and reuse cached statements") {
        Database db(":memory:");
        db.exec("CREATE TABLE items (id INTEGER PRIMARY KEY);");
        db.transaction([&](Tx &tx) {
            for (int64_t i = 1; i <= 100; ++i) {
                tx.run("INSERT INTO items (id) VALUES (?);", {i});
            }
        });

        int64_t calls = 0;
        db.registerFunction("count_step", [&](const std::vector<SqlValue> &args) -> SqlValue {
            ++calls;
            return args[0]; }, /*deterministic=*/false);

        auto row = db.get("SELECT count_step(id) AS val FROM items ORDER BY id;");
        CHECK(row.has_value());
        CHECK((*row)["val"].get<int64_t>() == 1);
        CHECK(calls == 1);

        calls = 0;
        auto val = db.pluck<int64_t>("SELECT count_step(id) FROM items ORDER BY id;");
        CHECK(val.has_value());
        CHECK(*val == 1);
        CHECK(calls == 1);

        // Reuse the same cached statement for get() again, then full all()
        calls = 0;
        auto val2 = db.pluck<int64_t>("SELECT count_step(id) FROM items ORDER BY id;");
        CHECK(val2.value_or(0) == 1);
        CHECK(calls == 1);

        calls = 0;
        auto allVals = db.pluckAll<int64_t>("SELECT count_step(id) FROM items ORDER BY id;");
        CHECK(allVals.size() == 100);
        CHECK(calls == 100);
    }

    TEST_CASE("Embedded NUL bytes round-trip in TEXT across queries and scalar functions") {
        Database db(":memory:");
        db.exec("CREATE TABLE payloads (id INTEGER PRIMARY KEY, txt TEXT NOT NULL);");
        std::string embedded("a\0b", 3);
        std::string leading("\0\0z", 3);

        db.run("INSERT INTO payloads (txt) VALUES (?);", {embedded});
        db.run("INSERT INTO payloads (txt) VALUES (?);", {leading});

        auto p1 = db.pluck<std::string>("SELECT txt FROM payloads WHERE id = 1;");
        REQUIRE(p1.has_value());
        CHECK(p1->size() == 3);
        CHECK(*p1 == embedded);

        auto p2 = db.get("SELECT txt FROM payloads WHERE id = 2;");
        REQUIRE(p2.has_value());
        auto s2 = (*p2)["txt"].get<std::string>();
        CHECK(s2.size() == 3);
        CHECK(s2 == leading);

        auto sv = db.pluck<SqlValue>("SELECT txt FROM payloads WHERE id = 1;");
        REQUIRE(sv.has_value());
        REQUIRE(std::holds_alternative<std::string>(*sv));
        CHECK(std::get<std::string>(*sv) == embedded);

        db.registerFunction("echo_txt", [](const std::vector<SqlValue> &args) -> SqlValue {
            return args.at(0);
        });
        auto fnOut = db.pluck<std::string>("SELECT echo_txt(?);", {embedded});
        REQUIRE(fnOut.has_value());
        CHECK(fnOut->size() == 3);
        CHECK(*fnOut == embedded);
    }

    TEST_CASE("Strict column conversion errors and optional NULL handling") {
        Database db(":memory:");
        db.exec("CREATE TABLE vals (id INTEGER PRIMARY KEY, s TEXT, n INTEGER);");
        db.run("INSERT INTO vals (s, n) VALUES ('not_int', 300);");
        db.run("INSERT INTO vals (s, n) VALUES (NULL, NULL);");

        CHECK_THROWS_AS(db.pluck<int64_t>("SELECT s FROM vals WHERE id = 1;"), std::runtime_error);
        CHECK_THROWS_AS(db.pluck<int8_t>("SELECT n FROM vals WHERE id = 1;"), std::overflow_error);
        CHECK_THROWS_AS(db.pluck<int64_t>("SELECT n FROM vals WHERE id = 2;"), std::runtime_error);
        CHECK_THROWS_AS(db.pluck<std::string>("SELECT s FROM vals WHERE id = 2;"), std::runtime_error);

        auto optNull = db.pluck<std::optional<int64_t>>("SELECT n FROM vals WHERE id = 2;");
        REQUIRE(optNull.has_value());
        CHECK(!optNull->has_value());

        auto optPresent = db.pluck<std::optional<int64_t>>("SELECT n FROM vals WHERE id = 1;");
        REQUIRE(optPresent.has_value());
        REQUIRE(optPresent->has_value());
        CHECK(**optPresent == 300);
    }

    TEST_CASE("Statement cache does not evict in-use statements during reentrant queries") {
        Database db(":memory:");
        db.exec("CREATE TABLE nums (v INTEGER);");
        db.run("INSERT INTO nums VALUES (7);");

        db.registerFunction("nested_lookup", [&](const std::vector<SqlValue> &args) -> SqlValue {
            // Fill >256 distinct statements while the outer query holds an in-use cached statement
            for (int i = 0; i < 270; ++i) {
                auto v = db.pluck<int64_t>("SELECT " + std::to_string(i) + ";");
                (void)v;
            }
            return args[0]; }, /*deterministic=*/false);

        auto res = db.pluck<int64_t>("SELECT nested_lookup(v) FROM nums;");
        CHECK(res.value_or(0) == 7);
    }

    TEST_CASE("Concurrent WAL reader/writer across independent connections") {
        CHECK(sqlite3_threadsafe() != 0);

        testing::TempDirGuard dirGuard("sqlite_conc_");
        std::string dbPath = (dirGuard.path / "conc.db").string();

        {
            Database init(dbPath);
            init.exec("CREATE TABLE events (id INTEGER PRIMARY KEY, seq INTEGER NOT NULL);");
        }

        constexpr int kTotalWrites = 80;
        std::atomic<bool> writerDone{false};
        std::atomic<int> readErrors{0};
        std::atomic<int> writeErrors{0};
        int64_t maxObserved = 0;

        std::thread writer([&]() {
            try {
                Database wdb(dbPath);
                for (int i = 1; i <= kTotalWrites; ++i) {
                    wdb.transaction([&](Tx &tx) {
                        tx.run("INSERT INTO events (seq) VALUES (?);", {int64_t(i)});
                    });
                }
            } catch (...) {
                writeErrors.fetch_add(1);
            }
            writerDone.store(true);
        });

        std::thread reader([&]() {
            try {
                Database rdb(dbPath, SqliteOptions{.readOnly = true});
                while (!writerDone.load()) {
                    auto c = rdb.pluck<int64_t>("SELECT count(*) FROM events;");
                    if (c && *c > maxObserved) {
                        maxObserved = *c;
                    }
                    auto iter = rdb.iterateLimited("SELECT seq FROM events ORDER BY id DESC;", IterateOptions{
                                                                                                   .limit = 5,
                                                                                                   .readOnlyConnection = true});
                    (void)iter;
                }
            } catch (...) {
                readErrors.fetch_add(1);
            }
        });

        writer.join();
        reader.join();

        CHECK(writeErrors.load() == 0);
        CHECK(readErrors.load() == 0);

        Database verify(dbPath, SqliteOptions{.readOnly = true});
        CHECK(verify.pluck<int64_t>("SELECT count(*) FROM events;").value_or(0) == kTotalWrites);
    }

    TEST_CASE("SqlValue types, column conversions, and Glaze struct extraction") {
        struct ItemRow {
            int64_t id = 0;
            std::string name;
            double ratio = 0.0;
        };
        struct EmbeddedMeta {
            std::string tag;
        };

        Database db(":memory:");
        db.exec("CREATE TABLE typed (id INTEGER PRIMARY KEY, b BLOB, f REAL, flag INTEGER, n TEXT, meta TEXT);");

        std::vector<uint8_t> blobData{0x01, 0x00, 0xFE, 0x7F};
        db.run("INSERT INTO typed (b, f, flag, n, meta) VALUES (?, ?, ?, ?, ?);",
               {blobData, 3.25, true, std::monostate{}, std::string(R"({"tag":"ok"})")});
        db.run("INSERT INTO typed (b, f, flag, n, meta) VALUES (X'', ?, ?, ?, ?);",
               {int64_t(4), false, std::string("alpha"), std::string("not-json")});

        auto rows = db.all("SELECT * FROM typed ORDER BY id;");
        REQUIRE(rows.size() == 2);
        CHECK(rows[0]["n"].is_null());
        CHECK(rows[0]["b"].get<std::string>().size() == 4);
        CHECK(rows[1]["b"].get<std::string>().empty());

        auto vals = db.pluckAll<SqlValue>("SELECT b FROM typed ORDER BY id;");
        REQUIRE(vals.size() == 2);
        CHECK(std::get<std::vector<uint8_t>>(vals[0]) == blobData);
        CHECK(std::get<std::vector<uint8_t>>(vals[1]).empty());

        auto fVal = db.pluck<SqlValue>("SELECT f FROM typed WHERE id = 1;");
        REQUIRE(fVal.has_value());
        CHECK(std::get<double>(*fVal) == doctest::Approx(3.25));

        auto nVal = db.pluck<SqlValue>("SELECT n FROM typed WHERE id = 1;");
        REQUIRE(nVal.has_value());
        CHECK(std::holds_alternative<std::monostate>(*nVal));

        CHECK(db.pluck<double>("SELECT f FROM typed WHERE id = 1;").value_or(0.0) == doctest::Approx(3.25));
        CHECK(db.pluck<double>("SELECT flag FROM typed WHERE id = 1;").value_or(0.0) == doctest::Approx(1.0));
        CHECK(db.pluck<int>("SELECT flag FROM typed WHERE id = 1;").value_or(0) == 1);
        CHECK(db.pluck<bool>("SELECT flag FROM typed WHERE id = 1;").value_or(false) == true);
        CHECK(db.pluck<bool>("SELECT flag FROM typed WHERE id = 2;").value_or(true) == false);

        CHECK_FALSE(db.get("SELECT * FROM typed WHERE id = 999;").has_value());
        CHECK_FALSE(db.pluck<int64_t>("SELECT id FROM typed WHERE id = 999;").has_value());

        auto jsonCol = db.pluck<Json>("SELECT meta FROM typed WHERE id = 1;");
        REQUIRE(jsonCol.has_value());
        CHECK(jsonCol->get<std::string>() == R"({"tag":"ok"})");

        auto structRow = db.get<ItemRow>("SELECT id, n AS name, f AS ratio FROM typed WHERE id = 2;");
        REQUIRE(structRow.has_value());
        CHECK(structRow->id == 2);
        CHECK(structRow->name == "alpha");
        CHECK(structRow->ratio == doctest::Approx(4.0));

        CHECK_THROWS_AS(db.pluck<int>("SELECT f FROM typed WHERE id = 1;"), std::runtime_error);
        CHECK_THROWS_AS(db.pluck<double>("SELECT n FROM typed WHERE id = 2;"), std::runtime_error);
        CHECK_THROWS_AS(db.pluck<EmbeddedMeta>("SELECT meta FROM typed WHERE id = 2;"), std::runtime_error);

        db.run("CREATE TABLE big (v INTEGER);");
        db.run("INSERT INTO big (v) VALUES (?);", {int64_t(std::numeric_limits<int>::max()) + 1});
        CHECK_THROWS_AS(db.pluck<int>("SELECT v FROM big;"), std::overflow_error);
    }

    TEST_CASE("Named parameter prefixes and URI memory paths") {
        Database db("file::memory:");
        db.exec("CREATE TABLE np (a INTEGER, b TEXT, c REAL);");
        db.run("INSERT INTO np (a, b, c) VALUES (:a, $b, @c);",
               {{"a", int64_t(7)}, {"b", std::string("hi")}, {"@c", 2.5}, {"unused", int64_t(99)}});

        auto row = db.get("SELECT a, b, c FROM np WHERE a = $a AND b = @b;",
                          {{"$a", int64_t(7)}, {"b", std::string("hi")}});
        REQUIRE(row.has_value());
        CHECK((*row)["a"].get<int64_t>() == 7);
        CHECK((*row)["b"].get<std::string>() == "hi");
        CHECK((*row)["c"].get<double>() == doctest::Approx(2.5));

        Database sharedMem("file:memdb_cov?mode=memory&cache=shared");
        sharedMem.exec("CREATE TABLE t (x INTEGER);");
        CHECK(sharedMem.pluck<std::string>("PRAGMA journal_mode;").value_or("") == "memory");
    }

    TEST_CASE("Database lifecycle, move semantics, pragmas, and open/exec errors") {
        testing::TempDirGuard dirGuard("sqlite_life_");
        CHECK_THROWS_AS(Database(dirGuard.path.string()), std::runtime_error);
        CHECK_THROWS_AS(Database((dirGuard.path / "nonexistent.db").string(), SqliteOptions{.readOnly = true}),
                        std::runtime_error);

        Database a(":memory:", SqliteOptions{
                                   .enableDefaultPragmas = false,
                                   .pragmas = std::string("PRAGMA foreign_keys = ON;"),
                               });
        CHECK(a.handle() != nullptr);
        CHECK(a.path() == ":memory:");
        CHECK(a.pluck<int64_t>("PRAGMA foreign_keys;").value_or(0) == 1);
        a.exec("CREATE TABLE t (id INTEGER PRIMARY KEY);");
        a.run("INSERT INTO t (id) VALUES (?);", {int64_t(1)});

        Database b(std::move(a));
        CHECK(a.handle() == nullptr);
        CHECK(b.handle() != nullptr);
        CHECK(b.pluck<int64_t>("SELECT id FROM t;").value_or(0) == 1);

        Database c(":memory:");
        c = std::move(b);
        CHECK(b.handle() == nullptr);
        CHECK(c.pluck<int64_t>("SELECT id FROM t;").value_or(0) == 1);

        CHECK_THROWS_AS(c.exec("INVALID SQL SYNTAX;"), std::runtime_error);
    }

    TEST_CASE("Empty migrations and migration rollback on error") {
        Database emptyVec(":memory:", SqliteOptions{.migrations = std::vector<std::string>{}});
        CHECK(emptyVec.pluck<int64_t>("PRAGMA user_version;").value_or(-1) == 0);

        Database emptyMap(":memory:", SqliteOptions{.migrations = std::map<int, std::string>{}});
        CHECK(emptyMap.pluck<int64_t>("PRAGMA user_version;").value_or(-1) == 0);

        CHECK_THROWS_AS(
            Database(":memory:", SqliteOptions{
                                     .migrations = std::vector<std::string>{
                                         "CREATE TABLE m1 (id INTEGER);",
                                         "INVALID SQL;",
                                     },
                                 }),
            std::runtime_error);
    }

    TEST_CASE("Tx methods, transaction modes, non-void return, and rollback") {
        Database db(":memory:");
        db.exec("CREATE TABLE tx_t (id INTEGER PRIMARY KEY, name TEXT);");

        auto total = db.transaction([](Tx &tx) -> int64_t {
            CHECK(tx.depth() == 1);
            tx.exec("INSERT INTO tx_t (id, name) VALUES (1, 'one');");
            auto r1 = tx.run("INSERT INTO tx_t (id, name) VALUES (?, ?);", {int64_t(2), std::string("two")});
            CHECK(r1.changes == 1);
            auto r2 = tx.run("INSERT INTO tx_t (id, name) VALUES (:id, :name);",
                             {{"id", int64_t(3)}, {"name", std::string("three")}});
            CHECK(r2.changes == 1);

            auto g1 = tx.get("SELECT name FROM tx_t WHERE id = ?;", {int64_t(1)});
            REQUIRE(g1.has_value());
            CHECK((*g1)["name"].get<std::string>() == "one");

            std::map<std::string, SqlValue> p2{{"id", int64_t(2)}};
            std::map<std::string, SqlValue> p3{{"id", int64_t(3)}};
            auto g2 = tx.get("SELECT name FROM tx_t WHERE id = :id;", p2);
            REQUIRE(g2.has_value());
            CHECK((*g2)["name"].get<std::string>() == "two");

            CHECK(tx.all("SELECT * FROM tx_t WHERE id >= ?;", {int64_t(2)}).size() == 2);
            CHECK(tx.all("SELECT * FROM tx_t WHERE id = :id;", p3).size() == 1);
            CHECK(tx.pluck<std::string>("SELECT name FROM tx_t WHERE id = ?;", {int64_t(1)}).value_or("") == "one");
            CHECK(tx.pluck<std::string>("SELECT name FROM tx_t WHERE id = :id;", p2).value_or("") == "two");
            CHECK(tx.pluckAll<int64_t>("SELECT id FROM tx_t WHERE id <= ? ORDER BY id;", {int64_t(2)}).size() == 2);
            CHECK(tx.pluckAll<int64_t>("SELECT id FROM tx_t WHERE id = :id;", p3).size() == 1);

            auto inner = tx.transaction([](Tx &nested) -> int64_t {
                CHECK(nested.depth() == 2);
                nested.run("INSERT INTO tx_t (id, name) VALUES (4, 'four');");
                return nested.pluck<int64_t>("SELECT count(*) FROM tx_t;").value_or(0);
            });
            CHECK(inner == 4);
            return inner;
        },
                                    TransactionMode::Deferred);
        CHECK(total == 4);

        db.transaction([](Tx &tx) {
            tx.run("INSERT INTO tx_t (id, name) VALUES (5, 'five');");
        },
                       TransactionMode::Exclusive);
        CHECK(db.pluck<int64_t>("SELECT count(*) FROM tx_t;").value_or(0) == 5);

        CHECK_THROWS_AS(
            db.transaction([](Tx &tx) {
                tx.run("INSERT INTO tx_t (id, name) VALUES (6, 'six');");
                throw std::runtime_error("abort tx");
            }),
            std::runtime_error);
        CHECK(db.pluck<int64_t>("SELECT count(*) FROM tx_t;").value_or(0) == 5);
    }

    TEST_CASE("Statement cache error paths and deep re-entrancy fallback") {
        Database db(":memory:");
        db.exec("CREATE TABLE sc (id INTEGER PRIMARY KEY);");
        db.run("INSERT INTO sc (id) VALUES (1);");

        CHECK_THROWS_AS(db.run("SELECT * FROM nonexistent_table;"), std::runtime_error);
        CHECK_THROWS_AS(db.run("INSERT INTO sc (id) VALUES (1);"), std::runtime_error);

        db.registerFunction("boom", [](const std::vector<SqlValue> &) -> SqlValue {
            throw std::runtime_error("boom");
        });
        CHECK_THROWS_AS(db.all("SELECT boom(id) FROM sc;"), std::runtime_error);

        // Exceed kMaxCacheSize (64) active statements simultaneously via nested re-entrant callbacks
        int depth = 0;
        db.registerFunction("recurse_q", [&](const std::vector<SqlValue> &args) -> SqlValue {
            int n = static_cast<int>(std::get<int64_t>(args[0]));
            if (n <= 0) {
                return int64_t(0);
            }
            ++depth;
            std::string sql = "SELECT recurse_q(" + std::to_string(n - 1) + ") + " + std::to_string(n) + ";";
            auto v = db.pluck<int64_t>(sql);
            return v.value_or(0); }, false);
        auto res = db.pluck<int64_t>("SELECT recurse_q(66);");
        REQUIRE(res.has_value());
        CHECK(depth == 66);
    }

    TEST_CASE("iterateLimited overloads, readOnlyConnection, setupSql, and error paths") {
        testing::TempDirGuard dirGuard("sqlite_iter_");
        std::string dbPath = (dirGuard.path / "iter.db").string();

        Database mem(":memory:");
        mem.exec("CREATE TABLE nums (n INTEGER);");
        for (int64_t i = 1; i <= 3; ++i) {
            mem.run("INSERT INTO nums (n) VALUES (?);", {i});
        }

        auto exact = mem.iterateLimited("SELECT n FROM nums WHERE n <= :max ORDER BY n;",
                                        std::map<std::string, SqlValue>{{"max", int64_t(3)}},
                                        IterateOptions{.limit = 3});
        CHECK(exact.rows.size() == 3);
        CHECK_FALSE(exact.truncated);

        auto memRo = mem.iterateLimited("SELECT n FROM nums ORDER BY n;",
                                        IterateOptions{.limit = 2, .readOnlyConnection = true, .setupSql = "PRAGMA temp_store = MEMORY;"});
        CHECK(memRo.rows.size() == 2);
        CHECK(memRo.truncated);
        CHECK(mem.pluck<int64_t>("PRAGMA query_only;").value_or(-1) == 0);

        Database fileDb(dbPath);
        fileDb.exec("CREATE TABLE items (id INTEGER PRIMARY KEY, val INTEGER);");
        fileDb.run("INSERT INTO items (val) VALUES (10), (20), (30);");
        fileDb.registerFunction("add_one", [](const std::vector<SqlValue> &args) -> SqlValue {
            return std::get<int64_t>(args[0]) + 1;
        });

        auto fileRo = fileDb.iterateLimited("SELECT add_one(val) AS v FROM items ORDER BY id;",
                                            IterateOptions{.limit = 2, .readOnlyConnection = true, .setupSql = "PRAGMA temp_store = MEMORY;"});
        CHECK(fileRo.columns == std::vector<std::string>{"v"});
        CHECK(fileRo.rows.size() == 2);
        CHECK(fileRo.truncated);
        CHECK(fileRo.rows[0]["v"].get<int64_t>() == 11);

        CHECK_THROWS_AS(
            fileDb.iterateLimited("SELECT * FROM items;", IterateOptions{.readOnlyConnection = true, .setupSql = "BAD SQL;"}),
            std::runtime_error);
        CHECK_THROWS_AS(fileDb.iterateLimited("SELECT * FROM missing_tbl;", IterateOptions{}), std::runtime_error);

        fileDb.registerFunction("fail_step", [](const std::vector<SqlValue> &) -> SqlValue {
            throw std::runtime_error("step err");
        });
        CHECK_THROWS_AS(fileDb.iterateLimited("SELECT fail_step() FROM items;", IterateOptions{.limit = 1}), std::runtime_error);
    }

    TEST_CASE("registerFunction types, NULL/BLOB/FLOAT args, and non-std exception") {
        Database db(":memory:");
        db.registerFunction("echo_type", [](const std::vector<SqlValue> &args) -> SqlValue {
            if (args.empty() || std::holds_alternative<std::monostate>(args[0])) {
                return std::monostate{};
            }
            if (std::holds_alternative<double>(args[0])) {
                return std::get<double>(args[0]) * 2.0;
            }
            if (std::holds_alternative<std::vector<uint8_t>>(args[0])) {
                return std::get<std::vector<uint8_t>>(args[0]);
            }
            return true;
        });
        db.registerFunction("throw_int", [](const std::vector<SqlValue> &) -> SqlValue {
            throw 42;
        });

        CHECK(std::holds_alternative<std::monostate>(db.pluck<SqlValue>("SELECT echo_type(NULL);").value()));
        CHECK(db.pluck<double>("SELECT echo_type(1.5);").value_or(0.0) == doctest::Approx(3.0));
        auto blobRes = db.pluck<std::vector<uint8_t>>("SELECT echo_type(X'AABB');");
        REQUIRE(blobRes.has_value());
        CHECK(*blobRes == std::vector<uint8_t>{0xAA, 0xBB});
        auto emptyBlobRes = db.pluck<SqlValue>("SELECT echo_type(X'');");
        REQUIRE(emptyBlobRes.has_value());
        CHECK(std::holds_alternative<std::monostate>(*emptyBlobRes));
        CHECK(db.pluck<bool>("SELECT echo_type(1);").value_or(false) == true);
        CHECK_THROWS_AS(db.pluck<int64_t>("SELECT throw_int();"), std::runtime_error);
    }

    TEST_CASE("backup creates parent directories and throws on invalid destination") {
        testing::TempDirGuard dirGuard("sqlite_bk_");
        Database db(":memory:");
        db.exec("CREATE TABLE bk (id INTEGER PRIMARY KEY);");
        db.run("INSERT INTO bk (id) VALUES (42);");

        auto nestedDest = dirGuard.path / "nested" / "sub" / "backup.db";
        db.backup(nestedDest.string());
        CHECK(std::filesystem::exists(nestedDest));

        Database restored(nestedDest.string(), SqliteOptions{.readOnly = true});
        CHECK(restored.pluck<int64_t>("SELECT id FROM bk;").value_or(0) == 42);

        CHECK_THROWS_AS(db.backup(dirGuard.path.string()), std::runtime_error);
    }
}
