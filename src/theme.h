#pragma once
#include <QColor>

namespace ivia {
// Keep the reading surround neutral; iris is reserved for interactive details.
struct Theme {
    QColor canvas;
    QColor surface;
    QColor ink;
    QColor muted;
    QColor border;
    QColor accent;
    QColor accentWash;
};

inline Theme theme(bool dark)
{
    if (dark)
        return {QColor("#242329"), QColor("#302e37"), QColor("#e6e1ee"),
                QColor("#ada6ba"), QColor("#46414f"), QColor("#c2b2df"), QColor("#443c54")};
    return {QColor("#e3e2e7"), QColor("#f7f5fa"), QColor("#393440"),
            QColor("#7d7588"), QColor("#d9d3e2"), QColor("#79658f"), QColor("#eae3f3")};
}
}
