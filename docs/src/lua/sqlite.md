# Using SQLite in Vitrinli

Vitrinli supports [SQLite](https://www.sqlite.org/) 3 databases, which can
be accessed from Lua scripts. You should use
[pcall](https://www.lua.org/pil/8.4.html) to handle errors that may be
returned by SQLite.

## Opening an SQLite Database

    db = sqlite(path_to_db [, access])

This creates a new opaque database object. Errors raise real Lua errors
(hence the `pcall` advice above). The parameters are:

- `path_to_db`: path to the sqlite3 database file.
- `access`: Method of opening the database. Allowed are
    - `'RO'`: readonly access. The file must exist and the Vitrinli server must have read access to it.
    - `'RW'`: read and write access. The file must exist and the Vitrinli server must have read/write access to it. This is the default when the parameter is omitted.
    - `'CRW'`: If the database file does not exist, it will be created and opened with read/write access.

If the database is locked by another writer, calls wait at most the remaining
execution deadline of the request.

To close the database, you can do this:

    db = ~db

This marks the connection closed: any later use of the database or of a query
object prepared from it raises a `database is closed` error. The underlying
connection is freed once every query object prepared from it is gone.
Alternatively, Lua's garbage collection will free the database object and all
resources when they are no longer used.

### Preparing a Query
The Vitrinli sqlite interface supports direct queries as well as prepared statements. A direct query is constructed as
follows:

    qry = db << 'SELECT * FROM image'

Or, if you want to use a prepared query statement:

    qry = db << 'INSERT INTO image (id, description) VALUES (?,?)'

The result of the `<<` operator (`qry`) will then be a query object containing a prepared query. If the
query object is not needed anymore, it may be destroyed:

    qry = ~qry

Query objects should be destroyed explicitly if not needed any longer.

### Executing a Query
Executing (calling) a query object gets the next row of data. If there are no
more rows, `nil` is returned. The row is returned as a table of values with
**0-based** column indices; a column whose value is SQL `NULL` is absent from
the table (its index is `nil`).

    row = qry()
    while (row) do
        print(row[0], ' -> ', row[1])
        row = qry()
    end

Or with a prepared statement:

    row = qry('SGV_1960_00315', 'This is an image of a steam engine...')

The second way is used for prepared queries that contain parameters
(`?` placeholders, bound 1-based from the first call argument). Calling a
query object with arguments resets the statement and rebinds all parameters.
Supported bind types are strings, integers, floats, booleans and `nil`.
