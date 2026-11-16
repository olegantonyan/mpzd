#include "player/pw_sinks.h"

#include <pipewire/pipewire.h>
#include <string.h>

#define LOG_MODULE "pipewire"

typedef struct {
  struct pw_main_loop *loop;
  OutputList *out;
  int seq;
  bool done;
} Ctx;

static void on_global(void *data, uint32_t id, uint32_t perms, const char *type, uint32_t version, const struct spa_dict *props) {
  Ctx *c = data;
  if (!props || strcmp(type, PW_TYPE_INTERFACE_Node) != 0) {
    return;
  }
  const char *cls = spa_dict_lookup(props, PW_KEY_MEDIA_CLASS);
  const char *name = spa_dict_lookup(props, PW_KEY_NODE_NAME);
  if (cls && name && strcmp(cls, "Audio/Sink") == 0) {
    output_list_push(c->out, name, spa_dict_lookup(props, PW_KEY_NODE_DESCRIPTION));
  }
}

static const struct pw_registry_events registry_events = {
  PW_VERSION_REGISTRY_EVENTS,
  .global = on_global,
};

static void on_done(void *data, uint32_t id, int seq) {
  Ctx *c = data;
  if (id == PW_ID_CORE && seq == c->seq) {
    c->done = true;
    pw_main_loop_quit(c->loop);
  }
}

static void on_error(void *data, uint32_t id, int seq, int res, const char *message) {
  Ctx *c = data;
  if (id == PW_ID_CORE) {
    LOG("%s", message);
    pw_main_loop_quit(c->loop);
  }
}

static const struct pw_core_events core_events = {
  PW_VERSION_CORE_EVENTS,
  .done = on_done,
  .error = on_error,
};

static void on_timeout(void *data, uint64_t expirations) {
  Ctx *c = data;
  LOG("timed out listing sinks");
  pw_main_loop_quit(c->loop);
}

void pw_sinks_init(void) {
  pw_init(NULL, NULL);
}

void pw_sinks_deinit(void) {
  pw_deinit();
}

bool pw_sinks_list(OutputList *out) {
  Ctx c = { .out = out };
  c.loop = pw_main_loop_new(NULL);
  if (!c.loop) {
    return false;
  }
  struct pw_loop *loop = pw_main_loop_get_loop(c.loop);
  struct pw_context *ctx = pw_context_new(loop, pw_properties_new("support.dbus", "false", NULL), 0);
  struct pw_core *core = ctx ? pw_context_connect(ctx, NULL, 0) : NULL;
  if (core) {
    struct pw_registry *reg = pw_core_get_registry(core, PW_VERSION_REGISTRY, 0);
    struct spa_hook core_hook = { 0 };
    struct spa_hook reg_hook = { 0 };
    pw_core_add_listener(core, &core_hook, &core_events, &c);
    pw_registry_add_listener(reg, &reg_hook, &registry_events, &c);
    struct spa_source *timer = pw_loop_add_timer(loop, on_timeout, &c);
    struct timespec timeout = { .tv_sec = 1 };
    pw_loop_update_timer(loop, timer, &timeout, NULL, false);
    c.seq = pw_core_sync(core, PW_ID_CORE, 0);
    pw_main_loop_run(c.loop);
    pw_loop_destroy_source(loop, timer);
    spa_hook_remove(&reg_hook);
    spa_hook_remove(&core_hook);
    pw_proxy_destroy((struct pw_proxy *)reg);
    pw_core_disconnect(core);
  } else {
    LOG("cannot connect to PipeWire");
  }
  if (ctx) {
    pw_context_destroy(ctx);
  }
  pw_main_loop_destroy(c.loop);
  return c.done;
}
