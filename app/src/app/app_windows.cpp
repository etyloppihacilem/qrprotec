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
#include <cfloat>
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

// --- Disposition par defaut du mode (normal ou gestion) ---

void App::open_mode_windows() {
  for (const auto &[id, layout] : mode_layout())
    if (AppWindow *target = window(id); target && layout.open && can_open(*target))
      target->open = true;
}

void App::request_layout_reset() {
  layout_pending_ = true;
  fullscreen_     = false;
  open_mode_windows(); // en gestion : le tableau de bord ferme revient sous l'Etat des lots
}

// --- Magnetisme des bords ---

namespace {

// Valeur du bord (x si axis 0, y si 1) collee au bord le plus proche a moins de threshold : bords de la zone de travail,
// et bords des autres fenetres qui sont en face (leur etendue sur l'autre axe touche [span_min, span_max]).
float magnet_edge(const App::EdgeMagnet &magnet, int axis, float value, float span_min, float span_max) {
  float      best      = value;
  float      best_dist = magnet.threshold;
  const auto consider  = [&](float edge) {
    const float dist = std::fabs(edge - value);
    if (dist < best_dist) {
      best_dist = dist;
      best      = edge;
    }
  };
  consider(axis == 0 ? magnet.screen_min.x : magnet.screen_min.y);
  consider(axis == 0 ? magnet.screen_max.x : magnet.screen_max.y);
  for (const auto &[min, max] : magnet.others) {
    const float other_min = axis == 0 ? min.y : min.x, other_max = axis == 0 ? max.y : max.x;
    if (other_max < span_min - magnet.threshold || other_min > span_max + magnet.threshold)
      continue;
    consider(axis == 0 ? min.x : min.y);
    consider(axis == 0 ? max.x : max.y);
  }
  return best;
}

// Contrainte de taille d'ImGui pendant le redimensionnement : le bord tenu colle, le bord oppose ne bouge pas
// (ImGui recalcule la position a partir de la taille rendue pour les bords gauche et haut).
void magnet_size_callback(ImGuiSizeCallbackData *data) {
  const auto  &magnet = *static_cast< const App::EdgeMagnet * >(data->UserData);
  ImVec2       size   = data->DesiredSize;
  const float  right  = data->Pos.x + data->CurrentSize.x; // bords fixes quand on tire a gauche ou en haut
  const float  bottom = data->Pos.y + data->CurrentSize.y;
  const float  x0     = magnet.left ? right - size.x : data->Pos.x;
  const float  y0     = magnet.top ? bottom - size.y : data->Pos.y;
  ImVec2       snapped = size;
  if (magnet.right)
    snapped.x = magnet_edge(magnet, 0, x0 + size.x, y0, y0 + size.y) - x0;
  else if (magnet.left)
    snapped.x = right - magnet_edge(magnet, 0, x0, y0, y0 + size.y);
  if (magnet.bottom)
    snapped.y = magnet_edge(magnet, 1, y0 + size.y, x0, x0 + size.x) - y0;
  else if (magnet.top)
    snapped.y = bottom - magnet_edge(magnet, 1, y0, x0, x0 + size.x);
  // jamais plus petite que la taille minimale d'ImGui
  data->DesiredSize = ImVec2(snapped.x >= magnet.min_size.x ? snapped.x : size.x,
                             snapped.y >= magnet.min_size.y ? snapped.y : size.y);
}

} // namespace

void App::apply_edge_magnet(AppWindow &target, const std::string &label) {
  ImGuiContext &g      = *GImGui;
  ImGuiWindow  *window = ImGui::FindWindowByName(label.c_str());
  if (window == nullptr || window->Collapsed || g.ActiveId == 0)
    return;
  const bool moving = g.MovingWindow != nullptr && g.MovingWindow->RootWindow == window;
  EdgeMagnet &magnet = edge_magnet_;
  magnet.left = magnet.right = magnet.top = magnet.bottom = false;
  if (!moving) {
    // poignees des coins : 0 bas droite, 1 bas gauche, 2 haut gauche, 3 haut droite ; bords : gauche, droite, haut, bas
    for (int corner = 0; corner < 4; ++corner)
      if (g.ActiveId == ImGui::GetWindowResizeCornerID(window, corner)) {
        magnet.left   = corner == 1 || corner == 2;
        magnet.right  = !magnet.left;
        magnet.top    = corner >= 2;
        magnet.bottom = !magnet.top;
      }
    for (int border = 0; border < 4; ++border)
      if (g.ActiveId == ImGui::GetWindowResizeBorderID(window, static_cast< ImGuiDir >(border))) {
        magnet.left   = border == ImGuiDir_Left;
        magnet.right  = border == ImGuiDir_Right;
        magnet.top    = border == ImGuiDir_Up;
        magnet.bottom = border == ImGuiDir_Down;
      }
    if (!magnet.left && !magnet.right && !magnet.top && !magnet.bottom)
      return;
  }
  ImVec2 origin, work;
  work_area(origin, work);
  magnet.screen_min = origin;
  magnet.screen_max = ImVec2(origin.x + work.x, origin.y + work.y);
  magnet.threshold  = std::round(ImGui::GetFontSize() * 0.75f);
  magnet.min_size   = g.Style.WindowMinSize;
  magnet.others.clear();
  for (const auto &other : windows)
    if (other.get() != &target && other->open && can_open(*other) && other->last_size.x > 0.0f)
      magnet.others.emplace_back(other->last_pos,
                                 ImVec2(other->last_pos.x + other->last_size.x, other->last_pos.y + other->last_size.y));
  if (!moving) {
    ImGui::SetNextWindowSizeConstraints(ImVec2(0.0f, 0.0f), ImVec2(FLT_MAX, FLT_MAX), magnet_size_callback, &magnet);
    return;
  }
  // deplacement : ImGui a deja place la fenetre sous la souris a cette image ; le bord le plus proche d'un bord voisin
  // y colle (gauche ou droite, haut ou bas), recalcule a chaque image depuis la souris
  const ImVec2 pos = window->Pos, size = window->Size;
  ImVec2       snapped = pos;
  for (int axis = 0; axis < 2; ++axis) {
    const float start = axis == 0 ? pos.x : pos.y, length = axis == 0 ? size.x : size.y;
    const float span_min = axis == 0 ? pos.y : pos.x, span_max = span_min + (axis == 0 ? size.y : size.x);
    const float to_start = magnet_edge(magnet, axis, start, span_min, span_max) - start;
    const float to_end   = magnet_edge(magnet, axis, start + length, span_min, span_max) - (start + length);
    const float delta = to_start == 0.0f ? to_end : to_end == 0.0f ? to_start
                        : std::fabs(to_start) <= std::fabs(to_end) ? to_start : to_end;
    (axis == 0 ? snapped.x : snapped.y) += delta;
  }
  if (snapped.x != pos.x || snapped.y != pos.y)
    ImGui::SetNextWindowPos(snapped, ImGuiCond_Always);
}

} // namespace qrprotec
