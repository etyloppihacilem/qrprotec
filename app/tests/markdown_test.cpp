// Lecteur de documentation du poste : Markdown de docs/, liens relatifs, ancres et arborescence
#include "../src/core/markdown.hpp"

#include <algorithm>
#include <cassert>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <map>
#include <sstream>
#include <string>

namespace md = qrprotec::markdown;

int main()
{
    // texte en ligne : gras, italique, code, liens, echappements
    {
        const std::vector< md::Span > spans = md::parse_inline("Le **TECH** voit `make doc` et [la doc](utilisateur/README.md), *pas* snake_case_name \\*x\\*.");
        assert(md::plain_text(spans) == "Le TECH voit make doc et la doc, pas snake_case_name *x*.");
        bool bold = false, code = false, link = false, italic = false;
        for (const md::Span &span : spans) {
            bold   = bold || (span.bold && span.text == "TECH");
            code   = code || (span.code && span.text == "make doc");
            link   = link || (span.link == "utilisateur/README.md" && span.text == "la doc");
            italic = italic || (span.italic && span.text == "pas");
            assert(!(span.italic && span.text.find("case") != std::string::npos)); // _ dans un mot : pas d'italique
        }
        assert(bold && code && link && italic);
        // lien avec du code dans le libelle, ** dans un code non interprete
        const std::vector< md::Span > inner = md::parse_inline("[`README.md`](../README.md) et `a**b**`");
        assert(inner[0].code && inner[0].link == "../README.md" && inner[0].text == "README.md");
        assert(inner.back().code && inner.back().text == "a**b**");
        const std::vector< md::Span > auto_link = md::parse_inline("voir <https://example.org/x>");
        assert(auto_link.back().link == "https://example.org/x");
    }

    // blocs : titres, listes imbriquees, suite d'element, code, citation, tableau
    {
        const std::string source = "# Titre\n\nUn paragraphe\nsur deux lignes.\n\n- un\n  suite de un\n  - sous-element\n- deux\n\n"
                                   "  paragraphe de deux\n\n1. premier\n2. second\n\n```sh\nmake doc\n  indente\n```\n\n"
                                   "> **Note.** citee\n> - liste citee\n\n| A | B |\n|---|:-:|\n| `x \\| y` | [l](a.md) |\n\n---\n## Titre\n";
        const std::vector< md::Block > blocks = md::parse(source);
        std::size_t index = 0;
        assert(blocks[index].kind == md::BlockKind::Heading && blocks[index].level == 1 && blocks[index].anchor == "titre");
        ++index;
        assert(blocks[index].kind == md::BlockKind::Paragraph && md::plain_text(blocks[index].spans) == "Un paragraphe sur deux lignes.");
        ++index;
        assert(blocks[index].kind == md::BlockKind::ListItem && blocks[index].level == 0 && blocks[index].marker == "•");
        assert(md::plain_text(blocks[index].spans) == "un suite de un");
        ++index;
        assert(blocks[index].kind == md::BlockKind::ListItem && blocks[index].level == 1);
        ++index;
        assert(blocks[index].kind == md::BlockKind::ListItem && blocks[index].level == 0 && md::plain_text(blocks[index].spans) == "deux");
        ++index;
        assert(blocks[index].kind == md::BlockKind::ListItem && blocks[index].marker.empty() && blocks[index].level == 0);
        ++index;
        assert(blocks[index].kind == md::BlockKind::ListItem && blocks[index].marker == "1." && blocks[index].level == 0);
        ++index;
        assert(blocks[index].marker == "2.");
        ++index;
        assert(blocks[index].kind == md::BlockKind::Code && blocks[index].code == "make doc\n  indente");
        ++index;
        assert(blocks[index].kind == md::BlockKind::Quote && blocks[index].children.size() == 2);
        assert(blocks[index].children[0].spans[0].bold && blocks[index].children[1].kind == md::BlockKind::ListItem);
        ++index;
        assert(blocks[index].kind == md::BlockKind::Table && blocks[index].header.size() == 2 && blocks[index].rows.size() == 1);
        assert(md::plain_text(blocks[index].rows[0][0]) == "x | y" && blocks[index].rows[0][1][0].link == "a.md");
        ++index;
        assert(blocks[index].kind == md::BlockKind::Rule);
        ++index;
        assert(blocks[index].kind == md::BlockKind::Heading && blocks[index].anchor == "titre-1"); // ancre en double
        assert(index + 1 == blocks.size());
    }

    // ancres a la GitHub (accents gardes, ponctuation retiree)
    {
        assert(md::slug("Badge, PIN et rôles") == "badge-pin-et-rôles");
        assert(md::slug("Changer de modèle d'imprimante") == "changer-de-modèle-dimprimante");
        assert(md::slug("Répartition des items entre les lots") == "répartition-des-items-entre-les-lots");
        assert(md::slug("Poste de bureau (sans kiosk)") == "poste-de-bureau-sans-kiosk");
        assert(md::slug("État des lots") == "état-des-lots");
        assert(md::slug("`make doc` : « PDF »") == "make-doc---pdf-"); // comme GitHub : chaque espace donne un tiret
    }

    // liens relatifs
    {
        md::LinkTarget target = md::resolve_link("technique/back.md", "../installation/README.md#mise-à-jour");
        assert(target.kind == md::LinkTarget::Page && target.path == "installation/README.md" && target.anchor == "mise-à-jour");
        target = md::resolve_link("README.md", "utilisateur/README.md");
        assert(target.kind == md::LinkTarget::Page && target.path == "utilisateur/README.md");
        target = md::resolve_link("technique/back.md", "regles-de-gestion.md");
        assert(target.kind == md::LinkTarget::Page && target.path == "technique/regles-de-gestion.md");
        target = md::resolve_link("README.md", "../app/templates/README.md");
        assert(target.kind == md::LinkTarget::Outside && target.path == "app/templates/README.md");
        target = md::resolve_link("technique/back.md", "#deux-api");
        assert(target.kind == md::LinkTarget::Anchor && target.anchor == "deux-api");
        target = md::resolve_link("README.md", "https://github.com/x");
        assert(target.kind == md::LinkTarget::External);
        target = md::resolve_link("a/b.md", "c%20d.md");
        assert(target.path == "a/c d.md");
    }

    // arborescence : README d'abord, puis l'ordre du sommaire
    {
        const md::Library library({
          { "README.md", "# Doc\n\n[Utiliser](utilisateur/README.md)\n[Installer](installation/README.md)\n" },
          { "installation/README.md", "# Installer\n" },
          { "utilisateur/README.md", "# Guide\n\n- [Notions](notions.md)\n- [Admin](administration.md)\n" },
          { "utilisateur/administration.md", "# Administration\n" },
          { "utilisateur/notions.md", "# Notions\n" },
          { "utilisateur/zzz.md", "Pas de titre\n" },
        });
        const md::Folder &root = library.root();
        assert(root.readme == "README.md" && root.folders.size() == 2);
        assert(root.folders[0].path == "utilisateur" && root.folders[0].title == "Guide");
        assert(root.folders[1].path == "installation");
        const std::vector< std::string > expected{ "utilisateur/notions.md", "utilisateur/administration.md", "utilisateur/zzz.md" };
        assert(root.folders[0].pages == expected);
        assert(library.page("utilisateur/zzz.md")->title == "zzz.md");
    }

    // vraies pages de docs/ : chaque lien relatif mene a une page embarquee (ou hors de docs/), chaque ancre existe
#ifdef QRPROTEC_DOCS_SOURCE_DIR
    {
        const std::filesystem::path          root(QRPROTEC_DOCS_SOURCE_DIR);
        std::map< std::string, std::string > files;
        for (const auto &entry : std::filesystem::recursive_directory_iterator(root)) {
            if (!entry.is_regular_file() || entry.path().extension() != ".md")
                continue;
            const std::string path = std::filesystem::relative(entry.path(), root).generic_string();
            if (path.rfind("pdf/", 0) == 0)
                continue;
            std::ifstream      input(entry.path(), std::ios::binary);
            std::ostringstream content;
            content << input.rdbuf();
            files[path] = content.str();
        }
        assert(files.count("README.md") == 1);
        const md::Library library(files);
        std::size_t       checked = 0;
        const auto        anchors = [&](const std::string &path) {
            std::vector< std::string > list;
            for (const md::Block &block : md::parse(library.page(path)->source))
                if (block.kind == md::BlockKind::Heading)
                    list.push_back(block.anchor);
            return list;
        };
        const auto check_spans = [&](const md::Page &page, const std::vector< md::Span > &spans) {
            for (const md::Span &span : spans) {
                if (span.link.empty())
                    continue;
                const md::LinkTarget target = md::resolve_link(page.path, span.link);
                std::string          path   = target.kind == md::LinkTarget::Anchor ? page.path : target.path;
                if (target.kind == md::LinkTarget::Page && library.page(path) == nullptr)
                    path += "/README.md";
                if (target.kind == md::LinkTarget::Page || target.kind == md::LinkTarget::Anchor) {
                    if (library.page(path) == nullptr) {
                        std::fprintf(stderr, "%s : lien casse %s\n", page.path.c_str(), span.link.c_str());
                        assert(false);
                    }
                    if (!target.anchor.empty()) {
                        const std::vector< std::string > list = anchors(path);
                        if (std::find(list.begin(), list.end(), target.anchor) == list.end()) {
                            std::fprintf(stderr, "%s : ancre introuvable %s\n", page.path.c_str(), span.link.c_str());
                            assert(false);
                        }
                    }
                    ++checked;
                }
            }
        };
        for (const md::Page &page : library.pages()) {
            const auto walk = [&](const auto &self, const std::vector< md::Block > &blocks) -> void {
                for (const md::Block &block : blocks) {
                    check_spans(page, block.spans);
                    self(self, block.children);
                    for (const auto &cell : block.header)
                        check_spans(page, cell);
                    for (const auto &row : block.rows)
                        for (const auto &cell : row)
                            check_spans(page, cell);
                }
            };
            walk(walk, md::parse(page.source));
        }
        assert(checked > 20);
        // toutes les pages sont dans l'arborescence
        std::size_t listed = 0;
        const auto  count  = [&](const auto &self, const md::Folder &folder) -> void {
            listed += folder.pages.size() + (folder.readme.empty() ? 0 : 1);
            for (const md::Folder &child : folder.folders)
                self(self, child);
        };
        count(count, library.root());
        assert(listed == library.pages().size());
        assert(library.root().folders.size() >= 3 && library.root().folders[0].path == "utilisateur");
    }
#endif
    return 0;
}
