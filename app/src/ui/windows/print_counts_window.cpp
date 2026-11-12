/* ##################################646f75627420796f7572206f776e206578697374656e6365###################################

               """          print_counts_window.cpp
        -\-    _|__
         |\___/  . \        Created on 12 Nov. 2026 at 17:00
         \     /(((/        by hmelica
          \___/)))/         hmelica@student.42.fr

##################################################################################################################### */

#include "../../core/paths.hpp"
#include "../../core/plural.hpp"
#include "../../core/template_io.hpp"
#include "../widgets.hpp"
#include "windows.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace qrprotec {

namespace {

const char *const kMonths[12] = { "Janvier", "Février", "Mars",      "Avril",   "Mai",      "Juin",
                                  "Juillet", "Août",    "Septembre", "Octobre", "Novembre", "Décembre" };

std::string mm(double value) {
  char text[32];
  std::snprintf(text, sizeof(text), "%g", std::round(value * 10.0) / 10.0);
  std::string result = text;
  for (char &c : result)
    if (c == '.')
      c = ',';
  return result;
}

// « 40 × 30 mm · direct » : en-tete de colonne (le libelle complet est dans l'infobulle)
std::string short_label(const Json &category) {
  return mm(category["width_mm"].num()) + " × " + mm(category["height_mm"].num()) + " mm · "
         + (category["support"].str() == "transfer" ? "transfert" : "direct");
}

std::string thousands(long long value) {
  std::string digits = std::to_string(value);
  for (int i = static_cast< int >(digits.size()) - 3; i > 0; i -= 3)
    digits.insert(static_cast< std::size_t >(i), " "); // espace fine insecable
  return digits;
}

void right_aligned(const std::string &text, bool disabled = false) {
  const float width = ImGui::CalcTextSize(text.c_str()).x;
  const float space = ImGui::GetContentRegionAvail().x - width;
  if (space > 0.0f)
    ImGui::SetCursorPosX(ImGui::GetCursorPosX() + space);
  if (disabled)
    ImGui::TextDisabled("%s", text.c_str());
  else
    ImGui::TextUnformatted(text.c_str());
}

// Compteur d'impressions (Gestion) : etiquettes envoyees a l'imprimante par tous les fronts, par mois calendaire et par
// categorie d'etiquette (taille et support), total de l'annee. Pour suivre la consommation de bobines et de ruban.
class PrintCountsWindow final : public AppWindow {
  public:
    PrintCountsWindow() : AppWindow("print_counts", "Impressions d'étiquettes", true, true) {}

    void on_open(App &app) override {
      scan_label_types();
      load(app, year_);
    }

    void reset_view(App &) override { year_ = 0; }

    void draw(App &app) override {
      const Json &years = data_["years"];
      const int   year  = data_["year"].integer();
      ImGui::SetNextItemWidth(ImGui::GetFontSize() * 6.0f);
      if (ImGui::BeginCombo("Année", year > 0 ? std::to_string(year).c_str() : "")) {
        for (const Json &entry : years.items()) {
          const int value = entry.integer();
          if (ImGui::Selectable(std::to_string(value).c_str(), value == year))
            load(app, value);
        }
        ImGui::EndCombo();
      }
      ImGui::SameLine();
      if (button(loading_ ? "Chargement..." : "Actualiser"))
        load(app, year_);
      ImGui::PushTextWrapPos(0.0f);
      ImGui::TextDisabled("Une étiquette compte quand elle est envoyée à l'imprimante (pas en cas d'erreur ni "
                          "d'annulation), depuis tous les ordinateurs. Survolez une colonne pour voir ses types "
                          "d'étiquettes.");
      ImGui::PopTextWrapPos();
      if (const int waiting = app.print_counter.pending(); waiting > 0)
        ImGui::TextColored(colors::orange, "%s sur cet ordinateur, pas encore envoyée%s au serveur.",
                           nb(waiting, "étiquette imprimée", "étiquettes imprimées").c_str(), waiting > 1 ? "s" : "");
      if (!error_.empty())
        ImGui::TextColored(colors::red, "%s", error_.c_str());
      ImGui::Spacing();

      const Json &categories = data_["categories"];
      const int   count      = static_cast< int >(categories.size());
      if (count == 0) {
        if (!loading_ && error_.empty())
          ImGui::TextDisabled("Aucune étiquette imprimée en %d.", year > 0 ? year : 0);
        return;
      }
      const bool  with_total    = count > 1;
      const int   current_month = data_["current_month"].integer(); // 0 : annee passee
      const Json &totals        = data_["months_total"];
      // moyenne par mois : mois finis depuis le premier mois compte (le compteur a demarre en cours d'annee) ; le
      // mois en cours seulement s'il est le seul
      int first_month = 0;
      while (first_month < 11 && totals[first_month].integer() == 0)
        ++first_month;
      int last_month = current_month > 0 ? current_month - 1 : 12; // dernier mois fini (1 a 12)
      if (last_month <= first_month)
        last_month = current_month > 0 ? current_month : 12;
      const int months_counted = std::max(1, last_month - first_month);

      if (!ImGui::BeginTable("print_counts", 1 + count + (with_total ? 1 : 0),
                             ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_BordersOuter
                               | ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingFixedFit))
        return;
      ImGui::TableSetupScrollFreeze(1, 1);
      ImGui::TableSetupColumn("Mois", ImGuiTableColumnFlags_WidthFixed, ImGui::GetFontSize() * 9.0f);
      for (const Json &category : categories.items())
        ImGui::TableSetupColumn(short_label(category).c_str(), ImGuiTableColumnFlags_WidthStretch);
      if (with_total)
        ImGui::TableSetupColumn("Total", ImGuiTableColumnFlags_WidthStretch);
      // en-tetes : libelle complet et types d'etiquettes des modeles au survol
      ImGui::TableNextRow(ImGuiTableRowFlags_Headers);
      for (int column = 0; column < ImGui::TableGetColumnCount(); ++column) {
        ImGui::TableSetColumnIndex(column);
        ImGui::TableHeader(ImGui::TableGetColumnName(column));
        if (column >= 1 && column <= count && ImGui::IsItemHovered()) {
          const Json &category = categories[column - 1];
          ImGui::BeginTooltip();
          ImGui::TextUnformatted(category["label"].str().c_str());
          const std::string names = type_names(category);
          if (!names.empty())
            ImGui::TextDisabled("Types d'étiquettes : %s", names.c_str());
          ImGui::EndTooltip();
        }
      }
      for (int month = 0; month < 12; ++month) {
        ImGui::TableNextRow();
        const bool future = current_month > 0 && month + 1 > current_month;
        if (month + 1 == current_month)
          row_color(colors::blue, 0.25f);
        ImGui::TableNextColumn();
        if (future)
          ImGui::TextDisabled("%s", kMonths[month]);
        else
          ImGui::TextUnformatted(month + 1 == current_month ? (std::string(kMonths[month]) + " (en cours)").c_str()
                                                             : kMonths[month]);
        for (int column = 0; column <= count; ++column) {
          if (column == count && !with_total)
            break;
          ImGui::TableNextColumn();
          if (future)
            continue;
          const int value = column < count ? categories[column]["months"][month].integer() : totals[month].integer();
          right_aligned(value > 0 ? thousands(value) : "–", value == 0);
        }
      }
      // total de l'annee
      ImGui::TableNextRow();
      row_color(colors::grey, 0.30f);
      ImGui::TableNextColumn();
      ImGui::TextUnformatted(("Total " + std::to_string(year)).c_str());
      for (const Json &category : categories.items()) {
        ImGui::TableNextColumn();
        right_aligned(thousands(category["total"].integer()));
      }
      if (with_total) {
        ImGui::TableNextColumn();
        right_aligned(thousands(data_["total"].integer()));
      }
      ImGui::TableNextRow();
      ImGui::TableNextColumn();
      ImGui::TextDisabled("Moyenne par mois");
      if (ImGui::IsItemHovered())
      {
        if (months_counted > 1)
          ImGui::SetTooltip("Sur %d mois, de %s à %s (le mois en cours n'est pas fini).", months_counted,
                            kMonths[first_month], kMonths[last_month - 1]);
        else
          ImGui::SetTooltip("Sur 1 mois : %s.", kMonths[first_month]);
      }
      for (int column = 0; column <= count; ++column) {
        if (column == count && !with_total)
          break;
        ImGui::TableNextColumn();
        long long total = 0;
        for (int month = first_month; month < last_month; ++month)
          total += column < count ? categories[column]["months"][month].integer() : totals[month].integer();
        right_aligned(thousands(std::lround(static_cast< double >(total) / std::max(1, months_counted))), true);
      }
      ImGui::EndTable();
    }

  private:
    void load(App &app, int year) {
      loading_          = true;
      const int request = ++request_;
      app.api.get("/api/print-counts/" + (year > 0 ? "?year=" + std::to_string(year) : std::string()),
                  [this, request, year](const ApiResult &result) {
                    if (request != request_)
                      return;
                    loading_ = false;
                    if (!result.ok) {
                      error_ = "Compteur d'impressions : " + result.error;
                      return;
                    }
                    error_.clear();
                    data_ = result.data;
                    year_ = year;
                  });
    }

    // types d'etiquettes des modeles (Editeur d'etiquettes), pour nommer les categories
    void scan_label_types() {
      std::vector< TemplateDocument > documents;
      for (const auto &path : glob_templates("*.qr")) {
        TemplateDocument document;
        std::string      error;
        if (load_template(document, path.string(), error))
          documents.push_back(std::move(document));
      }
      label_types_ = collect_label_types(documents);
    }

    std::string type_names(const Json &category) const {
      const auto        same = [](double a, double b) { return std::fabs(a - b) < 0.05; };
      const std::string support = category["support"].str();
      std::string       names;
      for (const LabelType &type : label_types_) {
        if (!same(type.width_mm, category["width_mm"].num()) || !same(type.height_mm, category["height_mm"].num())
            || support != label_support_id(type.support))
          continue;
        names += (names.empty() ? "" : ", ") + type.name;
      }
      return names;
    }

    Json                     data_ = Json::object();
    std::vector< LabelType > label_types_;
    std::string              error_;
    int                      year_    = 0; // 0 : annee en cours
    bool                     loading_ = false;
    int                      request_ = 0;
};

} // namespace

std::unique_ptr< AppWindow > make_print_counts_window() {
  return std::make_unique< PrintCountsWindow >();
}

} // namespace qrprotec
