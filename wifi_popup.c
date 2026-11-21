#include <X11/Xlib.h>
#include <X11/keysym.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dbus/dbus.h>

// Dimensiones del popup
#define WIN_WIDTH  200
#define WIN_HEIGHT 400
#define ROW_HEIGHT 40
#define FONT "fixed" // Fuente básica de X11

typedef struct {
    Display *dpy;
    Window win;
    GC gc;
    XFontStruct *font;
    int screen;
    // Datos simulados de redes (en una app real, se obtendrían por D-Bus)
    const char *networks[5];
    int net_count;
    int selected_idx;
} AppData;

// Función para dibujar la interfaz completa
void draw_ui(AppData *app) {
    // Limpiar ventana
    XSetForeground(app->dpy, app->gc, WhitePixel(app->dpy, app->screen));
    XFillRectangle(app->dpy, app->win, app->gc, 0, 0, WIN_WIDTH, WIN_HEIGHT);
    
    // Dibujar título
    XSetForeground(app->dpy, app->gc, BlackPixel(app->dpy, app->screen));
    XDrawString(app->dpy, app->win, app->gc, 10, 20, "WiFi", 4);
    
    // Dibujar lista de redes
    for (int i = 0; i < app->net_count; i++) {
        int y = 40 + i * ROW_HEIGHT;
        // Resaltar selección
        if (i == app->selected_idx) {
            XSetForeground(app->dpy, app->gc, 0xDDDDDD); // Gris claro
            XFillRectangle(app->dpy, app->win, app->gc, 5, y, WIN_WIDTH - 10, ROW_HEIGHT - 5);
        }
        XSetForeground(app->dpy, app->gc, BlackPixel(app->dpy, app->screen));
        XDrawString(app->dpy, app->win, app->gc, 10, y + 25, app->networks[i], strlen(app->networks[i]));
    }
    
    // Dibujar botón "Conectar"
    int btn_y = WIN_HEIGHT - 60;
    XSetForeground(app->dpy, app->gc, 0xCCCCCC);
    XFillRectangle(app->dpy, app->win, app->gc, 50, btn_y, 100, 30);
    XSetForeground(app->dpy, app->gc, BlackPixel(app->dpy, app->screen));
    XDrawString(app->dpy, app->win, app->gc, 70, btn_y + 20, "Conectar", 8);
    
    XFlush(app->dpy);
}

// Función que simula la llamada a NetworkManager por D-Bus (esbozo)
void connect_to_network(const char *ssid) {
    printf("Solicitando conexión a: %s (vía D-Bus)\n", ssid);
    // Aquí iría el código real de D-Bus:
    // 1. dbus_bus_get(DBUS_BUS_SYSTEM, &err);
    // 2. Construir mensaje para org.freedesktop.NetworkManager
    // 3. Enviar y esperar respuesta.
    // Es complejo, pero no requiere libnm.
}

int main() {
    AppData app = {0};
    app.networks[0] = "Red_Casa";
    app.networks[1] = "Vecino_2.4G";
    app.networks[2] = "Cafeteria_Free";
    app.networks[3] = "Oficina_Segura";
    app.net_count = 4;
    app.selected_idx = -1;
    
    // 1. Conectar al servidor X
    app.dpy = XOpenDisplay(NULL);
    if (!app.dpy) {
        fprintf(stderr, "No se pudo abrir el display X\n");
        return 1;
    }
    app.screen = DefaultScreen(app.dpy);
    
    // 2. Crear ventana
    app.win = XCreateSimpleWindow(app.dpy, RootWindow(app.dpy, app.screen),
                                  0, 0, WIN_WIDTH, WIN_HEIGHT, 1,
                                  BlackPixel(app.dpy, app.screen),
                                  WhitePixel(app.dpy, app.screen));
    XStoreName(app.dpy, app.win, "WiFi Popup");
    
    // 3. Seleccionar eventos (exposición y clics de ratón)
    XSelectInput(app.dpy, app.win, ExposureMask | ButtonPressMask | KeyPressMask);
    
    // 4. Crear contexto gráfico y cargar fuente
    app.gc = XCreateGC(app.dpy, app.win, 0, NULL);
    app.font = XLoadQueryFont(app.dpy, FONT);
    if (app.font) {
        XSetFont(app.dpy, app.gc, app.font->fid);
    }
    
    // 5. Mostrar ventana
    XMapWindow(app.dpy, app.win);
    
    // 6. Bucle de eventos
    XEvent ev;
    while (1) {
        XNextEvent(app.dpy, &ev);
        switch (ev.type) {
            case Expose:
                if (ev.xexpose.count == 0) {
                    draw_ui(&app);
                }
                break;
            case ButtonPress:
                // Lógica simple para detectar clics en la lista o en el botón
                int mx = ev.xbutton.x;
                int my = ev.xbutton.y;
                
                // Comprobar clic en la lista de redes
                for (int i = 0; i < app.net_count; i++) {
                    int y = 40 + i * ROW_HEIGHT;
                    if (my >= y && my < y + ROW_HEIGHT - 5) {
                        app.selected_idx = i;
                        draw_ui(&app); // Redibujar para mostrar selección
                        break;
                    }
                }
                
                // Comprobar clic en el botón "Conectar"
                if (mx >= 50 && mx <= 150 && my >= WIN_HEIGHT - 60 && my <= WIN_HEIGHT - 30) {
                    if (app.selected_idx >= 0) {
                        connect_to_network(app.networks[app.selected_idx]);
                    }
                }
                break;
            case KeyPress:
                // Cerrar con ESC
                if (XLookupKeysym(&ev.xkey, 0) == XK_Escape) {
                    XCloseDisplay(app.dpy);
                    return 0;
                }
                break;
        }
    }
    
    return 0;
}
