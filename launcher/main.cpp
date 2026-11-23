// Rule of Rose launcher.
//   RuleOfRose                    normal start
//   --iso PATH                    use this disc image for this run
//   --autoplay                    press PLAY once the window is up
//   --exit-with-game              with --autoplay: quit when the game ends
//                                 (exit status 0 if it ended cleanly, else 1)
//   --grab FILE.png               save a picture of the window and quit
//   --settings-dir DIR            read and write settings under DIR
//   --set NAME=VALUE              operate a control as a click would, before
//                                 --show / --autoplay (renderer=gpu|software,
//                                 fps=0|display|N, vsync=0|1, filter=NAME,
//                                 internal=N); repeatable
//   --extra-arg X                 append X to the game's command line (tests,
//                                 e.g. --extra-arg --frames --extra-arg 400)
//   --prepare                     run the "prepare the game" step now and exit
//                                 with its result (0 ready, 1 failed/cancelled);
//                                 --cancel-after MS cancels it; with --grab,
//                                 the picture is taken --grab-after MS into it
//   --dry-run                     PLAY prints the game command line and ROR_*
//                                 environment on stdout instead of starting it
//   --show PANEL                  with --grab: open a panel first: options,
//                                 options:res (list unrolled), options:quality, options:image,
//                                 options:game or controls
//   --hover                       with --grab: draw PLAY as if pointed at
#include <QApplication>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QProcess>
#include <QScreen>
#include <QSettings>
#include <QTimer>
#include <cstdio>

#include "mainwindow.h"
#include "style.h"

#ifdef Q_OS_LINUX
#include <dlfcn.h>
// Qt's xcb platform plugin needs a few xcb helper libraries that a stock
// Ubuntu does not install. We ship them (lib/ in the package; tools/debs/xcbrt
// in the build tree) and they are on this executable's RUNPATH, but a RUNPATH
// is not consulted for the dependencies of the libraries Qt loads later. So
// load them here by name, dependencies first; once loaded, the dynamic linker
// reuses them. Anything missing is simply skipped (a system copy may exist).
static void preloadXcbRuntime()
{
    static const char *const libs[] = {
        "libxcb-util.so.1",        "libxcb-image.so.0",   "libxcb-render-util.so.0",
        "libxcb-cursor.so.0",      "libxcb-icccm.so.4",   "libxcb-keysyms.so.1",
        "libxcb-shape.so.0",       "libxcb-xkb.so.1",     "libxkbcommon-x11.so.0",
    };
    for (const char *name : libs)
        dlopen(name, RTLD_NOW | RTLD_GLOBAL);
}
#endif

int main(int argc, char **argv)
{
#ifdef Q_OS_LINUX
    // Only the xcb (X11) platform plugin is shipped; under WSLg/XWayland
    // that is the one that works. An explicit QT_QPA_PLATFORM still wins.
    if (qEnvironmentVariableIsEmpty("QT_QPA_PLATFORM"))
        qputenv("QT_QPA_PLATFORM", "xcb");
    if (qgetenv("QT_QPA_PLATFORM").startsWith("xcb"))
        preloadXcbRuntime();
#endif

    QApplication app(argc, argv);
    QApplication::setApplicationName("Rule of Rose");
    QApplication::setOrganizationName("RoR Port");
    QSettings::setDefaultFormat(QSettings::IniFormat);
    // --settings-dir DIR: keep the settings file under DIR instead of the
    // user's profile (tests, or a self-contained copy on a stick).
    {
        const QStringList a = QCoreApplication::arguments();
        const int i = a.indexOf("--settings-dir");
        if (i > 0 && i + 1 < a.size()) {
            QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, a[i + 1]);
            MainWindow::setGameDir(a[i + 1] + "/game");     // a game prepared in a test stays with it
        }
    }

    // Fusion as the base style everywhere (it is built in: no style plugin
    // to ship); it only ever shows in the file dialog. Then our own look.
    QApplication::setStyle("Fusion");
    theme::apply(&app);

    QString iso, grabPath, showPanel;
    bool autoplay = false, exitWithGame = false, hover = false, dryRun = false;
    QStringList sets, extra;
    bool prepare = false;
    int cancelAfter = 0, grabAfter = 0;
    const QStringList args = QCoreApplication::arguments();
    for (int i = 1; i < args.size(); ++i) {
        const QString &a = args[i];
        if (a == "--grab" && i + 1 < args.size())
            grabPath = args[++i];
        else if (a == "--iso" && i + 1 < args.size())
            iso = args[++i];
        else if (a == "--show" && i + 1 < args.size())
            showPanel = args[++i];
        else if (a == "--hover")
            hover = true;
        else if (a == "--dry-run")
            dryRun = true;
        else if (a == "--set" && i + 1 < args.size())
            sets << args[++i];
        else if (a == "--prepare")
            prepare = true;
        else if (a == "--cancel-after" && i + 1 < args.size())
            cancelAfter = args[++i].toInt();
        else if (a == "--grab-after" && i + 1 < args.size())
            grabAfter = args[++i].toInt();
        else if (a == "--extra-arg" && i + 1 < args.size())
            extra << args[++i];
        else if (a == "--settings-dir")
            ++i;                    // handled above
        else if (a == "--autoplay")
            autoplay = true;
        else if (a == "--exit-with-game")
            exitWithGame = true;
    }

    // The window has a fixed size (1100 x 640 logical pixels) and no frame. On
    // a desktop too small for it at the current scale factor (a 4K screen at
    // 350 %, a laptop at 200 %) it would hang over the edges, so start again
    // with the scale factor lowered just enough to fit.
    if (const QScreen *scr = QGuiApplication::primaryScreen();
        scr && !qEnvironmentVariableIsSet("ROR_LAUNCHER_FITTED")) {
        const QSize avail = scr->availableGeometry().size();
        const double fit = qMin(avail.width() / 1116.0, avail.height() / 656.0);   // 8 px to spare all round
        if (fit < 1.0) {
            bool ok = false;
            double base = qEnvironmentVariable("QT_SCALE_FACTOR").toDouble(&ok);
            if (!ok || base <= 0)
                base = 1.0;
            qputenv("QT_SCALE_FACTOR", QByteArray::number(base * qMax(0.4, fit), 'f', 3));
            qputenv("ROR_LAUNCHER_FITTED", "1");
            fprintf(stderr, "[launcher] desktop %dx%d is smaller than the window: restarting at scale %s\n",
                    avail.width(), avail.height(), qgetenv("QT_SCALE_FACTOR").constData());
            fflush(stderr);
            return QProcess::execute(QCoreApplication::applicationFilePath(), args.mid(1));
        }
    }

    MainWindow w(iso);
    if (const QScreen *scr = QGuiApplication::primaryScreen()) {     // centred, fully on screen
        const QRect a = scr->availableGeometry();
        w.move(a.x() + qMax(0, (a.width() - w.width()) / 2), a.y() + qMax(0, (a.height() - w.height()) / 2));
    }
    w.setDryRun(dryRun);
    w.setTestMode(dryRun || !grabPath.isEmpty());
    for (const QString &x : extra)
        w.addExtraArg(x);
    for (const QString &kv : sets)
        w.testSet(kv.section('=', 0, 0), kv.section('=', 1));
    w.show();

    if (!grabPath.isEmpty()) {
        // Pump the event loop by hand (timers are unreliable on the
        // offscreen platform) so the window is painted, the start-up fade
        // is over and any requested state is reached before the grab.
        auto pump = [&app](int ms) {
            QElapsedTimer t;
            t.start();
            while (t.elapsed() < ms)
                app.processEvents(QEventLoop::AllEvents, 20);
        };
        pump(900);
        if (prepare) {            // picture of the prepare panel, --grab-after ms into it
            w.startPrepare();
            pump(grabAfter > 0 ? grabAfter : 2500);
        }
        if (hover)
            w.forcePlayHover();
        if (!showPanel.isEmpty())
            w.showPanel(showPanel);
        if (autoplay) {           // only useful with a game that fails at once
            w.play();
            pump(2500);
        }
        pump(700);
        const bool ok = w.grab().save(grabPath);
        const QRect fg = w.geometry(), av = w.screen()->availableGeometry();   // the window itself, without any shadow margin the window manager adds
        fprintf(stderr, "[grab] platform=%s saved ok=%d -> %s\n"
                        "[grab] window %dx%d at %d,%d; desktop area %dx%d at %d,%d; pixel ratio %.3f; fully on screen: %s\n",
                QGuiApplication::platformName().toLocal8Bit().constData(), int(ok),
                grabPath.toLocal8Bit().constData(), fg.width(), fg.height(), fg.x(), fg.y(),
                av.width(), av.height(), av.x(), av.y(), w.devicePixelRatioF(),
                av.contains(fg) ? "yes" : "NO");
        return ok ? 0 : 1;
    }

    if (prepare) {
        // --prepare: run the prepare step as a press on PREPARE GAME would and
        // leave with its result (0 ready, 1 failed or cancelled).
        QObject::connect(&w, &MainWindow::prepareFinished, &app, [](bool ok) {
            QCoreApplication::exit(ok ? 0 : 1);
        }, Qt::QueuedConnection);
        auto *t = new QTimer(&w);
        t->setSingleShot(true);
        QObject::connect(t, &QTimer::timeout, &w, &MainWindow::startPrepare);
        t->start(300);
        if (cancelAfter > 0) {
            auto *c = new QTimer(&w);
            c->setSingleShot(true);
            QObject::connect(c, &QTimer::timeout, &w, &MainWindow::cancelPrepare);
            c->start(300 + cancelAfter);
        }
        return app.exec();
    }

    if (autoplay) {
        if (exitWithGame)
            QObject::connect(&w, &MainWindow::gameFinished, &app, [](bool ok) {
                QCoreApplication::exit(ok ? 0 : 1);
            }, Qt::QueuedConnection);
        // (A QTimer object rather than QTimer::singleShot: the latter takes
        // std::chrono types, and this build uses libc++ against a libstdc++
        // Qt, so signatures with std:: types in them do not link.)
        QTimer *t = new QTimer(&w);
        t->setSingleShot(true);
        QObject::connect(t, &QTimer::timeout, &w, &MainWindow::play);
        t->start(300);
    }
    return app.exec();
}
