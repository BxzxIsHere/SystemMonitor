#include "Libs/Theme.h"

namespace slick {

const Theme& Theme::Dark() {
    // Every member already carries its dark-theme value as an in-class
    // initialiser; a second palette would override fields on a copy of this.
    static const Theme instance{};
    return instance;
}

} // namespace slick
