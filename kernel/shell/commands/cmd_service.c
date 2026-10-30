#include "commands.h"
#include "service.h"
#include "terminal.h"
#include "string.h"
#include "auth.h"

/* service: the programs that run in the background (an SSH server...). Packages bring their own (tpkg).
 *   service list | status <name> | log <name>         anyone
 *   service start|stop|restart|enable|disable <name>   root (sudo) */

static void put(const char *s) { terminal_writestring(s); }

static void show(const service_info_t *s)
{
    put(s->running ? "  running  " : "  stopped  ");
    put(s->enabled ? "starts at boot  " : "manual         ");
    put(s->name);
    put("  ");
    put(s->desc);
    put("\n");
}

void cmd_service(int argc, char **args)
{
    if (argc < 2 || !strcmp(args[1], "list")) {
        service_info_t list[SERVICE_MAX];
        int n = service_list(list, SERVICE_MAX);
        if (!n) { put("No services are installed. Packages bring them (tpkg look).\n"); }
        for (int i = 0; i < n; i++) show(&list[i]);
        if (argc < 2) {
            put("\nusage: service [list | status <name> | log <name>]\n"
                "       sudo service <start | stop | restart | enable | disable> <name>\n");
        }
        return;
    }
    const char *sub = args[1];
    if (argc < 3) { put("service: which one? (service list)\n"); return; }
    const char *name = args[2];

    if (!strcmp(sub, "status")) {
        service_info_t list[SERVICE_MAX];
        int n = service_list(list, SERVICE_MAX);
        for (int i = 0; i < n; i++) if (!strcmp(list[i].name, name)) { show(&list[i]); if (list[i].package[0]) { put("  from the package "); put(list[i].package); put("\n"); } return; }
        put("service: no service called "); put(name); put("\n");
        return;
    }
    if (!strcmp(sub, "log")) {
        static char buf[4096];
        int n = service_log(name, buf, sizeof(buf));
        if (n < 0) put("service: it has not run since tOS started\n");
        else if (n == 0) put("(nothing yet)\n");
        else put(buf);
        return;
    }
    if (strcmp(sub, "start") && strcmp(sub, "stop") && strcmp(sub, "restart") && strcmp(sub, "enable") && strcmp(sub, "disable")) {
        put("service: unknown command (list, status, log, start, stop, restart, enable, disable)\n");
        return;
    }
    if (!auth_is_root()) { put("service: "); put(service_strerror(-6)); put("\n"); return; }

    int rc = 0;
    if (!strcmp(sub, "start")) rc = service_start(name);
    else if (!strcmp(sub, "stop")) rc = service_stop(name);
    else if (!strcmp(sub, "restart")) { service_stop(name); rc = service_start(name); }
    else if (!strcmp(sub, "enable")) rc = service_enable(name, 1);
    else rc = service_enable(name, 0);
    if (rc) { put("service: "); put(name); put(": "); put(service_strerror(rc)); put("\n"); return; }
    put(name);
    put(!strcmp(sub, "start") ? " started\n" : !strcmp(sub, "stop") ? " stopped\n" : !strcmp(sub, "restart") ? " restarted\n" :
        !strcmp(sub, "enable") ? " will start when tOS starts\n" : " will not start by itself\n");
}
