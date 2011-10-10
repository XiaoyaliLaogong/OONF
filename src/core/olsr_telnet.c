/*
 * olsr_telnet.c
 *
 *  Created on: Sep 7, 2011
 *      Author: rogge
 */

#include "common/common_types.h"
#include "common/avl.h"
#include "common/avl_comp.h"

#include "builddata/version.h"

#include "config/cfg_delta.h"
#include "config/cfg_schema.h"

#include "olsr_cfg.h"
#include "olsr_logging.h"
#include "olsr_memcookie.h"
#include "olsr_netaddr_acl.h"
#include "olsr_plugins.h"
#include "olsr_stream_socket.h"
#include "olsr_timer.h"
#include "olsr.h"
#include "olsr_telnet.h"

#define _CFG_TELNET_SECTION "telnet"

/* static function prototypes */
static void _cb_config_changed(void);
static int _cb_telnet_init(struct olsr_stream_session *);
static void _cb_telnet_cleanup(struct olsr_stream_session *);
static void _cb_telnet_create_error(struct olsr_stream_session *,
    enum olsr_stream_errors);
static enum olsr_stream_session_state _cb_telnet_receive_data(
    struct olsr_stream_session *);
static enum olsr_telnet_result _telnet_handle_command(
    struct olsr_telnet_data *);
static struct olsr_telnet_command *_check_telnet_command(
    struct olsr_telnet_data *data, const char *name,
    struct olsr_telnet_command *cmd);

static void _cb_telnet_repeat_timer(void *data);
static enum olsr_telnet_result _cb_telnet_quit(struct olsr_telnet_data *data);
static enum olsr_telnet_result _cb_telnet_help(struct olsr_telnet_data *data);
static enum olsr_telnet_result _cb_telnet_echo(struct olsr_telnet_data *data);
static enum olsr_telnet_result _cb_telnet_repeat(struct olsr_telnet_data *data);
static enum olsr_telnet_result _cb_telnet_timeout(struct olsr_telnet_data *data);
static enum olsr_telnet_result _cb_telnet_version(struct olsr_telnet_data *data);
static enum olsr_telnet_result _cb_telnet_plugin(struct olsr_telnet_data *data);

/* global and static variables */
static struct cfg_schema_section telnet_section = {
  .t_type = _CFG_TELNET_SECTION,
  .t_help = "Settings for the telnet interface",
};

static struct cfg_schema_entry telnet_entries[] = {
  CFG_MAP_ACL_V46(olsr_stream_managed_config,
      acl, "127.0.0.1", "Access control list for telnet interface"),
  CFG_MAP_NETADDR_V4(olsr_stream_managed_config,
      bindto_v4, "127.0.0.1", "Bind telnet ipv4 socket to this address", false),
  CFG_MAP_NETADDR_V6(olsr_stream_managed_config,
      bindto_v6, "::1", "Bind telnet ipv6 socket to this address", false),
  CFG_MAP_INT_MINMAX(olsr_stream_managed_config,
      port, "2006", "Network port for telnet interface", 1, 65535),
};

static struct cfg_delta_handler telnet_handler = {
  .s_type = _CFG_TELNET_SECTION,
  .callback = _cb_config_changed
};

/* built-in telnet commands */
static struct olsr_telnet_command _builtin[] = {
  TELNET_CMD("quit", _cb_telnet_quit, "Ends telnet session"),
  TELNET_CMD("exit", _cb_telnet_quit, "Ends telnet session"),
  TELNET_CMD("help", _cb_telnet_help,
      "help: Display the online help text and a list of commands"),
  TELNET_CMD("echo", _cb_telnet_echo,"echo <string>: Prints a string"),
  TELNET_CMD("repeat", _cb_telnet_repeat,
      "repeat <seconds> <command>: Repeats a telnet command every X seconds"),
  TELNET_CMD("timeout", _cb_telnet_timeout,
      "timeout <seconds> :Sets telnet session timeout"),
  TELNET_CMD("version", _cb_telnet_version, "Displays version of the program"),
  TELNET_CMD("plugin", _cb_telnet_plugin,
        "control plugins dynamically, parameters are 'list',"
        " 'activate <plugin>', 'deactivate <plugin>', "
        "'load <plugin>' and 'unload <plugin>'"),
};

/* remember if initialized or not */
OLSR_SUBSYSTEM_STATE(olsr_telnet_state);

/* telnet session handling */
static struct olsr_memcookie_info *_telnet_memcookie;
static struct olsr_stream_managed _telnet_managed;
static struct olsr_timer_info *_telnet_repeat_timerinfo;

struct avl_tree telnet_cmd_tree;

/**
 * Initialize telnet subsystem
 * @return 0 if initialized successfully, -1 otherwise
 */
int
olsr_telnet_init(void) {
  size_t i;

  if (olsr_subsystem_init(&olsr_telnet_state))
    return 0;

  _telnet_memcookie = olsr_memcookie_add("telnet session",
      sizeof(struct olsr_telnet_session));
  if (_telnet_memcookie == NULL) {
    olsr_subsystem_cleanup(&olsr_telnet_state);
    return -1;
  }

  _telnet_repeat_timerinfo = olsr_timer_add("txt repeat timer", _cb_telnet_repeat_timer, true);
  if (_telnet_repeat_timerinfo == NULL) {
    olsr_memcookie_remove(_telnet_memcookie);
    olsr_subsystem_cleanup(&olsr_telnet_state);
    return -1;
  }

  cfg_schema_add_section(olsr_cfg_get_schema(), &telnet_section);
  cfg_schema_add_entries(&telnet_section, telnet_entries, ARRAYSIZE(telnet_entries));

  cfg_delta_add_handler(olsr_cfg_get_delta(), &telnet_handler);

  olsr_stream_add_managed(&_telnet_managed);
  _telnet_managed.config.session_timeout = 120000; /* 120 seconds */
  _telnet_managed.config.maximum_input_buffer = 4096;
  _telnet_managed.config.allowed_sessions = 3;
  _telnet_managed.config.memcookie = _telnet_memcookie;
  _telnet_managed.config.init = _cb_telnet_init;
  _telnet_managed.config.cleanup = _cb_telnet_cleanup;
  _telnet_managed.config.receive_data = _cb_telnet_receive_data;
  _telnet_managed.config.create_error = _cb_telnet_create_error;

  /* initialize telnet commands */
  avl_init(&telnet_cmd_tree, avl_comp_strcasecmp, false, NULL);
  for (i=0; i<ARRAYSIZE(_builtin); i++) {
    olsr_telnet_add(&_builtin[i]);
  }

  return 0;
}

/**
 * Cleanup all allocated data of telnet subsystem
 */
void
olsr_telnet_cleanup(void) {
  if (olsr_subsystem_cleanup(&olsr_telnet_state))
    return;

  olsr_stream_remove_managed(&_telnet_managed);

  cfg_delta_remove_handler(olsr_cfg_get_delta(), &telnet_handler);
  cfg_schema_remove_section(olsr_cfg_get_schema(), &telnet_section);

  olsr_memcookie_remove(_telnet_memcookie);
}

/**
 * Add a new telnet command to telnet subsystem
 * @param command pointer to initialized telnet command object
 * @return -1 if an error happened, 0 otherwise
 */
int
olsr_telnet_add(struct olsr_telnet_command *command) {
  command->node.key = command->command;
  if (avl_insert(&telnet_cmd_tree, &command->node)) {
    return -1;
  }
  return 0;
}

/**
 * Remove a telnet command from telnet subsystem
 * @param command pointer to telnet command object
 */
void
olsr_telnet_remove(struct olsr_telnet_command *command) {
  avl_remove(&telnet_cmd_tree, &command->node);
}

void
olsr_telnet_stop(struct olsr_telnet_data *data) {
  if (data->stop_handler) {
    data->stop_handler(data);
    data->stop_handler = NULL;
  }
}

/**
 * Execute a telnet command.
 * @param cmd pointer to name of command
 * @param para pointer to parameter string
 * @param out buffer for output of command
 * @param remote pointer to address which triggers the execution
 * @return result of telnet command
 */
enum olsr_telnet_result
olsr_telnet_execute(const char *cmd, const char *para,
    struct autobuf *out, struct netaddr *remote) {
  struct olsr_telnet_data data;
  enum olsr_telnet_result result;

  memset(&data, 0, sizeof(data));
  data.command = cmd;
  data.parameter = para;
  data.out = out;
  data.remote = remote;

  result = _telnet_handle_command(&data);
  olsr_telnet_stop(&data);
  return result;
}

/**
 * Handler for configuration changes
 */
static void
_cb_config_changed(void) {
  struct olsr_stream_managed_config config;
  int ret = -1;

  /* generate binary config */
  memset(&config, 0, sizeof(config));
  if (cfg_schema_tobin(&config, telnet_handler.post,
      telnet_entries, ARRAYSIZE(telnet_entries))) {
    /* error in conversion */
    OLSR_WARN(LOG_TELNET, "Cannot map telnet config to binary data");
    goto apply_config_failed;
  }

  if (olsr_stream_apply_managed(&_telnet_managed, &config)) {
    /* error while updating sockets */
    goto apply_config_failed;
  }
  ret = 0;

  /* fall through */
apply_config_failed:
  olsr_acl_remove(&config.acl);
}

/**
 * Initialization of incoming telnet session
 * @param session pointer to TCP session
 * @return 0
 */
static int
_cb_telnet_init(struct olsr_stream_session *session) {
  struct olsr_telnet_session *telnet_session;

  /* get telnet session pointer */
  telnet_session = (struct olsr_telnet_session *)session;

  telnet_session->data.show_echo = true;
  telnet_session->data.stop_handler = NULL;
  telnet_session->data.timeout_value = 120000;
  telnet_session->data.out = &telnet_session->session.out;
  telnet_session->data.remote = &telnet_session->session.remote_address;

  list_init_head(&telnet_session->data.cleanup_list);

  return 0;
}

/**
 * Cleanup of telnet session
 * @param session pointer to TCP session
 */
static void
_cb_telnet_cleanup(struct olsr_stream_session *session) {
  struct olsr_telnet_session *telnet_session;
  struct olsr_telnet_cleanup *handler, *it;

  /* get telnet session pointer */
  telnet_session = (struct olsr_telnet_session *)session;

  /* stop continuous commands */
  olsr_telnet_stop(&telnet_session->data);

  /* call all cleanup handlers */
  list_for_each_element_safe(&telnet_session->data.cleanup_list, handler, node, it) {
    /* remove from list first */
    olsr_telnet_remove_cleanup(handler);

    /* after this command the handler pointer might not be valid anymore */
    handler->cleanup_handler(handler);
  }
}

/**
 * Create error string for telnet session
 * @param session pointer to TCP stream
 * @param error TCP error code to generate
 */
static void
_cb_telnet_create_error(struct olsr_stream_session *session,
    enum olsr_stream_errors error) {
  switch(error) {
    case STREAM_REQUEST_FORBIDDEN:
      /* no message */
      break;
    case STREAM_REQUEST_TOO_LARGE:
      abuf_puts(&session->out, "Input buffer overflow, ending connection\n");
      break;
    case STREAM_SERVICE_UNAVAILABLE:
      abuf_puts(&session->out, "Telnet service unavailable, too many sessions\n");
      break;
  }
}

/**
 * Handler for receiving data from telnet session
 * @param session pointer to TCP session
 * @return TCP session state
 */
static enum olsr_stream_session_state
_cb_telnet_receive_data(struct olsr_stream_session *session) {
  static const char defaultCommand[] = "/link/neigh/topology/hna/mid/routes";
  static char tmpbuf[128];

  struct olsr_telnet_session *telnet_session;
  enum olsr_telnet_result cmd_result;
  char *eol;
  int len;
  bool processedCommand = false, chainCommands = false;
  uint32_t old_timeout;

  /* get telnet session pointer */
  telnet_session = (struct olsr_telnet_session *)session;
  old_timeout = session->timeout->timer_period;

  /* loop over input */
  while (session->in.len > 0) {
    char *para = NULL, *cmd = NULL, *next = NULL;

    /* search for end of line */
    eol = memchr(session->in.buf, '\n', session->in.len);

    if (eol == NULL) {
      break;
    }

    /* terminate line with a 0 */
    if (eol != session->in.buf && eol[-1] == '\r') {
      eol[-1] = 0;
    }
    *eol++ = 0;

    /* handle line */
    OLSR_DEBUG(LOG_TELNET, "Interactive console: %s\n", session->in.buf);
    cmd = &session->in.buf[0];
    processedCommand = true;

    /* apply default command */
    if (strcmp(cmd, "/") == 0) {
      strcpy(tmpbuf, defaultCommand);
      cmd = tmpbuf;
    }

    if (cmd[0] == '/') {
      cmd++;
      chainCommands = true;
    }
    while (cmd) {
      len = session->out.len;

      /* handle difference between multicommand and singlecommand mode */
      if (chainCommands) {
        next = strchr(cmd, '/');
        if (next) {
          *next++ = 0;
        }
      }
      para = strchr(cmd, ' ');
      if (para != NULL) {
        *para++ = 0;
      }

      /* if we are doing continous output, stop it ! */
      if (telnet_session->data.stop_handler) {
        telnet_session->data.stop_handler(&telnet_session->data);
        telnet_session->data.stop_handler = NULL;
      }

      if (strlen(cmd) != 0) {
        OLSR_DEBUG(LOG_TELNET, "Processing telnet command: '%s' '%s'",
            cmd, para);

        telnet_session->data.command = cmd;
        telnet_session->data.parameter = para;
        cmd_result = _telnet_handle_command(&telnet_session->data);
        switch (cmd_result) {
          case TELNET_RESULT_ACTIVE:
            break;
          case TELNET_RESULT_CONTINOUS:
            break;
          case TELNET_RESULT_INTERNAL_ERROR:
            session->out.len = len;
            abuf_appendf(&session->out,
                "Error in autobuffer during command '%s'.\n", cmd);
            break;
          case TELNET_RESULT_UNKNOWN_COMMAND:
            session->out.len = len;
            abuf_appendf(&session->out, "Error, unknown command '%s'\n", cmd);
            break;
          case TELNET_RESULT_QUIT:
            return STREAM_SESSION_SEND_AND_QUIT;
        }
        /* put an empty line behind each command */
        if (telnet_session->data.show_echo) {
          abuf_puts(&session->out, "\n");
        }
      }
      cmd = next;
    }

    /* remove line from input buffer */
    abuf_pull(&session->in, eol - session->in.buf);

    if (session->in.buf[0] == '/') {
      /* end of multiple command line */
      return STREAM_SESSION_SEND_AND_QUIT;
    }
  }

  /* reset timeout */
  olsr_stream_set_timeout(session, telnet_session->data.timeout_value);

  /* print prompt */
  if (processedCommand && session->state == STREAM_SESSION_ACTIVE
      && telnet_session->data.show_echo) {
    abuf_puts(&session->out, "> ");
  }

  return STREAM_SESSION_ACTIVE;
}

/**
 * Helper function to call telnet command handler
 * @param data pointer to telnet data
 * @return telnet command result
 */
static enum olsr_telnet_result
_telnet_handle_command(struct olsr_telnet_data *data) {
  struct olsr_telnet_command *cmd;
#if !defined(REMOVE_LOG_INFO)
  struct netaddr_str buf;
#endif
  cmd = _check_telnet_command(data, data->command, NULL);
  if (cmd == NULL) {
    return TELNET_RESULT_UNKNOWN_COMMAND;
  }

  OLSR_INFO(LOG_TELNET, "Executing command from %s: %s %s",
      netaddr_to_string(&buf, data->remote), data->command,
      data->parameter == NULL ? "" : data->parameter);
  return cmd->handler(data);
}

/**
 * Checks for existing (and allowed) telnet command.
 * Either name or cmd should be NULL, but not both.
 * @param data pointer to telnet data
 * @param name pointer to command name (might be NULL)
 * @param cmd pointer to telnet command object (might be NULL)
 * @return telnet command object or NULL if not found or forbidden
 */
static struct olsr_telnet_command *
_check_telnet_command(struct olsr_telnet_data *data,
    const char *name, struct olsr_telnet_command *cmd) {
#if !defined(REMOVE_LOG_DEBUG)
  struct netaddr_str buf;
#endif

  if (cmd == NULL) {
    cmd = avl_find_element(&telnet_cmd_tree, name, cmd, node);
    if (cmd == NULL) {
      return cmd;
    }
  }
  if (cmd->acl == NULL) {
    return cmd;
  }

  if (!olsr_acl_check_accept(cmd->acl, data->remote)) {
    OLSR_DEBUG(LOG_TELNET, "Blocked telnet command '%s' to '%s' because of acl",
        cmd->command, netaddr_to_string(&buf, data->remote));
    return NULL;
  }
  return cmd;
}

/**
 * Telnet command 'quit'
 * @param data pointer to telnet data
 * @return telnet command result
 */
static enum olsr_telnet_result
_cb_telnet_quit(struct olsr_telnet_data *data __attribute__((unused))) {
  return TELNET_RESULT_QUIT;
}

/**
 * Telnet command 'help'
 * @param data pointer to telnet data
 * @return telnet command result
 */
static enum olsr_telnet_result
_cb_telnet_help(struct olsr_telnet_data *data) {
  struct olsr_telnet_command *ptr, *iterator;

  if (data->parameter != NULL && data->parameter[0] != 0) {
    ptr = _check_telnet_command(data, data->parameter, NULL);
    if (ptr == NULL) {
      abuf_appendf(data->out, "No help text found for command: %s\n", data->parameter);
      return TELNET_RESULT_ACTIVE;
    }

    if (ptr->help_handler) {
      ptr->help_handler(data);
    }
    else {
      if (abuf_appendf(data->out, "%s\n", ptr->help) < 0) {
        return TELNET_RESULT_INTERNAL_ERROR;
      }
    }
    return TELNET_RESULT_ACTIVE;
  }

  if (abuf_puts(data->out, "Known commands:\n") < 0) {
    return TELNET_RESULT_INTERNAL_ERROR;
  }

  FOR_ALL_TELNET_COMMANDS(ptr, iterator) {
    if (_check_telnet_command(data, NULL, ptr)) {
      if (abuf_appendf(data->out, "  %s\n", ptr->command) < 0) {
        return TELNET_RESULT_INTERNAL_ERROR;
      }
    }
  }

  if (abuf_puts(data->out, "Use 'help <command> to see a help text for one command\n") < 0) {
    return TELNET_RESULT_INTERNAL_ERROR;
  }
  return TELNET_RESULT_ACTIVE;
}

/**
 * Telnet command 'echo'
 * @param data pointer to telnet data
 * @return telnet command result
 */
static enum olsr_telnet_result
_cb_telnet_echo(struct olsr_telnet_data *data) {

  if (abuf_appendf(data->out, "%s\n",
      data->parameter == NULL ? "" : data->parameter) < 0) {
    return TELNET_RESULT_INTERNAL_ERROR;
  }
  return TELNET_RESULT_ACTIVE;
}

/**
 * Telnet command 'timeout'
 * @param data pointer to telnet data
 * @return telnet command result
 */
static enum olsr_telnet_result
_cb_telnet_timeout(struct olsr_telnet_data *data) {
  data->timeout_value = (uint32_t)strtoul(data->parameter, NULL, 10) * 1000;
  return TELNET_RESULT_ACTIVE;
}

/**
 * Stop handler for repeating telnet commands
 * @param data pointer to telnet data
 */
static void
_cb_telnet_repeat_stophandler(struct olsr_telnet_data *data) {
  olsr_timer_stop((struct olsr_timer_entry *)data->stop_data[0]);
  free(data->stop_data[1]);

  data->stop_handler = NULL;
  data->stop_data[0] = NULL;
  data->stop_data[1] = NULL;
  data->stop_data[2] = NULL;
}

/**
 * Timer event handler for repeating telnet commands
 * @param ptr pointer to custom data
 */
static void
_cb_telnet_repeat_timer(void *ptr) {
  struct olsr_telnet_data *telnet_data = ptr;
  struct olsr_telnet_session *session;

  /* set command/parameter with repeat settings */
  telnet_data->command = telnet_data->stop_data[1];
  telnet_data->parameter = telnet_data->stop_data[2];

  if (_telnet_handle_command(telnet_data) != TELNET_RESULT_ACTIVE) {
    telnet_data->stop_handler(telnet_data);
  }

  /* reconstruct original session pointer */
  session = container_of(telnet_data, struct olsr_telnet_session, data);
  olsr_stream_flush(&session->session);
}

/**
 * Telnet command 'repeat'
 * @param data pointer to telnet data
 * @return telnet command result
 */
static enum olsr_telnet_result
_cb_telnet_repeat(struct olsr_telnet_data *data) {
  struct olsr_timer_entry *timer;
  int interval = 0;
  char *ptr = NULL;

  if (data->stop_handler) {
    abuf_puts(data->out, "Error, you cannot stack continous output commands\n");
    return TELNET_RESULT_ACTIVE;
  }

  if (data->parameter == NULL || (ptr = strchr(data->parameter, ' ')) == NULL) {
    abuf_puts(data->out, "Missing parameters for repeat\n");
    return TELNET_RESULT_ACTIVE;
  }

  ptr++;

  interval = atoi(data->parameter);

  timer = olsr_timer_start(interval * 1000, 0, data, _telnet_repeat_timerinfo);
  data->stop_handler = _cb_telnet_repeat_stophandler;
  data->stop_data[0] = timer;
  data->stop_data[1] = strdup(ptr);
  data->stop_data[2] = NULL;

  /* split command/parameter and remember it */
  ptr = strchr(data->stop_data[1], ' ');
  if (ptr != NULL) {
    /* found a parameter */
    *ptr++ = 0;
    data->stop_data[2] = ptr;
  }

  /* start command the first time */
  data->command = data->stop_data[1];
  data->parameter = data->stop_data[2];

  if (_telnet_handle_command(data) != TELNET_RESULT_ACTIVE) {
    data->stop_handler(data);
  }

  return TELNET_RESULT_CONTINOUS;
}

/**
 * Telnet command 'version'
 * @param data pointer to telnet data
 * @return telnet command result
 */
static enum olsr_telnet_result
_cb_telnet_version(struct olsr_telnet_data *data) {
  olsr_builddata_printversion(data->out);
  return TELNET_RESULT_ACTIVE;
}

/**
 * Telnet command 'plugin'
 * @param data pointer to telnet data
 * @return telnet command result
 */
static enum olsr_telnet_result
_cb_telnet_plugin(struct olsr_telnet_data *data) {
  struct olsr_plugin *plugin, *iterator;
  const char *plugin_name = NULL;

  if (data->parameter == NULL || strcasecmp(data->parameter, "list") == 0) {
    if (abuf_puts(data->out, "Plugins:\n") < 0) {
      return TELNET_RESULT_INTERNAL_ERROR;
    }
    OLSR_FOR_ALL_PLUGIN_ENTRIES(plugin, iterator) {
      if (abuf_appendf(data->out, " %-30s\t%s\t%s\n",
          plugin->name, olsr_plugins_is_enabled(plugin) ? "enabled" : "",
          olsr_plugins_is_static(plugin) ? "static" : "") < 0) {
        return TELNET_RESULT_INTERNAL_ERROR;
      }
    }
    return TELNET_RESULT_ACTIVE;
  }

  plugin_name = strchr(data->parameter, ' ');
  if (plugin_name == NULL) {
    if (abuf_appendf(data->out, "Error, missing or unknown parameter\n") < 0) {
      return TELNET_RESULT_INTERNAL_ERROR;
    }
    return TELNET_RESULT_ACTIVE;
  }

  /* skip whitespaces */
  while (isspace(*plugin_name)) {
    plugin_name++;
  }

  plugin = olsr_plugins_get(plugin_name);
  if (str_hasnextword(data->parameter, "load") == NULL) {
    if (plugin != NULL) {
      abuf_appendf(data->out, "Plugin %s already loaded\n", plugin_name);
      return TELNET_RESULT_ACTIVE;
    }
    plugin = olsr_plugins_load(plugin_name);
    if (plugin != NULL) {
      abuf_appendf(data->out, "Plugin %s successfully loaded\n", plugin_name);
    }
    else {
      abuf_appendf(data->out, "Could not load plugin %s\n", plugin_name);
    }
    return TELNET_RESULT_ACTIVE;
  }

  if (plugin == NULL) {
    if (abuf_appendf(data->out,
        "Error, could not find plugin '%s'.\n", plugin_name) < 0) {
      return TELNET_RESULT_INTERNAL_ERROR;
    }
    return TELNET_RESULT_ACTIVE;
  }
  if (str_hasnextword(data->parameter, "activate") == NULL) {
    if (olsr_plugins_is_enabled(plugin)) {
      abuf_appendf(data->out, "Plugin %s already active\n", plugin_name);
    }
    else {
      if (olsr_plugins_enable(plugin)) {
        abuf_appendf(data->out, "Could not activate plugin %s\n", plugin_name);
      }
      else {
        abuf_appendf(data->out, "Plugin %s successfully activated\n", plugin_name);
      }
    }
  }
  else if (str_hasnextword(data->parameter, "deactivate") == NULL) {
    if (!olsr_plugins_is_enabled(plugin)) {
      abuf_appendf(data->out, "Plugin %s is not active\n", plugin_name);
    }
    else {
      if (olsr_plugins_disable(plugin)) {
        abuf_appendf(data->out, "Could not deactivate plugin %s\n", plugin_name);
      }
      else {
        abuf_appendf(data->out, "Plugin %s successfully deactivated\n", plugin_name);
      }
    }
  }
  else if (str_hasnextword(data->parameter, "unload") == NULL) {
    if (olsr_plugins_is_static(plugin)) {
      abuf_appendf(data->out, "Plugin %s is static and cannot be unloaded\n", plugin_name);
    }
    else {
      if (olsr_plugins_unload(plugin)) {
        abuf_appendf(data->out, "Could not unload plugin %s\n", plugin_name);
      }
      else {
        abuf_appendf(data->out, "Plugin %s successfully unloaded\n", plugin_name);
      }
    }
  }
  else {
    abuf_appendf(data->out, "Unknown command '%s %s %s'.\n",
        data->command, data->parameter, plugin_name);
  }
  return TELNET_RESULT_ACTIVE;
}
