# Local development SQL protocol

This compatibility adapter is explicitly selected with --protocol dev-tsv. The native CLI and Docker default to the official JSONL protocol published in upstream f4d4e26653af823a7601668802f80e374ad6643c. See the JSONL notes below.

## Launch

minivectordb --protocol dev-tsv --data-dir /data --sort-memory 8388608

The database is /data/database.mvdb and sort runs are under /data/tmp. The sort option is a byte budget for sort state, not a total process-memory limit. Minimum CLI value: 4096 bytes. The current parser materializes one SQL statement, with a CLI limit of 16 MiB.

Input is UTF-8 SQL on stdin. Semicolons separate statements outside single-quoted strings, backtick identifiers, -- line comments and /* block comments */. Doubled quote/backtick characters escape themselves. A complete last statement may end at EOF without a semicolon. An unterminated quote/comment produces an error and is not executed. No backslash SQL string-escape mode is implied.

The engine is single-threaded and single-process. The data file is locked against a second writer. EOF/destruction rolls back any active transaction. A successful commit response is emitted only after durable commit returns.

## Output on stdout

- Non-query success: OK<TAB>affected_rows
- Query header: RESULT<TAB>column_name...
- Each query row: ROW<TAB>typed_value...
- Successful query completion: END<TAB>row_count
- Statement failure: ERROR<TAB>category<TAB>message

Categories are internal diagnostics, not MySQL error numbers. Output flushes after every completed statement/error. A result is valid only after END; if a streaming query fails, ERROR invalidates any rows already emitted for that result. Consumers must drain row output while queries are running.

Cells use i: for signed integers, s: for text, v:[...] for float32 vectors, and d: for double-precision distances. Text, names and messages escape backslash, tab, LF, CR and NUL as \\, \t, \n, \r and \0. Other UTF-8 bytes are preserved. Floats use enough decimal digits to round trip.

Exit status: 0 for an error-free session, 1 if any SQL/framing error occurred, 2 for command-line/startup/fatal transport errors. SQL errors do not end the input session. Diagnostics for fatal errors go to stderr.

## Example

Input:

    CREATE TABLE items (id BIGINT PRIMARY KEY, label VARCHAR(30), v VECTOR(2));
    INSERT INTO items VALUES (1,'first',[0,0]),(2,'second',[3,4]);
    SELECT id,L2_DISTANCE(v,[0,0]) AS distance FROM items ORDER BY distance LIMIT 1;

Expected output (tabs shown as spaces):

    OK  0
    OK  2
    RESULT  id  distance
    ROW  i:1  d:0
    END  1

The dev-tsv contract is for local development only. Official clients must use JSONL.
## Default official JSONL adapter

Launch `minivectordb --data-dir /data` (or explicitly `--protocol jsonl`). Recovery and file opening finish before the exact `{"type":"ready"}` handshake is flushed. Each UTF-8 physical line is one JSON request, with a 16 MiB request limit and nesting limit 64. Duplicate object keys, malformed numbers, invalid UTF-8 and unpaired Unicode surrogates are rejected. Invalid framing returns an error with null ID when no unambiguous validated ID can be recovered.

Execute requests contain `id`, `op:"execute"`, `sql`, and optional `search_mode:"exact"|"ann"` (default exact). The mode reaches the database API. IDs retain their original JSON type and number lexeme; BIGINT results use exact decimal integers. Query output has columns and two-dimensional rows; vectors are arrays with finite numbers. The current SQL engine does not implement NULL values, so no NULL cell is produced.

Successful nonqueries include integer affected_rows; DDL and transaction controls use zero. SQL failures return boolean ok:false and a nonempty internal error category; no unpublished official error-code mapping is assumed. A failed statement does not terminate the JSONL session or change its normal exit status.

Response payloads are completed before any stdout output. A 64 KiB buffer handles the payload excluding the validated request ID; larger payloads spill to a uniquely created anonymous file under the data directory, streamed back in 64 KiB chunks. The ID is emitted separately only after payload preparation succeeds, so a long ID cannot force a small write/transaction acknowledgement to open a spool after changing database state. Unlinking immediately after opening lets the OS reclaim the spool on both normal close and process death. SELECT execution/spool failures therefore produce one error instead of partial success. An output transport failure after emission begins is fatal, without emitting a second response. Result size is not held in memory as a single string.

Shutdown explicitly rolls back an active transaction and releases the database before returning exactly id and ok:true, then exits zero. EOF also rolls back. Every protocol message ends with a newline and is flushed. Logs and fatal diagnostics use stderr. CLI/startup/transport failures exit 2.
