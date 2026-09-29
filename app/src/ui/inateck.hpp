/* ##################################646f75627420796f7572206f776e206578697374656e6365###################################

               """          inateck.hpp
        -\-    _|__
         |\___/  . \        Created on 29 Sep. 2026 at 11:29
         \     /(((/        by hmelica
          \___/)))/         hmelica@student.42.fr

##################################################################################################################### */

#pragma once

#include "../inateck/inateck_worker.hpp"

namespace qrprotec {

class Inateck {

  public:
    Inateck();
    ~Inateck();

    void draw();

  private:
    void draw_inateck_window();

    InateckWorker inateck_worker_;
    bool inateck_window_open_ = false;
    int inateck_volume_ = 2;
    bool inateck_vibration_ = false;
    bool inateck_sdk_output_ = true;
    char inateck_prefix_[64] = {};
    char inateck_suffix_[64] = {};
    char inateck_name_[128] = {};
};

} // namespace qrprotec
