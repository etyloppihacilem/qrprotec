/* ##################################646f75627420796f7572206f776e206578697374656e6365###################################

               """          doc_window.cpp
        -\-    _|__
         |\___/  . \        Created on 26 Nov. 2026 at 20:00
         \     /(((/        by hmelica
          \___/)))/         hmelica@student.42.fr

##################################################################################################################### */

#include "../widgets.hpp"
#include "../../core/docs.hpp"
#include "../../core/markdown.hpp"
#include "windows.hpp"

#include <algorithm>
#include <cmath>
#include <map>
#include <memory>

namespace qrprotec {

namespace {

namespace md = markdown;

const ImVec4 kPageBg(1.0f, 1.0f, 1.0f, 1.0f);
const ImVec4 kCodeBg(0.93f, 0.94f, 0.96f, 1.0f);
const ImVec4 kCodeText(0.15f, 0.20f, 0.30f, 1.0f);

// tailles en pixels entiers : moins d'arrondis d'ImGui dans la hauteur de la page
float px(float value) {
  return std::floor(value + 0.5f);
}

struct Visit {
    std::string path;
    float       scroll = 0.0f;
};

// Documentation du projet (docs/, embarquee a la construction) : arborescence a gauche, page a droite, liens
// relatifs suivis, Precedent / Suivant. Administration > Documentation, admins seulement.
class DocWindow final : public AppWindow {
  public:
    DocWindow() : AppWindow("docs", "Documentation", true, true) { admin_only = true; }

    void on_open(App &) override {
      load_library();
      if (current_.empty())
        navigate("README.md", "", false);
    }

    void reset_view(App &) override {
      back_.clear();
      forward_.clear();
      current_.clear();
      blocks_.clear();
      message_.clear();
      pending_anchor_.clear();
    }

    void draw(App &) override {
      load_library();
      if (library_->empty()) {
        ImGui::TextWrapped("La documentation n'a pas été embarquée dans cette version du poste (dossier docs/ absent "
                           "à la construction).");
        return;
      }
      if (current_.empty())
        navigate("README.md", "", false);
      draw_toolbar();
      const float tree_width = ImGui::GetFontSize() * 17.0f;
      // dossiers de la page ouverte deplies une fois (une navigation pendant ce dessin vaut pour l'image suivante)
      follow_ = tree_follow_;
      tree_follow_ = false;
      if (ImGui::BeginChild("##doc_tree", ImVec2(tree_width, 0), ImGuiChildFlags_Borders | ImGuiChildFlags_ResizeX))
        draw_folder(library_->root(), true);
      ImGui::EndChild();
      ImGui::SameLine();
      ImGui::PushStyleColor(ImGuiCol_ChildBg, kPageBg);
      if (ImGui::BeginChild("##doc_page", ImVec2(0, 0), ImGuiChildFlags_Borders,
                            ImGuiWindowFlags_AlwaysVerticalScrollbar))
        draw_page();
      ImGui::EndChild();
      ImGui::PopStyleColor();
      // lien clique pendant le dessin : suivi une fois la page dessinee (elle peut changer)
      if (!clicked_link_.empty()) {
        const std::string link = clicked_link_;
        clicked_link_.clear();
        follow(link);
      }
    }

  private:
    std::unique_ptr< md::Library > library_;
    std::string                    current_;
    std::vector< md::Block >       blocks_;
    std::vector< Visit >           back_;
    std::vector< Visit >           forward_;
    std::string                    pending_anchor_; // section a amener en haut de la page
    int                            anchor_frames_ = 0;    // images ou la section est encore ramenee en haut
    bool                           anchor_seen_   = false; // section trouvee pendant ce dessin
    float                          pending_scroll_ = -1.0f; // position a reprendre (Precedent), -1 : aucune
    int                            scroll_frames_  = 0;     // reprise sur 2 images (taille de la page connue)
    float                          scroll_         = 0.0f;  // position actuelle dans la page
    bool                           tree_follow_    = false; // dossiers de la page ouverte deplies au prochain dessin
    bool                           follow_         = false; // tree_follow_ pendant le dessin de l'arborescence
    std::string                    message_;
    std::string                    clicked_link_;

    void load_library() {
      if (library_)
        return;
      std::map< std::string, std::string > files;
      for (std::size_t index = 0; index < kEmbeddedDocsCount; ++index) {
        const EmbeddedDoc &doc = kEmbeddedDocs[index];
        files[doc.path]        = std::string(reinterpret_cast< const char * >(doc.data), doc.size);
      }
      library_ = std::make_unique< md::Library >(files);
    }

    const md::Page *find_page(const std::string &path) const {
      if (const md::Page *page = library_->page(path))
        return page;
      return library_->page(path + (path.empty() || path.back() == '/' ? "" : "/") + "README.md"); // lien vers un dossier
    }

    void navigate(const std::string &path, const std::string &anchor, bool remember) {
      const md::Page *page = find_page(path);
      if (page == nullptr) {
        message_ = "Page absente de la documentation : " + path;
        return;
      }
      if (remember && !current_.empty()) {
        back_.push_back({ current_, scroll_ });
        forward_.clear();
      }
      message_.clear();
      if (page->path != current_) {
        current_ = page->path;
        blocks_  = md::parse(page->source);
      }
      pending_anchor_ = anchor;
      anchor_frames_  = 3;
      if (anchor.empty())
        scroll_to(0.0f);
      tree_follow_ = true;
    }

    void scroll_to(float y) {
      pending_scroll_ = y;
      scroll_frames_  = 2;
    }

    void go_back() {
      if (back_.empty())
        return;
      forward_.push_back({ current_, scroll_ });
      const Visit visit = back_.back();
      back_.pop_back();
      navigate(visit.path, "", false);
      scroll_to(visit.scroll);
    }

    void go_forward() {
      if (forward_.empty())
        return;
      back_.push_back({ current_, scroll_ });
      const Visit visit = forward_.back();
      forward_.pop_back();
      navigate(visit.path, "", false);
      scroll_to(visit.scroll);
    }

    void follow(const std::string &link) {
      const md::LinkTarget target = md::resolve_link(current_, link);
      switch (target.kind) {
        case md::LinkTarget::External:
          ImGui::SetClipboardText(target.path.c_str());
          message_ = "Adresse copiée dans le presse-papiers : " + target.path;
          break;
        case md::LinkTarget::Anchor: navigate(current_, target.anchor, true); break;
        case md::LinkTarget::Page: navigate(target.path, target.anchor, true); break;
        case md::LinkTarget::Outside:
          message_ = "« " + target.path + " » est un fichier du dépôt, hors de la documentation : à lire dans les "
                     "sources (GitHub).";
          break;
      }
    }

    void draw_toolbar() {
      const bool focused = ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows);
      ImGui::BeginDisabled(back_.empty());
      if (button("← Précédent") || (focused && !back_.empty() && ImGui::IsKeyChordPressed(ImGuiMod_Alt | ImGuiKey_LeftArrow)))
        go_back();
      ImGui::SetItemTooltip("Alt+←");
      ImGui::EndDisabled();
      ImGui::SameLine();
      ImGui::BeginDisabled(forward_.empty());
      if (button("Suivant →")
          || (focused && !forward_.empty() && ImGui::IsKeyChordPressed(ImGuiMod_Alt | ImGuiKey_RightArrow)))
        go_forward();
      ImGui::SetItemTooltip("Alt+→");
      ImGui::EndDisabled();
      ImGui::SameLine();
      if (button("Accueil de la documentation"))
        navigate("README.md", "", true);
      ImGui::SameLine();
      ImGui::AlignTextToFramePadding();
      ImGui::TextDisabled("docs/%s", current_.c_str());
      if (!message_.empty()) {
        ImGui::PushStyleColor(ImGuiCol_Text, colors::blue);
        ImGui::TextWrapped("%s", message_.c_str());
        ImGui::PopStyleColor();
        ImGui::SameLine();
        if (ImGui::SmallButton("OK##doc_message"))
          message_.clear();
      }
    }

    // ---- Arborescence --------------------------------------------------------------------------------------------

    static bool contains(const md::Folder &folder, const std::string &path) {
      return folder.path.empty() || path.rfind(folder.path + "/", 0) == 0;
    }

    void draw_page_node(const std::string &path) {
      const md::Page *page = library_->page(path);
      if (page == nullptr)
        return;
      const bool selected = path == current_;
      // page ouverte : ses sections (titres de niveau 2) en dessous
      std::vector< const md::Block * > sections;
      if (selected)
        for (const md::Block &block : blocks_)
          if (block.kind == md::BlockKind::Heading && block.level == 2)
            sections.push_back(&block);
      ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_SpanAvailWidth | ImGuiTreeNodeFlags_OpenOnArrow;
      if (selected)
        flags |= ImGuiTreeNodeFlags_Selected;
      if (sections.empty())
        flags |= ImGuiTreeNodeFlags_Leaf | ImGuiTreeNodeFlags_NoTreePushOnOpen;
      if (selected && follow_)
        ImGui::SetNextItemOpen(true);
      const bool open = ImGui::TreeNodeEx(path.c_str(), flags, "%s", page->title.c_str());
      if (ImGui::IsItemClicked() && !ImGui::IsItemToggledOpen() && !selected)
        navigate(path, "", true);
      if (sections.empty() || !open)
        return;
      for (const md::Block *section : sections) {
        ImGui::TreeNodeEx(section->anchor.c_str(),
                          ImGuiTreeNodeFlags_Leaf | ImGuiTreeNodeFlags_NoTreePushOnOpen | ImGuiTreeNodeFlags_SpanAvailWidth,
                          "%s", md::plain_text(section->spans).c_str());
        if (ImGui::IsItemClicked())
          navigate(current_, section->anchor, true);
      }
      ImGui::TreePop();
    }

    void draw_folder(const md::Folder &folder, bool root) {
      if (root) {
        // racine : son README en tete, puis les parties
        if (!folder.readme.empty())
          draw_page_node(folder.readme);
        for (const std::string &page : folder.pages)
          draw_page_node(page);
        for (const md::Folder &child : folder.folders)
          draw_folder(child, false);
        return;
      }
      if (follow_ && contains(folder, current_))
        ImGui::SetNextItemOpen(true);
      const bool open = ImGui::TreeNodeEx(folder.path.c_str(),
                                          ImGuiTreeNodeFlags_SpanAvailWidth | ImGuiTreeNodeFlags_OpenOnArrow
                                            | ImGuiTreeNodeFlags_OpenOnDoubleClick,
                                          "%s", folder.title.c_str());
      if (ImGui::IsItemClicked() && !ImGui::IsItemToggledOpen() && !folder.readme.empty()) {
        navigate(folder.readme, "", true);
        ImGui::GetStateStorage()->SetInt(ImGui::GetItemID(), 1); // ouvert en meme temps
      }
      if (!open)
        return;
      if (!folder.readme.empty())
        draw_page_node(folder.readme);
      for (const std::string &page : folder.pages)
        draw_page_node(page);
      for (const md::Folder &child : folder.folders)
        draw_folder(child, false);
      ImGui::TreePop();
    }

    // ---- Page ----------------------------------------------------------------------------------------------------

    void draw_page() {
      if (scroll_frames_ > 0 && pending_scroll_ >= 0.0f) {
        ImGui::SetScrollY(pending_scroll_);
        if (--scroll_frames_ == 0)
          pending_scroll_ = -1.0f;
      }
      const float width = ImGui::GetFontSize() * 48.0f; // lignes de ~100 caracteres au plus, lisibles
      const float avail = ImGui::GetContentRegionAvail().x;
      if (avail > width) {
        // colonne centree
        ImGui::Indent((avail - width) * 0.5f);
        page_right_ = ImGui::GetCursorScreenPos().x + width;
      } else {
        page_right_ = ImGui::GetCursorScreenPos().x + avail;
      }
      for (std::size_t index = 0; index < blocks_.size(); ++index) {
        ImGui::PushID(static_cast< int >(index));
        draw_block(blocks_[index], index == 0);
        ImGui::PopID();
      }
      if (avail > width)
        ImGui::Unindent((avail - width) * 0.5f);
      ImGui::Dummy(ImVec2(0, px(ImGui::GetFontSize() * 2.0f)));
      if (!pending_anchor_.empty() && !anchor_seen_) {
        message_ = "Section « " + pending_anchor_ + " » introuvable dans cette page.";
        pending_anchor_.clear();
      }
      anchor_seen_ = false;
      scroll_ = ImGui::GetScrollY();
    }

    float page_right_ = 0.0f; // bord droit du texte (colonne de lecture), coordonnees ecran

    float right_edge() const {
      const float cell = ImGui::GetCursorScreenPos().x + ImGui::GetContentRegionAvail().x;
      return std::min(cell, page_right_);
    }

    void draw_block(const md::Block &block, bool first) {
      const float font_size = ImGui::GetFontSize();
      const float indent    = px(font_size * 1.8f);
      switch (block.kind) {
        case md::BlockKind::Heading: {
          if (!first)
            ImGui::Dummy(ImVec2(0, px(font_size * (block.level <= 2 ? 0.9f : 0.5f))));
          if (!pending_anchor_.empty() && pending_anchor_ == block.anchor) {
            // repris sur quelques images : ImGui arrondit les positions a l'ecran, la hauteur de ce qui precede
            // varie d'un pixel par ligne selon le defilement
            ImGui::SetScrollY(std::max(0.0f, ImGui::GetCursorPosY() - ImGui::GetStyle().WindowPadding.y));
            anchor_seen_ = true;
            if (--anchor_frames_ <= 0)
              pending_anchor_.clear();
          }
          const float scale = block.level == 1 ? 1.7f : block.level == 2 ? 1.4f : block.level == 3 ? 1.18f : 1.0f;
          ImFont     *bold  = get_bold_font();
          ImGui::PushFont(bold, px(ImGui::GetStyle().FontSizeBase * scale));
          draw_spans(block.spans, true);
          ImGui::PopFont();
          if (block.level <= 2)
            ImGui::Separator();
          ImGui::Dummy(ImVec2(0, px(font_size * 0.2f)));
          break;
        }
        case md::BlockKind::Paragraph:
          draw_spans(block.spans);
          ImGui::Dummy(ImVec2(0, px(font_size * 0.45f)));
          break;
        case md::BlockKind::ListItem: {
          const float  offset = indent * static_cast< float >(block.level);
          const ImVec2 at     = ImGui::GetCursorScreenPos();
          if (!block.marker.empty())
            // marqueur aligne a droite dans le retrait (1. a 10. alignes)
            ImGui::GetWindowDrawList()->AddText(ImVec2(at.x + offset + indent - ImGui::CalcTextSize(block.marker.c_str()).x - font_size * 0.4f, at.y),
                                                ImGui::GetColorU32(ImGuiCol_Text), block.marker.c_str());
          ImGui::Indent(offset + indent);
          draw_spans(block.spans);
          ImGui::Unindent(offset + indent);
          ImGui::Dummy(ImVec2(0, px(font_size * 0.15f)));
          break;
        }
        case md::BlockKind::Code: {
          // Indent(0) retirerait le retrait par defaut d'ImGui : seulement dans une liste
          const float offset = block.level > 0 ? indent * static_cast< float >(block.level + 1) : 0.0f;
          if (offset > 0.0f)
            ImGui::Indent(offset);
          // chasse fixe et lignes jamais coupees (arborescences, commandes) : barre de defilement horizontale
          // si le bloc depasse la colonne
          const ImVec2 pad(px(font_size * 0.6f), px(font_size * 0.4f));
          ImGui::PushFont(get_mono_font(), 0.0f);
          const ImVec2 text   = ImGui::CalcTextSize(block.code.c_str());
          const float  width  = std::max(right_edge() - ImGui::GetCursorScreenPos().x, font_size * 4.0f);
          const bool   scroll = text.x + pad.x * 2.0f > width;
          const float  height = text.y + pad.y * 2.0f + (scroll ? ImGui::GetStyle().ScrollbarSize : 0.0f);
          ImGui::PushStyleColor(ImGuiCol_ChildBg, kCodeBg);
          ImGui::PushStyleColor(ImGuiCol_Text, kCodeText);
          ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, pad);
          ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, px(font_size * 0.25f));
          if (ImGui::BeginChild("##code", ImVec2(width, px(height)), ImGuiChildFlags_AlwaysUseWindowPadding,
                                ImGuiWindowFlags_HorizontalScrollbar))
            ImGui::TextUnformatted(block.code.c_str());
          ImGui::EndChild();
          ImGui::PopStyleVar(2);
          ImGui::PopStyleColor(2);
          ImGui::PopFont();
          ImGui::Dummy(ImVec2(0, px(font_size * 0.45f)));
          if (offset > 0.0f)
            ImGui::Unindent(offset);
          break;
        }
        case md::BlockKind::Quote: {
          const float offset = indent * static_cast< float >(block.level) + (block.level > 0 ? indent : 0.0f);
          const float bar    = font_size * 0.8f;
          ImGui::Indent(offset + bar);
          const ImVec2 top = ImGui::GetCursorScreenPos();
          for (std::size_t index = 0; index < block.children.size(); ++index) {
            ImGui::PushID(static_cast< int >(index));
            draw_block(block.children[index], index == 0);
            ImGui::PopID();
          }
          const float bottom = ImGui::GetCursorScreenPos().y - font_size * 0.45f;
          ImGui::GetWindowDrawList()->AddRectFilled(ImVec2(top.x - bar, top.y), ImVec2(top.x - bar + 3.0f, bottom),
                                                    ImGui::GetColorU32(colors::orange));
          ImGui::Unindent(offset + bar);
          break;
        }
        case md::BlockKind::Table: draw_table(block); break;
        case md::BlockKind::Rule:
          ImGui::Dummy(ImVec2(0, px(font_size * 0.3f)));
          ImGui::Separator();
          ImGui::Dummy(ImVec2(0, px(font_size * 0.3f)));
          break;
      }
    }

    void draw_table(const md::Block &block) {
      const int columns = static_cast< int >(block.header.size());
      if (columns == 0)
        return;
      // largeur des colonnes : au moins leur plus long mot, puis selon la longueur du texte (attenuee) ; les
      // colonnes courtes restent etroites sans couper les mots
      std::vector< float > weights(static_cast< std::size_t >(columns), 4.0f);
      const auto           measure = [&](const std::vector< std::vector< md::Span > > &row) {
        for (std::size_t column = 0; column < row.size() && column < weights.size(); ++column) {
          const std::string text    = md::plain_text(row[column]);
          std::size_t       longest = 0;
          for (std::size_t begin = 0; begin < text.size();) {
            const std::size_t space = std::min(text.find(' ', begin), text.size());
            longest                 = std::max(longest, space - begin);
            begin                   = space + 1;
          }
          const float weight = std::max(static_cast< float >(longest) * 1.1f, std::sqrt(static_cast< float >(text.size())) * 4.0f);
          weights[column]    = std::max(weights[column], weight);
        }
      };
      measure(block.header);
      for (const auto &row : block.rows)
        measure(row);
      const float width = std::max(right_edge() - ImGui::GetCursorScreenPos().x, 100.0f);
      if (!ImGui::BeginTable("##table", columns,
                             ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp,
                             ImVec2(width, 0)))
        return;
      for (int column = 0; column < columns; ++column)
        ImGui::TableSetupColumn(nullptr, ImGuiTableColumnFlags_WidthStretch, weights[static_cast< std::size_t >(column)]);
      ImGui::TableNextRow(ImGuiTableRowFlags_Headers);
      for (int column = 0; column < columns; ++column) {
        ImGui::TableSetColumnIndex(column);
        ImGui::TableSetBgColor(ImGuiTableBgTarget_CellBg, ImGui::GetColorU32(ImGuiCol_TableHeaderBg));
        ImGui::PushID(column);
        draw_spans(block.header[static_cast< std::size_t >(column)], true);
        ImGui::PopID();
      }
      for (std::size_t row = 0; row < block.rows.size(); ++row) {
        ImGui::TableNextRow();
        for (int column = 0; column < columns; ++column) {
          ImGui::TableSetColumnIndex(column);
          ImGui::PushID(static_cast< int >(row * 64 + static_cast< std::size_t >(column)));
          draw_spans(block.rows[row][static_cast< std::size_t >(column)]);
          ImGui::PopID();
        }
      }
      ImGui::EndTable();
      ImGui::Dummy(ImVec2(0, px(ImGui::GetFontSize() * 0.45f)));
    }

    // Texte mis en forme, coupe aux espaces au bord droit (gras, italique, code, liens cliquables). Commence au
    // curseur ; les lignes suivantes repartent du retrait courant.
    void draw_spans(const std::vector< md::Span > &spans, bool all_bold = false) {
      if (spans.empty()) {
        ImGui::NewLine();
        return;
      }
      const float right      = right_edge();
      const float size       = ImGui::GetFontSize();
      bool        line_start = true;
      float       line_end   = 0.0f; // bord droit du dernier morceau dessine
      for (const md::Span &span : spans) {
        if (span.text == "\n") {
          if (line_start)
            ImGui::NewLine();
          line_start = true;
          continue;
        }
        const bool bold = all_bold || span.bold;
        ImFont    *font = bold && get_bold_font() ? get_bold_font() : ImGui::GetFont();
        if (span.code && get_mono_font())
          font = get_mono_font();
        ImU32      color = ImGui::GetColorU32(ImGuiCol_Text);
        if (!span.link.empty())
          color = ImGui::GetColorU32(ImGuiCol_TextLink);
        else if (span.code)
          color = ImGui::GetColorU32(kCodeText);
        else if (span.italic)
          color = ImGui::GetColorU32(ImVec4(0.30f, 0.30f, 0.38f, 1.0f));
        // morceaux : un mot et les espaces qui le suivent (espaces de tete a part)
        const std::string &text  = span.text;
        std::size_t        start = 0;
        while (start < text.size()) {
          std::size_t end = start;
          if (text[start] == ' ') {
            while (end < text.size() && text[end] == ' ')
              ++end;
          } else {
            while (end < text.size() && text[end] != ' ')
              ++end;
            while (end < text.size() && text[end] == ' ')
              ++end;
          }
          const std::string chunk = text.substr(start, end - start);
          start                   = end;
          const bool        last  = start >= text.size(); // dernier morceau du lien : souligne sans l'espace
          std::size_t word_end    = chunk.find_last_not_of(' ');
          if (word_end == std::string::npos) {
            if (line_start)
              continue; // espaces en debut de ligne
            word_end = chunk.size() - 1;
          }
          const float word_width  = font->CalcTextSizeA(size, FLT_MAX, 0.0f, chunk.c_str(), chunk.c_str() + word_end + 1).x;
          const float chunk_width = font->CalcTextSizeA(size, FLT_MAX, 0.0f, chunk.c_str()).x;
          // bord droit du morceau precedent calcule ici : celui d'un element hors de la vue (GetItemRectMax) ne
          // compte pas, et la mise en page changerait avec le defilement
          if (!line_start && line_end + word_width <= right)
            ImGui::SameLine(0.0f, 0.0f);
          line_start       = false;
          const ImVec2 at  = ImGui::GetCursorScreenPos();
          line_end         = at.x + chunk_width;
          ImDrawList  *draw = ImGui::GetWindowDrawList();
          if (span.code)
            draw->AddRectFilled(ImVec2(at.x - 1.0f, at.y), ImVec2(at.x + word_width + 1.0f, at.y + size),
                                ImGui::GetColorU32(kCodeBg), 2.0f);
          if (span.link.empty()) {
            ImGui::PushFont(font, 0.0f);
            ImGui::PushStyleColor(ImGuiCol_Text, color);
            ImGui::TextUnformatted(chunk.c_str());
            ImGui::PopStyleColor();
            ImGui::PopFont();
            continue;
          }
          // lien : bouton invisible de la taille du morceau (survol et clic fiables), texte souligne par-dessus
          ImGui::PushID(&span);
          ImGui::InvisibleButton(std::to_string(start).c_str(), ImVec2(std::max(chunk_width, 1.0f), size));
          ImGui::PopID();
          const bool hovered = ImGui::IsItemHovered();
          const ImU32 ink     = hovered ? ImGui::GetColorU32(colors::blue) : color;
          draw->AddText(font, size, at, ink, chunk.c_str());
          draw->AddLine(ImVec2(at.x, at.y + size - 1.0f), ImVec2(at.x + (last ? word_width : chunk_width), at.y + size - 1.0f),
                        ink, hovered ? 1.5f : 1.0f);
          if (hovered) {
            ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
            ImGui::SetTooltip("%s", link_hint(span.link).c_str());
          }
          if (ImGui::IsItemClicked(ImGuiMouseButton_Left))
            clicked_link_ = span.link;
        }
      }
    }

    std::string link_hint(const std::string &link) const {
      const md::LinkTarget target = md::resolve_link(current_, link);
      switch (target.kind) {
        case md::LinkTarget::External: return target.path + "\n(clic : copie l'adresse)";
        case md::LinkTarget::Anchor: return "Section de cette page";
        case md::LinkTarget::Page:
          if (const md::Page *page = find_page(target.path))
            return page->title + (target.anchor.empty() ? "" : " (section)") + "\ndocs/" + page->path;
          return "Page absente de la documentation : " + target.path;
        case md::LinkTarget::Outside: return target.path + "\n(fichier du dépôt, hors documentation)";
      }
      return link;
    }
};

} // namespace

std::unique_ptr< AppWindow > make_doc_window() { return std::make_unique< DocWindow >(); }

} // namespace qrprotec
