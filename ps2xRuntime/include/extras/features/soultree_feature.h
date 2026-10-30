#ifndef PC_EXTRAS_SOULTREE_FEATURE_H
#define PC_EXTRAS_SOULTREE_FEATURE_H

#include "extras/extras_host.h"

namespace pc_extras
{
    // "Soul Tree Events": the 13 characters and the heart doors that really exist for each of them; plays the original event in an isolated child process.
    std::unique_ptr<Feature> makeSoulTreeFeature();
}

#endif // PC_EXTRAS_SOULTREE_FEATURE_H
