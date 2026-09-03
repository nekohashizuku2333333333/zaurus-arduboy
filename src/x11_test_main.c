#include <X11/Xlib.h>
#include <X11/keysym.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <unistd.h>

#include "arduboy_core.h"

#define WIN_W 640
#define WIN_H 480
#define SCALE 5
#define TOOLBAR_H 48
#define DRAW_W (ZAURUS_ARDUBOY_WIDTH * SCALE)
#define DRAW_H (ZAURUS_ARDUBOY_HEIGHT * SCALE)
#define DRAW_X ((WIN_W - DRAW_W) / 2)
#define DRAW_Y (TOOLBAR_H + ((WIN_H - TOOLBAR_H - DRAW_H) / 2))
#define CYCLES_PER_TICK (16000000 / 60)

typedef struct app_state {
	Display *display;
	Window window;
	GC gc;
	Atom wm_delete;
	zaurus_arduboy_t *emu;
	unsigned buttons;
	int paused;
	char current_hex[512];
	char eeprom_path[512];
	unsigned long color_bg;
	unsigned long color_panel;
	unsigned long color_button;
	unsigned long color_active;
	unsigned long color_text;
	unsigned long color_frame;
} app_state_t;

static unsigned long alloc_color(Display *dpy, const char *name,
				 unsigned long fallback)
{
	Colormap cmap = DefaultColormap(dpy, DefaultScreen(dpy));
	XColor exact;
	XColor screen;
	if (XAllocNamedColor(dpy, cmap, name, &screen, &exact))
		return screen.pixel;
	return fallback;
}

static unsigned long now_ms(void)
{
	struct timeval tv;
	gettimeofday(&tv, 0);
	return (unsigned long)tv.tv_sec * 1000UL + (unsigned long)tv.tv_usec / 1000UL;
}

static void draw_button(app_state_t *app, int x, int y, int w, int h,
			const char *label, int active)
{
	XSetForeground(app->display, app->gc, active ? app->color_active
						    : app->color_button);
	XFillRectangle(app->display, app->window, app->gc, x, y, w, h);
	XSetForeground(app->display, app->gc, app->color_frame);
	XDrawRectangle(app->display, app->window, app->gc, x, y, w, h);
	XSetForeground(app->display, app->gc, app->color_text);
	XDrawString(app->display, app->window, app->gc, x + 18, y + 21,
		    label, strlen(label));
}

static int load_hex(app_state_t *app, const char *path)
{
	zaurus_arduboy_t *next;

	if (!path || !path[0])
		return -1;
	next = zaurus_arduboy_create();
	if (!next)
		return -1;
	if (zaurus_arduboy_load_hex(next, path) != 0) {
		zaurus_arduboy_destroy(next);
		return -1;
	}
	if (app->emu) {
		zaurus_arduboy_save_eeprom(app->emu, app->eeprom_path);
		zaurus_arduboy_destroy(app->emu);
	}
	app->emu = next;
	zaurus_arduboy_load_eeprom(app->emu, app->eeprom_path);
	app->buttons = 0;
	app->paused = 0;
	strncpy(app->current_hex, path, sizeof(app->current_hex) - 1);
	app->current_hex[sizeof(app->current_hex) - 1] = 0;
	return 0;
}

static void draw(app_state_t *app)
{
	const unsigned char *fb;
	unsigned x;
	unsigned y;

	XSetForeground(app->display, app->gc, app->color_bg);
	XFillRectangle(app->display, app->window, app->gc, 0, 0, WIN_W, WIN_H);

	XSetForeground(app->display, app->gc, app->color_panel);
	XFillRectangle(app->display, app->window, app->gc, 0, 0, WIN_W, TOOLBAR_H);
	draw_button(app, 8, 8, 92, 32, "Load", 0);
	draw_button(app, 108, 8, 92, 32, app->paused ? "Run" : "Pause", app->paused);
	draw_button(app, 208, 8, 92, 32, "Reset", 0);

	XSetForeground(app->display, app->gc, app->color_text);
	if (app->current_hex[0])
		XDrawString(app->display, app->window, app->gc, 316, 29,
			    app->current_hex, strlen(app->current_hex));
	else
		XDrawString(app->display, app->window, app->gc, 316, 29,
			    "Pass a .hex path on the command line", 36);

	XSetForeground(app->display, app->gc, app->color_frame);
	XDrawRectangle(app->display, app->window, app->gc,
		       DRAW_X - 1, DRAW_Y - 1, DRAW_W + 1, DRAW_H + 1);

	if (!app->emu) {
		const char *msg = "No game loaded";
		XDrawString(app->display, app->window, app->gc,
			    DRAW_X + 260, DRAW_Y + 160, msg, strlen(msg));
		XFlush(app->display);
		return;
	}

	fb = zaurus_arduboy_framebuffer(app->emu);
	for (y = 0; y < ZAURUS_ARDUBOY_HEIGHT; y++) {
		for (x = 0; x < ZAURUS_ARDUBOY_WIDTH; x++) {
			unsigned page = y >> 3;
			unsigned bit = y & 7;
			int on = (fb[page * 128 + x] >> bit) & 1;
			XSetForeground(app->display, app->gc,
				       on ? app->color_text : app->color_bg);
			XFillRectangle(app->display, app->window, app->gc,
				       DRAW_X + x * SCALE, DRAW_Y + y * SCALE,
				       SCALE, SCALE);
		}
	}
	XFlush(app->display);
}

static int button_for_key(KeySym sym)
{
	switch (sym) {
	case XK_Left:
		return ZAURUS_ARDUBOY_BUTTON_LEFT;
	case XK_Right:
		return ZAURUS_ARDUBOY_BUTTON_RIGHT;
	case XK_Up:
		return ZAURUS_ARDUBOY_BUTTON_UP;
	case XK_Down:
		return ZAURUS_ARDUBOY_BUTTON_DOWN;
	case XK_z:
	case XK_Z:
	case XK_Return:
	case XK_space:
		return ZAURUS_ARDUBOY_BUTTON_A;
	case XK_x:
	case XK_X:
	case XK_Escape:
		return ZAURUS_ARDUBOY_BUTTON_B;
	default:
		return 0;
	}
}

static void handle_key(app_state_t *app, XKeyEvent *kev, int pressed)
{
	KeySym sym = XLookupKeysym(kev, 0);
	int bit = button_for_key(sym);
	if (!bit)
		return;
	if (pressed)
		app->buttons |= (unsigned)bit;
	else
		app->buttons &= ~(unsigned)bit;
	if (app->emu)
		zaurus_arduboy_set_buttons(app->emu, app->buttons);
}

static int init_x11(app_state_t *app)
{
	int screen;
	app->display = XOpenDisplay(0);
	if (!app->display) {
		fprintf(stderr, "Cannot open X display. Set DISPLAY first.\n");
		return -1;
	}

	screen = DefaultScreen(app->display);
	app->color_bg = BlackPixel(app->display, screen);
	app->color_text = WhitePixel(app->display, screen);
	app->color_panel = alloc_color(app->display, "#202020", app->color_bg);
	app->color_button = alloc_color(app->display, "#404040", app->color_bg);
	app->color_active = alloc_color(app->display, "#526f96", app->color_button);
	app->color_frame = alloc_color(app->display, "#606060", app->color_text);

	app->window = XCreateSimpleWindow(app->display,
					 RootWindow(app->display, screen),
					 40, 40, WIN_W, WIN_H, 0,
					 app->color_frame, app->color_bg);
	XStoreName(app->display, app->window, "Zaurus Arduboy X11 Test");
	XSelectInput(app->display, app->window,
		     ExposureMask | KeyPressMask | KeyReleaseMask |
			     ButtonReleaseMask | StructureNotifyMask);
	app->wm_delete = XInternAtom(app->display, "WM_DELETE_WINDOW", False);
	XSetWMProtocols(app->display, app->window, &app->wm_delete, 1);
	XMapWindow(app->display, app->window);
	app->gc = XCreateGC(app->display, app->window, 0, 0);
	return 0;
}

int main(int argc, char **argv)
{
	app_state_t app;
	const char *home;
	unsigned long next_tick;
	int running = 1;

	memset(&app, 0, sizeof(app));
	home = getenv("HOME");
	if (!home)
		home = ".";
	snprintf(app.eeprom_path, sizeof(app.eeprom_path),
		 "%s/.arduboy-x11-eeprom.bin", home);

	if (init_x11(&app) != 0)
		return 1;
	if (argc >= 2 && load_hex(&app, argv[1]) != 0)
		fprintf(stderr, "Failed to load %s\n", argv[1]);

	next_tick = now_ms();
	while (running) {
		while (XPending(app.display)) {
			XEvent ev;
			XNextEvent(app.display, &ev);
			if (ev.type == Expose) {
				draw(&app);
			} else if (ev.type == KeyPress) {
				handle_key(&app, &ev.xkey, 1);
			} else if (ev.type == KeyRelease) {
				handle_key(&app, &ev.xkey, 0);
			} else if (ev.type == ButtonRelease) {
				int x = ev.xbutton.x;
				int y = ev.xbutton.y;
				if (x >= 108 && x < 200 && y >= 8 && y < 40) {
					app.paused = !app.paused;
					draw(&app);
				} else if (x >= 208 && x < 300 && y >= 8 && y < 40) {
					if (app.current_hex[0])
						load_hex(&app, app.current_hex);
					draw(&app);
				}
			} else if (ev.type == ClientMessage) {
				if ((Atom)ev.xclient.data.l[0] == app.wm_delete)
					running = 0;
			}
		}

		if (now_ms() >= next_tick) {
			if (app.emu && !app.paused)
				zaurus_arduboy_run_cycles(app.emu, CYCLES_PER_TICK);
			if (!app.emu || zaurus_arduboy_frame_dirty(app.emu)) {
				if (app.emu)
					zaurus_arduboy_clear_frame_dirty(app.emu);
				draw(&app);
			}
			next_tick += 16;
		}
		usleep(1000);
	}

	if (app.emu) {
		zaurus_arduboy_save_eeprom(app.emu, app.eeprom_path);
		zaurus_arduboy_destroy(app.emu);
	}
	XFreeGC(app.display, app.gc);
	XDestroyWindow(app.display, app.window);
	XCloseDisplay(app.display);
	return 0;
}
