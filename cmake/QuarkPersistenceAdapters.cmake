# Optional 012 persistence backends (RocksDB / SQLite). OFF by default so the std-only core build
# links nothing extra — the engine "gains nothing" unless a backend is explicitly requested. When an
# option is ON, we locate the library, expose an INTERFACE target that carries its include + link,
# and (if tests are on) build a Store-conformance check that verifies the adapter is crash-durable.
#
# Enable with:  cmake -B build -DQUARK_WITH_SQLITE=ON        (needs libsqlite3-dev)
#               cmake -B build -DQUARK_WITH_ROCKSDB=ON       (needs librocksdb-dev)
#
# FileStore (the std-only default) needs NO option — it is header-only in the core include tree.

option(QUARK_WITH_SQLITE  "Build the SQLite persistence Store adapter (needs libsqlite3-dev)"  OFF)
option(QUARK_WITH_ROCKSDB "Build the RocksDB persistence Store adapter (needs librocksdb-dev)" OFF)

# ---- SQLite ----------------------------------------------------------------
if(QUARK_WITH_SQLITE)
  find_path(SQLITE3_INCLUDE_DIR sqlite3.h)
  find_library(SQLITE3_LIBRARY NAMES sqlite3)
  if(NOT SQLITE3_INCLUDE_DIR OR NOT SQLITE3_LIBRARY)
    message(FATAL_ERROR "QUARK_WITH_SQLITE=ON but sqlite3.h / libsqlite3 not found. "
                        "Install libsqlite3-dev (Debian/Ubuntu) or set SQLITE3_INCLUDE_DIR/SQLITE3_LIBRARY.")
  endif()
  add_library(quark_persistence_sqlite INTERFACE)
  target_include_directories(quark_persistence_sqlite INTERFACE "${SQLITE3_INCLUDE_DIR}")
  target_link_libraries(quark_persistence_sqlite INTERFACE quark::quark "${SQLITE3_LIBRARY}")
  add_library(quark::persistence_sqlite ALIAS quark_persistence_sqlite)
  message(STATUS "Quark: SQLite persistence adapter ENABLED (${SQLITE3_LIBRARY})")
endif()

# ---- RocksDB ---------------------------------------------------------------
if(QUARK_WITH_ROCKSDB)
  # Prefer the packaged CMake config; fall back to a plain library/header search.
  find_package(RocksDB CONFIG QUIET)
  add_library(quark_persistence_rocksdb INTERFACE)
  if(TARGET RocksDB::rocksdb)
    target_link_libraries(quark_persistence_rocksdb INTERFACE quark::quark RocksDB::rocksdb)
  else()
    find_path(ROCKSDB_INCLUDE_DIR rocksdb/db.h)
    find_library(ROCKSDB_LIBRARY NAMES rocksdb)
    if(NOT ROCKSDB_INCLUDE_DIR OR NOT ROCKSDB_LIBRARY)
      message(FATAL_ERROR "QUARK_WITH_ROCKSDB=ON but RocksDB not found. "
                          "Install librocksdb-dev (Debian/Ubuntu) or set ROCKSDB_INCLUDE_DIR/ROCKSDB_LIBRARY.")
    endif()
    target_include_directories(quark_persistence_rocksdb INTERFACE "${ROCKSDB_INCLUDE_DIR}")
    target_link_libraries(quark_persistence_rocksdb INTERFACE quark::quark "${ROCKSDB_LIBRARY}")
  endif()
  add_library(quark::persistence_rocksdb ALIAS quark_persistence_rocksdb)
  message(STATUS "Quark: RocksDB persistence adapter ENABLED")
endif()

# ---- Conformance checks (only when tests are on AND the backend is enabled) -------------------
# Each verifies the adapter is crash-durable against a shared conformance harness. There are TWO seams
# per backend: the 012 event `Store` (store_conformance.hpp) and the 027 durable `ReminderStore`
# (reminder_store_conformance.hpp) — the SQLite/RocksDB backends implement both, so both are verified.
if(QUARK_BUILD_TESTS AND QUARK_WITH_SQLITE)
  add_executable(sqlite_store_check tests/adapters/sqlite_store_check.cpp)
  target_include_directories(sqlite_store_check PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/tests")
  target_link_libraries(sqlite_store_check PRIVATE quark::persistence_sqlite quark_warnings)
  add_test(NAME sqlite_store_check COMMAND sqlite_store_check)

  add_executable(sqlite_reminder_store_check tests/adapters/sqlite_reminder_store_check.cpp)
  target_include_directories(sqlite_reminder_store_check PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/tests")
  target_link_libraries(sqlite_reminder_store_check PRIVATE quark::persistence_sqlite quark_warnings)
  add_test(NAME sqlite_reminder_store_check COMMAND sqlite_reminder_store_check)
endif()
if(QUARK_BUILD_TESTS AND QUARK_WITH_ROCKSDB)
  add_executable(rocksdb_store_check tests/adapters/rocksdb_store_check.cpp)
  target_include_directories(rocksdb_store_check PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/tests")
  target_link_libraries(rocksdb_store_check PRIVATE quark::persistence_rocksdb quark_warnings)
  add_test(NAME rocksdb_store_check COMMAND rocksdb_store_check)

  add_executable(rocksdb_reminder_store_check tests/adapters/rocksdb_reminder_store_check.cpp)
  target_include_directories(rocksdb_reminder_store_check PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/tests")
  target_link_libraries(rocksdb_reminder_store_check PRIVATE quark::persistence_rocksdb quark_warnings)
  add_test(NAME rocksdb_reminder_store_check COMMAND rocksdb_reminder_store_check)
endif()
