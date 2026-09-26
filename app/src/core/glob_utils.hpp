/* ##################################646f75627420796f7572206f776e206578697374656e6365###################################

               """          glob_utils.hpp
        -\-    _|__
         |\___/  . \        Created on 26 Sep. 2026 at 11:15
         \     /(((/        by hmelica
          \___/)))/         hmelica@student.42.fr

##################################################################################################################### */

#pragma once

#include <filesystem>
#include <string>
#include <vector>

std::vector< std::filesystem::path > glob_current_dir(const std::string &pattern);
