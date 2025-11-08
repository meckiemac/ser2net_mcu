#ifndef SER2NET_OPTS_H
#define SER2NET_OPTS_H

/*
 * Build-time feature switches for the ser2net MCU runtime.
 *
 * Projects may override any of these macros by defining them before this
 * header is included (e.g. via compiler -D flags or a dedicated config file).
 */

#ifndef ENABLE_DYNAMIC_SESSIONS
#define ENABLE_DYNAMIC_SESSIONS 1
#endif

#ifndef ENABLE_CONTROL_PORT
#define ENABLE_CONTROL_PORT 1
#endif

#ifndef ENABLE_MONITORING
#define ENABLE_MONITORING 1
#endif

#ifndef ENABLE_WEB_SERVER
#define ENABLE_WEB_SERVER 1
#endif

#ifndef ENABLE_SECURITY_STORE
#define ENABLE_SECURITY_STORE 1
#endif

#ifndef ENABLE_JSON_CONFIG
#define ENABLE_JSON_CONFIG ENABLE_DYNAMIC_SESSIONS
#endif

#if !ENABLE_DYNAMIC_SESSIONS
#undef ENABLE_JSON_CONFIG
#define ENABLE_JSON_CONFIG 0
#endif

#if !ENABLE_CONTROL_PORT
#undef ENABLE_MONITORING
#define ENABLE_MONITORING 0
#endif

#if !ENABLE_SECURITY_STORE
#undef ENABLE_SECURITY_STORE
#define ENABLE_SECURITY_STORE 0
#endif

#endif /* SER2NET_OPTS_H */
