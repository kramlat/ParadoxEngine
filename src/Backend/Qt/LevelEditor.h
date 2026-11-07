#pragma once

// The built-in level editor's window (Tools > Level Editor): edits the running level's objects and rings (EditorModel.h, EditorLive.h) and saves them to a folder, as the files the game keeps in res/.

#include <QPoint>
#include <QString>

class QMainWindow;
class QPainter;
class QRectF;

namespace LevelEditor {

void Show(QMainWindow *window);      // opens the editor (and starts editing the level that is running)
void ShowKeymaker(QMainWindow *window); // Keymaker mode: a new level in a zone the game does not have (Tools > Keymaker)
void KeymakerForTest(QMainWindow *window, int zone, int act, const QString &code); // the automated check's Start (no question asked)
bool IsOpen();
bool Editing();                      // the editor has the level: the picture takes clicks and shows the markers

// The picture's mouse: `x`, `y` are the pixel of the game picture, `button` Qt's, `kind` 0 press, 1 move (a button held), 2 release. Returns true when the editor used it.
bool Mouse(int kind, int button, int x, int y);

// Draws the markers over the picture: `dest` is where the picture is drawn in the widget, `scale` its size over the picture's.
void DrawOverlay(QPainter *p, const QRectF &dest, double scale);

void TogglePlaytest();               // what the Playtest button does (the automated check's)
bool CloseNow();                     // closes the editor as its window's button does (the automated check's)
void SelectObjectRow(int row);       // selects a row of the objects' table (the automated check's)
bool SaveNow();                      // writes the level's files to the output folder (what the Save button does)
void GrabTabs(const QString &prefix); // saves a picture of the window with each of its tabs in front, as <prefix>-tab<n>.png, and leaves the first in front (the automated check's: it edits nothing)

void Poll();                         // once a frame: keeps the window in step with the game's level

} // namespace LevelEditor
