/* ##################################646f75627420796f7572206f776e206578697374656e6365###################################

               """          label_prints.cpp
        -\-    _|__
         |\___/  . \        Created on 07 Nov. 2026 at 11:00
         \     /(((/        by hmelica
          \___/)))/         hmelica@student.42.fr

##################################################################################################################### */

// Impressions demandees au poste depuis ailleurs (voir database/inventory/label_prints.py) :
// - etiquettes d'items demandees depuis le front web par un admin (Gestion de la TECH, bouton +) : un secouriste qui
//   rentre de poste avec du reassort lance l'impression et colle lui-meme les etiquettes. Les items sont crees par le
//   serveur au moment ou l'on lance l'impression, au nom de l'admin ;
// - etiquettes d'un front sur un autre ordinateur (cle de front, sans imprimante) : « Imprimer » y rend chaque
//   etiquette en image 1 bit et l'envoie (send_to_poste) ; le poste l'imprime telle quelle.
// Sur le poste (le seul front branche sur l'API locale), la fenetre « Impressions en attente » s'ouvre d'elle-meme a
// chaque nouvelle demande, meme sans personne de connecte. Une demande pas imprimee expire en 24 h (serveur).

#include "../core/plural.hpp"
#include "../render/raster.hpp"
#include "../ui/widgets.hpp"
#include "app.hpp"
#include "labels.hpp"

#include "imgui.h"

#include <algorithm>
#include <cmath>
#include <memory>

namespace qrprotec {

namespace {

constexpr double      kPollSeconds = 20.0;
constexpr std::size_t kSendBytes   = 1500000; // par envoi au poste (le serveur refuse les requetes de plus de 2,5 Mo)
constexpr std::size_t kSendLabels  = 200;     // par envoi (label_prints.MAX_COUNT)

// Etiquette rendue -> {description, width, height, ppm, bits} : 1 bit par pixel (1 = noir), lignes de gauche a
// droite, bit de poids fort d'abord (format de label_prints._check_label)
Json encode_label(const PrintJob &job) {
  const RasterImage image = render_template(job.document);
  Json              label = Json::object();
  label["description"] = job.description;
  label["width"]       = image.width;
  label["height"]      = image.height;
  label["ppm"]         = static_cast< double >(job.document.media.pixels_per_mm);
  label["bits"]        = encode_raster_bits(image);
  if (const std::optional< LabelType > &type = job.document.label_type) {
    // le poste imprime l'etiquette sur le rouleau de son type (taille, orientation, ruban)
    Json label_type          = Json::object();
    label_type["name"]       = type->name;
    label_type["width_mm"]   = type->width_mm;
    label_type["height_mm"]  = type->height_mm;
    label_type["rotation"]   = normalize_rotation(type->rotation);
    label_type["support"]    = label_support_id(type->support);
    label_type["margin_mm"]  = type->margin_mm;
    label["label_type"]      = label_type;
  }
  return label;
}

bool decode_label(const Json &label, PrintJob &job) {
  if (!decode_raster_bits(label["bits"].str(), label["width"].integer(), label["height"].integer(), job.image))
    return false;
  job.description                  = label["description"].str();
  job.document.media.pixels_per_mm = static_cast< float >(label["ppm"].num(8.0));
  const Json &label_type           = label["label_type"];
  if (label_type.is_object() && !label_type["name"].str().empty()) {
    LabelType type;
    type.name               = label_type["name"].str();
    type.width_mm           = label_type["width_mm"].num(type.width_mm);
    type.height_mm          = label_type["height_mm"].num(type.height_mm);
    type.rotation           = normalize_rotation(static_cast< int >(std::lround(label_type["rotation"].num(0.0))));
    type.support            = label_support_from_id(label_type["support"].str());
    type.margin_mm          = std::clamp(label_type["margin_mm"].num(LabelType::kDefaultMarginMm), 0.0, 10.0);
    job.document.label_type = type;
  }
  return true;
}

// « Lot B+ 1 (clé) 2/3 » -> « Lot B+ 1 (clé) » : ce qui est imprime, pour la fenetre du poste
std::string jobs_title(const std::vector< PrintJob > &jobs) {
  std::string title = jobs.empty() ? std::string() : jobs.front().description;
  if (jobs.size() > 1)
    if (const auto space = title.rfind(' '); space != std::string::npos && title.find('/', space) != std::string::npos)
      title.erase(space);
  return title;
}

// Qui a demande, et d'ou : « Sophie Bernard », « Sophie Bernard (Ordi du TECH) », « Ordi du TECH »
std::string request_who(const Json &request) {
  const std::string by = request["by"].str(), front = request["front"].str();
  if (front.empty())
    return by;
  return by.empty() ? front : by + " (" + front + ")";
}

} // namespace

void App::refresh_label_prints() {
  label_prints_.loading   = true;
  label_prints_.next_poll = ImGui::GetTime() + kPollSeconds;
  api.get("/api/label-prints/pending/", [this](const ApiResult &result) {
    label_prints_.loading = false;
    if (!result.ok)
      return; // serveur injoignable ou ancien serveur : rien a montrer, on reessaie au prochain tour
    label_prints_.data = result.data;
    bool fresh         = false;
    for (const Json &request : result.data["pending"].items())
      fresh |= label_prints_.seen.insert(request["id"].integer()).second;
    if (fresh)
      open_label_prints();
  });
}

void App::send_to_poste(std::vector< PrintJob > jobs) {
  if (jobs.empty())
    return;
  const std::string title = jobs_title(jobs);
  // envois de 1,5 Mo au plus : une etiquette de 40 x 30 mm pese environ 13 ko
  std::vector< Json > batches;
  Json                labels = Json::array();
  std::size_t         bytes  = 0;
  for (const PrintJob &job : jobs) {
    Json              label = encode_label(job);
    const std::size_t size  = label["bits"].str().size() + 200;
    if (labels.size() > 0 && (bytes + size > kSendBytes || labels.size() >= kSendLabels)) {
      batches.push_back(std::move(labels));
      labels = Json::array();
      bytes  = 0;
    }
    labels.push_back(std::move(label));
    bytes += size;
  }
  batches.push_back(std::move(labels));
  const std::size_t count = jobs.size();
  auto              left  = std::make_shared< std::size_t >(batches.size());
  auto              failed = std::make_shared< bool >(false);
  for (Json &batch : batches) {
    Json body      = Json::object();
    body["title"]  = title;
    body["labels"] = std::move(batch);
    api.post("/api/label-prints/send/", body, [this, count, left, failed](const ApiResult &result) {
      if (!result.ok && !*failed) {
        *failed = true;
        notify("Envoi au poste refusé : " + result.error, true);
      }
      if (--*left == 0 && !*failed)
        notify(nb(count, "étiquette envoyée", "étiquettes envoyées") + " au poste : lancez l'impression dans sa "
               "fenêtre « Impressions en attente ».");
    });
  }
}

void App::print_label_request(const Json &request) {
  if (request["kind"].str() == "labels") {
    std::vector< PrintJob > jobs;
    for (const Json &label : request["labels"].items()) {
      PrintJob job;
      if (decode_label(label, job))
        jobs.push_back(std::move(job));
    }
    if (jobs.size() < request["labels"].size())
      notify("Étiquettes illisibles ignorées : demandez un nouvel envoi.", true);
    if (!jobs.empty())
      print_documents(std::move(jobs));
    return;
  }
  const Json               &items = request["items"];
  std::vector< Parameters > labels;
  const int                 count = static_cast< int >(items.size());
  for (int index = 0; index < count; ++index) {
    Parameters parameters = item_parameters(items[index], index + 1, count);
    parameters["printed_by"] = request["by"].str(); // l'admin qui les a demandees (personne n'est forcement connecte)
    labels.push_back(std::move(parameters));
  }
  if (!labels.empty())
    print_labels(TemplateCategory::Item, labels, request["type_name"].str());
}

// Demandes envoyees une par une : le serveur cree les items de chacune, puis leurs etiquettes partent a l'imprimante
void App::launch_label_prints(std::vector< int > ids) {
  if (ids.empty()) {
    label_prints_.busy = false;
    refresh_label_prints();
    return;
  }
  label_prints_.busy = true;
  const int id       = ids.front();
  ids.erase(ids.begin());
  api.post("/api/label-prints/" + std::to_string(id) + "/print/", Json::object(),
           [this, ids](const ApiResult &result) {
             if (result.ok)
               print_label_request(result.data);
             else
               notify("Impression refusée : " + result.error, true);
             launch_label_prints(ids);
           });
}

void App::draw_label_prints() {
  if (!label_prints_.open)
    return;
  const ImGuiViewport *viewport = ImGui::GetMainViewport();
  ImGui::SetNextWindowPos(viewport->GetCenter(), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
  ImGui::SetNextWindowSize(ImVec2(std::min(760.0f, viewport->WorkSize.x * 0.9f), std::min(520.0f, viewport->WorkSize.y * 0.8f)),
                           ImGuiCond_Appearing);
  if (label_prints_.focus) {
    ImGui::SetNextWindowFocus();
    ImGui::SetNextWindowCollapsed(false);
    label_prints_.focus = false;
  }
  if (!ImGui::Begin("Impressions en attente###label_prints", &label_prints_.open, ImGuiWindowFlags_NoCollapse)) {
    ImGui::End();
    return;
  }
  const Json  &pending = label_prints_.data["pending"];
  const Json  &recent  = label_prints_.data["recent"];
  const float  base    = ImGui::GetStyle().FontSizeBase;
  ImGui::PushTextWrapPos(0.0f);
  ImGui::TextUnformatted("Étiquettes demandées depuis le web ou un autre ordinateur. Lancez l'impression. Pour de "
                         "nouvelles étiquettes d'items, collez chacune sur un item de ce type : les items sont ajoutés "
                         "au stock. Une demande pas imprimée en 24 h disparaît.");
  ImGui::PopTextWrapPos();
  ImGui::Spacing();

  int labels = 0;
  std::vector< int > ids;
  for (const Json &request : pending.items()) {
    labels += request["count"].integer();
    ids.push_back(request["id"].integer());
  }
  if (pending.size() == 0) {
    ImGui::TextColored(colors::green, "Aucune impression en attente.");
  } else if (ImGui::BeginTable("label_prints_pending", 3, ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH)) {
    ImGui::TableSetupColumn("Étiquettes", ImGuiTableColumnFlags_WidthStretch);
    ImGui::TableSetupColumn("Demandé par", ImGuiTableColumnFlags_WidthFixed, 200.0f);
    ImGui::TableSetupColumn("Quand", ImGuiTableColumnFlags_WidthFixed, 160.0f);
    ImGui::TableHeadersRow();
    for (const Json &request : pending.items()) {
      ImGui::TableNextRow();
      ImGui::TableNextColumn();
      ImGui::PushFont(nullptr, base * 1.2f);
      ImGui::PushTextWrapPos(0.0f);
      ImGui::TextUnformatted(request["what"].str().c_str());
      ImGui::PopTextWrapPos();
      ImGui::PopFont();
      ImGui::TableNextColumn();
      ImGui::PushTextWrapPos(0.0f);
      ImGui::TextUnformatted(request_who(request).c_str());
      ImGui::PopTextWrapPos();
      ImGui::TableNextColumn();
      ImGui::TextUnformatted(display_datetime(request["created"]).c_str());
    }
    ImGui::EndTable();
  }
  ImGui::Spacing();
  if (ids.empty() && !label_prints_.busy) {
    if (button("Fermer"))
      label_prints_.open = false;
  } else {
    ImGui::BeginDisabled(label_prints_.busy);
    const std::string launch = label_prints_.busy ? std::string("Impression en cours...")
                                                  : "Lancer l'impression (" + nb(labels, "étiquette") + ")";
    ImGui::PushFont(nullptr, base * 1.3f);
    if (primary_button(launch.c_str(), ImVec2(-1, ImGui::GetFrameHeight() * 1.6f)))
      launch_label_prints(ids);
    ImGui::PopFont();
    ImGui::EndDisabled();
    if (button("Plus tard"))
      label_prints_.open = false;
    ImGui::SameLine();
    ImGui::TextDisabled("La demande reste en attente (bouton bleu de la barre de menu).");
  }

  // imprimees dans les dernieres 24 h : reimpression si une etiquette a ete abimee ou le rouleau a coince
  if (recent.size() > 0) {
    ImGui::SeparatorText("Imprimées dans les dernières 24 h");
    for (const Json &request : recent.items()) {
      ImGui::PushID(request["id"].integer());
      // demande complete (items ou images) lue a la demande : la liste, interrogee toutes les 20 s, ne les a pas
      if (small_button("Réimprimer"))
        api.get("/api/label-prints/" + std::to_string(request["id"].integer()) + "/detail/",
                [this](const ApiResult &result) {
                  if (result.ok)
                    print_label_request(result.data);
                  else
                    notify("Réimpression impossible : " + result.error, true);
                });
      ImGui::SameLine();
      ImGui::Text("%s", request["what"].str().c_str());
      ImGui::SameLine();
      ImGui::TextDisabled("imprimées %s", display_on(request["printed"]).c_str());
      ImGui::PopID();
    }
  }
  ImGui::End();
}

} // namespace qrprotec
