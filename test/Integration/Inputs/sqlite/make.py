import sqlite3, sys
db = sqlite3.connect(sys.argv[1])
db.execute("CREATE TABLE c (host TEXT, name TEXT, value TEXT, n INTEGER, b BLOB)")
db.executemany("INSERT INTO c VALUES (?,?,?,?,?)", [
    (".a.org", "x", "1", 10, None),
    ("b.org", "tab", "a\tb\nc\\d", 4102444800000, b"\x00\xff"),
])
db.commit()
