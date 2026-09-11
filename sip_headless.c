/* ======================================================================
 * sip_headless.c — Headless-SIP-Client für Linux
 *
 *   Steuerung : Unix-Domain-Socket /tmp/sipctl.sock (zeilenweise Befehle)
 *   Audio     : ALSA-Geräte (pjmedia-ALSA-Backend)
 *   Signaling : PJSIP (pjsua-API aus pjproject)
 *
 * Bauen:
 *   gcc -O2 -Wall -o sip_headless sip_headless.c \
 *       $(pkg-config --cflags --libs libpjproject) -lpthread
 *
 * Steuerung (z. B. mit socat):
 *   socat - UNIX-CONNECT:/tmp/sipctl.sock
 *     register sip:100@pbx.local user=100 pass=geheim
 *     devs
 *     snd 0 0
 *     call sip:200@pbx.local
 *     answer
 *     hangup
 *     status
 *     quit
 * ====================================================================== */

#include <pthread.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h> /* strcasecmp */
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

#include <pjsua-lib/pjsua.h>

#define CTL_SOCK_PATH "/tmp/sipctl.sock"
#define DEFAULT_UDP_PORT 5060
#define LINE_BUF_LEN 1024
#define MAX_AUDIO_DEVS 64 /* <-- diese Zeile ergänzen */

/* ------------------------------ Globals ---------------------------- */

static volatile sig_atomic_t g_running = 1;

static pjsua_acc_id g_acc_id = PJSUA_INVALID_ID;
static int g_ctl_fd = -1; /* aktueller Client-FD */
static pthread_mutex_t g_out_mutex = PTHREAD_MUTEX_INITIALIZER;

/* PJSIP speichert nur die pj_str-Zeiger, nicht die Daten!
 * Deshalb müssen Account-Strings in langlebigen Puffern liegen. */
static char g_id_str[128];
static char g_reg_str[128];
static char g_user_str[64];
static char g_pass_str[64];

/* ------------------------- Hilfsfunktionen ------------------------ */

/* Ausgabe nach stdout UND an den verbundenen Control-Client */
static void out(const char *fmt, ...) {
  char msg[512];
  va_list ap;

  va_start(ap, fmt);
  vsnprintf(msg, sizeof(msg), fmt, ap);
  va_end(ap);

  fputs(msg, stdout);
  fflush(stdout);

  pthread_mutex_lock(&g_out_mutex);
  if (g_ctl_fd >= 0)
    dprintf(g_ctl_fd, "%s", msg);
  pthread_mutex_unlock(&g_out_mutex);
}

static void fatal(pj_status_t status, const char *what) {
  char err[PJ_ERR_MSG_SIZE];
  pj_strerror(status, err, sizeof(err));
  fprintf(stderr, "FATAL: %s: %s\n", what, err);
  exit(1);
}

static void sig_handler(int signo) {
  (void)signo;
  g_running = 0;
}

/* --------------------------- PJSUA-Callbacks ----------------------- */

static void cb_incoming_call(pjsua_acc_id acc_id, pjsua_call_id call_id,
                             pjsip_rx_data *rdata) {
  pjsua_call_info ci;
  PJ_UNUSED_ARG(acc_id);
  PJ_UNUSED_ARG(rdata);

  pjsua_call_get_info(call_id, &ci);
  out("[event] Eingehender Anruf von %.*s -> nehme automatisch an\n",
      (int)ci.remote_info.slen, ci.remote_info.ptr);
  pjsua_call_answer(call_id, 200, NULL, NULL);
}

static void cb_call_state(pjsua_call_id call_id, pjsip_event *e) {
  pjsua_call_info ci;
  PJ_UNUSED_ARG(e);

  pjsua_call_get_info(call_id, &ci);
  out("[event] call %d: %.*s (letzter Status %d)\n", call_id,
      (int)ci.state_text.slen, ci.state_text.ptr, ci.last_status);
}

static void cb_call_media_state(pjsua_call_id call_id) {
  pjsua_call_info ci;
  pjsua_call_get_info(call_id, &ci);

  if (ci.media_status == PJSUA_CALL_MEDIA_ACTIVE) {
    pjsua_conf_connect(ci.conf_slot, 0); /* Gegenseite -> ALSA-Out */
    pjsua_conf_connect(0, ci.conf_slot); /* ALSA-Mic -> Gegenseite */
    out("[event] Medien aktiv, Ruf mit Soundgerät verbunden\n");
  }
}

static void cb_reg_state(pjsua_acc_id acc_id) {
  pjsua_acc_info ai;
  if (pjsua_acc_get_info(acc_id, &ai) == PJ_SUCCESS)
    out("[event] Registrierung: Code %d\n", (int)ai.status);
}
/* ------------------------------ Befehle ---------------------------- */

/* register sip:100@pbx.local [reg=sip:pbx.local] [user=100] [pass=xy] */
static void do_register(char *args) {
  char *sv = NULL;
  char *uri = strtok_r(args, " \t", &sv);
  char *t;

  if (!uri || strncmp(uri, "sip:", 4) != 0) {
    out("ERR usage: register sip:user@host [reg=sip:host] [user=U] [pass=P]\n");
    return;
  }
  snprintf(g_id_str, sizeof(g_id_str), "%s", uri);
  g_reg_str[0] = '\0';
  g_user_str[0] = '\0';
  g_pass_str[0] = '\0';

  while ((t = strtok_r(NULL, " \t", &sv)) != NULL) {
    if (!strncmp(t, "reg=", 4))
      snprintf(g_reg_str, sizeof(g_reg_str), "%s", t + 4);
    else if (!strncmp(t, "user=", 5))
      snprintf(g_user_str, sizeof(g_user_str), "%s", t + 5);
    else if (!strncmp(t, "pass=", 5))
      snprintf(g_pass_str, sizeof(g_pass_str), "%s", t + 5);
  }
  /* Registrar aus eigener URI ableiten, falls nicht explizit gesetzt */
  if (!g_reg_str[0]) {
    char *h = strchr(g_id_str, '@');
    snprintf(g_reg_str, sizeof(g_reg_str), "sip:%s",
             h ? h + 1 : g_id_str + 4);
  }

  if (g_acc_id != PJSUA_INVALID_ID) {
    pjsua_acc_del(g_acc_id);
    g_acc_id = PJSUA_INVALID_ID;
  }

  pjsua_acc_config ac;
  pjsua_acc_config_default(&ac);
  ac.id = pj_str(g_id_str);
  ac.reg_uri = pj_str(g_reg_str);

  if (g_user_str[0]) {
    ac.cred_count = 1;
    ac.cred_info[0].realm = pj_str("*");
    ac.cred_info[0].scheme = pj_str("Digest");
    ac.cred_info[0].username = pj_str(g_user_str);
    ac.cred_info[0].data_type = PJSIP_CRED_DATA_PLAIN_PASSWD;
    ac.cred_info[0].data = pj_str(g_pass_str);
  }

  {
    pj_status_t st = pjsua_acc_add(&ac, PJ_TRUE, &g_acc_id);
    if (st == PJ_SUCCESS) {
      out("OK Account %d angelegt, REGISTER an %s gesendet\n",
          g_acc_id, g_reg_str);
    } else {
      char e[PJ_ERR_MSG_SIZE];
      pj_strerror(st, e, sizeof(e));
      g_acc_id = PJSUA_INVALID_ID;
      out("ERR acc_add: %s\n", e);
    }
  }
}

/* call sip:ziel@host */
static void do_call(char *args) {
  pj_str_t dest;
  pjsua_call_id call_id;
  pj_status_t st;
  char *uri = args;

  if (g_acc_id == PJSUA_INVALID_ID) {
    out("ERR kein Account - zuerst 'register' ausfuehren\n");
    return;
  }
  if (!uri || !*uri) {
    out("ERR usage: call sip:ziel@host\n");
    return;
  }
  {
    char *sp = strpbrk(uri, " \t");
    if (sp)
      *sp = '\0'; /* nur die URI, Rest ignorieren */
  }

  dest = pj_str(uri);
  st = pjsua_call_make_call(g_acc_id, &dest, NULL, NULL, NULL, &call_id);
  if (st == PJ_SUCCESS)
    out("OK Anruf gestartet (call id=%d)\n", call_id);
  else {
    char e[PJ_ERR_MSG_SIZE];
    pj_strerror(st, e, sizeof(e));
    out("ERR make_call: %s\n", e);
  }
}

/* answer [id] */
static void do_answer(char *args) {
  pjsua_call_id ids[PJSUA_MAX_CALLS];
  unsigned count = PJ_ARRAY_SIZE(ids);
  unsigned i;
  pjsua_call_id target = PJSUA_INVALID_ID;

  if (args && *args) {
    int id = atoi(args);
    if (id >= 0 && (unsigned)id < PJSUA_MAX_CALLS) {
      pjsua_call_answer((pjsua_call_id)id, 200, NULL, NULL);
      out("OK Anruf %d beantwortet\n", id);
      return;
    }
    out("ERR ungueltige call id\n");
    return;
  }

  pjsua_enum_calls(ids, &count);
  for (i = 0; i < count; ++i) {
    pjsua_call_info ci;
    pjsua_call_get_info(ids[i], &ci);
    if (ci.state == PJSIP_INV_STATE_INCOMING) {
      target = ids[i];
      break;
    }
  }

  if (target == PJSUA_INVALID_ID) {
    out("ERR kein eingehender Anruf\n");
    return;
  }
  pjsua_call_answer(target, 200, NULL, NULL);
  out("OK Anruf %d angenommen\n", target);
}

/* hangup [id] */
static void do_hangup(char *args) {
  if (args && *args)
    pjsua_call_hangup((pjsua_call_id)atoi(args), 0, NULL, NULL);
  else
    pjsua_call_hangup_all();
  out("OK hangup ausgefuehrt\n");
}

/* devs — Audio-Geräte auflisten */
static void do_devs(void) {
  pjmedia_snd_dev_info info[MAX_AUDIO_DEVS];
  unsigned count = MAX_AUDIO_DEVS;
  unsigned i;

  if (pjsua_enum_snd_devs(info, &count) != PJ_SUCCESS) {
    out("ERR Aufzaehlung der Geraete fehlgeschlagen\n");
    return;
  }
  out("Audio-Geraete (%u):\n", count);
  for (i = 0; i < count; ++i)
    out("  [%2u] %-40s in=%u out=%u (Standard %u Hz)\n", i,
        info[i].name,
        info[i].input_count,
        info[i].output_count,
        info[i].default_samples_per_sec);
}

/* snd <capture_idx> <playback_idx> */
static void do_snd(char *args) {
  int cap = -1, play = -1;
  if (sscanf(args, "%d %d", &cap, &play) == 2 &&
      pjsua_set_snd_dev(cap, play) == PJ_SUCCESS)
    out("OK Soundgeraet capture=%d playback=%d\n", cap, play);
  else
    out("ERR usage: snd <capture_idx> <playback_idx>\n");
}

/* status */
static void do_status(void) {
  pjsua_call_id ids[PJSUA_MAX_CALLS];
  unsigned count = PJ_ARRAY_SIZE(ids);

  pjsua_enum_calls(ids, &count);

  if (g_acc_id == PJSUA_INVALID_ID) {
    out("account=none calls=%u\n", count);
  } else {
    pjsua_acc_info ai;
    int reg_code = -1;
    if (pjsua_acc_get_info(g_acc_id, &ai) == PJ_SUCCESS)
      reg_code = ai.status;
    out("account=%d reg_status=%d calls=%u\n", g_acc_id, reg_code, count);
  }
}

/* quit — Programm sauber beenden */
static void do_quit(void) {
  out("OK Herunterfahren\n");
  unlink(CTL_SOCK_PATH);
  pjsua_destroy();
  exit(0);
}

/* --------------------------- Dispatcher ---------------------------- */

static void dispatch_line(char *line) {
  char *sv = NULL;
  char *cmd = strtok_r(line, " \t", &sv);
  char *args = sv;

  if (!cmd || *cmd == '#') /* leer oder Kommentar */
    return;
  if (!args)
    args = ""; /* strtok liefert evtl. NULL */

  if (!strcasecmp(cmd, "register"))
    do_register(args);
  else if (!strcasecmp(cmd, "call"))
    do_call(args);
  else if (!strcasecmp(cmd, "answer"))
    do_answer(args);
  else if (!strcasecmp(cmd, "hangup"))
    do_hangup(args);
  else if (!strcasecmp(cmd, "devs") ||
           !strcasecmp(cmd, "devices"))
    do_devs();
  else if (!strcasecmp(cmd, "snd"))
    do_snd(args);
  else if (!strcasecmp(cmd, "status"))
    do_status();
  else if (!strcasecmp(cmd, "quit") ||
           !strcasecmp(cmd, "exit"))
    do_quit();
  else
    out("ERR unbekanntes Kommando '%s'\n", cmd);
}

/* -------------------- Unix-Socket-Server-Thread ------------------- */

static void *ctl_thread(void *arg) {
  pj_thread_desc thread_desc;    /* muss lebenslang gueltig bleiben   */
  pj_thread_t *my_thread = NULL; /* -> lokal im Thread OK             */
  int sfd, cfd;
  struct sockaddr_un addr;
  char buf[LINE_BUF_LEN];
  size_t len = 0;

  (void)arg;

  /* Thread bei pjlib registrieren, sonst duerfen wir von hier
   * keine pjsua-Funktionen aufrufen */
  pj_thread_register("ctl_thread", thread_desc, &my_thread);

  unlink(CTL_SOCK_PATH);

  sfd = socket(AF_UNIX, SOCK_STREAM, 0);
  if (sfd < 0) {
    perror("socket()");
    return NULL;
  }

  memset(&addr, 0, sizeof(addr));
  addr.sun_family = AF_UNIX;
  strncpy(addr.sun_path, CTL_SOCK_PATH, sizeof(addr.sun_path) - 1);

  if (bind(sfd, (struct sockaddr *)&addr, sizeof(addr)) < 0 ||
      chmod(CTL_SOCK_PATH, 0660) < 0 ||
      listen(sfd, 4) < 0) {
    perror("bind()/listen()");
    close(sfd);
    return NULL;
  }

  printf("[sipctl] Steuer-Socket bereit: %s\n", CTL_SOCK_PATH);
  fflush(stdout);

  for (;;) {
    ssize_t n;

    cfd = accept(sfd, NULL, NULL);
    if (cfd < 0) {
      if (errno == EINTR)
        continue;
      perror("accept()");
      continue;
    }

    /* neuen Client uebernehmen, alten Verbindung schliessen */
    pthread_mutex_lock(&g_out_mutex);
    if (g_ctl_fd >= 0)
      close(g_ctl_fd);
    g_ctl_fd = cfd;
    pthread_mutex_unlock(&g_out_mutex);

    len = 0;
    while ((n = read(cfd, buf + len, sizeof(buf) - 1 - len)) > 0) {
      char *nl;
      len += (size_t)n;
      buf[len] = '\0';

      /* alle vollstaendigen Zeilen abarbeiten */
      while ((nl = strchr(buf, '\n')) != NULL) {
        size_t rest;
        *nl = '\0';
        if (buf[0])
          dispatch_line(buf);
        rest = strlen(nl + 1);
        memmove(buf, nl + 1, rest + 1);
        len = rest;
      }
      if (len >= sizeof(buf) - 1)
        len = 0; /* uebergrosse Zeile: verwerfen */
    }

    /* Client weg */
    pthread_mutex_lock(&g_out_mutex);
    if (g_ctl_fd == cfd)
      g_ctl_fd = -1;
    pthread_mutex_unlock(&g_out_mutex);
    close(cfd);
  }
  return NULL;
}

/* -------------------------------- main ------------------------------ */

int main(int argc, char **argv) {
  unsigned udp_port = DEFAULT_UDP_PORT;
  pjsua_config ua_cfg;
  pjsua_logging_config log_cfg;
  pjsua_media_config med_cfg;
  pjsua_transport_config tp_cfg;
  pthread_t ctl_tid;

  if (argc >= 2 && !strcmp(argv[1], "-h")) {
    printf("usage: %s [-p udp_port]\n", argv[0]);
    return 0;
  }
  if (argc == 3 && !strcmp(argv[1], "-p"))
    udp_port = (unsigned)atoi(argv[2]);

  signal(SIGINT, sig_handler);
  signal(SIGTERM, sig_handler);
  signal(SIGPIPE, SIG_IGN);

  puts("Headless-SIP-Client startet...");

  // fatal(pjsua_create(), "pjsua_create");
  pjsua_create();

  pjsua_config_default(&ua_cfg);
  ua_cfg.max_calls = 8;
  ua_cfg.thread_cnt = 1;
  ua_cfg.cb.on_incoming_call = &cb_incoming_call;
  ua_cfg.cb.on_call_state = &cb_call_state;
  ua_cfg.cb.on_call_media_state = &cb_call_media_state;
  ua_cfg.cb.on_reg_state = &cb_reg_state;

  pjsua_logging_config_default(&log_cfg);
  log_cfg.level = 3;

  pjsua_media_config_default(&med_cfg);

  // fatal(pjsua_init(&ua_cfg, &log_cfg, &med_cfg), "pjsua_init");
  pjsua_init(&ua_cfg, &log_cfg, &med_cfg);

  pjsua_transport_config_default(&tp_cfg);
  tp_cfg.port = (unsigned short)udp_port;
  // fatal(pjsua_transport_create(PJSIP_TRANSPORT_UDP, &tp_cfg, NULL), "pjsua_transport_create (UDP-Port belegt?)");
  pjsua_transport_create(PJSIP_TRANSPORT_UDP, &tp_cfg, NULL);

  // fatal(pjsua_start(), "pjsua_start");
  pjsua_start();

  if (pthread_create(&ctl_tid, NULL, ctl_thread, NULL) != 0) {
    perror("pthread_create(ctl)");
    pjsua_destroy();
    return 1;
  }

  printf("Bereit. UDP-Port %u, Steuer-Socket %s\n", udp_port, CTL_SOCK_PATH);
  fflush(stdout);

  /* Main-Loop: nur schlafen, bis Signal kommt */
  while (g_running)
    sleep(1);

  puts("\nBeende...");
  pthread_cancel(ctl_tid);
  pthread_join(ctl_tid, NULL);
  unlink(CTL_SOCK_PATH);
  pjsua_destroy();
  return 0;
}
