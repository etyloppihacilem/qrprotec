/* ##################################646f75627420796f7572206f776e206578697374656e6365###################################

               """          markdown.hpp
        -\-    _|__
         |\___/  . \        Created on 26 Nov. 2026 at 20:00
         \     /(((/        by hmelica
          \___/)))/         hmelica@student.42.fr

##################################################################################################################### */

#pragma once

#include <cstddef>
#include <map>
#include <string>
#include <vector>

// Lecteur de la documentation au poste (Administration > Documentation) : sous-ensemble du Markdown ecrit dans
// docs/ (titres, paragraphes, listes imbriquees, blocs de code, citations, tableaux, liens, gras, italique, code),
// liens relatifs entre pages et ancres a la GitHub. Sans interface : teste par tests/markdown_test.cpp, dessine par
// ui/windows/doc_window.cpp.
namespace qrprotec::markdown {

// Morceau de texte d'un meme style
struct Span {
    std::string text; // "\n" seul : retour a la ligne force
    bool        bold   = false;
    bool        italic = false;
    bool        code   = false;
    std::string link; // cible du lien tel qu'ecrit (vide : pas un lien)
};

std::vector< Span > parse_inline(const std::string &text);
// Texte sans mise en forme (titres dans l'arborescence, ancres)
std::string plain_text(const std::vector< Span > &spans);

enum class BlockKind { Heading, Paragraph, ListItem, Code, Quote, Table, Rule };

struct Block {
    BlockKind                               kind  = BlockKind::Paragraph;
    int                                     level = 0; // titre : 1 a 6 ; element de liste : profondeur (0, 1...)
    std::string                             marker;    // element de liste : "•" ou "1."
    std::vector< Span >                     spans;     // titre, paragraphe, element de liste
    std::string                             code;      // bloc de code
    std::vector< Block >                    children;  // citation
    std::vector< std::vector< Span > >      header;    // tableau
    std::vector< std::vector< std::vector< Span > > > rows;
    std::string                             anchor; // titre : ancre a la GitHub (unique dans la page)
};

std::vector< Block > parse(const std::string &source);

// Ancre GitHub d'un titre : minuscules, ponctuation retiree (accents gardes), espaces en tirets.
std::string slug(const std::string &heading);

// Cible d'un lien depuis la page `from` (chemin relatif a docs/, ex : "technique/back.md")
struct LinkTarget {
    enum Kind { External, Anchor, Page, Outside } kind = External;
    std::string path;   // Page : chemin dans docs/ ; Outside : chemin dans le depot ; External : URL
    std::string anchor; // ancre (sans #), decodee
};
LinkTarget resolve_link(const std::string &from, const std::string &link);

// Pages de la documentation et arborescence. Dans chaque dossier : son README d'abord, puis les pages dans l'ordre
// des liens de ce README (son sommaire), les autres par nom ; les dossiers dans l'ordre des liens du README parent.
struct Page {
    std::string path;  // "utilisateur/notions.md"
    std::string title; // premier titre de niveau 1, sinon nom du fichier
    std::string source;
};

struct Folder {
    std::string            path; // "" : racine, sinon "utilisateur"
    std::string            title; // titre de son README, sinon nom du dossier
    std::string            readme; // chemin du README (vide s'il n'y en a pas)
    std::vector< std::string > pages; // chemins des pages hors README, dans l'ordre
    std::vector< Folder >  folders;
};

class Library {
  public:
    // files : chemin relatif a docs/ (separateur /) -> contenu
    explicit Library(const std::map< std::string, std::string > &files);

    const Page             *page(const std::string &path) const;
    const Folder           &root() const { return root_; }
    const std::vector< Page > &pages() const { return pages_; }
    bool                    empty() const { return pages_.empty(); }

  private:
    std::vector< Page > pages_;
    std::map< std::string, std::size_t > index_;
    Folder              root_;
};

} // namespace qrprotec::markdown
