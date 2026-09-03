#include <qkeycode.h>
#ifdef ZAURUS_QVFB_HOST
#include <qapplication.h>
#else
#include <qpe/qpeapplication.h>
#endif
#include <qpainter.h>
#include <qrect.h>
#include <qstring.h>
#include <qwidget.h>

#include <dirent.h>
#include <sys/stat.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

extern "C" {
#include "arduboy_core.h"
}

#define SCALE 5
#define SCREEN_W 640
#define SCREEN_H 480
#define DRAW_W (ZAURUS_ARDUBOY_WIDTH * SCALE)
#define DRAW_H (ZAURUS_ARDUBOY_HEIGHT * SCALE)
#define DRAW_X ((SCREEN_W - DRAW_W) / 2)
#define TOOLBAR_H 48
#define DRAW_Y (TOOLBAR_H + ((SCREEN_H - TOOLBAR_H - DRAW_H) / 2))
#define CYCLES_PER_TICK (16000000 / 60)
#define MAX_ENTRIES 96
#define ROW_H 28
#define MAX_KEYS_PER_BUTTON 4
#define BUTTON_COUNT 6
#define BUTTON_NONE (-1)

struct BrowserEntry {
	char name[192];
	int isDir;
};

struct ButtonBinding {
	const char *name;
	unsigned bit;
	int defaults[MAX_KEYS_PER_BUTTON];
};

static const ButtonBinding BINDINGS[BUTTON_COUNT] = {
	{ "Left",  ZAURUS_ARDUBOY_BUTTON_LEFT,  { Qt::Key_Left,  Qt::Key_A, 0, 0 } },
	{ "Right", ZAURUS_ARDUBOY_BUTTON_RIGHT, { Qt::Key_Right, Qt::Key_D, 0, 0 } },
	{ "Up",    ZAURUS_ARDUBOY_BUTTON_UP,    { Qt::Key_Up,    Qt::Key_W, 0, 0 } },
	{ "Down",  ZAURUS_ARDUBOY_BUTTON_DOWN,  { Qt::Key_Down,  Qt::Key_S, 0, 0 } },
	{ "A",     ZAURUS_ARDUBOY_BUTTON_A,     { Qt::Key_Z, Qt::Key_Return, Qt::Key_Space, 0 } },
	{ "B",     ZAURUS_ARDUBOY_BUTTON_B,     { Qt::Key_X, Qt::Key_Escape, 0, 0 } }
};

class ArduboyWidget : public QWidget
{
public:
	ArduboyWidget(const char *hexPath, QWidget *parent = 0,
		      const char *name = 0)
		: QWidget(parent, name), emu(0), buttons(0), paused(0),
		  browserMode(0), keyMenuMode(0), mappingButton(BUTTON_NONE),
		  mouseButton(BUTTON_NONE), entryCount(0), scrollRow(0),
		  messageTicks(0), slowMode(0)
	{
		setFixedSize(SCREEN_W, SCREEN_H);
		setFocusPolicy(QWidget::StrongFocus);

		char eepromPath[512];
		const char *home = getenv("HOME");
		if (!home)
			home = "/home/zaurus";
		snprintf(eepromPath, sizeof(eepromPath), "%s/.arduboy-eeprom.bin",
			 home);
		zaurus_arduboy_load_eeprom(emu, eepromPath);
		strncpy(savePath, eepromPath, sizeof(savePath) - 1);
		savePath[sizeof(savePath) - 1] = 0;
		snprintf(configPath, sizeof(configPath), "%s/.arduboy-zaurus.keys",
			 home);
		currentHex[0] = 0;
		message[0] = 0;
		loadDefaultKeys();
		loadKeyConfig();
		setBrowserDir(home);

		setButtons();
		if (hexPath && hexPath[0])
			loadHex(hexPath);

		startTimer(16);
	}

	~ArduboyWidget()
	{
		if (emu) {
			zaurus_arduboy_save_eeprom(emu, savePath);
			zaurus_arduboy_destroy(emu);
		}
	}

protected:
	void paintEvent(QPaintEvent *)
	{
		QPainter p(this);
		p.fillRect(rect(), QColor(0, 0, 0));
		drawToolbar(p);
		if (browserMode)
			drawBrowser(p);
		else if (keyMenuMode)
			drawKeyMenu(p);
		else if (emu)
			drawFrame(p);
		else
			drawEmptyState(p);
	}

	void mouseReleaseEvent(QMouseEvent *e)
	{
		if (mouseButton != BUTTON_NONE) {
			setButtonState(mouseButton, 0);
			mouseButton = BUTTON_NONE;
			update();
			setFocus();
			return;
		}

		if (loadRect.contains(e->pos())) {
			openBrowser();
		} else if (pauseRect.contains(e->pos())) {
			if (browserMode) {
				closeBrowser();
			} else if (keyMenuMode) {
				closeKeyMenu();
			} else {
				paused = !paused;
				setButtons();
				update(0, 0, SCREEN_W, TOOLBAR_H);
			}
		} else if (resetRect.contains(e->pos())) {
			if (browserMode) {
				goParentDir();
			} else if (keyMenuMode) {
				loadDefaultKeys();
				saveKeyConfig();
				setMessage("Default keys restored");
				update();
			} else if (currentHex[0]) {
				loadHex(currentHex);
			}
		} else if (keysRect.contains(e->pos())) {
			if (browserMode)
				closeBrowser();
			keyMenuMode = !keyMenuMode;
			mappingButton = BUTTON_NONE;
			update();
		} else if (speedRect.contains(e->pos())) {
			slowMode = !slowMode;
			setMessage(slowMode ? "Slow mode" : "Full speed");
			update();
		} else if (browserMode) {
			handleBrowserClick(e->pos().y());
		} else if (keyMenuMode) {
			handleKeyMenuClick(e->pos().y());
		}
		setFocus();
	}

	void mousePressEvent(QMouseEvent *e)
	{
		int idx;
		if (browserMode || keyMenuMode)
			return;
		idx = virtualButtonAt(e->pos());
		if (idx != BUTTON_NONE) {
			mouseButton = idx;
			setButtonState(idx, 1);
			update();
			setFocus();
		}
	}

	void keyPressEvent(QKeyEvent *e)
	{
		if (e->isAutoRepeat())
			return;
		if (mappingButton != BUTTON_NONE) {
			assignKey(mappingButton, e->key());
			mappingButton = BUTTON_NONE;
			saveKeyConfig();
			setMessage("Key mapped");
			update();
			e->accept();
			return;
		}
		if (mapKey(e->key(), 1))
			e->accept();
		else
			QWidget::keyPressEvent(e);
	}

	void keyReleaseEvent(QKeyEvent *e)
	{
		if (e->isAutoRepeat())
			return;
		if (mapKey(e->key(), 0))
			e->accept();
		else
			QWidget::keyReleaseEvent(e);
	}

	void timerEvent(QTimerEvent *)
	{
		tick();
		if (messageTicks > 0) {
			messageTicks--;
			if (messageTicks == 0) {
				message[0] = 0;
				update(0, 0, SCREEN_W, TOOLBAR_H);
			}
		}
	}

private:
	zaurus_arduboy_t *emu;
	unsigned buttons;
	int paused;
	char savePath[512];
	char configPath[512];
	char currentHex[512];
	char browserDir[512];
	char message[256];
	QRect loadRect;
	QRect pauseRect;
	QRect resetRect;
	QRect keysRect;
	QRect speedRect;
	QRect virtualRects[BUTTON_COUNT];
	int keyMap[BUTTON_COUNT][MAX_KEYS_PER_BUTTON];
	BrowserEntry entries[MAX_ENTRIES];
	int browserMode;
	int keyMenuMode;
	int mappingButton;
	int mouseButton;
	int entryCount;
	int scrollRow;
	int messageTicks;
	int slowMode;

	void tick()
	{
		if (browserMode || !emu || paused)
			return;
		zaurus_arduboy_run_cycles(emu, slowMode ? CYCLES_PER_TICK / 2
							: CYCLES_PER_TICK);
		if (zaurus_arduboy_frame_dirty(emu)) {
			zaurus_arduboy_clear_frame_dirty(emu);
			update(DRAW_X, DRAW_Y, DRAW_W, DRAW_H);
		}
	}

	void setButtonState(int idx, int pressed)
	{
		if (idx < 0 || idx >= BUTTON_COUNT)
			return;
		if (pressed)
			buttons |= BINDINGS[idx].bit;
		else
			buttons &= ~BINDINGS[idx].bit;
		zaurus_arduboy_set_buttons(emu, buttons);
	}

	int mapKey(int key, int pressed)
	{
		int i, j;
		for (i = 0; i < BUTTON_COUNT; i++) {
			for (j = 0; j < MAX_KEYS_PER_BUTTON; j++) {
				if (keyMap[i][j] == key) {
					setButtonState(i, pressed);
					return 1;
				}
			}
		}
		return 0;
	}

	void setButtons()
	{
		loadRect = QRect(8, 8, 82, 32);
		pauseRect = QRect(96, 8, 82, 32);
		resetRect = QRect(184, 8, 82, 32);
		keysRect = QRect(272, 8, 82, 32);
		speedRect = QRect(360, 8, 82, 32);
		virtualRects[0] = QRect(32, 402, 48, 34);
		virtualRects[1] = QRect(136, 402, 48, 34);
		virtualRects[2] = QRect(84, 368, 48, 34);
		virtualRects[3] = QRect(84, 436, 48, 34);
		virtualRects[4] = QRect(500, 384, 52, 42);
		virtualRects[5] = QRect(572, 420, 52, 42);
	}

	void setMessage(const char *text)
	{
		strncpy(message, text, sizeof(message) - 1);
		message[sizeof(message) - 1] = 0;
		messageTicks = 180;
		update(0, 0, SCREEN_W, TOOLBAR_H);
	}

	void setBrowserDir(const char *dir)
	{
		if (!dir || !dir[0])
			dir = "/home/zaurus";
		strncpy(browserDir, dir, sizeof(browserDir) - 1);
		browserDir[sizeof(browserDir) - 1] = 0;
	}

	int hasHexSuffix(const char *name)
	{
		int len = strlen(name);
		if (len < 5)
			return 0;
		return strcmp(name + len - 4, ".hex") == 0 ||
		       strcmp(name + len - 4, ".HEX") == 0;
	}

	void addEntry(const char *name, int isDir)
	{
		if (entryCount >= MAX_ENTRIES)
			return;
		strncpy(entries[entryCount].name, name,
			sizeof(entries[entryCount].name) - 1);
		entries[entryCount].name[sizeof(entries[entryCount].name) - 1] = 0;
		entries[entryCount].isDir = isDir;
		entryCount++;
	}

	void sortEntries()
	{
		int i, j;
		for (i = 0; i < entryCount; i++) {
			for (j = i + 1; j < entryCount; j++) {
				if ((entries[j].isDir > entries[i].isDir) ||
				    (entries[j].isDir == entries[i].isDir &&
				     strcmp(entries[j].name, entries[i].name) < 0)) {
					BrowserEntry tmp = entries[i];
					entries[i] = entries[j];
					entries[j] = tmp;
				}
			}
		}
	}

	void scanDir()
	{
		DIR *dir;
		struct dirent *de;
		entryCount = 0;
		scrollRow = 0;
		addEntry("..", 1);

		dir = opendir(browserDir);
		if (!dir) {
			setMessage("Cannot open directory");
			return;
		}

		while ((de = readdir(dir)) != 0) {
			char full[768];
			struct stat st;
			if (de->d_name[0] == '.')
				continue;
			snprintf(full, sizeof(full), "%s/%s", browserDir, de->d_name);
			if (stat(full, &st) != 0)
				continue;
			if (S_ISDIR(st.st_mode))
				addEntry(de->d_name, 1);
			else if (hasHexSuffix(de->d_name))
				addEntry(de->d_name, 0);
		}
		closedir(dir);
		sortEntries();
	}

	void openBrowser()
	{
		const char *home = getenv("HOME");
		if (!home)
			home = "/home/zaurus";
		if (!browserDir[0])
			setBrowserDir(home);
		browserMode = 1;
		scanDir();
		update();
	}

	void closeBrowser()
	{
		browserMode = 0;
		update();
	}

	void closeKeyMenu()
	{
		keyMenuMode = 0;
		mappingButton = BUTTON_NONE;
		update();
	}

	void goParentDir()
	{
		char *slash;
		if (strcmp(browserDir, "/") == 0)
			return;
		slash = strrchr(browserDir, '/');
		if (!slash || slash == browserDir) {
			strcpy(browserDir, "/");
		} else {
			*slash = 0;
		}
		scanDir();
		update();
	}

	void enterBrowserEntry(int row)
	{
		char full[768];
		if (row < 0 || row >= entryCount)
			return;
		if (strcmp(entries[row].name, "..") == 0) {
			goParentDir();
			return;
		}
		snprintf(full, sizeof(full), "%s/%s", browserDir, entries[row].name);
		if (entries[row].isDir) {
			setBrowserDir(full);
			scanDir();
			update();
		} else {
			if (loadHex(full)) {
				browserMode = 0;
			} else {
				setMessage("Load failed");
			}
			update();
		}
	}

	void handleBrowserClick(int y)
	{
		int listTop = TOOLBAR_H + 34;
		int row = (y - listTop) / ROW_H;
		if (y < listTop)
			return;
		enterBrowserEntry(scrollRow + row);
	}

	void handleKeyMenuClick(int y)
	{
		int listTop = TOOLBAR_H + 54;
		int row = (y - listTop) / ROW_H;
		if (row >= 0 && row < BUTTON_COUNT) {
			mappingButton = row;
			setMessage("Press a key");
			update();
		}
	}

	int loadHex(const char *path)
	{
		zaurus_arduboy_t *next;
		if (!path || !path[0])
			return 0;

		next = zaurus_arduboy_create();
		if (!next)
			return 0;
		if (zaurus_arduboy_load_hex(next, path) != 0) {
			zaurus_arduboy_destroy(next);
			setMessage("Load failed");
			return 0;
		}

		if (emu) {
			zaurus_arduboy_save_eeprom(emu, savePath);
			zaurus_arduboy_destroy(emu);
		}
		emu = next;
		zaurus_arduboy_load_eeprom(emu, savePath);
		buttons = 0;
		paused = 0;
		zaurus_arduboy_set_buttons(emu, buttons);
		strncpy(currentHex, path, sizeof(currentHex) - 1);
		currentHex[sizeof(currentHex) - 1] = 0;
		setMessage("Loaded");
		update();
		return 1;
	}

	void drawButton(QPainter &p, const QRect &r, const char *label, int active)
	{
		QColor fill = active ? QColor(80, 110, 150) : QColor(45, 45, 45);
		p.fillRect(r, fill);
		p.setPen(QColor(160, 160, 160));
		p.drawRect(r);
		p.setPen(QColor(245, 245, 245));
		p.drawText(r, AlignCenter, label);
	}

	void drawToolbar(QPainter &p)
	{
		p.fillRect(0, 0, SCREEN_W, TOOLBAR_H, QColor(24, 24, 24));
		drawButton(p, loadRect, "Load", browserMode);
		drawButton(p, pauseRect, browserMode ? "Close" : (paused ? "Run" : "Pause"),
			   paused);
		drawButton(p, resetRect, keyMenuMode ? "Defaults" :
			   (browserMode ? "Up" : "Reset"), 0);
		drawButton(p, keysRect, "Keys", keyMenuMode);
		drawButton(p, speedRect, slowMode ? "Slow" : "Speed", slowMode);
		p.setPen(QColor(210, 210, 210));
		if (message[0])
			p.drawText(454, 8, SCREEN_W - 462, 32,
				   AlignVCenter | AlignLeft, message);
		else if (browserMode)
			p.drawText(454, 8, SCREEN_W - 462, 32,
				   AlignVCenter | AlignLeft, browserDir);
		else if (currentHex[0])
			p.drawText(454, 8, SCREEN_W - 462, 32,
				   AlignVCenter | AlignLeft, currentHex);
		else
			p.drawText(454, 8, SCREEN_W - 462, 32,
				   AlignVCenter | AlignLeft, "No game loaded");
	}

	void drawEmptyState(QPainter &p)
	{
		QRect r(DRAW_X, DRAW_Y, DRAW_W, DRAW_H);
		p.fillRect(r, QColor(8, 8, 8));
		p.setPen(QColor(70, 70, 70));
		p.drawRect(r);
		p.setPen(QColor(230, 230, 230));
		p.drawText(r, AlignCenter, "Tap Load to choose an Arduboy .hex");
	}

	void drawBrowser(QPainter &p)
	{
		int i;
		int listTop = TOOLBAR_H + 34;
		int visible = (SCREEN_H - listTop - 10) / ROW_H;

		p.fillRect(0, TOOLBAR_H, SCREEN_W, SCREEN_H - TOOLBAR_H,
			   QColor(10, 12, 14));
		p.setPen(QColor(210, 210, 210));
		p.drawText(12, TOOLBAR_H + 8, SCREEN_W - 24, 22,
			   AlignVCenter | AlignLeft, "Choose a .hex file");

		for (i = 0; i < visible; i++) {
			int idx = scrollRow + i;
			QRect r(10, listTop + i * ROW_H, SCREEN_W - 20, ROW_H - 2);
			if (idx >= entryCount)
				break;
			p.fillRect(r, idx & 1 ? QColor(24, 27, 30)
					      : QColor(18, 21, 24));
			p.setPen(entries[idx].isDir ? QColor(170, 210, 255)
						    : QColor(242, 242, 242));
			if (entries[idx].isDir) {
				QString label("[");
				label += entries[idx].name;
				label += "]";
				p.drawText(r.x() + 8, r.y(), r.width() - 16, r.height(),
					   AlignVCenter | AlignLeft, label);
			} else {
				p.drawText(r.x() + 8, r.y(), r.width() - 16, r.height(),
					   AlignVCenter | AlignLeft, entries[idx].name);
			}
		}
		if (entryCount == 1) {
			QRect empty(10, listTop, SCREEN_W - 20, 80);
			p.setPen(QColor(160, 160, 160));
			p.drawText(empty, AlignCenter, "No .hex files here");
		}
	}

	void drawKeyMenu(QPainter &p)
	{
		int i;
		int listTop = TOOLBAR_H + 54;
		p.fillRect(0, TOOLBAR_H, SCREEN_W, SCREEN_H - TOOLBAR_H,
			   QColor(10, 12, 14));
		p.setPen(QColor(230, 230, 230));
		p.drawText(12, TOOLBAR_H + 8, SCREEN_W - 24, 22,
			   AlignVCenter | AlignLeft,
			   "Tap a row, then press the replacement key");
		p.setPen(QColor(150, 150, 150));
		p.drawText(12, TOOLBAR_H + 30, SCREEN_W - 24, 20,
			   AlignVCenter | AlignLeft,
			   "Defaults: arrows/WASD, Z/Return/Space=A, X/Esc=B");
		for (i = 0; i < BUTTON_COUNT; i++) {
			QRect r(10, listTop + i * ROW_H, SCREEN_W - 20, ROW_H - 2);
			char label[256];
			snprintf(label, sizeof(label), "%s    %s",
				 BINDINGS[i].name, bindingText(i));
			p.fillRect(r, i == mappingButton ? QColor(80, 110, 150) :
				   (i & 1 ? QColor(24, 27, 30) : QColor(18, 21, 24)));
			p.setPen(QColor(245, 245, 245));
			p.drawText(r.x() + 8, r.y(), r.width() - 16, r.height(),
				   AlignVCenter | AlignLeft, label);
		}
	}

	void drawVirtualControls(QPainter &p)
	{
		int i;
		for (i = 0; i < BUTTON_COUNT; i++) {
			int active = (buttons & BINDINGS[i].bit) != 0;
			drawButton(p, virtualRects[i], BINDINGS[i].name, active);
		}
	}

	void drawFrame(QPainter &p)
	{
		const unsigned char *fb = zaurus_arduboy_framebuffer(emu);
		unsigned x, y;

		for (y = 0; y < ZAURUS_ARDUBOY_HEIGHT; y++) {
			for (x = 0; x < ZAURUS_ARDUBOY_WIDTH; x++) {
				unsigned page = y >> 3;
				unsigned bit = y & 7;
				int on = (fb[page * 128 + x] >> bit) & 1;
				p.fillRect(DRAW_X + x * SCALE, DRAW_Y + y * SCALE,
					   SCALE, SCALE,
					   on ? QColor(240, 240, 240)
					      : QColor(0, 0, 0));
			}
		}
		p.setPen(QColor(60, 60, 60));
		p.drawRect(DRAW_X - 1, DRAW_Y - 1, DRAW_W + 1, DRAW_H + 1);
		drawVirtualControls(p);
	}

	int virtualButtonAt(const QPoint &pt)
	{
		int i;
		for (i = 0; i < BUTTON_COUNT; i++) {
			if (virtualRects[i].contains(pt))
				return i;
		}
		return BUTTON_NONE;
	}

	const char *keyName(int key)
	{
		static char buf[32];
		switch (key) {
		case Qt::Key_Left: return "Left";
		case Qt::Key_Right: return "Right";
		case Qt::Key_Up: return "Up";
		case Qt::Key_Down: return "Down";
		case Qt::Key_Return: return "Return";
		case Qt::Key_Space: return "Space";
		case Qt::Key_Escape: return "Esc";
		default:
			if (key >= Qt::Key_A && key <= Qt::Key_Z) {
				buf[0] = (char)('A' + (key - Qt::Key_A));
				buf[1] = 0;
				return buf;
			}
			snprintf(buf, sizeof(buf), "0x%x", key);
			return buf;
		}
	}

	const char *bindingText(int idx)
	{
		static char buf[128];
		int j;
		buf[0] = 0;
		if (idx < 0 || idx >= BUTTON_COUNT)
			return "";
		for (j = 0; j < MAX_KEYS_PER_BUTTON; j++) {
			if (!keyMap[idx][j])
				continue;
			if (buf[0])
				strncat(buf, ", ", sizeof(buf) - strlen(buf) - 1);
			strncat(buf, keyName(keyMap[idx][j]),
				sizeof(buf) - strlen(buf) - 1);
		}
		return buf;
	}

	void loadDefaultKeys()
	{
		int i, j;
		for (i = 0; i < BUTTON_COUNT; i++) {
			for (j = 0; j < MAX_KEYS_PER_BUTTON; j++)
				keyMap[i][j] = BINDINGS[i].defaults[j];
		}
	}

	void assignKey(int idx, int key)
	{
		int i;
		if (idx < 0 || idx >= BUTTON_COUNT)
			return;
		for (i = 0; i < BUTTON_COUNT; i++) {
			int j;
			for (j = 0; j < MAX_KEYS_PER_BUTTON; j++) {
				if (keyMap[i][j] == key)
					keyMap[i][j] = 0;
			}
		}
		keyMap[idx][0] = key;
		for (i = 1; i < MAX_KEYS_PER_BUTTON; i++)
			keyMap[idx][i] = 0;
	}

	void loadKeyConfig()
	{
		FILE *f = fopen(configPath, "r");
		int idx, k0, k1, k2, k3;
		if (!f)
			return;
		while (fscanf(f, "%d %d %d %d %d", &idx, &k0, &k1, &k2, &k3) == 5) {
			if (idx >= 0 && idx < BUTTON_COUNT) {
				keyMap[idx][0] = k0;
				keyMap[idx][1] = k1;
				keyMap[idx][2] = k2;
				keyMap[idx][3] = k3;
			}
		}
		fclose(f);
	}

	void saveKeyConfig()
	{
		FILE *f = fopen(configPath, "w");
		int i;
		if (!f)
			return;
		for (i = 0; i < BUTTON_COUNT; i++) {
			fprintf(f, "%d %d %d %d %d\n", i, keyMap[i][0],
				keyMap[i][1], keyMap[i][2], keyMap[i][3]);
		}
		fclose(f);
	}
};

int main(int argc, char **argv)
{
#ifdef ZAURUS_QVFB_HOST
	QApplication app(argc, argv);
#else
	QPEApplication app(argc, argv);
#endif
	ArduboyWidget w(argc >= 2 ? argv[1] : 0);
#ifndef ZAURUS_QVFB_HOST
	app.setMainWidget(&w);
#endif
	w.show();
	return app.exec();
}
