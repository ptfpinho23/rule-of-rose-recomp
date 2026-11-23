#pragma once
#include <QColor>
#include <QFont>
#include <QIcon>
#include <QImage>
#include <QPixmap>
#include <QSize>

class QApplication;

// Look of the launcher: colours taken from the game's title screen (chalk on
// green-black, one dried-rose accent), the bundled serif, and the artwork.
namespace theme {

inline const QColor ink(0x0a, 0x0e, 0x0b);        // darkest green-black
inline const QColor panel(0x10, 0x16, 0x12);      // overlay panel
inline const QColor chalk(0xdd, 0xdb, 0xcf);      // text
inline const QColor chalkDim(0x93, 0x9a, 0x8d);   // secondary text
inline const QColor rose(0xa5, 0x47, 0x52);       // the accent
inline const QColor roseLight(0xd0, 0x7c, 0x84);  // accent as text on dark

inline QColor alpha(QColor c, qreal a) { c.setAlphaF(a); return c; }
QColor mix(const QColor &a, const QColor &b, qreal t);

enum Face { Regular, SemiBold, Italic };
// Loads the bundled fonts, sets the application font, palette and the
// few style-sheet rules needed for tool tips and the file dialog.
void apply(QApplication *app);
// A font in pixels; tracking = extra letter spacing in pixels.
QFont serif(int px, Face face = Regular, qreal tracking = 0);

// The window background for a window of `size` logical pixels: a drawn
// title card (glow, the name between two hairlines, vignette, grain). No
// game artwork is used.
QPixmap background(const QSize &size);
// Window icon: a drawn monogram.
QIcon icon();

} // namespace theme
