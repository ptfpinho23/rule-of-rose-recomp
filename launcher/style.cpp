#include "style.h"

#include <QApplication>
#include <QFontDatabase>
#include <QFontMetricsF>
#include <QPolygonF>
#include <QImage>
#include <QLinearGradient>
#include <QPainter>
#include <QPainterPath>
#include <QPalette>
#include <QRadialGradient>
#include <QStringList>
#include <QToolTip>

namespace theme {

namespace {

QString g_family[3];
QString g_style[3];

quint32 g_seed = 0x2545f491u;
int noise()   // small deterministic generator: the grain is the same every run
{
    g_seed ^= g_seed << 13; g_seed ^= g_seed >> 17; g_seed ^= g_seed << 5;
    return int(g_seed & 0xff);
}

} // namespace

QColor mix(const QColor &a, const QColor &b, qreal t)
{
    t = qBound(0.0, t, 1.0);
    return QColor::fromRgbF(a.redF() + (b.redF() - a.redF()) * t,
                            a.greenF() + (b.greenF() - a.greenF()) * t,
                            a.blueF() + (b.blueF() - a.blueF()) * t,
                            a.alphaF() + (b.alphaF() - a.alphaF()) * t);
}

void apply(QApplication *app)
{
    static const char *const files[3] = {
        ":/assets/fonts/CormorantGaramond-Medium.ttf",
        ":/assets/fonts/CormorantGaramond-SemiBold.ttf",
        ":/assets/fonts/CormorantGaramond-MediumItalic.ttf",
    };
    for (int i = 0; i < 3; ++i) {
        const int id = QFontDatabase::addApplicationFont(files[i]);
        const QStringList fams = QFontDatabase::applicationFontFamilies(id);
        if (fams.isEmpty())
            continue;   // falls back to the default serif below
        g_family[i] = fams.first();
        const QStringList styles = QFontDatabase::styles(g_family[i]);
        static const char *const want[3] = {"Medium", "SemiBold", "Medium Italic"};
        g_style[i] = styles.contains(want[i]) ? QString(want[i])
                                              : styles.value(0);
    }
    app->setFont(serif(17));

    QPalette p;
    p.setColor(QPalette::Window, ink);
    p.setColor(QPalette::WindowText, chalk);
    p.setColor(QPalette::Base, panel);
    p.setColor(QPalette::AlternateBase, QColor(0x16, 0x1d, 0x18));
    p.setColor(QPalette::Text, chalk);
    p.setColor(QPalette::Button, QColor(0x18, 0x20, 0x1b));
    p.setColor(QPalette::ButtonText, chalk);
    p.setColor(QPalette::ToolTipBase, panel);
    p.setColor(QPalette::ToolTipText, chalk);
    p.setColor(QPalette::BrightText, roseLight);
    p.setColor(QPalette::Link, chalk);
    p.setColor(QPalette::Highlight, rose);
    p.setColor(QPalette::HighlightedText, QColor(0xf3, 0xef, 0xe4));
    p.setColor(QPalette::PlaceholderText, chalkDim);
    p.setColor(QPalette::Disabled, QPalette::Text, chalkDim);
    p.setColor(QPalette::Disabled, QPalette::ButtonText, chalkDim);
    p.setColor(QPalette::Disabled, QPalette::WindowText, chalkDim);
    app->setPalette(p);
    // No style sheet anywhere: with one installed, Qt stops honouring later
    // palette changes on labels (dimmed rows stayed bright).
    QToolTip::setPalette(p);
    QToolTip::setFont(serif(15));
}

QFont serif(int px, Face face, qreal tracking)
{
    QFont f;
    if (!g_family[face].isEmpty()) {
        f = QFontDatabase::font(g_family[face], g_style[face], 12);
    } else {
        f.setFamilies({"Georgia", "Times New Roman", "serif"});
        f.setWeight(face == SemiBold ? QFont::DemiBold : QFont::Medium);
        f.setItalic(face == Italic);
    }
    f.setPixelSize(px);
    f.setHintingPreference(QFont::PreferNoHinting);
    f.setStyleStrategy(QFont::PreferAntialias);
    f.setFeature("lnum", 1);   // lining figures: serial numbers and paths read better
    if (tracking != 0)
        f.setLetterSpacing(QFont::AbsoluteSpacing, tracking);
    return f;
}

QPixmap background(const QSize &size)
{
    QImage out(size, QImage::Format_RGB32);
    out.fill(ink);
    QPainter p(&out);
    p.setRenderHints(QPainter::Antialiasing | QPainter::SmoothPixmapTransform);
    {
        // A quiet title card.
        // A soft green glow behind the name, the name set in the bundled
        // serif between two hairlines with a small lozenge.
        const qreal cx = size.width() / 2.0, cy = size.height() * 0.40;
        QRadialGradient glow(cx, cy, size.width() * 0.52);
        glow.setColorAt(0.0, QColor(0x27, 0x33, 0x2b));
        glow.setColorAt(0.45, QColor(0x17, 0x1f, 0x1a));
        glow.setColorAt(1.0, ink);
        p.fillRect(out.rect(), glow);

        const QFont big = serif(62, Regular, 17), small = serif(25, Italic, 2);
        const QFontMetricsF bfm(big), sfm(small);
        const qreal wRule = bfm.horizontalAdvance("RULE") - 17, wRose = bfm.horizontalAdvance("ROSE") - 17;
        const qreal wOf = sfm.horizontalAdvance("of") - 2, gapW = 30;
        const qreal total = wRule + gapW + wOf + gapW + wRose;
        qreal x = cx - total / 2;
        const qreal base = cy + bfm.capHeight() / 2;
        p.setPen(alpha(chalk, 0.86));
        p.setFont(big);
        p.drawText(QPointF(x, base), "RULE");
        x += wRule + gapW;
        p.setPen(alpha(roseLight, 0.90));
        p.setFont(small);
        p.drawText(QPointF(x, base - 6), "of");
        x += wOf + gapW;
        p.setPen(alpha(chalk, 0.86));
        p.setFont(big);
        p.drawText(QPointF(x, base), "ROSE");

        auto rule = [&](qreal y) {
            const qreal half = total / 2 + 26, d = 4.5;
            p.setPen(QPen(alpha(chalk, 0.28), 1));
            p.drawLine(QPointF(cx - half, y), QPointF(cx - d - 9, y));
            p.drawLine(QPointF(cx + d + 9, y), QPointF(cx + half, y));
            QPolygonF loz;
            loz << QPointF(cx, y - d) << QPointF(cx + d, y) << QPointF(cx, y + d) << QPointF(cx - d, y);
            p.setPen(QPen(alpha(rose, 0.95), 1));
            p.setBrush(Qt::NoBrush);
            p.drawPolygon(loz);
        };
        rule(qRound(base - bfm.capHeight() - 34) + 0.5);
        rule(qRound(base + 34) + 0.5);
    }

    // Vignette, then darkness rising from the bottom for the controls
    // and a little at the top for the window buttons.
    QRadialGradient v(size.width() * 0.5, size.height() * 0.42, size.width() * 0.62);
    v.setColorAt(0.0, alpha(ink, 0.0));
    v.setColorAt(0.55, alpha(ink, 0.12));
    v.setColorAt(1.0, alpha(ink, 0.88));
    p.fillRect(out.rect(), v);

    QLinearGradient b(0, size.height() * 0.60, 0, size.height());
    b.setColorAt(0.0, alpha(ink, 0.0));
    b.setColorAt(0.45, alpha(ink, 0.72));
    b.setColorAt(1.0, alpha(ink, 0.97));
    p.fillRect(out.rect(), b);

    QLinearGradient t(0, 0, 0, 70);
    t.setColorAt(0.0, alpha(ink, 0.75));
    t.setColorAt(1.0, alpha(ink, 0.0));
    p.fillRect(QRect(0, 0, size.width(), 70), t);
    p.end();

    // Film grain.
    g_seed = 0x2545f491u;
    for (int y = 0; y < out.height(); ++y) {
        QRgb *px = reinterpret_cast<QRgb *>(out.scanLine(y));
        for (int x = 0; x < out.width(); ++x) {
            const int n = (noise() + noise() + noise()) / 3 - 127;   // ~triangular
            const int d = n / 9;
            px[x] = qRgb(qBound(0, qRed(px[x]) + d, 255), qBound(0, qGreen(px[x]) + d, 255),
                         qBound(0, qBlue(px[x]) + d, 255));
        }
    }
    return QPixmap::fromImage(out);
}

QIcon icon()
{
    // A drawn monogram: nothing of the game's own artwork is in the launcher.
    QIcon ic;
    for (int s : {16, 24, 32, 48, 64, 128, 256}) {
        QImage img(s, s, QImage::Format_ARGB32_Premultiplied);
        img.fill(Qt::transparent);
        QPainter p(&img);
        p.setRenderHints(QPainter::Antialiasing | QPainter::TextAntialiasing);
        p.setPen(Qt::NoPen);
        p.setBrush(QColor(0x12, 0x18, 0x14));
        p.drawRoundedRect(QRectF(0, 0, s, s), s * 0.2, s * 0.2);
        p.setPen(chalk);
        p.setFont(serif(qRound(s * 0.74), SemiBold));
        p.drawText(QRectF(0, -s * 0.06, s, s), Qt::AlignCenter, "R");
        p.setPen(QPen(rose, qMax(1.0, s / 18.0)));
        p.drawLine(QPointF(s * 0.26, s * 0.84), QPointF(s * 0.74, s * 0.84));
        p.end();
        ic.addPixmap(QPixmap::fromImage(img));
    }
    return ic;
}

} // namespace theme
