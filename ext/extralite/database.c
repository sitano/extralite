#include <stdio.h>
#include "extralite.h"

VALUE cDatabase;
VALUE cError;
VALUE cSQLError;
VALUE cBusyError;
VALUE cInterruptError;

ID ID_call;
ID ID_keys;
ID ID_new;
ID ID_strip;
ID ID_to_s;

static size_t Database_size(const void *ptr) {
  return sizeof(Database_t);
}

static void Database_free(void *ptr) {
  Database_t *db = ptr;
  if (db->sqlite3_db) sqlite3_close_v2(db->sqlite3_db);
  free(ptr);
}

static const rb_data_type_t Database_type = {
    "Database",
    {0, Database_free, Database_size,},
    0, 0, RUBY_TYPED_FREE_IMMEDIATELY
};

static VALUE Database_allocate(VALUE klass) {
  Database_t *db = ALLOC(Database_t);
  db->sqlite3_db = 0;
  return TypedData_Wrap_Struct(klass, &Database_type, db);
}

#define GetDatabase(obj, database) \
  TypedData_Get_Struct((obj), Database_t, &Database_type, (database))

// make sure the database is open
#define GetOpenDatabase(obj, database) { \
  TypedData_Get_Struct((obj), Database_t, &Database_type, (database)); \
  if (!(database)->sqlite3_db) { \
    rb_raise(cError, "Database is closed"); \
  } \
}

Database_t *Database_struct(VALUE self) {
  Database_t *db;
  GetDatabase(self, db);
  return db;
}

sqlite3 *Database_sqlite3_db(VALUE self) {
  Database_t *db;
  GetDatabase(self, db);
  return db->sqlite3_db;
}

/* call-seq:
 *   Extralite.sqlite3_version -> version
 *
 * Returns the sqlite3 version used by Extralite.
 */

VALUE Extralite_sqlite3_version(VALUE self) {
  return rb_str_new_cstr(sqlite3_version);
}

/* call-seq:
 *   db.initialize(path)
 *
 * Initializes a new SQLite database with the given path.
 */

VALUE Database_initialize(VALUE self, VALUE path) {
  int rc;
  Database_t *db;
  GetDatabase(self, db);

  rc = sqlite3_open(StringValueCStr(path), &db->sqlite3_db);
  if (rc) {
    sqlite3_close_v2(db->sqlite3_db);
    rb_raise(cError, "%s", sqlite3_errstr(rc));
  }

  // Enable extended result codes
  rc = sqlite3_extended_result_codes(db->sqlite3_db, 1);
  if (rc) {
    sqlite3_close_v2(db->sqlite3_db);
    rb_raise(cError, "%s", sqlite3_errmsg(db->sqlite3_db));
  }

#ifdef HAVE_SQLITE3_ENABLE_LOAD_EXTENSION
  rc = sqlite3_enable_load_extension(db->sqlite3_db, 1);
  if (rc) {
    sqlite3_close_v2(db->sqlite3_db);
    rb_raise(cError, "%s", sqlite3_errmsg(db->sqlite3_db));
  }
#endif

  db->trace_block = Qnil;

  return Qnil;
}

/* call-seq:
 *   db.close -> db
 *
 * Closes the database.
 */
VALUE Database_close(VALUE self) {
  int rc;
  Database_t *db;
  GetDatabase(self, db);

  rc = sqlite3_close_v2(db->sqlite3_db);
  if (rc) {
    rb_raise(cError, "%s", sqlite3_errmsg(db->sqlite3_db));
  }

  db->sqlite3_db = 0;
  return self;
}

/* call-seq:
 *   db.closed? -> bool
 *
 * Returns true if the database is closed.
 *
 * @return [bool] is database closed
 */
VALUE Database_closed_p(VALUE self) {
  Database_t *db;
  GetDatabase(self, db);

  return db->sqlite3_db ? Qfalse : Qtrue;
}

static inline VALUE Database_perform_query(int argc, VALUE *argv, VALUE self, VALUE (*call)(query_ctx *)) {
  Database_t *db;
  sqlite3_stmt *stmt;
  VALUE sql;

  // extract query from args
  rb_check_arity(argc, 1, UNLIMITED_ARGUMENTS);
  sql = rb_funcall(argv[0], ID_strip, 0);
  if (RSTRING_LEN(sql) == 0) return Qnil;

  // prepare query ctx
  GetOpenDatabase(self, db);
  if (db->trace_block != Qnil) rb_funcall(db->trace_block, ID_call, 1, sql);
  prepare_multi_stmt(db->sqlite3_db, &stmt, sql);
  RB_GC_GUARD(sql);

  bind_all_parameters(stmt, argc - 1, argv + 1);
  query_ctx ctx = { self, db->sqlite3_db, stmt };

  return rb_ensure(SAFE(call), (VALUE)&ctx, SAFE(cleanup_stmt), (VALUE)&ctx);
}

/* call-seq:
 *    db.query(sql, *parameters, &block) -> [...]
 *    db.query_hash(sql, *parameters, &block) -> [...]
 *
 * Runs a query returning rows as hashes (with symbol keys). If a block is
 * given, it will be called for each row. Otherwise, an array containing all
 * rows is returned.
 * 
 * Query parameters to be bound to placeholders in the query can be specified as
 * a list of values or as a hash mapping parameter names to values. When
 * parameters are given as a least, the query should specify parameters using
 * `?`:
 * 
 *     db.query('select * from foo where x = ?', 42)
 *
 * Named placeholders are specified using `:`. The placeholder values are
 * specified using a hash, where keys are either strings are symbols. String
 * keys can include or omit the `:` prefix. The following are equivalent:
 * 
 *     db.query('select * from foo where x = :bar', bar: 42)
 *     db.query('select * from foo where x = :bar', 'bar' => 42)
 *     db.query('select * from foo where x = :bar', ':bar' => 42)
 */
VALUE Database_query_hash(int argc, VALUE *argv, VALUE self) {
  return Database_perform_query(argc, argv, self, safe_query_hash);
}

/* call-seq:
 *   db.query_ary(sql, *parameters, &block) -> [...]
 *
 * Runs a query returning rows as arrays. If a block is given, it will be called
 * for each row. Otherwise, an array containing all rows is returned.
 *
 * Query parameters to be bound to placeholders in the query can be specified as
 * a list of values or as a hash mapping parameter names to values. When
 * parameters are given as a least, the query should specify parameters using
 * `?`:
 *
 *     db.query_ary('select * from foo where x = ?', 42)
 *
 * Named placeholders are specified using `:`. The placeholder values are
 * specified using a hash, where keys are either strings are symbols. String
 * keys can include or omit the `:` prefix. The following are equivalent:
 *
 *     db.query_ary('select * from foo where x = :bar', bar: 42)
 *     db.query_ary('select * from foo where x = :bar', 'bar' => 42)
 *     db.query_ary('select * from foo where x = :bar', ':bar' => 42)
 */
VALUE Database_query_ary(int argc, VALUE *argv, VALUE self) {
  return Database_perform_query(argc, argv, self, safe_query_ary);
}

/* call-seq:
 *   db.query_single_row(sql, *parameters) -> {...}
 *
 * Runs a query returning a single row as a hash.
 *
 * Query parameters to be bound to placeholders in the query can be specified as
 * a list of values or as a hash mapping parameter names to values. When
 * parameters are given as a least, the query should specify parameters using
 * `?`:
 *
 *     db.query_single_row('select * from foo where x = ?', 42)
 *
 * Named placeholders are specified using `:`. The placeholder values are
 * specified using a hash, where keys are either strings are symbols. String
 * keys can include or omit the `:` prefix. The following are equivalent:
 *
 *     db.query_single_row('select * from foo where x = :bar', bar: 42)
 *     db.query_single_row('select * from foo where x = :bar', 'bar' => 42)
 *     db.query_single_row('select * from foo where x = :bar', ':bar' => 42)
 */
VALUE Database_query_single_row(int argc, VALUE *argv, VALUE self) {
  return Database_perform_query(argc, argv, self, safe_query_single_row);
}

/* call-seq:
 *   db.query_single_column(sql, *parameters, &block) -> [...]
 *
 * Runs a query returning single column values. If a block is given, it will be called
 * for each value. Otherwise, an array containing all values is returned.
 *
 * Query parameters to be bound to placeholders in the query can be specified as
 * a list of values or as a hash mapping parameter names to values. When
 * parameters are given as a least, the query should specify parameters using
 * `?`:
 *
 *     db.query_single_column('select x from foo where x = ?', 42)
 *
 * Named placeholders are specified using `:`. The placeholder values are
 * specified using a hash, where keys are either strings are symbols. String
 * keys can include or omit the `:` prefix. The following are equivalent:
 *
 *     db.query_single_column('select x from foo where x = :bar', bar: 42)
 *     db.query_single_column('select x from foo where x = :bar', 'bar' => 42)
 *     db.query_single_column('select x from foo where x = :bar', ':bar' => 42)
 */
VALUE Database_query_single_column(int argc, VALUE *argv, VALUE self) {
  return Database_perform_query(argc, argv, self, safe_query_single_column);
}

/* call-seq:
 *   db.query_single_value(sql, *parameters) -> value
 *
 * Runs a query returning a single value from the first row.
 *
 * Query parameters to be bound to placeholders in the query can be specified as
 * a list of values or as a hash mapping parameter names to values. When
 * parameters are given as a least, the query should specify parameters using
 * `?`:
 *
 *     db.query_single_value('select x from foo where x = ?', 42)
 *
 * Named placeholders are specified using `:`. The placeholder values are
 * specified using a hash, where keys are either strings are symbols. String
 * keys can include or omit the `:` prefix. The following are equivalent:
 *
 *     db.query_single_value('select x from foo where x = :bar', bar: 42)
 *     db.query_single_value('select x from foo where x = :bar', 'bar' => 42)
 *     db.query_single_value('select x from foo where x = :bar', ':bar' => 42)
 */
VALUE Database_query_single_value(int argc, VALUE *argv, VALUE self) {
  return Database_perform_query(argc, argv, self, safe_query_single_value);
}

/* call-seq:
 *   db.execute_multi(sql, params_array) -> changes
 *
 * Executes the given query for each list of parameters in params_array. Returns
 * the number of changes effected. This method is designed for inserting
 * multiple records.
 *
 *     records = [
 *       [1, 2, 3],
 *       [4, 5, 6]
 *     ]
 *     db.execute_multi_query('insert into foo values (?, ?, ?)', records)
 *
 */
VALUE Database_execute_multi(VALUE self, VALUE sql, VALUE params_array) {
  Database_t *db;
  sqlite3_stmt *stmt;

  if (RSTRING_LEN(sql) == 0) return Qnil;

  // prepare query ctx
  GetOpenDatabase(self, db);
  prepare_single_stmt(db->sqlite3_db, &stmt, sql);
  query_ctx ctx = { self, db->sqlite3_db, stmt, params_array };

  return rb_ensure(SAFE(safe_execute_multi), (VALUE)&ctx, SAFE(cleanup_stmt), (VALUE)&ctx);
}

/* call-seq:
 *   db.columns(sql) -> columns
 *
 * Returns the column names for the given query, without running it.
 */
VALUE Database_columns(VALUE self, VALUE sql) {
  return Database_perform_query(1, &sql, self, safe_query_columns);
}

/* call-seq:
 *   db.last_insert_rowid -> int
 *
 * Returns the rowid of the last inserted row.
 */
VALUE Database_last_insert_rowid(VALUE self) {
  Database_t *db;
  GetOpenDatabase(self, db);

  return INT2FIX(sqlite3_last_insert_rowid(db->sqlite3_db));
}

/* call-seq:
 *   db.changes -> int
 *
 * Returns the number of changes made to the database by the last operation.
 */
VALUE Database_changes(VALUE self) {
  Database_t *db;
  GetOpenDatabase(self, db);

  return INT2FIX(sqlite3_changes(db->sqlite3_db));
}

/* call-seq:
 *   db.filename -> string
 *
 * Returns the database filename.
 */
VALUE Database_filename(int argc, VALUE *argv, VALUE self) {
  const char *db_name;
  const char *filename;
  Database_t *db;
  GetOpenDatabase(self, db);

  rb_check_arity(argc, 0, 1);
  db_name = (argc == 1) ? StringValueCStr(argv[0]) : "main";
  filename = sqlite3_db_filename(db->sqlite3_db, db_name);
  return filename ? rb_str_new_cstr(filename) : Qnil;
}

/* call-seq:
 *   db.transaction_active? -> bool
 *
 * Returns true if a transaction is currently in progress.
 */
VALUE Database_transaction_active_p(VALUE self) {
  Database_t *db;
  GetOpenDatabase(self, db);

  return sqlite3_get_autocommit(db->sqlite3_db) ? Qfalse : Qtrue;
}

#ifdef HAVE_SQLITE3_LOAD_EXTENSION
/* call-seq:
 *   db.load_extension(path) -> db
 *
 * Loads an extension with the given path.
 */
VALUE Database_load_extension(VALUE self, VALUE path) {
  Database_t *db;
  GetOpenDatabase(self, db);
  char *err_msg;

  int rc = sqlite3_load_extension(db->sqlite3_db, RSTRING_PTR(path), 0, &err_msg);
  if (rc != SQLITE_OK) {
    VALUE error = rb_exc_new2(cError, err_msg);
    sqlite3_free(err_msg);
    rb_exc_raise(error);
  }

  return self;
}
#endif

/* call-seq:
 *   db.prepare(sql) -> Extralite::PreparedStatement
 *
 * Creates a prepared statement with the given SQL query.
 */
VALUE Database_prepare(VALUE self, VALUE sql) {
  return rb_funcall(cPreparedStatement, ID_new, 2, self, sql);
}

/* call-seq:
 *   db.interrupt -> db
 *
 * Interrupts a long running query. This method is to be called from a different
 * thread than the one running the query. Upon calling `#interrupt` the running
 * query will stop and raise an `Extralite::InterruptError` exception.
 *
 * It is not safe to call `#interrupt` on a database that is about to be closed.
 * For more information, consult the [sqlite3 API docs](https://sqlite.org/c3ref/interrupt.html).
 */
VALUE Database_interrupt(VALUE self) {
  Database_t *db;
  GetOpenDatabase(self, db);

  sqlite3_interrupt(db->sqlite3_db);
  return self;
}

typedef struct {
  sqlite3 *dst;
  int close_dst_on_cleanup;
  sqlite3_backup *backup;
  int block_given;
  int rc;
} backup_ctx;

#define BACKUP_STEP_MAX_PAGES 16
#define BACKUP_SLEEP_MS       100

void *backup_step_without_gvl(void *ptr) {
  backup_ctx *ctx = (backup_ctx *)ptr;
  ctx->rc = sqlite3_backup_step(ctx->backup, BACKUP_STEP_MAX_PAGES);
  return NULL;
}

void *backup_sleep_without_gvl(void *unused) {
  sqlite3_sleep(BACKUP_SLEEP_MS);
  return NULL;
}

VALUE backup_safe_iterate(VALUE ptr) {
  backup_ctx *ctx = (backup_ctx *)ptr;
  int done = 0;

  while (!done) {
    rb_thread_call_without_gvl(backup_step_without_gvl, (void *)ctx, RUBY_UBF_IO, 0);
    switch(ctx->rc) {
      case SQLITE_DONE:
        if (ctx->block_given) {
          VALUE total     = INT2FIX(sqlite3_backup_pagecount(ctx->backup));
          rb_yield_values(2, total, total);
        }
        done = 1;
        continue;
      case SQLITE_OK:
        if (ctx->block_given) {
          VALUE remaining = INT2FIX(sqlite3_backup_remaining(ctx->backup));
          VALUE total     = INT2FIX(sqlite3_backup_pagecount(ctx->backup));
          rb_yield_values(2, remaining, total);
        }
        continue;
      case SQLITE_BUSY:
      case SQLITE_LOCKED:
        rb_thread_call_without_gvl(backup_sleep_without_gvl, NULL, RUBY_UBF_IO, 0);
        continue;
      default:
        rb_raise(cError, "%s", sqlite3_errstr(ctx->rc));
    }
  };

  return Qnil;
}

VALUE backup_cleanup(VALUE ptr) {
  backup_ctx *ctx = (backup_ctx *)ptr;

  sqlite3_backup_finish(ctx->backup);

  if (ctx->close_dst_on_cleanup)
    sqlite3_close_v2(ctx->dst);
  return Qnil;
}

/* call-seq:
 *   db.backup(dest) -> db
 *   db.backup(dest) { |remaining, total| } -> db
 *
 * Creates a backup of the database to the given destination, which can be
 * either a filename or a database instance. In order to monitor the backup
 * progress you can pass a block that will be called periodically by the backup
 * method with two arguments: the remaining page count, and the total page
 * count, which can be used to display the progress to the user or to collect
 * statistics.
 */
VALUE Database_backup(int argc, VALUE *argv, VALUE self) {
  VALUE dst;
  VALUE src_name;
  VALUE dst_name;
  rb_scan_args(argc, argv, "12", &dst, &src_name, &dst_name);
  if (src_name == Qnil) src_name = rb_str_new_literal("main");
  if (dst_name == Qnil) dst_name = rb_str_new_literal("main");

  int dst_is_fn = TYPE(dst) == T_STRING;

  Database_t *src;
  GetOpenDatabase(self, src);
  sqlite3 *dst_db;

  if (dst_is_fn) {
    int rc = sqlite3_open(StringValueCStr(dst), &dst_db);
    if (rc) {
      sqlite3_close_v2(dst_db);
      rb_raise(cError, "%s", sqlite3_errmsg(dst_db));
    }
  }
  else {
    Database_t *dst_struct;
    GetOpenDatabase(dst, dst_struct);
    dst_db = dst_struct->sqlite3_db;
  }

  // TODO: add possibility to use different src and dest db names (main, tmp, or
  // attached db's).
  sqlite3_backup *backup;
  backup = sqlite3_backup_init(dst_db, StringValueCStr(dst_name), src->sqlite3_db, StringValueCStr(src_name));
  if (!backup) {
    if (dst_is_fn)
      sqlite3_close_v2(dst_db);
    rb_raise(cError, "%s", sqlite3_errmsg(dst_db));
  }

  backup_ctx ctx = { dst_db, dst_is_fn, backup, rb_block_given_p(), 0 };
  rb_ensure(SAFE(backup_safe_iterate), (VALUE)&ctx, SAFE(backup_cleanup), (VALUE)&ctx);

  return self;
}

/* call-seq:
 *   Extralite.runtime_status(op[, reset]) -> [value, highwatermark]
 *
 * Returns runtime status values for the given op as an array containing the
 * current value and the high water mark value. To reset the high water mark,
 * pass true as reset.
 */
VALUE Extralite_runtime_status(int argc, VALUE* argv, VALUE self) {
  VALUE op, reset;
  sqlite3_int64 cur, hwm;

  rb_scan_args(argc, argv, "11", &op, &reset);

  int rc = sqlite3_status64(NUM2INT(op), &cur, &hwm, RTEST(reset) ? 1 : 0);
  if (rc != SQLITE_OK) rb_raise(cError, "%s", sqlite3_errstr(rc));

  return rb_ary_new3(2, LONG2FIX(cur), LONG2FIX(hwm));
}

/* call-seq:
 *   db.status(op[, reset]) -> [value, highwatermark]
 *
 * Returns database status values for the given op as an array containing the
 * current value and the high water mark value. To reset the high water mark,
 * pass true as reset.
 */
VALUE Database_status(int argc, VALUE *argv, VALUE self) {
  VALUE op, reset;
  int cur, hwm;

  rb_scan_args(argc, argv, "11", &op, &reset);

  Database_t *db;
  GetOpenDatabase(self, db);

  int rc = sqlite3_db_status(db->sqlite3_db, NUM2INT(op), &cur, &hwm, RTEST(reset) ? 1 : 0);
  if (rc != SQLITE_OK) rb_raise(cError, "%s", sqlite3_errstr(rc));

  return rb_ary_new3(2, INT2NUM(cur), INT2NUM(hwm));
}

/* call-seq:
 *   db.limit(category) -> value
 *   db.limit(category, new_value) -> prev_value
 *
 * Returns the current limit for the given category. If a new value is given,
 * sets the limit to the new value and returns the previous value.
 */
VALUE Database_limit(int argc, VALUE *argv, VALUE self) {
  VALUE category, new_value;

  rb_scan_args(argc, argv, "11", &category, &new_value);

  Database_t *db;
  GetOpenDatabase(self, db);

  int value = sqlite3_limit(db->sqlite3_db, NUM2INT(category), RTEST(new_value) ? NUM2INT(new_value) : -1);

  if (value == -1) rb_raise(cError, "Invalid limit category");

  return INT2NUM(value);
}

/* call-seq:
 *   db.busy_timeout=(sec) -> db
 *   db.busy_timeout=nil -> db
 *
 * Sets the busy timeout for the database, in seconds or fractions thereof. To
 * disable the busy timeout, set it to 0 or nil.
 */
VALUE Database_busy_timeout_set(VALUE self, VALUE sec) {
  Database_t *db;
  GetOpenDatabase(self, db);

  int ms = (sec == Qnil) ? 0 : (int)(NUM2DBL(sec) * 1000);
  int rc = sqlite3_busy_timeout(db->sqlite3_db, ms);
  if (rc != SQLITE_OK) rb_raise(cError, "Failed to set busy timeout");

  return self;
}

/* call-seq:
 *   db.total_changes -> value
 *
 * Returns the total number of changes made to the database since opening it.
 */
VALUE Database_total_changes(VALUE self) {
  Database_t *db;
  GetOpenDatabase(self, db);

  int value = sqlite3_total_changes(db->sqlite3_db);
  return INT2NUM(value);
}

/* call-seq:
 *   db.trace { |sql| } -> db
 *   db.trace -> db
 *
 * Installs or removes a block that will be invoked for every SQL statement
 * executed.
 */
VALUE Database_trace(VALUE self) {
  Database_t *db;
  GetOpenDatabase(self, db);

  db->trace_block = rb_block_given_p() ? rb_block_proc() : Qnil;
  return self;
}

/* call-seq:
 *   db.errcode -> errcode
 *
 * Returns the last error code for the database.
 */
VALUE Database_errcode(VALUE self) {
  Database_t *db;
  GetOpenDatabase(self, db);

  return INT2NUM(sqlite3_errcode(db->sqlite3_db));
}

/* call-seq:
 *   db.errmsg -> errmsg
 *
 * Returns the last error message for the database.
 */
VALUE Database_errmsg(VALUE self) {
  Database_t *db;
  GetOpenDatabase(self, db);

  return rb_str_new2(sqlite3_errmsg(db->sqlite3_db));
}

#ifdef HAVE_SQLITE3_ERROR_OFFSET
/* call-seq:
 *   db.error_offset -> ofs
 *
 * Returns the offset for the last error
 */
VALUE Database_error_offset(VALUE self) {
  Database_t *db;
  GetOpenDatabase(self, db);

  return INT2NUM(sqlite3_error_offset(db->sqlite3_db));
}
#endif

void Init_ExtraliteDatabase(void) {
  VALUE mExtralite = rb_define_module("Extralite");
  rb_define_singleton_method(mExtralite, "runtime_status", Extralite_runtime_status, -1);
  rb_define_singleton_method(mExtralite, "sqlite3_version", Extralite_sqlite3_version, 0);

  cDatabase = rb_define_class_under(mExtralite, "Database", rb_cObject);
  rb_define_alloc_func(cDatabase, Database_allocate);

  rb_define_method(cDatabase, "backup", Database_backup, -1);
  rb_define_method(cDatabase, "busy_timeout=", Database_busy_timeout_set, 1);
  rb_define_method(cDatabase, "changes", Database_changes, 0);
  rb_define_method(cDatabase, "close", Database_close, 0);
  rb_define_method(cDatabase, "closed?", Database_closed_p, 0);
  rb_define_method(cDatabase, "columns", Database_columns, 1);
  rb_define_method(cDatabase, "errcode", Database_errcode, 0);
  rb_define_method(cDatabase, "errmsg", Database_errmsg, 0);

  #ifdef HAVE_SQLITE3_ERROR_OFFSET
  rb_define_method(cDatabase, "error_offset", Database_error_offset, 0);
  #endif

  rb_define_method(cDatabase, "execute_multi", Database_execute_multi, 2);
  rb_define_method(cDatabase, "filename", Database_filename, -1);
  rb_define_method(cDatabase, "initialize", Database_initialize, 1);
  rb_define_method(cDatabase, "interrupt", Database_interrupt, 0);
  rb_define_method(cDatabase, "last_insert_rowid", Database_last_insert_rowid, 0);
  rb_define_method(cDatabase, "limit", Database_limit, -1);
  rb_define_method(cDatabase, "prepare", Database_prepare, 1);
  rb_define_method(cDatabase, "query", Database_query_hash, -1);
  rb_define_method(cDatabase, "query_ary", Database_query_ary, -1);
  rb_define_method(cDatabase, "query_hash", Database_query_hash, -1);
  rb_define_method(cDatabase, "query_single_column", Database_query_single_column, -1);
  rb_define_method(cDatabase, "query_single_row", Database_query_single_row, -1);
  rb_define_method(cDatabase, "query_single_value", Database_query_single_value, -1);
  rb_define_method(cDatabase, "status", Database_status, -1);
  rb_define_method(cDatabase, "total_changes", Database_total_changes, 0);
  rb_define_method(cDatabase, "trace", Database_trace, 0);
  rb_define_method(cDatabase, "transaction_active?", Database_transaction_active_p, 0);

#ifdef HAVE_SQLITE3_LOAD_EXTENSION
  rb_define_method(cDatabase, "load_extension", Database_load_extension, 1);
#endif

  cError = rb_define_class_under(mExtralite, "Error", rb_eStandardError);
  cSQLError = rb_define_class_under(mExtralite, "SQLError", cError);
  cBusyError = rb_define_class_under(mExtralite, "BusyError", cError);
  cInterruptError = rb_define_class_under(mExtralite, "InterruptError", cError);
  rb_gc_register_mark_object(cError);
  rb_gc_register_mark_object(cSQLError);
  rb_gc_register_mark_object(cBusyError);
  rb_gc_register_mark_object(cInterruptError);

  ID_call   = rb_intern("call");
  ID_keys   = rb_intern("keys");
  ID_new    = rb_intern("new");
  ID_strip  = rb_intern("strip");
  ID_to_s   = rb_intern("to_s");
}
