#pragma once
#include <QColor>

namespace ivia {
// Base RGB values come from the upstream defaults, not a custom color scheme:
// https://github.com/ahrm/sioyek/blob/main/pdf_viewer/prefs.config
// https://github.com/ahrm/sioyek/blob/main/pdf_viewer/main.cpp
// https://github.com/pwmt/zathura/blob/develop/zathura/config.c
struct Theme {
    QColor canvas;
    QColor surface;
    QColor ink;
    QColor muted;
    QColor border;
    QColor accent;
    QColor accentWash;
    QColor inputBackground;
    QColor inputForeground;
    QColor searchHighlight;
    QColor activeSearchHighlight;
    QColor selectionHighlight;
};

inline Theme theme(bool dark)
{
    if (dark)
        // Zathura: default-bg/fg, completion-bg, inputbar-bg/fg and highlights.
        return {QColor("#000000"), QColor("#232323"), QColor("#dddddd"),
                QColor("#999999"), QColor("#232323"), QColor("#dddddd"), QColor("#444444"),
                QColor("#131313"), QColor("#9fbc00"),
                QColor(159, 251, 0, 128), QColor(0, 188, 0, 128), QColor(255, 255, 0, 80)};
    // Sioyek's shipped prefs override main.cpp's white canvas with RGB .97.
    // Overlay opacity is Ivia's; these are Sioyek's default highlight RGB values.
    return {QColor::fromRgbF(0.97, 0.97, 0.97), QColor("#ffffff"), QColor("#000000"),
            QColor("#666666"), QColor("#dddddd"), QColor("#000000"), QColor("#dddddd"),
            QColor("#000000"), QColor("#ffffff"),
            QColor(0, 255, 0, 65), QColor(0, 255, 0, 100), QColor(255, 255, 0, 80)};
}
}
