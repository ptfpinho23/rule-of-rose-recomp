#include "discinfo.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QRegularExpression>
#include <QStringList>

namespace {

// The editions the port knows (the same list as recomp/games.py, which is
// what the builder goes by).
struct KnownDisc {
    const char *bootFile, *serial, *region;
    int hz;
};
const KnownDisc kDiscs[] = {
    {"SLES_542.18", "SLES-54218", "Europe", 50},
    {"SLUS_214.48", "SLUS-21448", "USA", 60},
};
const qint64 kSector = 2048;

const QStringList kIsoNames = {
    "Rule of Rose (Europe) (En,Fr,De,Es,It).iso",
    "Rule of Rose (USA).iso",
    "RuleOfRose.iso",
};

quint32 le32(const QByteArray &b, int off)
{
    const auto *p = reinterpret_cast<const unsigned char *>(b.constData()) + off;
    return quint32(p[0]) | quint32(p[1]) << 8 | quint32(p[2]) << 16 | quint32(p[3]) << 24;
}

// "SLES_542.18" -> "SLES-54218"
QString serialFromBootFile(QString name)
{
    name.remove('.');
    name.replace('_', '-');
    return name;
}

} // namespace

QString supportedDiscsText()
{
    return QString("the European (%1) or USA (%2) release").arg(kDiscs[0].serial, kDiscs[1].serial);
}

QString findDiscIso()
{
    QStringList dirs;
    QDir d(QCoreApplication::applicationDirPath());
    for (int up = 0; up <= 4; ++up) {
        dirs << d.absolutePath();
        if (!d.cdUp())
            break;
    }
    dirs << QDir::currentPath();
    for (const QString &dir : dirs)
        for (const QString &name : kIsoNames) {
            const QFileInfo fi(QDir(dir), name);
            if (fi.isFile())
                return fi.absoluteFilePath();
        }
    return {};
}

DiscInfo checkDisc(const QString &path)
{
    DiscInfo info;
    if (path.isEmpty()) {
        info.message = "No disc image selected.";
        return info;
    }
    const QFileInfo fi(path);
    info.path = fi.absoluteFilePath();
    if (!fi.isFile()) {
        info.message = "File not found.";
        return info;
    }
    QFile f(info.path);
    if (!f.open(QIODevice::ReadOnly)) {
        info.message = "Cannot open the file: " + f.errorString();
        return info;
    }

    // Primary volume descriptor: sector 16, type 1, "CD001".
    info.state = DiscInfo::NotIso;
    info.message = "Not an ISO9660 disc image (expected a plain .iso with 2048-byte sectors).";
    if (!f.seek(16 * kSector))
        return info;
    const QByteArray pvd = f.read(kSector);
    if (pvd.size() != kSector || pvd[0] != 1 || pvd.mid(1, 5) != "CD001")
        return info;

    // Root directory record sits at offset 156 of the PVD.
    const qint64 rootLba = le32(pvd, 156 + 2);
    const qint64 rootLen = le32(pvd, 156 + 10);
    if (rootLen <= 0 || rootLen > 64 * kSector || !f.seek(rootLba * kSector))
        return info;
    const QByteArray root = f.read(rootLen);
    if (root.size() != rootLen)
        return info;

    // Walk the root directory. Records never straddle a sector; a zero
    // length byte means "skip to the next sector".
    static const QRegularExpression bootRe("^S[CL][A-Z]{2}_\\d{3}\\.\\d{2}$");
    const KnownDisc *found = nullptr;
    QString other;
    for (int pos = 0; pos + 33 < root.size();) {
        const int len = quint8(root[pos]);
        if (len == 0) {
            pos = (pos / kSector + 1) * kSector;
            continue;
        }
        const int nameLen = quint8(root[pos + 32]);
        if (len < 33 + nameLen || pos + len > root.size())
            break;
        QString name = QString::fromLatin1(root.constData() + pos + 33, nameLen);
        const int semi = name.indexOf(';');
        if (semi >= 0)
            name.truncate(semi);
        const bool isDir = quint8(root[pos + 25]) & 2;
        if (!isDir) {
            const KnownDisc *known = nullptr;
            for (const KnownDisc &k : kDiscs)
                if (name == k.bootFile)
                    known = &k;
            if (known)
                found = known;
            else if (other.isEmpty() && bootRe.match(name).hasMatch())
                other = name;
        }
        pos += len;
    }

    if (found) {
        info.state = DiscInfo::Ok;
        info.serial = found->serial;
        info.region = found->region;
        info.hz = found->hz;
        info.message = QString("Rule of Rose (%1)  ·  %2  ·  %3 MB")
                           .arg(info.region, info.serial)
                           .arg(fi.size() / (1024 * 1024));
        return info;
    }
    info.state = DiscInfo::WrongDisc;
    if (!other.isEmpty()) {
        info.serial = serialFromBootFile(other);
        info.message = QString("Wrong disc: this image is %1. The port needs %2.")
                           .arg(info.serial, supportedDiscsText());
    } else {
        info.message = QString("Wrong disc: no Rule of Rose program on this image. The port "
                               "needs %1.").arg(supportedDiscsText());
    }
    return info;
}
