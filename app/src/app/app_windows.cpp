/* ##################################646f75627420796f7572206f776e206578697374656e6365###################################

               """          app_windows.cpp
        -\-    _|__
         |\___/  . \        Created on 16 Nov. 2026 at 19:40
         \     /(((/        by hmelica
          \___/)))/         hmelica@student.42.fr

##################################################################################################################### */

// App (suite de app.cpp) : Rangement des fenetres : moitie de l'ecran sur un bord, quart dans un coin, Super+fleches.

#include "app.hpp"
#include "../net/front_config.hpp"
#include "../core/french.hpp"
#include "../core/plural.hpp"
#include "../printer/logging.hpp"
#include "../render/label_types.hpp"

#include "../ui/inateck.hpp"
#include "../ui/label_preview.hpp"
#include "../ui/widgets.hpp"
#include "../ui/windows/windows.hpp"
#include "labels.hpp"
#include "../core/paths.hpp"
#include "../core/template_io.hpp"

#include "imgui_internal.h"
#include "imgui_stdlib.h"

#include <algorithm>
#include <cmath>
#include <cctype>
#include <cstdlib>
#include <ctime>
#include <iterator>
#include <map>
#include <random>
#include <set>
#include <utility>

namespace qrprotec {

// --- Rangement des fenetres : moitie de l'ecran sur un bord, quart dans un coin ---

bool App::can_snap(const AppWindow &target) const {
  // fenetres a taille automatique ou fixes : rien a ranger
  return !fullscreen_ && !(target.flags() & (ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize));
}

void App::snap_rect(int h, int v, ImVec2 &pos, ImVec2 &size) const {
  ImVec2 origin, work;
  work_area(origin, work);
  const float half_w = std::floor(work.x * 0.5f);
  const float half_h = std::floor(work.y * 0.5f);
  pos  = ImVec2(h > 0 ? origin.x + half_w : origin.x, v > 0 ? origin.y + half_h : origin.y);
  size = ImVec2(h == 0 ? work.x : h < 0 ? half_w : work.x - half_w, v == 0 ? work.y : v < 0 ? half_h : work.y - half_h);
}

void App::snap_window(AppWindow &target, int h, int v) {
  if (h == 0 && v == 0) {
    // sortie du rangement : place et taille d'avant
    if (target.snap_h != 0 || target.snap_v != 0) {
      if (target.snap_restore_size.x > 0.0f) {
        target.last_pos       = target.snap_restore_pos;
        target.last_size      = target.snap_restore_size;
        target.resize_pending = true;
      }
      target.snap_h = target.snap_v = 0;
    }
    return;
  }
  if (target.snap_h == 0 && target.snap_v == 0) {
    target.snap_restore_pos  = target.last_pos;
    target.snap_restore_size = target.last_size;
  }
  snap_rect(h, v, target.last_pos, target.last_size);
  target.snap_h         = h;
  target.snap_v         = v;
  target.resize_pending = true;
  target.focus_pending  = true;
}

// Super+fleche : la fenetre qui a le focus va vers ce bord. Gauche puis Haut : quart en haut a gauche ; la fleche
// opposee la ramene d'un cran (quart -> moitie -> place d'avant).
void App::snap_focused(int dh, int dv) {
  AppWindow *target = window(focused_window_);
  if (target == nullptr || !target->open || !can_open(*target) || !can_snap(*target))
    return;
  int h = target->snap_h, v = target->snap_v;
  if (dh != 0)
    h = h == -dh ? 0 : dh;
  if (dv != 0)
    v = v == -dv ? 0 : dv;
  snap_window(*target, h, v);
}

void App::update_snap_drag(AppWindow *moving) {
  if (moving != nullptr && can_snap(*moving) && ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
    if (!ImGui::IsMouseDragging(ImGuiMouseButton_Left))
      return; // simple clic sur la fenetre : rien ne bouge
    ImGuiContext &g = *GImGui;
    // fenetre rangee qu'on decroche : elle reprend sa taille d'avant, sous la souris au meme endroit en proportion
    if ((moving->snap_h != 0 || moving->snap_v != 0) && !snap_drag_restored_ && g.MovingWindow != nullptr) {
      ImGuiWindow *root = g.MovingWindow->RootWindow;
      if (moving->snap_restore_size.x > 0.0f && root->Size.x > 0.0f) {
        g.ActiveIdClickOffset.x *= moving->snap_restore_size.x / root->Size.x;
        ImGui::SetWindowSize(root, moving->snap_restore_size, ImGuiCond_Always);
      }
      moving->snap_h = moving->snap_v = 0;
    }
    snap_drag_restored_ = true;
    // zone sous la souris : bord (moitie) ou coin (quart, le long du bord sur un cinquieme de l'ecran)
    ImVec2 origin, work;
    work_area(origin, work);
    const ImVec2 mouse  = ImGui::GetIO().MousePos;
    const float  edge   = ImGui::GetFontSize() * 1.5f;
    const float  corner_w = work.x * 0.2f, corner_h = work.y * 0.2f;
    const bool   left   = mouse.x <= origin.x + edge;
    const bool   right  = mouse.x >= origin.x + work.x - edge;
    const bool   top    = mouse.y <= origin.y + edge;
    const bool   bottom = mouse.y >= origin.y + work.y - edge;
    int          h = 0, v = 0;
    if (left || right) {
      h = left ? -1 : 1;
      v = mouse.y <= origin.y + corner_h ? -1 : mouse.y >= origin.y + work.y - corner_h ? 1 : 0;
    } else if (top || bottom) {
      v = top ? -1 : 1;
      h = mouse.x <= origin.x + corner_w ? -1 : mouse.x >= origin.x + work.x - corner_w ? 1 : 0;
    }
    snap_drag_id_ = moving->id;
    snap_drag_h_  = h;
    snap_drag_v_  = v;
    if (h != 0 || v != 0) {
      // apercu de la place que prendra la fenetre
      ImVec2 pos, size;
      snap_rect(h, v, pos, size);
      const float  pad  = 6.0f;
      const ImVec2 min(pos.x + pad, pos.y + pad), max(pos.x + size.x - pad, pos.y + size.y - pad);
      ImVec4       tint = ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive);
      ImDrawList  *draw = ImGui::GetForegroundDrawList();
      tint.w            = 0.18f;
      draw->AddRectFilled(min, max, ImGui::GetColorU32(tint), 8.0f);
      tint.w = 0.9f;
      draw->AddRect(min, max, ImGui::GetColorU32(tint), 8.0f, 0, 3.0f);
    }
    return;
  }
  // souris lachee au dessus d'une zone : la fenetre y est rangee
  if (!snap_drag_id_.empty() && !ImGui::IsMouseDown(ImGuiMouseButton_Left) && (snap_drag_h_ != 0 || snap_drag_v_ != 0))
    if (AppWindow *target = window(snap_drag_id_); target && target->open && can_snap(*target))
      snap_window(*target, snap_drag_h_, snap_drag_v_);
  snap_drag_id_.clear();
  snap_drag_h_ = snap_drag_v_ = 0;
  snap_drag_restored_ = false;
}

} // namespace qrprotec
