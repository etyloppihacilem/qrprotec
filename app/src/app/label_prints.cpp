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
// chaque nouvelle demande, meme sans personne de connecte. Une demande pas imprimee expire en 24h (serveur).
// Le poste interroge la file toutes les 20 s. Des qu'il y voit une nouvelle demande, il ouvre un WebSocket
// (/ws/label-prints, database/inventory/print_live.py) : chaque changement de la file lui est signale et il la relit
// aussitot, sans attendre. 15 min sans nouvelle impression : le WebSocket se ferme, l'interrogation reprend.

#include "../core/codes.hpp"
#include "../core/json.hpp"
#include "../core/plural.hpp"
#include "../core/template_io.hpp"
#include "../render/raster.hpp"
#include "../ui/widgets.hpp"
#include "app.hpp"
#include "app_internal.hpp"
#include "labels.hpp"

#include "imgui.h"

#include <algorithm>
#include <cmath>
#include <memory>
#include <utility>

namespace qrprotec {

namespace {

constexpr double      kPollSeconds = 20.0;
// en direct, la file est quand meme relue de temps en temps : un signal perdu (connexion coupee sans fermeture,
// erreur d'envoi du serveur) ne bloque plus les impressions demandees ou lancees a distance
constexpr double      kLivePollSeconds = 60.0;
constexpr std::size_t kSendBytes   = 1500000; // par envoi au poste (le serveur refuse les requetes de plus de 2,5 Mo)
constexpr std::size_t kSendLabels  = 200;     // par envoi (label_prints.MAX_COUNT)
constexpr double      kAutoCloseSeconds = 15 * 60.0; // fenetre fermee apres 15 min sans rien en attente
constexpr double      kLiveSeconds = 15 * 60.0; // en direct jusqu'a 15 min apres la derniere impression

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
    write_label_margins(label_type, type->margins);
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
    type.margins            = read_label_margins(label_type);
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

} // namespace

void App::request_label_prints_refresh() {
  if (label_prints_.loading)
    label_prints_.again = true; // la lecture en cours a pu partir avant le changement
  else
    refresh_label_prints();
}

void App::update_label_prints_live() {
  LabelPrints &state = label_prints_;
  if (label_prints_link_.session_gone()) {
    // 404 : serveur d'avant ce WebSocket, on s'en tient a l'interrogation toutes les 20 s
    label_prints_link_.stop();
    state.live_unsupported = true;
  }
  const bool wanted = kLocalPrinting && main_poste() && !state.live_unsupported && ImGui::GetTime() < state.live_until;
  if (!wanted) {
    if (label_prints_link_.running())
      label_prints_link_.stop(); // 15 min sans impression : l'interrogation reprend
    return;
  }
  if (!label_prints_link_.running()) {
    label_prints_link_.start(settings.api_endpoint(), "/ws/label-prints", "impressions en direct");
    return;
  }
  for (const std::string &text : label_prints_link_.take_messages()) {
    std::string error;
    const Json  message = Json::parse(text, &error);
    if (!error.empty() || !message.is_object())
      continue;
    // hello : (re)connecte, rien n'a ete rate entre-temps ; changed : nouvelle demande, file lancee ou annulee
    const std::string type = message["type"].str();
    if (type == "hello" || type == "changed")
      request_label_prints_refresh();
  }
}

void App::refresh_label_prints() {
  label_prints_.loading   = true;
  label_prints_.again     = false;
  label_prints_.next_poll = ImGui::GetTime() + (label_prints_link_.connected() ? kLivePollSeconds : kPollSeconds);
  if (label_prints_.remote) {
    // autre ordinateur, admins : files du poste, a gerer et lancer a distance
    api.post("/api/label-prints/queues/", Json::object(), [this](const ApiResult &result) {
      label_prints_.loading = false;
      if (label_prints_.again) {
        refresh_label_prints();
        return;
      }
      if (!result.ok) {
        label_prints_.error = result.error;
        return;
      }
      label_prints_.error   = "";
      label_prints_.data    = result.data;
      label_prints_.checked = now_hhmm();
    });
    return;
  }
  api.get("/api/label-prints/pending/", [this](const ApiResult &result) {
    label_prints_.loading = false;
    if (label_prints_.again) {
      refresh_label_prints(); // changement signale pendant la lecture : on relit
      return;
    }
    if (!result.ok)
      return; // serveur injoignable ou ancien serveur : rien a montrer, on reessaie au prochain tour
    label_prints_.data    = result.data;
    label_prints_.checked = now_hhmm();
    bool fresh            = false;
    for (const Json &request : result.data["pending"].items())
      fresh |= label_prints_.seen.insert(request["id"].integer()).second;
    if (fresh) {
      open_label_prints();
      // nouvelle impression : les suivantes arrivent en direct pendant 15 min
      label_prints_.live_until = ImGui::GetTime() + kLiveSeconds;
    }
    // file lancee a distance par un admin (web ou autre ordinateur) : imprimee sans que personne n'appuie, une file
    // a la fois ; la suivante part au tour d'apres
    if (label_prints_.busy)
      return;
    for (const Json &queue : result.data["queues"].items()) {
      if (queue["launch_requested"].str().empty())
        continue;
      std::vector< int > ids;
      for (const Json &id : queue["ids"].items())
        if (label_prints_.launched.insert(id.integer()).second)
          ids.push_back(id.integer());
      if (ids.empty())
        continue;
      open_label_prints();
      label_prints_.queue         = queue["queue"].str();
      label_prints_.queue_by_user = true;
      std::string by = queue["launch_requested_by"].str();
      notify("Impression lancée à distance" + (by.empty() ? std::string() : " par " + by) + " : file de "
             + queue["name"].str() + ".");
      launch_label_prints(ids);
      return;
    }
  });
}

bool App::label_prints_badge(const std::string &code, ScanSource source) {
  if (!label_prints_.open || label_prints_.remote)
    return false;
  const ParsedScan scan = parse_scan(code);
  if (scan.kind != ScanKind::User || !scan.temp.empty()) // QR code de connexion temporaire : connexion normale
    return false;
  label_prints_.badge_matricule = scan.id;
  label_prints_.badge_key       = scan.key;
  label_prints_.badge_source    = static_cast< int >(source);
  label_prints_.focus           = true;
  label_prints_.queue.clear();
  label_prints_.queue_by_user = false;
  for (const Json &queue : std::as_const(label_prints_.data)["queues"].items())
    if (!queue["matricule"].str().empty() && queue["matricule"].str() == scan.id) {
      label_prints_.queue         = queue["queue"].str();
      label_prints_.queue_by_user = true;
    }
  return true;
}

void App::label_queue_action(const std::string &queue, const std::string &action) {
  Json body     = Json::object();
  body["queue"] = queue;
  label_prints_.error = "";
  api.post("/api/label-prints/queues/" + action + "/", body, [this, action](const ApiResult &result) {
    if (!result.ok) {
      label_prints_.error = result.error;
    } else if (action == "launch") {
      notify("Impression lancée : le poste l'imprime dans moins d'une minute.");
    } else {
      notify(nb(result.data["cancelled"].integer(), "impression annulée", "impressions annulées") + ".");
    }
    refresh_label_prints();
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
  // etiquette de paquet ouvert demandee depuis la fiche du paquet du web (paquet pre-etiquete)
  if (request["kind"].str() == "pack" && request["open_label"].boolean()) {
    if (!request["pack_data"].is_null())
      print_labels(TemplateCategory::PackOpen, { pack_open_parameters(request["pack_data"]) }, "Paquet ouvert");
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
  label_prints_.busy       = true;
  label_prints_.live_until = std::max(label_prints_.live_until, ImGui::GetTime() + kLiveSeconds);
  const int id             = ids.front();
  ids.erase(ids.begin());
  api.post("/api/label-prints/" + std::to_string(id) + "/print/", Json::object(),
           [this, id, ids](const ApiResult &result) {
             if (result.ok)
               print_label_request(result.data);
             else
               notify("Impression refusée : " + result.error, true);
             // serveur injoignable : la demande, toujours lancee cote serveur, est reprise a la prochaine lecture
             // (une demande deja imprimee entre-temps serait refusee par le serveur, jamais imprimee deux fois)
             if (result.status == 0)
               label_prints_.launched.erase(id);
             launch_label_prints(ids);
           });
}

namespace {

// D'ou vient une demande : « Web » (etiquettes d'items) ou l'ordinateur qui l'a envoyee
std::string request_from(const Json &request) {
  if (!request["front"].str().empty())
    return request["front"].str();
  return request["kind"].str() == "labels" ? "Poste" : "Web";
}

} // namespace

// Demandes d'une file : ce qui sera imprime, d'ou et quand
void App::draw_label_queue(const Json &queue) {
  const float base = ImGui::GetStyle().FontSizeBase;
  std::vector< const Json * > rows;
  if (label_prints_.remote) {
    for (const Json &request : queue["prints"].items())
      rows.push_back(&request);
  } else {
    for (const Json &request : std::as_const(label_prints_.data)["pending"].items())
      if (request["queue"].str() == queue["queue"].str())
        rows.push_back(&request);
  }
  if (!ImGui::BeginTable("label_prints_pending", 3, ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH))
    return;
  ImGui::TableSetupColumn("Étiquettes", ImGuiTableColumnFlags_WidthStretch);
  ImGui::TableSetupColumn("Depuis", ImGuiTableColumnFlags_WidthFixed, 160.0f);
  ImGui::TableSetupColumn("Quand", ImGuiTableColumnFlags_WidthFixed, 160.0f);
  ImGui::TableHeadersRow();
  for (const Json *request : rows) {
    ImGui::TableNextRow();
    ImGui::TableNextColumn();
    ImGui::PushFont(nullptr, base * 1.2f);
    ImGui::PushTextWrapPos(0.0f);
    ImGui::TextUnformatted((*request)["what"].str().c_str());
    ImGui::PopTextWrapPos();
    ImGui::PopFont();
    ImGui::TableNextColumn();
    ImGui::PushTextWrapPos(0.0f);
    ImGui::TextUnformatted(request_from(*request).c_str());
    ImGui::PopTextWrapPos();
    ImGui::TableNextColumn();
    ImGui::TextUnformatted(display_datetime((*request)["created"]).c_str());
  }
  ImGui::EndTable();
}

void App::draw_label_prints() {
  LabelPrints &state = label_prints_;
  // fenetre des files du poste sur un autre ordinateur : gestion et admins
  if (state.open && state.remote && !privileged())
    state.open = false;
  if (!state.open) {
    // fermee : la prochaine ouverture repart sans file choisie ni badge
    state.queue.clear();
    state.queue_by_user = false;
    state.badge_matricule.clear();
    state.badge_key.clear();
    state.empty_since = -1.0;
    state.error.clear();
    return;
  }
  const ImGuiViewport *viewport = ImGui::GetMainViewport();
  ImGui::SetNextWindowPos(viewport->GetCenter(), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
  ImGui::SetNextWindowSize(ImVec2(std::min(760.0f, viewport->WorkSize.x * 0.9f), std::min(560.0f, viewport->WorkSize.y * 0.8f)),
                           ImGuiCond_Appearing);
  if (state.focus) {
    ImGui::SetNextWindowFocus();
    ImGui::SetNextWindowCollapsed(false);
    state.focus = false;
  }
  const char *title = state.remote ? "Files d'impression du poste###label_prints" : "Impressions en attente###label_prints";
  if (!ImGui::Begin(title, &state.open, ImGuiWindowFlags_NoCollapse)) {
    ImGui::End();
    return;
  }
  // lecture seule (std::as_const) : sur un autre ordinateur, data n'a pas « recent », et l'ajouter deplacait
  // « queues » sous la reference deja prise (boucle sans fin a l'ouverture de la fenetre)
  const Json  &queues = std::as_const(state.data)["queues"];
  const Json  &recent = std::as_const(state.data)["recent"];
  const float  base   = ImGui::GetStyle().FontSizeBase;
  const double now    = ImGui::GetTime();

  // plus rien en attente depuis 15 min : la fenetre se ferme d'elle-meme
  if (queues.size() == 0 && !state.busy) {
    if (state.empty_since < 0.0)
      state.empty_since = now;
    else if (now - state.empty_since >= kAutoCloseSeconds)
      state.open = false;
  } else {
    state.empty_since = -1.0;
  }

  ImGui::PushTextWrapPos(0.0f);
  if (state.remote)
    ImGui::TextUnformatted("Impressions en attente au poste, une file par personne qui les a demandées. « Lancer "
                           "l'impression » : le poste l'imprime dans la minute, sans que personne n'y touche.");
  else
    ImGui::TextUnformatted("Étiquettes demandées depuis le web ou un autre ordinateur, une file par personne. "
                           "Pour de nouvelles étiquettes d'items, collez chacune sur un item de ce type : les items "
                           "sont ajoutés au stock. Une demande pas imprimée en 24h disparaît.");
  ImGui::PopTextWrapPos();
  // nouvelles demandes : vues aussitot en direct, sinon au prochain tour (20 s) ou a la main
  ImGui::BeginDisabled(state.loading);
  if (small_button(state.loading ? "Rafraîchissement..." : "Rafraîchir"))
    request_label_prints_refresh();
  ImGui::EndDisabled();
  ImGui::SameLine();
  if (!state.remote && label_prints_link_.connected())
    ImGui::TextColored(colors::green, "En direct : les nouvelles impressions arrivent aussitôt.");
  else
    ImGui::TextDisabled("Nouvelles impressions vérifiées toutes les %d s%s.", static_cast< int >(kPollSeconds),
                        state.checked.empty() ? "" : (", dernière fois à " + state.checked).c_str());
  ImGui::Spacing();

  // file affichee : la seule, celle choisie (a la main ou par badge), sinon celle de la personne connectee
  const Json *shown = nullptr;
  for (const Json &queue : queues.items())
    if (!state.queue.empty() && queue["queue"].str() == state.queue)
      shown = &queue;
  if (shown == nullptr && state.queue_by_user) {
    state.queue.clear(); // imprimee, annulee ou expiree
    state.queue_by_user = false;
  }
  if (shown == nullptr && queues.size() == 1)
    shown = &queues[0];
  if (shown == nullptr && user && state.badge_matricule.empty())
    for (const Json &queue : queues.items())
      if (queue["matricule"].str() == user->matricule)
        shown = &queue;

  // badge scanne fenetre ouverte : ses impressions, et la connexion seulement sur demande
  if (!state.badge_matricule.empty()) {
    const bool has_queue = shown != nullptr && !(*shown)["matricule"].str().empty()
                        && (*shown)["matricule"].str() == state.badge_matricule;
    ImGui::PushFont(nullptr, base * 1.15f);
    if (has_queue)
      ImGui::TextColored(colors::green, "Badge scanné : impressions de %s.", (*shown)["name"].str().c_str());
    else
      ImGui::TextColored(colors::orange, "Badge scanné : rien en attente pour ce badge.");
    ImGui::PopFont();
    if (!(user && user->matricule == state.badge_matricule)) {
      ImGui::AlignTextToFramePadding();
      ImGui::TextUnformatted("Le badge ne vous a pas connecté. Pour vous connecter :");
      ImGui::SameLine();
      if (primary_button("Se connecter", ImVec2(ImGui::GetFontSize() * 9.0f, 0.0f))) {
        ParsedScan scan;
        scan.kind = ScanKind::User;
        scan.id   = state.badge_matricule;
        scan.key  = state.badge_key;
        state.badge_matricule.clear();
        state.badge_key.clear();
        login_with_badge(scan, static_cast< ScanSource >(state.badge_source));
      }
    }
    ImGui::Spacing();
  }

  if (queues.size() == 0) {
    ImGui::TextColored(colors::green, "Aucune impression en attente.");
    if (!state.remote && state.empty_since >= 0.0)
      ImGui::TextDisabled("Cette fenêtre se ferme d'elle-même dans %d min.",
                          std::max(1, static_cast< int >(std::ceil((kAutoCloseSeconds - (now - state.empty_since)) / 60.0))));
  } else if (queues.size() > 1) {
    // plusieurs personnes ont envoye : chacune choisit sa file, ou scanne son badge
    ImGui::PushTextWrapPos(0.0f);
    ImGui::TextUnformatted(state.remote ? "Plusieurs files en attente :"
                                        : "Plusieurs personnes ont envoyé des impressions. Choisissez la vôtre, ou "
                                          "scannez votre badge :");
    ImGui::PopTextWrapPos();
    ImGui::PushFont(nullptr, base * 1.15f);
    for (const Json &queue : queues.items()) {
      ImGui::PushID(queue["queue"].str().c_str());
      std::string label = queue["name"].str() + "  ·  " + nb(queue["labels"].integer(), "étiquette");
      if (!queue["launch_requested"].str().empty())
        label += "  ·  lancée à distance";
      if (ImGui::RadioButton(label.c_str(), shown == &queue)) {
        state.queue         = queue["queue"].str();
        state.queue_by_user = true;
        state.badge_matricule.clear();
        state.badge_key.clear();
        shown = &queue;
      }
      ImGui::PopID();
    }
    ImGui::PopFont();
  }

  std::vector< int > ids;
  int                labels = 0;
  if (shown != nullptr) {
    const Json &queue = *shown;
    for (const Json &id : queue["ids"].items())
      ids.push_back(id.integer());
    labels = queue["labels"].integer();
    ImGui::Spacing();
    ImGui::PushFont(nullptr, base * 1.3f);
    ImGui::Text("Impressions de %s", queue["name"].str().c_str());
    ImGui::PopFont();
    if (!queue["launch_requested"].str().empty()) {
      const std::string by = queue["launch_requested_by"].str();
      ImGui::TextColored(colors::blue, "Lancée à distance%s%s %s.", by.empty() ? "" : " par ", by.c_str(),
                         display_on(queue["launch_requested"]).c_str());
    }
    draw_label_queue(queue);
  } else if (queues.size() > 1) {
    ImGui::Spacing();
    ImGui::TextDisabled("Choisissez une file pour voir ses impressions.");
  }
  if (!state.error.empty())
    ImGui::TextColored(colors::red, "%s", state.error.c_str());
  ImGui::Spacing();

  const std::string name = shown != nullptr ? (*shown)["name"].str() : std::string();
  if (state.remote) {
    if (shown != nullptr) {
      ImGui::PushFont(nullptr, base * 1.2f);
      const std::string launch = "Lancer l'impression au poste (" + nb(labels, "étiquette") + ")";
      if (primary_button(launch.c_str(), ImVec2(-1, ImGui::GetFrameHeight() * 1.5f)))
        label_queue_action((*shown)["queue"].str(), "launch");
      ImGui::PopFont();
    }
  } else if (queues.size() == 0 && !state.busy) {
    if (button("Fermer") || (queues.size() == 0 && (enter_pressed() || escape_pressed())))
      state.open = false;
  } else {
    ImGui::BeginDisabled(state.busy || ids.empty());
    const std::string launch = state.busy    ? std::string("Impression en cours...")
                             : ids.empty()   ? std::string("Lancer l'impression")
                                             : "Lancer l'impression de " + name + " (" + nb(labels, "étiquette") + ")";
    ImGui::PushFont(nullptr, base * 1.3f);
    if (primary_button(launch.c_str(), ImVec2(-1, ImGui::GetFrameHeight() * 1.6f)) || (!ids.empty() && enter_pressed())) {
      for (int id : ids)
        state.launched.insert(id);
      launch_label_prints(ids);
    }
    ImGui::PopFont();
    ImGui::EndDisabled();
  }
  if (!state.remote && (queues.size() > 0 || state.busy)) {
    if (button("Plus tard") || escape_pressed())
      state.open = false;
    ImGui::SameLine();
    ImGui::TextDisabled("Les demandes restent en attente (bouton bleu de la barre de menu).");
  } else if (state.remote) {
    if (button("Fermer") || escape_pressed())
      state.open = false;
  }
  // gestion et admins : annuler toute une file (sur le poste comme a distance)
  if (shown != nullptr && privileged()) {
    ImGui::SameLine();
    if (danger_button("Annuler cette file"))
      ImGui::OpenPopup("cancel_label_queue");
    if (ImGui::BeginPopupModal("cancel_label_queue", nullptr, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoTitleBar)) {
      ImGui::Text("Annuler les %s de %s ?", nb(labels, "étiquette").c_str(), name.c_str());
      ImGui::TextDisabled("Elles ne seront pas imprimées.");
      if (danger_button("Annuler la file")) {
        label_queue_action((*shown)["queue"].str(), "cancel");
        ImGui::CloseCurrentPopup();
      }
      ImGui::SameLine();
      if (button("Garder") || escape_pressed())
        ImGui::CloseCurrentPopup();
      ImGui::EndPopup();
    }
  }

  // imprimees dans les dernieres 24h : reimpression si une etiquette a ete abimee ou le rouleau a coince
  if (!state.remote && recent.size() > 0) {
    ImGui::SeparatorText("Imprimées dans les dernières 24h");
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
      ImGui::TextDisabled("de %s, imprimées %s", request["queue_name"].str().c_str(), display_on(request["printed"]).c_str());
      ImGui::PopID();
    }
  }
  ImGui::End();
}

} // namespace qrprotec
