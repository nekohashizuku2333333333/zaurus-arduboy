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

struct BrowserEntry {
	char name[192];
	int isDir;
};

class ArduboyWidget : public QWidget
{
public:
	ArduboyWidget(const char *hexPath, QWidget *parent = 0,
		      const char *name = 0)
		: QWidget(parent, name), emu(0), buttons(0), paused(0),
		  browserMode(0), entryCount(0), scrollRow(0), messageTicks(0)
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
		currentHex[0] = 0;
		message[0] = 0;
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
		else if (emu)
			drawFrame(p);
		else
			drawEmptyState(p);
	}

	void mouseReleaseEvent(QMouseEvent *e)
	{
		if (loadRect.contains(e->pos())) {
			openBrowser();
		} else if (pauseRect.contains(e->pos())) {
			if (browserMode) {
				closeBrowser();
			} else {
				paused = !paused;
				setButtons();
				update(0, 0, SCREEN_W, TOOLBAR_H);
			}
		} else if (resetRect.contains(e->pos())) {
			if (browserMode)
				goParentDir();
			else if (currentHex[0])
				loadHex(currentHex);
		} else if (browserMode) {
			handleBrowserClick(e->pos().y());
		}
		setFocus();
	}

	void keyPressEvent(QKeyEvent *e)
	{
		if (e->isAutoRepeat())
			return;
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
	char currentHex[512];
	char browserDir[512];
	char message[256];
	QRect loadRect;
	QRect pauseRect;
	QRect resetRect;
	BrowserEntry entries[MAX_ENTRIES];
	int browserMode;
	int entryCount;
	int scrollRow;
	int messageTicks;

	void tick()
	{
		if (browserMode || !emu || paused)
			return;
		zaurus_arduboy_run_cycles(emu, CYCLES_PER_TICK);
		if (zaurus_arduboy_frame_dirty(emu)) {
			zaurus_arduboy_clear_frame_dirty(emu);
			update(DRAW_X, DRAW_Y, DRAW_W, DRAW_H);
		}
	}

	int mapKey(int key, int pressed)
	{
		unsigned bit = 0;
		switch (key) {
		case Qt::Key_Left:
			bit = ZAURUS_ARDUBOY_BUTTON_LEFT;
			break;
		case Qt::Key_Right:
			bit = ZAURUS_ARDUBOY_BUTTON_RIGHT;
			break;
		case Qt::Key_Up:
			bit = ZAURUS_ARDUBOY_BUTTON_UP;
			break;
		case Qt::Key_Down:
			bit = ZAURUS_ARDUBOY_BUTTON_DOWN;
			break;
		case Qt::Key_Z:
		case Qt::Key_Return:
		case Qt::Key_Space:
			bit = ZAURUS_ARDUBOY_BUTTON_A;
			break;
		case Qt::Key_X:
		case Qt::Key_Escape:
			bit = ZAURUS_ARDUBOY_BUTTON_B;
			break;
		default:
			return 0;
		}

		if (pressed)
			buttons |= bit;
		else
			buttons &= ~bit;
		zaurus_arduboy_set_buttons(emu, buttons);
		return 1;
	}

	void setButtons()
	{
		loadRect = QRect(8, 8, 92, 32);
		pauseRect = QRect(108, 8, 92, 32);
		resetRect = QRect(208, 8, 92, 32);
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
		drawButton(p, resetRect, browserMode ? "Up" : "Reset", 0);
		p.setPen(QColor(210, 210, 210));
		if (message[0])
			p.drawText(316, 8, SCREEN_W - 324, 32,
				   AlignVCenter | AlignLeft, message);
		else if (browserMode)
			p.drawText(316, 8, SCREEN_W - 324, 32,
				   AlignVCenter | AlignLeft, browserDir);
		else if (currentHex[0])
			p.drawText(316, 8, SCREEN_W - 324, 32,
				   AlignVCenter | AlignLeft, currentHex);
		else
			p.drawText(316, 8, SCREEN_W - 324, 32,
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
