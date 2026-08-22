// The application icon has to exist at real icon sizes, and the committed
// Windows .ico has to stay a well-formed multi-size resource.
//
// Both guard regressions that are invisible until someone looks at a taskbar:
// StyleKit's general icon() tops out at 48 px (paintIcon emits 16/20/24/32/48),
// and QIcon never upscales — so a 256 px request silently returns 48 px and the
// packaged Linux icon or an .ico entry ends up blurry. appIcon() exists to
// carry the large sizes; if someone routes it back through paintIcon() these
// tests fail rather than the icon quietly degrading.

#include "scope/style/StyleKit.h"

#include <gtest/gtest.h>

#include <QGuiApplication>
#include <QFile>
#include <QIcon>
#include <QPixmap>

#include <cstdint>

namespace {

// Leaked on purpose — destroying a QGuiApplication during static destruction
// crashes under the offscreen platform. See tests/test_save_chart_dialog.cpp.
void ensureGuiApp() {
    if (!QCoreApplication::instance()) {
        qputenv("QT_QPA_PLATFORM", "offscreen");
        static int argc = 1;
        static char a0[] = {'t', '\0'};
        static char* argv[] = {a0, nullptr};
        new QGuiApplication(argc, argv);   // NOLINT — intentionally not deleted
    }
}

}  // namespace

TEST(AppIcon, CarriesEveryRealIconSizeUpTo256) {
    ensureGuiApp();
    const QIcon ic = scope::style::appIcon();
    ASSERT_FALSE(ic.isNull());

    for (int size : {16, 24, 32, 48, 64, 128, 256}) {
        const QPixmap pm = ic.pixmap(size, size);
        EXPECT_EQ(pm.width(), size)
            << "appIcon() must supply a real " << size << "px raster; QIcon "
               "does not upscale, so a smaller one means a blurry taskbar / "
               ".ico entry";
        EXPECT_EQ(pm.height(), size);
        EXPECT_FALSE(pm.isNull());
    }
}

TEST(AppIcon, IsNotBlank) {
    ensureGuiApp();
    const QImage img = scope::style::appIcon().pixmap(256, 256).toImage();
    ASSERT_FALSE(img.isNull());

    // The logo is an accent rounded square with a white waveform, so a correct
    // render has both opaque accent pixels and near-white ones. A blank or
    // single-colour icon (the flat placeholder Linux packaging used to ship)
    // fails this.
    int accent = 0, white = 0;
    for (int y = 0; y < img.height(); y += 4) {
        for (int x = 0; x < img.width(); x += 4) {
            const QColor c = img.pixelColor(x, y);
            if (c.alpha() < 128) continue;
            if (c.red() > 230 && c.green() > 230 && c.blue() > 230) ++white;
            else if (c.blue() > c.red()) ++accent;
        }
    }
    EXPECT_GT(accent, 100) << "no accent-coloured body";
    EXPECT_GT(white, 20)   << "no white waveform — icon looks like a plain square";
}

// The .ico is committed rather than generated at build time, so a corrupt or
// truncated one would only show up as a wrong Explorer icon on Windows.
TEST(AppIcon, WindowsIcoIsAWellFormedMultiSizeResource) {
    QFile f(QStringLiteral(SCOPE_SOURCE_DIR) + "/resources/ScopeAnalyser.ico");
    ASSERT_TRUE(f.open(QIODevice::ReadOnly)) << "resources/ScopeAnalyser.ico missing";
    const QByteArray d = f.readAll();
    ASSERT_GE(d.size(), 22);

    auto u16 = [&](int off) {
        return static_cast<quint16>(static_cast<quint8>(d[off]) |
                                    (static_cast<quint8>(d[off + 1]) << 8));
    };
    auto u32 = [&](int off) {
        return static_cast<quint32>(static_cast<quint8>(d[off]) |
                                    (static_cast<quint8>(d[off + 1]) << 8) |
                                    (static_cast<quint8>(d[off + 2]) << 16) |
                                    (static_cast<quint8>(d[off + 3]) << 24));
    };

    EXPECT_EQ(u16(0), 0u);   // reserved
    EXPECT_EQ(u16(2), 1u);   // 1 = icon (2 would be a cursor)
    const int count = u16(4);
    EXPECT_GE(count, 5) << "an app icon needs several sizes, not just one";

    bool have256 = false, have16 = false;
    for (int i = 0; i < count; ++i) {
        const int e = 6 + 16 * i;
        ASSERT_LE(e + 16, d.size());
        const int w = static_cast<quint8>(d[e]) ? static_cast<quint8>(d[e]) : 256;
        const quint32 bytes = u32(e + 8);
        const quint32 off = u32(e + 12);
        EXPECT_GT(bytes, 0u);
        ASSERT_LE(off + bytes, static_cast<quint32>(d.size()))
            << "entry " << i << " points past the end of the file";
        if (w == 256) have256 = true;
        if (w == 16) have16 = true;
    }
    EXPECT_TRUE(have256) << "no 256px entry — Explorer's large-icon view needs one";
    EXPECT_TRUE(have16)  << "no 16px entry — needed for the title bar / file list";

    // Qt reads .ico natively, so it doubles as an end-to-end parse check.
    ensureGuiApp();
    const QIcon fromFile(QStringLiteral(SCOPE_SOURCE_DIR) +
                         "/resources/ScopeAnalyser.ico");
    EXPECT_FALSE(fromFile.isNull());
    EXPECT_EQ(fromFile.pixmap(256, 256).width(), 256);
}
