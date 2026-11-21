#include <X11/Xlib.h>
#include <X11/keysym.h>
#include <X11/Xutil.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <dbus/dbus.h>

#define WIN_WIDTH  260
#define WIN_HEIGHT 460
#define ROW_HEIGHT 40
#define MAX_NETS   16
#define SSID_MAX   33

typedef struct {
    Display *dpy;
    Window win;
    GC gc;
    XFontStruct *font;
    int screen;

    char ssids[MAX_NETS][SSID_MAX];
    int net_count;
    int selected_idx;

    int mode; // 0 = lista, 1 = pidiendo contraseña
    char password[64];
    int pwd_len;

    unsigned long bg_dark;
    unsigned long fg_light;
    unsigned long sel_bg;
    unsigned long btn_bg;
    unsigned long btn_fg;
    unsigned long btn_danger;

    DBusConnection *dbus_conn;
    char wifi_device_path[256];

    char selected_ap_path[256];
    int selected_secure;

    char status_msg[64]; // mensajito de estado (opcional)
} AppData;

static unsigned long alloc_color(Display *dpy, int screen, const char *hex) {
    XColor c;
    Colormap cmap = DefaultColormap(dpy, screen);
    if (!XParseColor(dpy, cmap, hex, &c)) return BlackPixel(dpy, screen);
    if (!XAllocColor(dpy, cmap, &c)) return BlackPixel(dpy, screen);
    return c.pixel;
}

// ---------- D-Bus: Detección de redes ----------

static int dbus_call_get_devices(AppData *app) {
    DBusMessage *msg = dbus_message_new_method_call(
        "org.freedesktop.NetworkManager",
        "/org/freedesktop/NetworkManager",
        "org.freedesktop.NetworkManager",
        "GetDevices");
    if (!msg) return -1;

    DBusError err;
    dbus_error_init(&err);
    DBusMessage *reply = dbus_connection_send_with_reply_and_block(
        app->dbus_conn, msg, 5000, &err);
    dbus_message_unref(msg);

    if (dbus_error_is_set(&err)) {
        fprintf(stderr, "GetDevices error: %s\n", err.message);
        dbus_error_free(&err);
        return -1;
    }
    if (!reply) return -1;

    DBusMessageIter iter, array;
    if (!dbus_message_iter_init(reply, &iter) ||
        dbus_message_iter_get_arg_type(&iter) != DBUS_TYPE_ARRAY) {
        dbus_message_unref(reply);
        return -1;
    }

    dbus_message_iter_recurse(&iter, &array);
    int found = 0;
    while (dbus_message_iter_get_arg_type(&array) == DBUS_TYPE_OBJECT_PATH) {
        const char *path;
        dbus_message_iter_get_basic(&array, &path);

        DBusMessage *tmsg = dbus_message_new_method_call(
            "org.freedesktop.NetworkManager", path,
            "org.freedesktop.DBus.Properties", "Get");
        if (!tmsg) { dbus_message_iter_next(&array); continue; }

        DBusMessageIter args;
        dbus_message_iter_init_append(tmsg, &args);
        const char *iface = "org.freedesktop.NetworkManager.Device";
        const char *prop = "DeviceType";
        dbus_message_iter_append_basic(&args, DBUS_TYPE_STRING, &iface);
        dbus_message_iter_append_basic(&args, DBUS_TYPE_STRING, &prop);

        DBusError terr;
        dbus_error_init(&terr);
        DBusMessage *treply = dbus_connection_send_with_reply_and_block(
            app->dbus_conn, tmsg, 3000, &terr);
        dbus_message_unref(tmsg);

        if (!dbus_error_is_set(&terr) && treply) {
            DBusMessageIter viter;
            if (dbus_message_iter_init(treply, &viter) &&
                dbus_message_iter_get_arg_type(&viter) == DBUS_TYPE_VARIANT) {
                DBusMessageIter inner;
                dbus_message_iter_recurse(&viter, &inner);
                if (dbus_message_iter_get_arg_type(&inner) == DBUS_TYPE_UINT32) {
                    dbus_uint32_t dtype;
                    dbus_message_iter_get_basic(&inner, &dtype);
                    if (dtype == 2) {
                        strncpy(app->wifi_device_path, path,
                                sizeof(app->wifi_device_path) - 1);
                        app->wifi_device_path[sizeof(app->wifi_device_path) - 1] = '\0';
                        found = 1;
                    }
                }
            }
            dbus_message_unref(treply);
        }
        dbus_error_free(&terr);
        dbus_message_iter_next(&array);
    }
    dbus_message_unref(reply);
    return found ? 0 : -1;
}

static int ap_get_property_string(DBusConnection *conn, const char *ap_path,
                                  const char *prop, char *out, int out_len) {
    DBusMessage *smsg = dbus_message_new_method_call(
        "org.freedesktop.NetworkManager", ap_path,
        "org.freedesktop.DBus.Properties", "Get");
    if (!smsg) return -1;

    DBusMessageIter sargs;
    dbus_message_iter_init_append(smsg, &sargs);
    const char *iface = "org.freedesktop.NetworkManager.AccessPoint";
    dbus_message_iter_append_basic(&sargs, DBUS_TYPE_STRING, &iface);
    dbus_message_iter_append_basic(&sargs, DBUS_TYPE_STRING, &prop);

    DBusError serr;
    dbus_error_init(&serr);
    DBusMessage *sreply = dbus_connection_send_with_reply_and_block(
        conn, smsg, 3000, &serr);
    dbus_message_unref(smsg);

    if (dbus_error_is_set(&serr) || !sreply) {
        dbus_error_free(&serr);
        if (sreply) dbus_message_unref(sreply);
        return -1;
    }

    int ok = -1;
    DBusMessageIter viter;
    if (dbus_message_iter_init(sreply, &viter) &&
        dbus_message_iter_get_arg_type(&viter) == DBUS_TYPE_VARIANT) {
        DBusMessageIter inner;
        dbus_message_iter_recurse(&viter, &inner);
        if (dbus_message_iter_get_arg_type(&inner) == DBUS_TYPE_ARRAY) {
            DBusMessageIter bytes;
            dbus_message_iter_recurse(&inner, &bytes);
            int i = 0;
            while (dbus_message_iter_get_arg_type(&bytes) == DBUS_TYPE_BYTE &&
                   i < out_len - 1) {
                unsigned char b;
                dbus_message_iter_get_basic(&bytes, &b);
                out[i++] = (char)b;
                dbus_message_iter_next(&bytes);
            }
            out[i] = '\0';
            ok = (i > 0 && out[0] != '\0') ? 0 : -1;
        }
    }
    dbus_message_unref(sreply);
    return ok;
}

static int ap_get_property_u32(DBusConnection *conn, const char *ap_path,
                               const char *prop, dbus_uint32_t *out) {
    DBusMessage *smsg = dbus_message_new_method_call(
        "org.freedesktop.NetworkManager", ap_path,
        "org.freedesktop.DBus.Properties", "Get");
    if (!smsg) return -1;

    DBusMessageIter sargs;
    dbus_message_iter_init_append(smsg, &sargs);
    const char *iface = "org.freedesktop.NetworkManager.AccessPoint";
    dbus_message_iter_append_basic(&sargs, DBUS_TYPE_STRING, &iface);
    dbus_message_iter_append_basic(&sargs, DBUS_TYPE_STRING, &prop);

    DBusError serr;
    dbus_error_init(&serr);
    DBusMessage *sreply = dbus_connection_send_with_reply_and_block(
        conn, smsg, 3000, &serr);
    dbus_message_unref(smsg);

    if (dbus_error_is_set(&serr) || !sreply) {
        dbus_error_free(&serr);
        if (sreply) dbus_message_unref(sreply);
        return -1;
    }

    int ok = -1;
    DBusMessageIter viter;
    if (dbus_message_iter_init(sreply, &viter) &&
        dbus_message_iter_get_arg_type(&viter) == DBUS_TYPE_VARIANT) {
        DBusMessageIter inner;
        dbus_message_iter_recurse(&viter, &inner);
        if (dbus_message_iter_get_arg_type(&inner) == DBUS_TYPE_UINT32) {
            dbus_message_iter_get_basic(&inner, out);
            ok = 0;
        }
    }
    dbus_message_unref(sreply);
    return ok;
}

static int dbus_get_access_points(AppData *app) {
    if (app->wifi_device_path[0] == '\0') return -1;

    DBusMessage *msg = dbus_message_new_method_call(
        "org.freedesktop.NetworkManager",
        app->wifi_device_path,
        "org.freedesktop.NetworkManager.Device.Wireless",
        "GetAllAccessPoints");
    if (!msg) return -1;

    DBusError err;
    dbus_error_init(&err);
    DBusMessage *reply = dbus_connection_send_with_reply_and_block(
        app->dbus_conn, msg, 5000, &err);
    dbus_message_unref(msg);

    if (dbus_error_is_set(&err)) {
        fprintf(stderr, "GetAllAccessPoints error: %s\n", err.message);
        dbus_error_free(&err);
        return -1;
    }
    if (!reply) return -1;

    DBusMessageIter iter, array;
    if (!dbus_message_iter_init(reply, &iter) ||
        dbus_message_iter_get_arg_type(&iter) != DBUS_TYPE_ARRAY) {
        dbus_message_unref(reply);
        return -1;
    }

    app->net_count = 0;
    dbus_message_iter_recurse(&iter, &array);
    while (dbus_message_iter_get_arg_type(&array) == DBUS_TYPE_OBJECT_PATH &&
           app->net_count < MAX_NETS) {
        const char *ap_path;
        dbus_message_iter_get_basic(&array, &ap_path);

        char ssid[SSID_MAX];
        if (ap_get_property_string(app->dbus_conn, ap_path, "Ssid",
                                   ssid, sizeof(ssid)) == 0) {
            int dup = 0;
            for (int j = 0; j < app->net_count; j++) {
                if (strcmp(app->ssids[j], ssid) == 0) { dup = 1; break; }
            }
            if (!dup) {
                strncpy(app->ssids[app->net_count], ssid, SSID_MAX - 1);
                app->ssids[app->net_count][SSID_MAX - 1] = '\0';
                app->net_count++;
            }
        }
        dbus_message_iter_next(&array);
    }
    dbus_message_unref(reply);
    return 0;
}

static void resolve_selected_ap(AppData *app) {
    app->selected_ap_path[0] = '\0';
    app->selected_secure = 0;
    if (app->selected_idx < 0 || app->wifi_device_path[0] == '\0') return;

    DBusMessage *msg = dbus_message_new_method_call(
        "org.freedesktop.NetworkManager",
        app->wifi_device_path,
        "org.freedesktop.NetworkManager.Device.Wireless",
        "GetAllAccessPoints");
    if (!msg) return;

    DBusError err;
    dbus_error_init(&err);
    DBusMessage *reply = dbus_connection_send_with_reply_and_block(
        app->dbus_conn, msg, 5000, &err);
    dbus_message_unref(msg);
    if (dbus_error_is_set(&err) || !reply) {
        dbus_error_free(&err);
        if (reply) dbus_message_unref(reply);
        return;
    }

    DBusMessageIter iter, array;
    if (!dbus_message_iter_init(reply, &iter) ||
        dbus_message_iter_get_arg_type(&iter) != DBUS_TYPE_ARRAY) {
        dbus_message_unref(reply);
        return;
    }

    const char *target = app->ssids[app->selected_idx];
    dbus_message_iter_recurse(&iter, &array);
    while (dbus_message_iter_get_arg_type(&array) == DBUS_TYPE_OBJECT_PATH) {
        const char *ap_path;
        dbus_message_iter_get_basic(&array, &ap_path);

        char ssid[SSID_MAX];
        if (ap_get_property_string(app->dbus_conn, ap_path, "Ssid",
                                   ssid, sizeof(ssid)) == 0 &&
            strcmp(ssid, target) == 0) {
            strncpy(app->selected_ap_path, ap_path,
                    sizeof(app->selected_ap_path) - 1);
            app->selected_ap_path[sizeof(app->selected_ap_path) - 1] = '\0';

            dbus_uint32_t wpa_flags = 0, rsn_flags = 0;
            ap_get_property_u32(app->dbus_conn, ap_path, "WpaFlags", &wpa_flags);
            ap_get_property_u32(app->dbus_conn, ap_path, "RsnFlags", &rsn_flags);
            app->selected_secure = !(wpa_flags == 0 && rsn_flags == 0);
            break;
        }
        dbus_message_iter_next(&array);
    }
    dbus_message_unref(reply);
}

static void refresh_networks(AppData *app) {
    if (dbus_call_get_devices(app) == 0) {
        dbus_get_access_points(app);
    }
}

// ---------- D-Bus: Desconectar todo ----------

static int disconnect_all(AppData *app) {
    if (app->wifi_device_path[0] == '\0') {
        fprintf(stderr, "No hay dispositivo WiFi disponible\n");
        return -1;
    }

    DBusMessage *msg = dbus_message_new_method_call(
        "org.freedesktop.NetworkManager",
        app->wifi_device_path,
        "org.freedesktop.NetworkManager.Device",
        "Disconnect");
    if (!msg) return -1;

    DBusError err;
    dbus_error_init(&err);
    DBusMessage *reply = dbus_connection_send_with_reply_and_block(
        app->dbus_conn, msg, 8000, &err);
    dbus_message_unref(msg);

    if (dbus_error_is_set(&err)) {
        fprintf(stderr, "Disconnect error: %s\n", err.message);
        dbus_error_free(&err);
        return -1;
    }
    if (reply) dbus_message_unref(reply);
    return 0;
}

// ---------- D-Bus: Conexión ----------

static void add_dict_string(DBusMessageIter *parent, const char *key,
                             const char *val) {
    DBusMessageIter entry, variant;
    dbus_message_iter_open_container(parent, DBUS_TYPE_DICT_ENTRY, NULL, &entry);
    dbus_message_iter_append_basic(&entry, DBUS_TYPE_STRING, &key);
    dbus_message_iter_open_container(&entry, DBUS_TYPE_VARIANT, "s", &variant);
    dbus_message_iter_append_basic(&variant, DBUS_TYPE_STRING, &val);
    dbus_message_iter_close_container(&entry, &variant);
    dbus_message_iter_close_container(parent, &entry);
}

static void add_dict_bytestring(DBusMessageIter *parent, const char *key,
                                 const char *bytes, int len) {
    DBusMessageIter entry, variant, array;
    dbus_message_iter_open_container(parent, DBUS_TYPE_DICT_ENTRY, NULL, &entry);
    dbus_message_iter_append_basic(&entry, DBUS_TYPE_STRING, &key);
    dbus_message_iter_open_container(&entry, DBUS_TYPE_VARIANT, "ay", &variant);
    dbus_message_iter_open_container(&variant, DBUS_TYPE_ARRAY, "y", &array);
    for (int i = 0; i < len; i++) {
        unsigned char b = (unsigned char)bytes[i];
        dbus_message_iter_append_basic(&array, DBUS_TYPE_BYTE, &b);
    }
    dbus_message_iter_close_container(&variant, &array);
    dbus_message_iter_close_container(&entry, &variant);
    dbus_message_iter_close_container(parent, &entry);
}

static int connect_wifi_real(AppData *app, const char *ssid,
                              const char *password, int secure) {
    if (app->wifi_device_path[0] == '\0') {
        fprintf(stderr, "No hay dispositivo WiFi disponible\n");
        return -1;
    }

    DBusMessage *msg = dbus_message_new_method_call(
        "org.freedesktop.NetworkManager",
        "/org/freedesktop/NetworkManager",
        "org.freedesktop.NetworkManager",
        "AddAndActivateConnection");
    if (!msg) return -1;

    DBusMessageIter args;
    DBusMessageIter conn_dict, entry, vals;

    dbus_message_iter_init_append(msg, &args);
    dbus_message_iter_open_container(&args, DBUS_TYPE_ARRAY, "{sa{sv}}", &conn_dict);

    dbus_message_iter_open_container(&conn_dict, DBUS_TYPE_DICT_ENTRY, NULL, &entry);
    const char *k_conn = "connection";
    dbus_message_iter_append_basic(&entry, DBUS_TYPE_STRING, &k_conn);
    dbus_message_iter_open_container(&entry, DBUS_TYPE_ARRAY, "{sv}", &vals);
    add_dict_string(&vals, "id", ssid);
    add_dict_string(&vals, "type", "802-11-wireless");
    dbus_message_iter_close_container(&entry, &vals);
    dbus_message_iter_close_container(&conn_dict, &entry);

    dbus_message_iter_open_container(&conn_dict, DBUS_TYPE_DICT_ENTRY, NULL, &entry);
    const char *k_wifi = "802-11-wireless";
    dbus_message_iter_append_basic(&entry, DBUS_TYPE_STRING, &k_wifi);
    dbus_message_iter_open_container(&entry, DBUS_TYPE_ARRAY, "{sv}", &vals);
    add_dict_bytestring(&vals, "ssid", ssid, strlen(ssid));
    add_dict_string(&vals, "mode", "infrastructure");
    dbus_message_iter_close_container(&entry, &vals);
    dbus_message_iter_close_container(&conn_dict, &entry);

    if (secure) {
        dbus_message_iter_open_container(&conn_dict, DBUS_TYPE_DICT_ENTRY, NULL, &entry);
        const char *k_sec = "802-11-wireless-security";
        dbus_message_iter_append_basic(&entry, DBUS_TYPE_STRING, &k_sec);
        dbus_message_iter_open_container(&entry, DBUS_TYPE_ARRAY, "{sv}", &vals);
        add_dict_string(&vals, "key-mgmt", "wpa-psk");
        add_dict_string(&vals, "psk", password);
        dbus_message_iter_close_container(&entry, &vals);
        dbus_message_iter_close_container(&conn_dict, &entry);
    }

    dbus_message_iter_open_container(&conn_dict, DBUS_TYPE_DICT_ENTRY, NULL, &entry);
    const char *k_ip4 = "ipv4";
    dbus_message_iter_append_basic(&entry, DBUS_TYPE_STRING, &k_ip4);
    dbus_message_iter_open_container(&entry, DBUS_TYPE_ARRAY, "{sv}", &vals);
    add_dict_string(&vals, "method", "auto");
    dbus_message_iter_close_container(&entry, &vals);
    dbus_message_iter_close_container(&conn_dict, &entry);

    dbus_message_iter_open_container(&conn_dict, DBUS_TYPE_DICT_ENTRY, NULL, &entry);
    const char *k_ip6 = "ipv6";
    dbus_message_iter_append_basic(&entry, DBUS_TYPE_STRING, &k_ip6);
    dbus_message_iter_open_container(&entry, DBUS_TYPE_ARRAY, "{sv}", &vals);
    add_dict_string(&vals, "method", "ignore");
    dbus_message_iter_close_container(&entry, &vals);
    dbus_message_iter_close_container(&conn_dict, &entry);

    dbus_message_iter_close_container(&args, &conn_dict);

    const char *dev_path = app->wifi_device_path;
    dbus_message_iter_append_basic(&args, DBUS_TYPE_OBJECT_PATH, &dev_path);

    const char *specific = app->selected_ap_path[0] ? app->selected_ap_path : "/";
    dbus_message_iter_append_basic(&args, DBUS_TYPE_OBJECT_PATH, &specific);

    DBusError err;
    dbus_error_init(&err);
    DBusMessage *reply = dbus_connection_send_with_reply_and_block(
        app->dbus_conn, msg, 15000, &err);
    dbus_message_unref(msg);

    if (dbus_error_is_set(&err)) {
        fprintf(stderr, "Conexión fallida: %s\n", err.message);
        dbus_error_free(&err);
        return -1;
    }
    if (reply) dbus_message_unref(reply);
    return 0;
}

static void try_connect(AppData *app, int *running) {
    if (app->selected_idx < 0) return;

    if (app->selected_secure && app->pwd_len == 0) {
        fprintf(stderr, "Esta red requiere contraseña\n");
        return;
    }

    printf("Conectando a %s...\n", app->ssids[app->selected_idx]);
    int r = connect_wifi_real(app, app->ssids[app->selected_idx],
                              app->password, app->selected_secure);
    if (r == 0) {
        printf("Conexión solicitada correctamente.\n");
        *running = 0;
    } else {
        printf("Error al conectar.\n");
    }
}

// ---------- UI ----------

void draw_ui(AppData *app) {
    XSetForeground(app->dpy, app->gc, app->bg_dark);
    XFillRectangle(app->dpy, app->win, app->gc, 0, 0, WIN_WIDTH, WIN_HEIGHT);

    XSetForeground(app->dpy, app->gc, app->fg_light);
    XDrawString(app->dpy, app->win, app->gc, 10, 20, "WiFi", 4);

    if (app->mode == 0) {
        for (int i = 0; i < app->net_count; i++) {
            int y = 40 + i * ROW_HEIGHT;
            if (i == app->selected_idx) {
                XSetForeground(app->dpy, app->gc, app->sel_bg);
                XFillRectangle(app->dpy, app->win, app->gc,
                               5, y, WIN_WIDTH - 10, ROW_HEIGHT - 5);
                XSetForeground(app->dpy, app->gc, app->bg_dark);
            } else {
                XSetForeground(app->dpy, app->gc, app->fg_light);
            }
            XDrawString(app->dpy, app->win, app->gc, 10, y + 25,
                        app->ssids[i], strlen(app->ssids[i]));
        }

        int btn_y = WIN_HEIGHT - 60;
        // Conectar
        XSetForeground(app->dpy, app->gc, app->btn_bg);
        XFillRectangle(app->dpy, app->win, app->gc, 10, btn_y, 80, 30);
        XSetForeground(app->dpy, app->gc, app->btn_fg);
        XDrawString(app->dpy, app->win, app->gc, 22, btn_y + 20, "Conectar", 8);

        // Refrescar
        XSetForeground(app->dpy, app->gc, app->btn_bg);
        XFillRectangle(app->dpy, app->win, app->gc, 95, btn_y, 75, 30);
        XSetForeground(app->dpy, app->gc, app->btn_fg);
        XDrawString(app->dpy, app->win, app->gc, 103, btn_y + 20, "Refrescar", 9);

        // Desconectar todo (botón rojo)
        XSetForeground(app->dpy, app->gc, app->btn_danger);
        XFillRectangle(app->dpy, app->win, app->gc, 175, btn_y, 75, 30);
        XSetForeground(app->dpy, app->gc, app->btn_fg);
        XDrawString(app->dpy, app->win, app->gc, 182, btn_y + 20, "Descon.", 7);
    } else {
        XSetForeground(app->dpy, app->gc, app->fg_light);
        XDrawString(app->dpy, app->win, app->gc, 10, 40,
                    "Contraseña para:", 16);
        XDrawString(app->dpy, app->win, app->gc, 10, 60,
                    app->ssids[app->selected_idx],
                    strlen(app->ssids[app->selected_idx]));

        XSetForeground(app->dpy, app->gc, app->btn_bg);
        XFillRectangle(app->dpy, app->win, app->gc, 20, 90, WIN_WIDTH - 40, 30);
        XSetForeground(app->dpy, app->gc, app->fg_light);

        char stars[64];
        int n = app->pwd_len < 63 ? app->pwd_len : 63;
        memset(stars, '*', n);
        stars[n] = '\0';
        XDrawString(app->dpy, app->win, app->gc, 30, 110, stars, n);

        XSetForeground(app->dpy, app->gc, app->fg_light);
        XDrawString(app->dpy, app->win, app->gc, 20, 150,
                    "Enter para conectar", 19);

        int btn_y = WIN_HEIGHT - 60;
        XSetForeground(app->dpy, app->gc, app->btn_bg);
        XFillRectangle(app->dpy, app->win, app->gc, 20, btn_y, 100, 30);
        XSetForeground(app->dpy, app->gc, app->btn_fg);
        XDrawString(app->dpy, app->win, app->gc, 40, btn_y + 20, "Conectar", 8);

        XSetForeground(app->dpy, app->gc, app->btn_bg);
        XFillRectangle(app->dpy, app->win, app->gc, 140, btn_y, 100, 30);
        XSetForeground(app->dpy, app->gc, app->btn_fg);
        XDrawString(app->dpy, app->win, app->gc, 165, btn_y + 20, "Volver", 6);
    }

    XFlush(app->dpy);
}

// ---------- Main ----------

int main(void) {
    AppData app;
    memset(&app, 0, sizeof(app));
    app.selected_idx = -1;
    app.mode = 0;

    DBusError dbus_err;
    dbus_error_init(&dbus_err);
    app.dbus_conn = dbus_bus_get(DBUS_BUS_SYSTEM, &dbus_err);
    if (!app.dbus_conn) {
        fprintf(stderr, "No se pudo conectar a D-Bus: %s\n", dbus_err.message);
        dbus_error_free(&dbus_err);
        return 1;
    }
    dbus_connection_set_exit_on_disconnect(app.dbus_conn, 0);

    app.dpy = XOpenDisplay(NULL);
    if (!app.dpy) {
        fprintf(stderr, "No se pudo abrir el display X\n");
        return 1;
    }
    app.screen = DefaultScreen(app.dpy);

    XSetWindowAttributes attrs;
    attrs.override_redirect = False; // WM gestiona foco y posición
    attrs.background_pixel = BlackPixel(app.dpy, app.screen);
    attrs.event_mask = ExposureMask | ButtonPressMask | KeyPressMask |
                       ButtonReleaseMask | StructureNotifyMask | FocusChangeMask;

    app.win = XCreateWindow(app.dpy, RootWindow(app.dpy, app.screen),
                            100, 100, WIN_WIDTH, WIN_HEIGHT, 0,
                            CopyFromParent, InputOutput, CopyFromParent,
                            CWOverrideRedirect | CWBackPixel | CWEventMask,
                            &attrs);

    XStoreName(app.dpy, app.win, "WiFi Popup");

    XClassHint *ch = XAllocClassHint();
    if (ch) {
        ch->res_name  = (char *)"wifi_popup";
        ch->res_class = (char *)"WifiPopup";
        XSetClassHint(app.dpy, app.win, ch);
        XFree(ch);
    }

    // === Sin bordes de minimizar/maximizar (pero gestionada por el WM) ===
    {
        struct {
            unsigned long flags;
            unsigned long functions;
            unsigned long decorations;
            long input_mode;
            unsigned long status;
        } motif_hints = { 2, 0, 0, 0, 0 }; // MWM_HINTS_DECORATIONS = 2, decorations = 0
        Atom motif_atom = XInternAtom(app.dpy, "_MOTIF_WM_HINTS", False);
        XChangeProperty(app.dpy, app.win, motif_atom, motif_atom, 32,
                        PropModeReplace, (unsigned char *)&motif_hints, 5);
    }

    app.gc = XCreateGC(app.dpy, app.win, 0, NULL);
    app.font = XLoadQueryFont(app.dpy, "fixed");
    if (app.font) XSetFont(app.dpy, app.gc, app.font->fid);

    app.bg_dark    = alloc_color(app.dpy, app.screen, "#2E2E2E");
    app.fg_light   = alloc_color(app.dpy, app.screen, "#E6E6E6");
    app.sel_bg     = alloc_color(app.dpy, app.screen, "#4A4A4A");
    app.btn_bg     = alloc_color(app.dpy, app.screen, "#3A3A3A");
    app.btn_fg     = alloc_color(app.dpy, app.screen, "#E6E6E6");
    app.btn_danger = alloc_color(app.dpy, app.screen, "#8B2E2E");

    XMapWindow(app.dpy, app.win);

    for (;;) {
        XEvent map_ev;
        XNextEvent(app.dpy, &map_ev);
        if (map_ev.type == MapNotify) break;
        if (map_ev.type == Expose && map_ev.xexpose.count == 0) draw_ui(&app);
    }

    XSetInputFocus(app.dpy, app.win, RevertToParent, CurrentTime);

    // Grab del puntero para recibir clics fuera de la ventana
    int grab_ok = XGrabPointer(app.dpy, app.win, False,
                                ButtonPressMask | ButtonReleaseMask,
                                GrabModeAsync, GrabModeAsync,
                                None, None, CurrentTime);
    if (grab_ok != GrabSuccess) {
        usleep(100000);
        grab_ok = XGrabPointer(app.dpy, app.win, False,
                                ButtonPressMask | ButtonReleaseMask,
                                GrabModeAsync, GrabModeAsync,
                                None, None, CurrentTime);
        if (grab_ok != GrabSuccess)
            fprintf(stderr, "Advertencia: XGrabPointer falló (%d)\n", grab_ok);
    }

    int kb_grab_ok = XGrabKeyboard(app.dpy, app.win, False,
                                    GrabModeAsync, GrabModeAsync,
                                    CurrentTime);
    if (kb_grab_ok != GrabSuccess)
        fprintf(stderr, "Advertencia: XGrabKeyboard falló (%d)\n", kb_grab_ok);

    refresh_networks(&app);
    draw_ui(&app);

    XEvent ev;
    int running = 1;
    while (running) {
        XNextEvent(app.dpy, &ev);
        switch (ev.type) {
            case Expose:
                if (ev.xexpose.count == 0) draw_ui(&app);
                break;

            case ButtonPress: {
                // === Cierre al clic fuera (funciona en CUALQUIER modo) ===
                // Comparamos coordenadas root contra la geometría real de la ventana.
                XWindowAttributes wa;
                XGetWindowAttributes(app.dpy, app.win, &wa);
                if (ev.xbutton.x_root < wa.x ||
                    ev.xbutton.x_root >= wa.x + wa.width ||
                    ev.xbutton.y_root < wa.y ||
                    ev.xbutton.y_root >= wa.y + wa.height) {
                    running = 0;
                    break;
                }

                int mx = ev.xbutton.x;
                int my = ev.xbutton.y;

                if (app.mode == 0) {
                    for (int i = 0; i < app.net_count; i++) {
                        int y = 40 + i * ROW_HEIGHT;
                        if (my >= y && my < y + ROW_HEIGHT - 5) {
                            app.selected_idx = i;
                            resolve_selected_ap(&app);
                            draw_ui(&app);
                            break;
                        }
                    }

                    int btn_y = WIN_HEIGHT - 60;

                    // Conectar (10..90)
                    if (mx >= 10 && mx <= 90 &&
                        my >= btn_y && my <= btn_y + 30) {
                        if (app.selected_idx >= 0) {
                            if (app.selected_secure) {
                                app.mode = 1;
                                app.pwd_len = 0;
                                memset(app.password, 0, sizeof(app.password));
                                draw_ui(&app);
                            } else {
                                try_connect(&app, &running);
                            }
                        }
                    }

                    // Refrescar (95..170)
                    if (mx >= 95 && mx <= 170 &&
                        my >= btn_y && my <= btn_y + 30) {
                        refresh_networks(&app);
                        if (app.selected_idx >= app.net_count)
                            app.selected_idx = -1;
                        draw_ui(&app);
                    }

                    // Desconectar todo (175..250)
                    if (mx >= 175 && mx <= 250 &&
                        my >= btn_y && my <= btn_y + 30) {
                        if (disconnect_all(&app) == 0)
                            printf("Desconectado de todas las redes.\n");
                        else
                            printf("No se pudo desconectar.\n");
                        // Refrescar la lista por si cambia el estado
                        refresh_networks(&app);
                        app.selected_idx = -1;
                        draw_ui(&app);
                    }
                } else {
                    int btn_y = WIN_HEIGHT - 60;

                    if (mx >= 20 && mx <= 120 &&
                        my >= btn_y && my <= btn_y + 30) {
                        try_connect(&app, &running);
                    }

                    if (mx >= 140 && mx <= 240 &&
                        my >= btn_y && my <= btn_y + 30) {
                        app.mode = 0;
                        draw_ui(&app);
                    }
                }
                break;
            }

            case KeyPress: {
                if (app.mode == 1) {
                    KeySym ks = XLookupKeysym(&ev.xkey, 0);
                    if (ks == XK_Return) {
                        try_connect(&app, &running);
                    } else if (ks == XK_Escape) {
                        app.mode = 0;
                        draw_ui(&app);
                    } else if (ks == XK_BackSpace) {
                        if (app.pwd_len > 0) {
                            app.pwd_len--;
                            app.password[app.pwd_len] = '\0';
                            draw_ui(&app);
                        }
                    } else {
                        char buf[8];
                        int n = XLookupString(&ev.xkey, buf, sizeof(buf),
                                              NULL, NULL);
                        if (n > 0 && app.pwd_len < 63) {
                            for (int i = 0; i < n; i++) {
                                if (buf[i] >= 32 && buf[i] < 127) {
                                    app.password[app.pwd_len++] = buf[i];
                                }
                            }
                            app.password[app.pwd_len] = '\0';
                            draw_ui(&app);
                        }
                    }
                } else {
                    if (XLookupKeysym(&ev.xkey, 0) == XK_Escape) {
                        running = 0;
                    }
                }
                break;
            }

            case ButtonRelease:
                break;
        }
    }

    XUngrabKeyboard(app.dpy, CurrentTime);
    XUngrabPointer(app.dpy, CurrentTime);
    if (app.font) XFreeFont(app.dpy, app.font);
    XFreeGC(app.dpy, app.gc);
    XDestroyWindow(app.dpy, app.win);
    XCloseDisplay(app.dpy);
    dbus_connection_unref(app.dbus_conn);
    return 0;
}
