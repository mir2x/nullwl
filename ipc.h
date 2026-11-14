/*
 * IPC transport: a Unix socket speaking newline-delimited JSON, driven by the
 * wayland event loop. Knows nothing about nullwl itself; ipcstate.h supplies
 * the commands and the events.
 *
 *   request   one JSON object per line:  {"cmd":"dispatch","action":"view","arg":"3"}
 *   reply     {"ok":true,"data":...}  or  {"ok":false,"error":"..."}
 *   event     {"event":"focus","data":{...}}     (only after "subscribe")
 *
 * Request objects are flat: values are strings, numbers, booleans, null, or
 * arrays of those (an array is delivered as one comma-joined string).
 */
#ifndef IPC_H
#define IPC_H

#include <stddef.h>
#include <wayland-server-core.h>

/* ---- growable output buffer with JSON helpers ---- */
typedef struct {
	char *s;
	size_t len, cap;
} JBuf;

void jb_init(JBuf *b);
void jb_free(JBuf *b);
void jb_printf(JBuf *b, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
void jb_str(JBuf *b, const char *s);   /* quoted and escaped; NULL becomes null */
void jb_sep(JBuf *b);                  /* comma, unless at the start or right after { [ : */
void jb_key(JBuf *b, const char *key); /* jb_sep() then "key": */

/* ---- requests ---- */
#define IPC_MAX_FIELDS 16
typedef struct {
	char *keys[IPC_MAX_FIELDS];
	char *vals[IPC_MAX_FIELDS];
	int n;
} IpcReq; /* keys and vals are malloc'd; free with ipc_req_free() */

/* Parse one line. Returns 0 on success; on failure writes a reason to err. */
int ipc_req_parse(IpcReq *req, const char *line, char *err, size_t errlen);
void ipc_req_free(IpcReq *req);
const char *ipc_req_get(const IpcReq *req, const char *key); /* NULL if absent */

/* ---- server ---- */
typedef struct Ipc Ipc;
typedef struct IpcClient IpcClient;

/* Called for every well-formed request. The handler must answer with exactly
 * one ipc_reply_*() call (it may do so later, but not after the client is
 * gone). */
typedef void (*IpcHandler)(IpcClient *client, const IpcReq *req, void *data);

Ipc *ipc_create(struct wl_event_loop *loop, const char *path, IpcHandler handler, void *data);
void ipc_destroy(Ipc *ipc);

void ipc_reply_ok(IpcClient *c, const char *data_json); /* NULL data -> null */
void ipc_reply_err(IpcClient *c, const char *fmt, ...) __attribute__((format(printf, 2, 3)));

/* "events" is a comma-separated list of event names; empty or NULL = all */
void ipc_subscribe(IpcClient *c, const char *events);
int ipc_nsubscribers(const Ipc *ipc);
Ipc *ipc_client_server(IpcClient *c);

/* Send {"event":name,"data":data_json} to every client subscribed to name. */
void ipc_emit(Ipc *ipc, const char *name, const char *data_json);

#endif /* IPC_H */
