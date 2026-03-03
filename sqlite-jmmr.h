/*
** sqlite-jmmr.h — Jaccard MMR virtual table for diversity reranking.
**
** BSD 3-Clause License. See LICENSE for details.
*/
#ifndef SQLITE_JMMR_H
#define SQLITE_JMMR_H

#ifndef SQLITE_CORE
#include "sqlite3ext.h"
#else
#include "sqlite3.h"
#endif

#ifdef SQLITE_JMMR_STATIC
  #define SQLITE_JMMR_API
#else
  #ifdef _WIN32
    #define SQLITE_JMMR_API __declspec(dllexport)
  #else
    #define SQLITE_JMMR_API
  #endif
#endif

#define SQLITE_JMMR_VERSION "v1.0.0"

#ifdef __cplusplus
extern "C" {
#endif

SQLITE_JMMR_API int sqlite3_jmmr_init(sqlite3 *db, char **pzErrMsg,
                                       const sqlite3_api_routines *pApi);

#ifdef __cplusplus
}  /* end of the 'extern "C"' block */
#endif

#endif /* ifndef SQLITE_JMMR_H */
