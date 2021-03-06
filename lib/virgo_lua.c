/*
 *  Copyright 2012 Rackspace
 *
 *  Licensed under the Apache License, Version 2.0 (the "License");
 *  you may not use this file except in compliance with the License.
 *  You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 *  Unless required by applicable law or agreed to in writing, software
 *  distributed under the License is distributed on an "AS IS" BASIS,
 *  WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *  See the License for the specific language governing permissions and
 *  limitations under the License.
 *
 */

#include "virgo.h"
#include "virgo__types.h"
#include "virgo__time.h"
#include "virgo__lua.h"
#include "virgo__util.h"
#include "virgo_paths.h"
#include "virgo_error.h"

#include "lua.h"
#include "lauxlib.h"
#include "lualib.h"

#include "luajit.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#ifdef _WIN32
#include <direct.h>
#else
#include <sys/file.h>
#endif

#include "luvit.h"
#include "uv.h"
#include "utils.h"
#include "luv.h"
#include "lconstants.h"
#include "lhttp_parser.h"
#include "lenv.h"
#include "lyajl.h"
#include "lcrypto.h"
#include "luvit_exports.h"
#include "virgo_exports.h"

extern int luaopen_sigar(lua_State *L);
extern int virgo__lua_exec(lua_State *L);
extern int virgo__lua_fetch_msi_version(lua_State *L);

static void
virgo__lua_luvit_init(virgo_t *v) {
  /* Hack to keep the linker from killing symbols that luajit pulls in at runtime :( */
  lua_State *L;
  virgo__suck_in_symbols();
  luvit__suck_in_symbols();
  L = v->L;

  luvit_init(L, uv_default_loop(), v->argc, v->argv);

  /* Pull up the preload table */
  lua_getglobal(L, "package");
  lua_getfield(L, -1, "preload");
  lua_remove(L, -2);

  /* Register constants */
  lua_pushcfunction(L, virgo__lua_logging_open);
  lua_setfield(L, -2, "logging");

  lua_pop(L, 1);
}

static void
virgo__set_virgo_key(lua_State *L, const char *key, const char *value) {
  /* Set virgo.os */
  lua_getglobal(L, "virgo");
  lua_pushstring(L, key);
  lua_pushstring(L, value);
  lua_settable(L, -3);
}

static void
virgo__push_function(lua_State *L, const char *name, lua_CFunction cfunc) {
  lua_getglobal(L, "virgo");
  lua_pushcfunction(L, cfunc);
  lua_setfield(L, -2, name);
}

static int
virgo__lua_close_pid(lua_State *L) {
  virgo_t *v = virgo__lua_context(L);
  
  if (v->pid_fd < 0) {
    return 0;
  }

  close(v->pid_fd);
  v->pid_fd = -1;

  return 0;
}

static int
virgo__lua_write_pid(lua_State *L) {
#ifdef _WIN32
  lua_pushnumber(L, VIRGO_ENOTIMPL);
  lua_pushstring(L, "Not Implemented on Windows");
  return 2;
#else
  virgo_t *v = virgo__lua_context(L);
  const char* path = luaL_checkstring(L, -1);
  int rv;
  int stale_file = FALSE;
  struct stat st;
  char buffer[24];

  if (v->pid_fd >= 0) {
    lua_pushnumber(L, 1);
    lua_pushstring(L, "pidfile in use");
    return 2;
  }

  rv = stat(path, &st);
  if (rv == 0) {
    stale_file = TRUE;
  }

  v->pid_fd = open(path, O_RDWR | O_CREAT, S_IRUSR | S_IWUSR | S_IRGRP | S_IROTH);
  if (v->pid_fd < 0) {
    lua_pushnumber(L, errno);
    lua_pushstring(L, strerror(errno));
    return 2;
  }

  rv = flock(v->pid_fd, LOCK_EX | LOCK_NB);
  if (rv < 0) {
    close(v->pid_fd);
    lua_pushnumber(L, errno);
    lua_pushstring(L, strerror(errno));
    return 2;
  }

  if (stale_file) {
    virgo_log_infof(v, "Stale PID file found. Overwriting.");
  }

  snprintf(buffer, sizeof(buffer), "%ld", (long)getpid());

  lseek(v->pid_fd, SEEK_SET, 0);
  ftruncate(v->pid_fd, 0);
  write(v->pid_fd, buffer, strlen(buffer));
  fsync(v->pid_fd);
  lseek(v->pid_fd, SEEK_SET, 0);

  lua_pushnil(L);
  return 1;
#endif
}

static int
virgo__lua_restart_on_upgrade(lua_State *L) {
#ifndef _WIN32
  int pid;

  pid = fork();
  if (pid < 0) {
    luaL_error(L, strerror(errno));
    return 0;
  }

  if (pid > 0) {
    setsid();
    system("/etc/init.d/rackspace-monitoring-agent restart");
    exit(0);
  }

#endif
  return 0;
}

static int
virgo__lua_force_dump(lua_State *L) {
  virgo__force_dump();
  return 0;
}

static int
virgo__lua_force_crash(lua_State *L) {
  volatile int* a = (int*)(NULL);
  *a = 1;
  return 0;
}

static int
virgo__lua_handle_crash(lua_State *L) {
  const char *lua_err;
  const char *lua_tb;
  char* lua_msg;
  size_t nlen;
  int rv;
  virgo_t* v;

  /* grab the error for reporting to stderr */
  lua_err = lua_tostring(L, -1);
  /* Push the exception into virgo for the dumper */
  lua_getglobal(L, "virgo");
  lua_insert(L, -2);
  lua_setfield(L, -2, "exception");
  lua_pop(L, 1);
  /* do dump */

  v = virgo__lua_context(L);
  if (virgo__argv_has_flag(v, NULL, "--production") == 1){
    virgo__force_dump();
  }

  /* push a traceback onto the stack */
  lua_getglobal(L, "require");
  lua_pushliteral(L, "debug");
  lua_call(L, 1, 1);
  lua_getfield(L, -1, "traceback");
  lua_pushliteral(L, "");
  /* skip the current function in the traceback */
  lua_pushnumber(L, 2);
  rv = lua_pcall(L, 2, 1, 0);
  if (rv != 0){
    lua_pushstring(L, lua_err);
    fprintf(stderr, "%s", "Warning: could not generate a lua traceback.");
    return 1;
  }
  /* grab the traceback and concat it with the error string */
  lua_tb = lua_tostring(L, -1);

  nlen = strlen(lua_err) + strlen(lua_tb) + 1;
  lua_msg = malloc(nlen);
  lua_msg[0] = '\0';
  strcat(lua_msg, lua_err);
  strcat(lua_msg, lua_tb);
  /* push the new error string back onto the stack */
  lua_pushstring(L, lua_msg);

  free((void*)lua_msg);
  return 1;
}


#ifdef _WIN32

#include "Shlwapi.h"

static int
virgo__lua_win32_get_associated_exe(lua_State *L) {
  DWORD exePathLen = MAX_PATH;
  HRESULT hr;
  TCHAR exePath[ MAX_PATH ] = { 0 };
  virgo_t* v = virgo__lua_context(L);
  const char *extension = luaL_checkstring(L, 1);
  const char *verb = luaL_checkstring(L, 2);

  hr = AssocQueryString(ASSOCF_INIT_IGNOREUNKNOWN, ASSOCSTR_EXECUTABLE,
                        extension, verb,
                        exePath, &exePathLen);
  if (hr < 0) {
    lua_pushnil(L);
    lua_pushfstring(L, "could not find file association: (ext: %s verb: %s) hr: '%d'", extension, verb, hr);
    return 2;
  }

  lua_pushlstring(L, exePath, exePathLen - 1);
  return 1;
}
#endif

static int
virgo__lua_is_stdio_available(lua_State *L) {
  int ret = 1;

#ifdef _WIN32
  int i;
  DWORD ios[3] = {STD_INPUT_HANDLE, STD_OUTPUT_HANDLE, STD_ERROR_HANDLE};
  for(i = 0; i < 3 && ret; i++) {
    HANDLE h = GetStdHandle(ios[i]);
    ret = (h != NULL) && (h != INVALID_HANDLE_VALUE);
  }
#endif

  lua_pushboolean(L, ret);
  return 1;
}

static int
virgo__lua_get_log_fileno(lua_State *L) {
  virgo_t* v = virgo__lua_context(L);

  lua_pushnumber(L, fileno(v->log_fp));
  return 1;
}

virgo_error_t*
virgo__lua_init(virgo_t *v)
{
  lua_State *L = luaL_newstate();

  v->L = L;

  lua_pushlightuserdata(L, v);
  lua_setfield(L, LUA_REGISTRYINDEX, "virgo.context");

  /* Create global config object called virgo */
  lua_newtable(L);
  lua_setglobal(L, "virgo");

  virgo__push_function(L, "force_crash", virgo__lua_force_crash);
  virgo__push_function(L, "gmtnow", virgo_time_now);
  virgo__push_function(L, "force_dump", virgo__lua_force_dump);
  virgo__push_function(L, "perform_restart_on_upgrade", virgo__lua_restart_on_upgrade);
  virgo__push_function(L, "write_pid", virgo__lua_write_pid);
  virgo__push_function(L, "close_pid", virgo__lua_close_pid);

#ifdef _WIN32
  virgo__push_function(L, "win32_get_associated_exe", virgo__lua_win32_get_associated_exe);
  virgo__push_function(L, "fetch_msi_version", virgo__lua_fetch_msi_version);
#endif
  virgo__push_function(L, "is_stdio_available", virgo__lua_is_stdio_available);
  virgo__push_function(L, "get_log_fileno", virgo__lua_get_log_fileno);
  virgo__push_function(L, "perform_upgrade", virgo__lua_perform_upgrade);
  virgo__set_virgo_key(L, "os", VIRGO_OS);
  virgo__set_virgo_key(L, "version", VIRGO_VERSION);
  virgo__set_virgo_key(L, "platform", VIRGO_PLATFORM);
  virgo__set_virgo_key(L, "default_name", VIRGO_DEFAULT_NAME);
  virgo__set_virgo_key(L, "default_config_filename", VIRGO_DEFAULT_CONFIG_FILENAME);
  virgo__set_virgo_key(L, "exit_on_upgrade", v->exit_on_upgrade ? "true" : "false");
  virgo__set_virgo_key(L, "restart_on_upgrade", v->restart_on_upgrade ? "true" : "false");
  virgo__set_virgo_key(L, "pkg_name", PKG_NAME);

  luaL_openlibs(L);
  luaopen_sigar(L);

  virgo__lua_paths(L);
  virgo__lua_vfs_init(L);
  virgo__lua_loader_init(L);
  virgo__lua_debugger_init(L);
  virgo__lua_crashreporter_init(L);
  virgo__lua_exec(L);

  virgo__lua_luvit_init(v);


  lua_getglobal(L, "require");
  lua_pushliteral(L, "virgo_utils");
  lua_call(L, 1, 1);

  lua_getglobal(L, "require");
  lua_pushliteral(L, "tls");
  lua_call(L, 1, 1);

  return VIRGO_SUCCESS;
}

virgo_error_t*
virgo__lua_run(virgo_t *v)
{
  int rv;
  const char* lua_err;
  lua_State* L = v->L;

  virgo__set_virgo_key(L, "loaded_zip_path", v->lua_load_path);

  lua_getglobal(L, "require");
  lua_pushliteral(L, "virgo_init");
  lua_call(L, 1, 1);

  /* push on the error handler */
  lua_pushcfunction(L, virgo__lua_handle_crash);

  lua_getglobal(L, "virgo_entry");
  if (!lua_isfunction(L, -1)) {
    return virgo_error_create(VIRGO_EINVAL, "virgo_init.lua was not properly installed");
  }

  lua_pushstring(L, v->lua_default_module);

  /* pcall virgo.run(default) with error handler handle_crash */
  rv = lua_pcall(L, 1, 0, -3);

  if (rv != 0) {
    lua_err = lua_tostring(v->L, -1);
    return virgo_error_createf(VIRGO_EINVAL, "\nLua Runtime Error: %s", lua_err);
  }

  return VIRGO_SUCCESS;
}

void
virgo__lua_destroy(virgo_t *v)
{
  if (v->L) {
    lua_close(v->L);
    v->L = NULL;
  }
}

virgo_t*
virgo__lua_context(lua_State *L)
{
  virgo_t* v;

  lua_getfield(L, LUA_REGISTRYINDEX, "virgo.context");
  v = lua_touserdata(L, -1);
  lua_pop(L, 1);

  return v;
}

